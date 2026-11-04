// The Win32 and NT subset the native runtime uses, for the console
// (see win32_ps5.h for what it covers and why).
//
// Kernel objects live behind HANDLEs in one table. Every wait goes through one
// lock: a waiting thread registers on the objects it waits for and sleeps on
// its own condition variable, and a state change wakes exactly the threads
// registered on that object. User APCs are queued per thread and run when that
// thread enters an alertable wait, as on Windows. Waitable timers fire from one
// service thread.
#if defined(__PROSPERO__)

#include "win32_ps5.h"

#include "input_ps5.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xmmintrin.h>

namespace {

using Clock = std::chrono::steady_clock;

// --- Last error -------------------------------------------------------------------

DWORD& lastError() {
    static thread_local DWORD value = 0;
    return value;
}

DWORD errorFromErrno(int error) {
    switch (error) {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM: return ERROR_ACCESS_DENIED;
    case EROFS: return ERROR_WRITE_PROTECT;
    case EEXIST: return ERROR_FILE_EXISTS;
    case ENOSPC: return ERROR_DISK_FULL;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case EBUSY: return ERROR_BUSY;
    default: return ERROR_INVALID_PARAMETER;
    }
}

BOOL fail(DWORD error) {
    lastError() = error;
    return FALSE;
}

// --- Threads' own state -------------------------------------------------------------

struct ThreadObject;

struct ThreadData {
    DWORD id = 0;
    // Waits sleep on this, under the wait lock.
    std::condition_variable wake;
    std::deque<std::function<void()>> apcs;
    std::weak_ptr<ThreadObject> self;
    // Creation suspends a thread here until its count reaches zero.
    int suspendCount = 0;
};

std::atomic<DWORD> nextThreadId{0x1000};

std::shared_ptr<ThreadData>& threadDataSlot() {
    static thread_local std::shared_ptr<ThreadData> data;
    return data;
}

ThreadData& threadData() {
    auto& data = threadDataSlot();
    if (!data) {
        data = std::make_shared<ThreadData>();
        data->id = nextThreadId.fetch_add(4);
    }
    return *data;
}

// --- Objects and handles -------------------------------------------------------------

enum class Kind { Event, Semaphore, Mutex, Timer, Thread, File, Module, Hash };

struct Object {
    explicit Object(Kind value) : kind(value) {}
    virtual ~Object() = default;
    const Kind kind;
    // Threads waiting for this object; guarded by the wait lock.
    std::vector<ThreadData*> waiters;
};

struct Event : Object {
    Event(bool manualReset, bool initial) : Object(Kind::Event), manual(manualReset), signaled(initial) {}
    bool manual;
    bool signaled;
};

struct Semaphore : Object {
    Semaphore(LONG initial, LONG maximumCount) : Object(Kind::Semaphore), count(initial), maximum(maximumCount) {}
    LONG count;
    LONG maximum;
};

struct Mutex : Object {
    Mutex() : Object(Kind::Mutex) {}
    DWORD owner = 0;
    LONG recursion = 0;
};

struct Timer : Object {
    explicit Timer(bool manualReset) : Object(Kind::Timer), manual(manualReset) {}
    bool manual;
    bool signaled = false;
    bool armed = false;
    Clock::time_point due{};
    LONG period = 0;
    PTIMERAPCROUTINE routine = nullptr;
    void* argument = nullptr;
    std::weak_ptr<ThreadData> apcThread;
};

struct ThreadObject : Object {
    ThreadObject() : Object(Kind::Thread) {}
    std::shared_ptr<ThreadData> data;
    pthread_t thread{};
    bool started = false;
    bool exited = false;
    DWORD exitCode = STILL_ACTIVE;
    LPTHREAD_START_ROUTINE start = nullptr;
    void* parameter = nullptr;
    void* stack = nullptr;
    int priority = THREAD_PRIORITY_NORMAL;
    DWORD_PTR affinity = 0;
};

struct FileObject : Object {
    FileObject() : Object(Kind::File) {}
    ~FileObject() override {
        if (fd >= 0) ::close(fd);
        if (deleteOnClose && !path.empty()) {
            if (directory) ::rmdir(path.c_str());
            else ::unlink(path.c_str());
        }
    }
    int fd = -1;
    std::string path;
    bool directory = false;
    bool deleteOnClose = false;
    // A directory's listing for NtQueryDirectoryFile, taken on its first
    // query or a restart.
    std::vector<std::string> listing;
    size_t cursor = 0;
    bool listed = false;
};

struct Module : Object {
    explicit Module(std::string value) : Object(Kind::Module), name(std::move(value)) {}
    std::string name;
};

std::mutex waitLock;

std::mutex handleLock;
std::unordered_map<uintptr_t, std::shared_ptr<Object>> handles;
uintptr_t nextHandle = 0x1000;

HANDLE insert(std::shared_ptr<Object> object) {
    std::lock_guard lock(handleLock);
    const uintptr_t value = nextHandle;
    nextHandle += 4;
    handles.emplace(value, std::move(object));
    return reinterpret_cast<HANDLE>(value);
}

std::shared_ptr<ThreadObject> currentThreadObject();

std::shared_ptr<Object> lookup(HANDLE handle) {
    const auto value = reinterpret_cast<uintptr_t>(handle);
    if (value == uintptr_t(-2)) return currentThreadObject();
    std::lock_guard lock(handleLock);
    auto found = handles.find(value);
    return found == handles.end() ? nullptr : found->second;
}

template <class T>
std::shared_ptr<T> lookupAs(HANDLE handle, Kind kind) {
    auto object = lookup(handle);
    if (!object || object->kind != kind) return nullptr;
    return std::static_pointer_cast<T>(object);
}

// A thread the runtime did not create (the main thread, the audio feed)
// gets its thread object the first time it is named.
std::shared_ptr<ThreadObject> currentThreadObject() {
    ThreadData& data = threadData();
    if (auto self = data.self.lock()) return self;
    auto object = std::make_shared<ThreadObject>();
    object->data = threadDataSlot();
    object->thread = pthread_self();
    object->started = true;
    data.self = object;
    // Kept alive for the thread's life, as a Windows thread object is while
    // the thread runs.
    static thread_local std::shared_ptr<ThreadObject> keep;
    keep = object;
    return object;
}

// --- Waiting ----------------------------------------------------------------------

void wakeWaiters(Object& object) {
    for (ThreadData* waiter : object.waiters) waiter->wake.notify_one();
}

bool signaled(const Object& object, const ThreadData& thread) {
    switch (object.kind) {
    case Kind::Event: return static_cast<const Event&>(object).signaled;
    case Kind::Semaphore: return static_cast<const Semaphore&>(object).count > 0;
    case Kind::Mutex: {
        const auto& mutex = static_cast<const Mutex&>(object);
        return mutex.owner == 0 || mutex.owner == thread.id;
    }
    case Kind::Timer: return static_cast<const Timer&>(object).signaled;
    case Kind::Thread: return static_cast<const ThreadObject&>(object).exited;
    case Kind::File: return true; // I/O on this layer completes before it returns
    default: return false;
    }
}

void acquire(Object& object, const ThreadData& thread) {
    switch (object.kind) {
    case Kind::Event: {
        auto& event = static_cast<Event&>(object);
        if (!event.manual) event.signaled = false;
        break;
    }
    case Kind::Semaphore: --static_cast<Semaphore&>(object).count; break;
    case Kind::Mutex: {
        auto& mutex = static_cast<Mutex&>(object);
        mutex.owner = thread.id;
        ++mutex.recursion;
        break;
    }
    case Kind::Timer: {
        auto& timer = static_cast<Timer&>(object);
        if (!timer.manual) timer.signaled = false;
        break;
    }
    default: break;
    }
}

// Runs the calling thread's queued APCs with the wait lock released. Returns
// true when it ran any.
bool runApcs(std::unique_lock<std::mutex>& lock, ThreadData& thread) {
    if (thread.apcs.empty()) return false;
    std::deque<std::function<void()>> pending;
    pending.swap(thread.apcs);
    lock.unlock();
    for (auto& apc : pending) apc();
    lock.lock();
    return true;
}

DWORD waitFor(DWORD count, const std::shared_ptr<Object>* objects, bool waitAll, DWORD milliseconds,
              bool alertable) {
    ThreadData& thread = threadData();
    const bool forever = milliseconds == INFINITE;
    const auto deadline = Clock::now() + std::chrono::milliseconds(forever ? 0 : milliseconds);
    std::unique_lock lock(waitLock);
    for (;;) {
        if (alertable && runApcs(lock, thread)) return WAIT_IO_COMPLETION;
        if (count) {
            if (waitAll) {
                bool all = true;
                for (DWORD i = 0; i < count && all; ++i) all = signaled(*objects[i], thread);
                if (all) {
                    for (DWORD i = 0; i < count; ++i) acquire(*objects[i], thread);
                    return WAIT_OBJECT_0;
                }
            } else {
                for (DWORD i = 0; i < count; ++i) {
                    if (signaled(*objects[i], thread)) {
                        acquire(*objects[i], thread);
                        return WAIT_OBJECT_0 + i;
                    }
                }
            }
        }
        if (!forever && Clock::now() >= deadline) return WAIT_TIMEOUT;
        for (DWORD i = 0; i < count; ++i) objects[i]->waiters.push_back(&thread);
        if (forever) thread.wake.wait(lock);
        else thread.wake.wait_until(lock, deadline);
        for (DWORD i = 0; i < count; ++i) {
            auto& waiters = objects[i]->waiters;
            waiters.erase(std::find(waiters.begin(), waiters.end(), &thread));
        }
    }
}

// --- Timers -------------------------------------------------------------------------

struct TimerService {
    std::condition_variable changed;
    std::vector<std::weak_ptr<Timer>> armed;
    bool started = false;
};

TimerService& timerService() {
    static TimerService service;
    return service;
}

void fire(Timer& timer) {
    timer.signaled = true;
    wakeWaiters(timer);
    if (timer.routine) {
        if (auto thread = timer.apcThread.lock()) {
            const auto routine = timer.routine;
            void* argument = timer.argument;
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            thread->apcs.emplace_back([routine, argument, now] {
                routine(argument, now.dwLowDateTime, now.dwHighDateTime);
            });
            thread->wake.notify_one();
        }
    }
    if (timer.period > 0) timer.due += std::chrono::milliseconds(timer.period);
    else timer.armed = false;
}

void timerThread() {
    TimerService& service = timerService();
    std::unique_lock lock(waitLock);
    for (;;) {
        Clock::time_point next = Clock::time_point::max();
        const auto now = Clock::now();
        for (auto it = service.armed.begin(); it != service.armed.end();) {
            auto timer = it->lock();
            if (!timer || !timer->armed) {
                it = service.armed.erase(it);
                continue;
            }
            if (timer->due <= now) fire(*timer);
            if (timer->armed) next = std::min(next, timer->due);
            ++it;
        }
        if (next == Clock::time_point::max()) service.changed.wait(lock);
        else service.changed.wait_until(lock, next);
    }
}

// Under the wait lock.
void armTimer(const std::shared_ptr<Timer>& timer) {
    TimerService& service = timerService();
    if (!service.started) {
        service.started = true;
        std::thread(timerThread).detach();
    }
    service.armed.push_back(timer);
    service.changed.notify_one();
}

// --- Threads ------------------------------------------------------------------------

// Finished threads' stacks, freed once their threads are joined.
std::mutex reaperLock;
std::vector<std::pair<pthread_t, void*>> finished;

void reapFinished() {
    std::vector<std::pair<pthread_t, void*>> done;
    {
        std::lock_guard lock(reaperLock);
        done.swap(finished);
    }
    for (auto& [thread, stack] : done) {
        pthread_join(thread, nullptr);
        std::free(stack);
    }
}

void finishThread(ThreadObject& object, DWORD code) {
    {
        std::lock_guard lock(waitLock);
        object.exited = true;
        object.exitCode = code;
        wakeWaiters(object);
    }
    std::lock_guard lock(reaperLock);
    finished.emplace_back(object.thread, object.stack);
    object.stack = nullptr;
}

void* threadMain(void* raw) {
    std::shared_ptr<ThreadObject> object(*static_cast<std::shared_ptr<ThreadObject>*>(raw));
    delete static_cast<std::shared_ptr<ThreadObject>*>(raw);
    threadDataSlot() = object->data;
    object->data->self = object;
    {
        std::unique_lock lock(waitLock);
        object->data->wake.wait(lock, [&] { return object->data->suspendCount == 0; });
    }
    const DWORD code = object->start(object->parameter);
    finishThread(*object, code);
    return nullptr;
}

// --- Guest address space ------------------------------------------------------------

struct PageRun {
    DWORD state;
    DWORD protect;
};

struct GuestSpace {
    std::mutex lock;
    uint8_t* base = nullptr;
    uint64_t size = 0;
    // Runs of pages with one state, keyed by their first byte offset; they
    // cover [0, size) without gaps.
    std::map<uint64_t, PageRun> runs;
};

GuestSpace& guestSpace() {
    static GuestSpace space;
    return space;
}

constexpr uint64_t kPage = 4096;

bool inGuest(const GuestSpace& space, const void* address, uint64_t bytes = 1) {
    const auto* at = static_cast<const uint8_t*>(address);
    return space.base && at >= space.base && uint64_t(at - space.base) + bytes <= space.size;
}

// Splits the run containing `offset` so that one starts there.
void splitAt(GuestSpace& space, uint64_t offset) {
    if (offset >= space.size) return;
    auto it = space.runs.upper_bound(offset);
    --it;
    if (it->first != offset) space.runs.emplace(offset, it->second);
}

void setRuns(GuestSpace& space, uint64_t begin, uint64_t end, PageRun run) {
    splitAt(space, begin);
    splitAt(space, end);
    auto first = space.runs.find(begin);
    auto last = end >= space.size ? space.runs.end() : space.runs.find(end);
    space.runs.erase(first, last);
    space.runs.emplace(begin, run);
    // Merge with equal neighbours.
    auto it = space.runs.find(begin);
    if (it != space.runs.begin()) {
        auto before = std::prev(it);
        if (before->second.state == run.state && before->second.protect == run.protect) {
            space.runs.erase(it);
            it = before;
        }
    }
    auto after = std::next(it);
    if (after != space.runs.end() && after->second.state == it->second.state &&
        after->second.protect == it->second.protect)
        space.runs.erase(after);
}

uint64_t runEnd(const GuestSpace& space, std::map<uint64_t, PageRun>::const_iterator it) {
    auto next = std::next(it);
    return next == space.runs.end() ? space.size : next->first;
}

// Host allocations made through VirtualAlloc with no address.
std::mutex hostAllocationLock;
std::map<uintptr_t, size_t> hostAllocations;

// --- Strings ------------------------------------------------------------------------

std::u32string decodeUtf8(const char* text, size_t bytes, bool& valid) {
    std::u32string out;
    valid = true;
    for (size_t i = 0; i < bytes;) {
        const auto c = static_cast<unsigned char>(text[i]);
        char32_t code;
        size_t extra;
        if (c < 0x80) { code = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { code = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { code = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { code = c & 0x07; extra = 3; }
        else { valid = false; out.push_back(0xFFFD); ++i; continue; }
        if (i + extra >= bytes + (extra ? 0 : 1) && extra) {
            valid = false;
            out.push_back(0xFFFD);
            break;
        }
        bool ok = true;
        for (size_t k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) { ok = false; break; }
            code = (code << 6) | (next & 0x3F);
        }
        if (!ok) { valid = false; out.push_back(0xFFFD); ++i; continue; }
        out.push_back(code);
        i += extra + 1;
    }
    return out;
}

void encodeUtf8(char32_t code, std::string& out) {
    if (code < 0x80) out.push_back(char(code));
    else if (code < 0x800) {
        out.push_back(char(0xC0 | (code >> 6)));
        out.push_back(char(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(char(0xE0 | (code >> 12)));
        out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(char(0x80 | (code & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (code >> 18)));
        out.push_back(char(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(char(0x80 | (code & 0x3F)));
    }
}

std::string utf8(const wchar_t* text, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) encodeUtf8(char32_t(text[i]), out);
    return out;
}

std::wstring widen(const char* text) {
    bool valid = true;
    const auto decoded = decodeUtf8(text, std::strlen(text), valid);
    return std::wstring(decoded.begin(), decoded.end());
}

int compareIgnoringCase(const wchar_t* a, const wchar_t* b, size_t limit) {
    for (size_t i = 0; i < limit; ++i) {
        const wint_t x = std::towlower(wint_t(a[i])), y = std::towlower(wint_t(b[i]));
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}

// --- Files ----------------------------------------------------------------------------

bool isDirectory(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

uint64_t fileTimeOf(const timespec& time) {
    return (uint64_t(time.tv_sec) + 11644473600ull) * 10000000ull + uint64_t(time.tv_nsec) / 100;
}

DWORD attributesOf(const struct stat& st) {
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
}

HANDLE openFile(const std::string& path, DWORD access, DWORD disposition, DWORD flags) {
    const bool write = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    const bool wantsDirectory = (flags & FILE_FLAG_BACKUP_SEMANTICS) && isDirectory(path);
    int mode = wantsDirectory ? O_RDONLY : write ? O_RDWR : O_RDONLY;
    struct stat existing {};
    const bool existed = ::stat(path.c_str(), &existing) == 0;
    switch (disposition) {
    case CREATE_NEW: mode |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: mode |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING: break;
    case OPEN_ALWAYS: mode |= O_CREAT; break;
    case TRUNCATE_EXISTING: mode |= O_TRUNC; break;
    default: fail(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE;
    }
    const int fd = ::open(path.c_str(), mode, 0644);
    if (fd < 0) {
        fail(errorFromErrno(errno));
        return INVALID_HANDLE_VALUE;
    }
    auto file = std::make_shared<FileObject>();
    file->fd = fd;
    file->path = path;
    file->directory = wantsDirectory;
    file->deleteOnClose = (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0;
    lastError() = (existed && (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS)) ? ERROR_ALREADY_EXISTS : 0;
    return insert(std::move(file));
}

// --- NT status ------------------------------------------------------------------------

constexpr NTSTATUS kSuccess = 0;
constexpr NTSTATUS kBufferOverflow = NTSTATUS(0x80000005u);
constexpr NTSTATUS kNoMoreFiles = NTSTATUS(0x80000006u);
constexpr NTSTATUS kNotImplemented = NTSTATUS(0xC0000002u);
constexpr NTSTATUS kInvalidInfoClass = NTSTATUS(0xC0000003u);
constexpr NTSTATUS kInfoLengthMismatch = NTSTATUS(0xC0000004u);
constexpr NTSTATUS kInvalidHandle = NTSTATUS(0xC0000008u);
constexpr NTSTATUS kInvalidParameter = NTSTATUS(0xC000000Du);
constexpr NTSTATUS kNoSuchFile = NTSTATUS(0xC000000Fu);
constexpr NTSTATUS kEndOfFile = NTSTATUS(0xC0000011u);
constexpr NTSTATUS kAccessDenied = NTSTATUS(0xC0000022u);
constexpr NTSTATUS kNameNotFound = NTSTATUS(0xC0000034u);
constexpr NTSTATUS kNameCollision = NTSTATUS(0xC0000035u);
constexpr NTSTATUS kPathNotFound = NTSTATUS(0xC000003Au);
constexpr NTSTATUS kDiskFull = NTSTATUS(0xC000007Fu);
constexpr NTSTATUS kMediaWriteProtected = NTSTATUS(0xC00000A2u);
constexpr NTSTATUS kFileIsADirectory = NTSTATUS(0xC00000BAu);
constexpr NTSTATUS kNotADirectory = NTSTATUS(0xC0000103u);
constexpr NTSTATUS kDirectoryNotEmpty = NTSTATUS(0xC0000101u);
constexpr NTSTATUS kNoMemory = NTSTATUS(0xC0000017u);
constexpr NTSTATUS kIoDeviceError = NTSTATUS(0xC0000185u);
constexpr ULONG kFileDeleteOnClose = 0x1000;

NTSTATUS statusFromErrno(int error) {
    switch (error) {
    case ENOENT: return kNameNotFound;
    case ENOTDIR: return kPathNotFound;
    case EACCES:
    case EPERM: return kAccessDenied;
    case EROFS: return kMediaWriteProtected;
    case EEXIST: return kNameCollision;
    case ENOSPC: return kDiskFull;
    case ENOMEM: return kNoMemory;
    case EISDIR: return kFileIsADirectory;
    case ENOTEMPTY: return kDirectoryNotEmpty;
    case EBADF: return kInvalidHandle;
    default: return kIoDeviceError;
    }
}

// The path an NT call names: "\??\" + a host path, relative to a directory
// handle when one is given. UNICODE_STRING lengths count two bytes per
// character, as filesystem.cpp builds them, whatever wchar_t's size.
bool ntPath(const OBJECT_ATTRIBUTES* attributes, std::string& path) {
    if (!attributes || !attributes->ObjectName || !attributes->ObjectName->Buffer) return false;
    const auto* name = attributes->ObjectName;
    std::string text = utf8(name->Buffer, name->Length / 2);
    if (text.rfind("\\??\\", 0) == 0) text.erase(0, 4);
    if (attributes->RootDirectory) {
        auto root = lookupAs<FileObject>(attributes->RootDirectory, Kind::File);
        if (!root) return false;
        std::replace(text.begin(), text.end(), '\\', '/');
        text = root->path + (text.empty() || text.front() == '/' ? "" : "/") + text;
    }
    path = std::move(text);
    return true;
}

void signalCompletion(HANDLE event, PIO_APC_ROUTINE apc, PVOID context, PIO_STATUS_BLOCK io) {
    if (event) SetEvent(event);
    if (apc) {
        ThreadData& thread = threadData();
        std::lock_guard lock(waitLock);
        thread.apcs.emplace_back([apc, context, io] { apc(context, io, 0); });
    }
}

void setIo(PIO_STATUS_BLOCK io, NTSTATUS status, ULONG_PTR information) {
    if (!io) return;
    io->Status = status;
    io->Information = information;
}

bool wildcardMatch(const wchar_t* pattern, const wchar_t* text) {
    if (!*pattern) return !*text;
    if (*pattern == L'*') {
        for (; *text; ++text)
            if (wildcardMatch(pattern + 1, text)) return true;
        return wildcardMatch(pattern + 1, text);
    }
    if (!*text) return false;
    if (*pattern != L'?' && std::towlower(wint_t(*pattern)) != std::towlower(wint_t(*text))) return false;
    return wildcardMatch(pattern + 1, text + 1);
}

// --- SHA-256 (bcrypt.h) ----------------------------------------------------------------

struct Sha256 : Object {
    Sha256() : Object(Kind::Hash) { reset(); }
    uint32_t state[8];
    uint8_t block[64];
    size_t used = 0;
    uint64_t length = 0;

    void reset() {
        static constexpr uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(state, initial, sizeof(state));
        used = 0;
        length = 0;
    }
    static uint32_t rotate(uint32_t value, int count) { return (value >> count) | (value << (32 - count)); }
    void compress(const uint8_t* data) {
        static constexpr uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(data[i * 4]) << 24 | uint32_t(data[i * 4 + 1]) << 16 | uint32_t(data[i * 4 + 2]) << 8 |
                   data[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
            const uint32_t choose = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + choose + k[i] + w[i];
            const uint32_t s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + majority;
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
    void update(const uint8_t* data, size_t bytes) {
        length += bytes;
        while (bytes) {
            const size_t take = std::min(bytes, sizeof(block) - used);
            std::memcpy(block + used, data, take);
            used += take;
            data += take;
            bytes -= take;
            if (used == sizeof(block)) {
                compress(block);
                used = 0;
            }
        }
    }
    void finish(uint8_t* out) {
        const uint64_t bits = length * 8;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (used != 56) update(&zero, 1);
        uint8_t size[8];
        for (int i = 0; i < 8; ++i) size[i] = uint8_t(bits >> (56 - i * 8));
        update(size, 8);
        for (int i = 0; i < 8; ++i)
            for (int b = 0; b < 4; ++b) out[i * 4 + b] = uint8_t(state[i] >> (24 - b * 8));
    }
};

// --- Settings files ------------------------------------------------------------------

struct IniLine {
    std::string text;
};

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool sameIgnoringCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() && ::strncasecmp(a.c_str(), b.c_str(), a.size()) == 0;
}

} // namespace

// --- Last error ------------------------------------------------------------------------

DWORD GetLastError() { return lastError(); }
void SetLastError(DWORD error) { lastError() = error; }

// --- Handles and waits -------------------------------------------------------------------

BOOL CloseHandle(HANDLE handle) {
    const auto value = reinterpret_cast<uintptr_t>(handle);
    if (value == uintptr_t(-1) || value == uintptr_t(-2)) return TRUE;
    std::shared_ptr<Object> object;
    {
        std::lock_guard lock(handleLock);
        auto found = handles.find(value);
        if (found == handles.end()) return fail(ERROR_INVALID_HANDLE);
        object = std::move(found->second);
        handles.erase(found);
    }
    // The object (a file's descriptor, a thread's bookkeeping) goes when its
    // last handle and any running thread let it go.
    return TRUE;
}

BOOL DuplicateHandle(HANDLE, HANDLE source, HANDLE, HANDLE* target, DWORD, BOOL, DWORD options) {
    auto object = lookup(source);
    if (!object || !target) return fail(ERROR_INVALID_HANDLE);
    *target = insert(std::move(object));
    if (options & DUPLICATE_CLOSE_SOURCE) CloseHandle(source);
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    return WaitForSingleObjectEx(handle, milliseconds, FALSE);
}

DWORD WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL alertable) {
    auto object = lookup(handle);
    if (!object) {
        lastError() = ERROR_INVALID_HANDLE;
        return WAIT_FAILED;
    }
    return waitFor(1, &object, false, milliseconds, alertable);
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE* list, BOOL waitAll, DWORD milliseconds) {
    return WaitForMultipleObjectsEx(count, list, waitAll, milliseconds, FALSE);
}

DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE* list, BOOL waitAll, DWORD milliseconds, BOOL alertable) {
    if (!count || count > MAXIMUM_WAIT_OBJECTS || !list) {
        lastError() = ERROR_INVALID_PARAMETER;
        return WAIT_FAILED;
    }
    std::vector<std::shared_ptr<Object>> objects(count);
    for (DWORD i = 0; i < count; ++i) {
        objects[i] = lookup(list[i]);
        if (!objects[i]) {
            lastError() = ERROR_INVALID_HANDLE;
            return WAIT_FAILED;
        }
    }
    return waitFor(count, objects.data(), waitAll != FALSE, milliseconds, alertable);
}

DWORD SleepEx(DWORD milliseconds, BOOL alertable) {
    if (milliseconds == 0 && !alertable) {
        sched_yield();
        return 0;
    }
    if (milliseconds == 0) {
        ThreadData& thread = threadData();
        std::unique_lock lock(waitLock);
        if (runApcs(lock, thread)) return WAIT_IO_COMPLETION;
        lock.unlock();
        sched_yield();
        return 0;
    }
    const DWORD result = waitFor(0, nullptr, false, milliseconds, alertable);
    return result == WAIT_IO_COMPLETION ? WAIT_IO_COMPLETION : 0;
}

BOOL SwitchToThread() {
    sched_yield();
    return TRUE;
}

HANDLE CreateEventW(LPSECURITY_ATTRIBUTES, BOOL manualReset, BOOL initialState, LPCWSTR) {
    return insert(std::make_shared<Event>(manualReset != FALSE, initialState != FALSE));
}

HANDLE CreateEventExW(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD flags, DWORD) {
    return insert(std::make_shared<Event>((flags & CREATE_EVENT_MANUAL_RESET) != 0,
                                          (flags & CREATE_EVENT_INITIAL_SET) != 0));
}

BOOL SetEvent(HANDLE handle) {
    auto event = lookupAs<Event>(handle, Kind::Event);
    if (!event) return fail(ERROR_INVALID_HANDLE);
    std::lock_guard lock(waitLock);
    event->signaled = true;
    wakeWaiters(*event);
    return TRUE;
}

BOOL ResetEvent(HANDLE handle) {
    auto event = lookupAs<Event>(handle, Kind::Event);
    if (!event) return fail(ERROR_INVALID_HANDLE);
    std::lock_guard lock(waitLock);
    event->signaled = false;
    return TRUE;
}

BOOL PulseEvent(HANDLE handle) {
    auto event = lookupAs<Event>(handle, Kind::Event);
    if (!event) return fail(ERROR_INVALID_HANDLE);
    std::lock_guard lock(waitLock);
    event->signaled = true;
    wakeWaiters(*event);
    event->signaled = false;
    return TRUE;
}

HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES, LONG initial, LONG maximum, LPCWSTR) {
    if (maximum <= 0 || initial < 0 || initial > maximum) {
        fail(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    return insert(std::make_shared<Semaphore>(initial, maximum));
}

HANDLE CreateSemaphoreExW(LPSECURITY_ATTRIBUTES attributes, LONG initial, LONG maximum, LPCWSTR name, DWORD,
                          DWORD) {
    return CreateSemaphoreW(attributes, initial, maximum, name);
}

BOOL ReleaseSemaphore(HANDLE handle, LONG count, LONG* previous) {
    auto semaphore = lookupAs<Semaphore>(handle, Kind::Semaphore);
    if (!semaphore) return fail(ERROR_INVALID_HANDLE);
    std::lock_guard lock(waitLock);
    if (count <= 0 || semaphore->count > semaphore->maximum - count) return fail(ERROR_INVALID_PARAMETER);
    if (previous) *previous = semaphore->count;
    semaphore->count += count;
    wakeWaiters(*semaphore);
    return TRUE;
}

// Named mutexes are machine-wide on Windows; the console runs one title, so a
// name is just this process's.
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES, BOOL initialOwner, LPCWSTR name) {
    static std::mutex namedLock;
    static std::unordered_map<std::wstring, std::weak_ptr<Mutex>> named;
    std::shared_ptr<Mutex> mutex;
    if (name) {
        std::lock_guard lock(namedLock);
        mutex = named[name].lock();
        if (!mutex) {
            mutex = std::make_shared<Mutex>();
            named[name] = mutex;
        }
    } else {
        mutex = std::make_shared<Mutex>();
    }
    if (initialOwner) {
        std::lock_guard lock(waitLock);
        if (mutex->owner == 0) {
            mutex->owner = threadData().id;
            mutex->recursion = 1;
        }
    }
    return insert(std::move(mutex));
}

BOOL ReleaseMutex(HANDLE handle) {
    auto mutex = lookupAs<Mutex>(handle, Kind::Mutex);
    if (!mutex) return fail(ERROR_INVALID_HANDLE);
    std::lock_guard lock(waitLock);
    if (mutex->owner != threadData().id) return fail(ERROR_ACCESS_DENIED);
    if (--mutex->recursion == 0) {
        mutex->owner = 0;
        wakeWaiters(*mutex);
    }
    return TRUE;
}

HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES, BOOL manualReset, LPCWSTR) {
    return insert(std::make_shared<Timer>(manualReset != FALSE));
}

HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD flags, DWORD) {
    return insert(std::make_shared<Timer>((flags & CREATE_WAITABLE_TIMER_MANUAL_RESET) != 0));
}

BOOL SetWaitableTimer(HANDLE handle, const LARGE_INTEGER* dueTime, LONG period, PTIMERAPCROUTINE routine,
                      void* argument, BOOL) {
    auto timer = lookupAs<Timer>(handle, Kind::Timer);
    if (!timer || !dueTime) return fail(ERROR_INVALID_HANDLE);
    // Negative due times are relative, in 100 ns; positive ones an absolute
    // UTC FILETIME, measured from now against the system clock.
    int64_t relative100ns;
    if (dueTime->QuadPart < 0) {
        relative100ns = -dueTime->QuadPart;
    } else {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        const int64_t current = int64_t((uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime);
        relative100ns = std::max<int64_t>(0, dueTime->QuadPart - current);
    }
    std::lock_guard lock(waitLock);
    timer->signaled = false;
    timer->due = Clock::now() + std::chrono::nanoseconds(relative100ns * 100);
    timer->period = period;
    timer->routine = routine;
    timer->argument = argument;
    timer->apcThread = threadDataSlot() ? threadDataSlot() : (threadData(), threadDataSlot());
    timer->armed = true;
    armTimer(timer);
    return TRUE;
}

BOOL CancelWaitableTimer(HANDLE handle) {
    auto timer = lookupAs<Timer>(handle, Kind::Timer);
    if (!timer) return fail(ERROR_INVALID_HANDLE);
    std::lock_guard lock(waitLock);
    timer->armed = false;
    return TRUE;
}

// --- Threads ---------------------------------------------------------------------------

HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T stackSize, LPTHREAD_START_ROUTINE start, void* parameter,
                    DWORD flags, DWORD* threadId) {
    reapFinished();
    auto object = std::make_shared<ThreadObject>();
    object->data = std::make_shared<ThreadData>();
    object->data->id = nextThreadId.fetch_add(4);
    object->data->suspendCount = (flags & CREATE_SUSPENDED) ? 1 : 0;
    object->start = start;
    object->parameter = parameter;
    // Stacks come from the title heap (direct memory): a thread's own mmap'd
    // stack would draw on the small flexible pool. Guest threads ask for
    // 64 MiB reservations, which Windows commits only as the stack grows; the
    // heap backs the whole stack at once, so a reservation is held to 16 MiB,
    // still sixteen times Windows' default thread stack.
    constexpr size_t kMaximumStack = size_t(16) << 20;
    size_t size = stackSize ? size_t(stackSize) : size_t(1) << 20;
    size = std::clamp<size_t>(size, 256u << 10, kMaximumStack);
    size = (size + 16383) & ~size_t(16383);
    object->stack = std::aligned_alloc(16384, size);
    if (!object->stack) {
        fail(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstack(&attributes, object->stack, size);
    auto* held = new std::shared_ptr<ThreadObject>(object);
    const int created = pthread_create(&object->thread, &attributes, threadMain, held);
    pthread_attr_destroy(&attributes);
    if (created != 0) {
        delete held;
        std::free(object->stack);
        fail(errorFromErrno(created));
        return nullptr;
    }
    object->started = true;
    if (threadId) *threadId = object->data->id;
    return insert(std::move(object));
}

DWORD ResumeThread(HANDLE handle) {
    auto thread = lookupAs<ThreadObject>(handle, Kind::Thread);
    if (!thread) {
        lastError() = ERROR_INVALID_HANDLE;
        return DWORD(-1);
    }
    std::lock_guard lock(waitLock);
    const DWORD previous = DWORD(thread->data->suspendCount);
    if (thread->data->suspendCount > 0 && --thread->data->suspendCount == 0) thread->data->wake.notify_one();
    return previous;
}

// A thread can be held only before it starts: the console cannot stop a
// running thread from another one.
DWORD SuspendThread(HANDLE handle) {
    auto thread = lookupAs<ThreadObject>(handle, Kind::Thread);
    if (!thread) {
        lastError() = ERROR_INVALID_HANDLE;
        return DWORD(-1);
    }
    std::lock_guard lock(waitLock);
    if (thread->data->suspendCount == 0) {
        lastError() = ERROR_NOT_SUPPORTED;
        return DWORD(-1);
    }
    return DWORD(thread->data->suspendCount++);
}

BOOL GetExitCodeThread(HANDLE handle, DWORD* exitCode) {
    auto thread = lookupAs<ThreadObject>(handle, Kind::Thread);
    if (!thread || !exitCode) return fail(ERROR_INVALID_HANDLE);
    std::lock_guard lock(waitLock);
    *exitCode = thread->exited ? thread->exitCode : STILL_ACTIVE;
    return TRUE;
}

void ExitThread(DWORD exitCode) {
    if (auto self = threadData().self.lock()) finishThread(*self, exitCode);
    pthread_exit(nullptr);
}

BOOL SetThreadPriority(HANDLE handle, int priority) {
    auto thread = lookupAs<ThreadObject>(handle, Kind::Thread);
    if (!thread) return fail(ERROR_INVALID_HANDLE);
    thread->priority = priority;
    return TRUE;
}

int GetThreadPriority(HANDLE handle) {
    auto thread = lookupAs<ThreadObject>(handle, Kind::Thread);
    return thread ? thread->priority : THREAD_PRIORITY_NORMAL;
}

// Affinity is recorded, not applied: the console schedules the title's threads
// over its cores itself.
DWORD_PTR SetThreadAffinityMask(HANDLE handle, DWORD_PTR mask) {
    auto thread = lookupAs<ThreadObject>(handle, Kind::Thread);
    if (!thread || !mask) {
        lastError() = ERROR_INVALID_PARAMETER;
        return 0;
    }
    DWORD_PTR system = 0, previous = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &previous, &system);
    if (thread->affinity) previous = thread->affinity;
    thread->affinity = mask;
    return previous;
}

DWORD GetThreadId(HANDLE handle) {
    auto thread = lookupAs<ThreadObject>(handle, Kind::Thread);
    return thread ? thread->data->id : 0;
}

DWORD GetCurrentThreadId() { return threadData().id; }

DWORD GetCurrentProcessId() { return DWORD(::getpid()); }

DWORD QueueUserAPC(PAPCFUNC routine, HANDLE handle, ULONG_PTR parameter) {
    auto thread = lookupAs<ThreadObject>(handle, Kind::Thread);
    if (!thread || !routine) return fail(ERROR_INVALID_HANDLE);
    std::lock_guard lock(waitLock);
    thread->data->apcs.emplace_back([routine, parameter] { routine(parameter); });
    thread->data->wake.notify_one();
    return TRUE;
}

void ExitProcess(UINT exitCode) {
    std::fflush(stdout);
    std::fflush(stderr);
    // stderr reaches the kernel log through a relay thread; give it the
    // moment it needs to forward the last lines before the process ends.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ::_exit(int(exitCode));
}

BOOL GetProcessAffinityMask(HANDLE, DWORD_PTR* processMask, DWORD_PTR* systemMask) {
    // The console's libc++ answers hardware_concurrency() with 0, which made
    // this a one-core mask and left the engine's scene worker pool empty: every
    // capture job ran on the game thread (2026-10-25). sysconf knows the count;
    // failing that, a title gets the 13 hardware threads of 7 SMT cores.
    // sysconf answered too low as well (the pool stayed empty), so an answer
    // below what any PS5 title has is not believed.
    static const unsigned counted = [] {
        unsigned count = std::thread::hardware_concurrency();
        const long online = ::sysconf(_SC_NPROCESSORS_ONLN);
        std::fprintf(stderr, "[Win32] hardware_concurrency=%u sysconf=%ld\n", count, online);
        if (count < 4 && online >= 4) count = unsigned(online);
        if (count < 4) count = 13;
        return count;
    }();
    const unsigned cores = std::max(1u, std::min<unsigned>(counted, 64));
    const DWORD_PTR mask = cores >= 64 ? ~DWORD_PTR(0) : (DWORD_PTR(1) << cores) - 1;
    if (processMask) *processMask = mask;
    if (systemMask) *systemMask = mask;
    return TRUE;
}

BOOL GetThreadGroupAffinity(HANDLE, GROUP_AFFINITY* affinity) {
    if (!affinity) return fail(ERROR_INVALID_PARAMETER);
    *affinity = {};
    DWORD_PTR system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &affinity->Mask, &system);
    return TRUE;
}

// No CPU sets on the console: callers fall back to the affinity mask.
BOOL GetSystemCpuSetInformation(SYSTEM_CPU_SET_INFORMATION*, ULONG, ULONG* returned, HANDLE, ULONG) {
    if (returned) *returned = 0;
    return fail(ERROR_NOT_SUPPORTED);
}

BOOL QueryThreadCycleTime(HANDLE handle, ULONG64* cycles) {
    if (!cycles) return fail(ERROR_INVALID_PARAMETER);
    if (reinterpret_cast<intptr_t>(handle) != -2) {
        *cycles = 0;
        return TRUE;
    }
    timespec now{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    *cycles = uint64_t(now.tv_sec) * 1000000000ull + uint64_t(now.tv_nsec);
    return TRUE;
}

// --- Critical sections and SRW locks --------------------------------------------------------

namespace {
std::recursive_mutex& sectionMutex(CRITICAL_SECTION* section) {
    return *static_cast<std::recursive_mutex*>(section->implementation);
}
}

void InitializeCriticalSection(CRITICAL_SECTION* section) {
    section->implementation = new std::recursive_mutex;
    section->LockCount = -1;
    section->RecursionCount = 0;
    section->OwningThread = nullptr;
}

BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION* section, DWORD) {
    InitializeCriticalSection(section);
    return TRUE;
}

void DeleteCriticalSection(CRITICAL_SECTION* section) {
    delete static_cast<std::recursive_mutex*>(section->implementation);
    section->implementation = nullptr;
}

void EnterCriticalSection(CRITICAL_SECTION* section) {
    sectionMutex(section).lock();
    section->OwningThread = reinterpret_cast<HANDLE>(uintptr_t(GetCurrentThreadId()));
    ++section->RecursionCount;
}

BOOL TryEnterCriticalSection(CRITICAL_SECTION* section) {
    if (!sectionMutex(section).try_lock()) return FALSE;
    section->OwningThread = reinterpret_cast<HANDLE>(uintptr_t(GetCurrentThreadId()));
    ++section->RecursionCount;
    return TRUE;
}

void LeaveCriticalSection(CRITICAL_SECTION* section) {
    if (--section->RecursionCount == 0) section->OwningThread = nullptr;
    sectionMutex(section).unlock();
}

namespace {
std::atomic<uintptr_t>& srwState(SRWLOCK* lock) {
    static_assert(sizeof(std::atomic<uintptr_t>) == sizeof(void*));
    return *reinterpret_cast<std::atomic<uintptr_t>*>(&lock->Ptr);
}
}

void InitializeSRWLock(SRWLOCK* lock) { lock->Ptr = nullptr; }

void AcquireSRWLockExclusive(SRWLOCK* lock) {
    auto& state = srwState(lock);
    for (unsigned spins = 0;; ++spins) {
        uintptr_t expected = 0;
        if (state.compare_exchange_weak(expected, 1, std::memory_order_acquire)) return;
        if (spins > 64) sched_yield();
        else _mm_pause();
    }
}

BOOLEAN TryAcquireSRWLockExclusive(SRWLOCK* lock) {
    uintptr_t expected = 0;
    return srwState(lock).compare_exchange_strong(expected, 1, std::memory_order_acquire);
}

void ReleaseSRWLockExclusive(SRWLOCK* lock) { srwState(lock).store(0, std::memory_order_release); }

void AcquireSRWLockShared(SRWLOCK* lock) {
    auto& state = srwState(lock);
    for (unsigned spins = 0;; ++spins) {
        uintptr_t value = state.load(std::memory_order_relaxed);
        if (!(value & 1) && state.compare_exchange_weak(value, value + 2, std::memory_order_acquire)) return;
        if (spins > 64) sched_yield();
        else _mm_pause();
    }
}

void ReleaseSRWLockShared(SRWLOCK* lock) { srwState(lock).fetch_sub(2, std::memory_order_release); }

// --- Time ---------------------------------------------------------------------------------

// posix_compat.h's QueryPerformanceCounter counts nanoseconds.
BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency) {
    frequency->QuadPart = 1000000000;
    return TRUE;
}

void GetSystemTimeAsFileTime(FILETIME* time) {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    const uint64_t value = fileTimeOf(now);
    time->dwLowDateTime = DWORD(value);
    time->dwHighDateTime = DWORD(value >> 32);
}

namespace {
// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
int64_t daysFromCivil(int64_t year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = unsigned(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

void civilFromDays(int64_t days, int64_t& year, unsigned& month, unsigned& day) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = unsigned(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    year = int64_t(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year += month <= 2;
}

constexpr int64_t kEpochDays1601 = -134774; // 1601-01-01 relative to 1970-01-01
}

BOOL FileTimeToSystemTime(const FILETIME* time, SYSTEMTIME* system) {
    if (!time || !system) return fail(ERROR_INVALID_PARAMETER);
    const uint64_t value = (uint64_t(time->dwHighDateTime) << 32) | time->dwLowDateTime;
    if (value >= 0x8000000000000000ull) return fail(ERROR_INVALID_PARAMETER);
    const uint64_t totalMs = value / 10000;
    const int64_t days = int64_t(totalMs / 86400000ull) + kEpochDays1601;
    uint64_t ms = totalMs % 86400000ull;
    int64_t year;
    unsigned month, day;
    civilFromDays(days, year, month, day);
    system->wYear = WORD(year);
    system->wMonth = WORD(month);
    system->wDay = WORD(day);
    system->wDayOfWeek = WORD(((days % 7) + 11) % 7); // 1970-01-01 was a Thursday (4)
    system->wHour = WORD(ms / 3600000);
    ms %= 3600000;
    system->wMinute = WORD(ms / 60000);
    ms %= 60000;
    system->wSecond = WORD(ms / 1000);
    system->wMilliseconds = WORD(ms % 1000);
    return TRUE;
}

BOOL SystemTimeToFileTime(const SYSTEMTIME* system, FILETIME* time) {
    if (!system || !time) return fail(ERROR_INVALID_PARAMETER);
    static constexpr unsigned monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const unsigned year = system->wYear, month = system->wMonth, day = system->wDay;
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (year < 1601 || year > 30827 || month < 1 || month > 12 || day < 1 ||
        day > monthDays[month - 1] + (month == 2 && leap) || system->wHour > 23 || system->wMinute > 59 ||
        system->wSecond > 59 || system->wMilliseconds > 999)
        return fail(ERROR_INVALID_PARAMETER);
    const int64_t days = daysFromCivil(year, month, day) - kEpochDays1601;
    const uint64_t ms = uint64_t(days) * 86400000ull + uint64_t(system->wHour) * 3600000ull +
                        uint64_t(system->wMinute) * 60000ull + uint64_t(system->wSecond) * 1000ull +
                        system->wMilliseconds;
    const uint64_t value = ms * 10000;
    time->dwLowDateTime = DWORD(value);
    time->dwHighDateTime = DWORD(value >> 32);
    return TRUE;
}

// The console's clock is kept in UTC here; its local time zone is not exposed
// to this layer.
DWORD GetTimeZoneInformation(TIME_ZONE_INFORMATION* information) {
    if (!information) return TIME_ZONE_ID_INVALID;
    *information = {};
    const wchar_t name[] = L"Coordinated Universal Time";
    std::memcpy(information->StandardName, name, sizeof(name));
    std::memcpy(information->DaylightName, name, sizeof(name));
    return TIME_ZONE_ID_UNKNOWN;
}

DWORD GetDynamicTimeZoneInformation(DYNAMIC_TIME_ZONE_INFORMATION* information) {
    if (!information) return TIME_ZONE_ID_INVALID;
    *information = {};
    const wchar_t name[] = L"Coordinated Universal Time";
    std::memcpy(information->StandardName, name, sizeof(name));
    std::memcpy(information->DaylightName, name, sizeof(name));
    const wchar_t key[] = L"UTC";
    std::memcpy(information->TimeZoneKeyName, key, sizeof(key));
    return TIME_ZONE_ID_UNKNOWN;
}

DWORD GetTickCount() { return DWORD(GetTickCount64()); }

// --- Virtual memory ---------------------------------------------------------------------

void win32RegisterGuestSpace(void* base, uint64_t size) {
    GuestSpace& space = guestSpace();
    std::lock_guard lock(space.lock);
    space.base = static_cast<uint8_t*>(base);
    space.size = size;
    space.runs.clear();
    space.runs.emplace(0, PageRun{MEM_RESERVE, PAGE_NOACCESS});
}

void win32UnregisterGuestSpace(void* base) {
    GuestSpace& space = guestSpace();
    std::lock_guard lock(space.lock);
    if (space.base != base) return;
    space.base = nullptr;
    space.size = 0;
    space.runs.clear();
}

void* VirtualAlloc(void* address, SIZE_T size, DWORD type, DWORD protection) {
    if (!size) {
        fail(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    GuestSpace& space = guestSpace();
    if (address) {
        std::lock_guard lock(space.lock);
        if (!inGuest(space, address, size)) {
            fail(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        const uint64_t begin = uint64_t(static_cast<uint8_t*>(address) - space.base) & ~(kPage - 1);
        const uint64_t end = std::min(space.size, (uint64_t(static_cast<uint8_t*>(address) - space.base) + size +
                                                   kPage - 1) & ~(kPage - 1));
        if (type & MEM_COMMIT) {
            // Newly committed pages read as zero, as Windows' do; pages that
            // were committed keep their contents.
            auto it = space.runs.upper_bound(begin);
            --it;
            for (; it != space.runs.end() && it->first < end; ++it) {
                if (it->second.state == MEM_COMMIT) continue;
                const uint64_t from = std::max(begin, it->first);
                const uint64_t to = std::min(end, runEnd(space, it));
                std::memset(space.base + from, 0, size_t(to - from));
            }
            setRuns(space, begin, end, PageRun{MEM_COMMIT, protection});
        }
        return space.base + begin;
    }
    // A host allocation (no guest address asked for).
    const size_t rounded = (size + 65535) & ~size_t(65535);
    void* memory = std::aligned_alloc(65536, rounded);
    if (!memory) {
        fail(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    std::memset(memory, 0, rounded);
    std::lock_guard lock(hostAllocationLock);
    hostAllocations.emplace(reinterpret_cast<uintptr_t>(memory), rounded);
    return memory;
}

BOOL VirtualFree(void* address, SIZE_T size, DWORD type) {
    GuestSpace& space = guestSpace();
    {
        std::lock_guard lock(space.lock);
        if (inGuest(space, address)) {
            const uint64_t begin = uint64_t(static_cast<uint8_t*>(address) - space.base) & ~(kPage - 1);
            const uint64_t end = size ? std::min(space.size, (uint64_t(static_cast<uint8_t*>(address) - space.base) +
                                                              size + kPage - 1) & ~(kPage - 1))
                                      : runEnd(space, std::prev(space.runs.upper_bound(begin)));
            if (type & (MEM_DECOMMIT | MEM_RELEASE)) setRuns(space, begin, end, PageRun{MEM_RESERVE, PAGE_NOACCESS});
            return TRUE;
        }
    }
    std::lock_guard lock(hostAllocationLock);
    auto found = hostAllocations.find(reinterpret_cast<uintptr_t>(address));
    if (found == hostAllocations.end()) return fail(ERROR_INVALID_PARAMETER);
    if (type & MEM_RELEASE) {
        std::free(address);
        hostAllocations.erase(found);
    }
    return TRUE;
}

BOOL VirtualProtect(void* address, SIZE_T size, DWORD protection, DWORD* oldProtection) {
    GuestSpace& space = guestSpace();
    {
        std::lock_guard lock(space.lock);
        if (inGuest(space, address, size)) {
            const uint64_t begin = uint64_t(static_cast<uint8_t*>(address) - space.base) & ~(kPage - 1);
            const uint64_t end = std::min(space.size, (uint64_t(static_cast<uint8_t*>(address) - space.base) + size +
                                                       kPage - 1) & ~(kPage - 1));
            auto it = std::prev(space.runs.upper_bound(begin));
            if (oldProtection) *oldProtection = it->second.protect;
            for (auto check = it; check != space.runs.end() && check->first < end; ++check)
                if (check->second.state != MEM_COMMIT) return fail(ERROR_INVALID_PARAMETER);
            setRuns(space, begin, end, PageRun{MEM_COMMIT, protection});
            return TRUE;
        }
    }
    if (oldProtection) *oldProtection = PAGE_READWRITE;
    return TRUE;
}

SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION information, SIZE_T length) {
    if (!information || length < sizeof(MEMORY_BASIC_INFORMATION)) return 0;
    GuestSpace& space = guestSpace();
    {
        std::lock_guard lock(space.lock);
        if (inGuest(space, address)) {
            const uint64_t offset = uint64_t(static_cast<const uint8_t*>(address) - space.base) & ~(kPage - 1);
            auto it = std::prev(space.runs.upper_bound(offset));
            information->BaseAddress = space.base + offset;
            information->AllocationBase = space.base;
            information->AllocationProtect = PAGE_NOACCESS;
            information->RegionSize = SIZE_T(runEnd(space, it) - offset);
            information->State = it->second.state;
            information->Protect = it->second.state == MEM_COMMIT ? it->second.protect : 0;
            information->Type = offset >= 0xa0000000ull && offset < 0xfffff000ull ? MEM_MAPPED : MEM_PRIVATE;
            return sizeof(MEMORY_BASIC_INFORMATION);
        }
    }
    std::lock_guard lock(hostAllocationLock);
    const auto at = reinterpret_cast<uintptr_t>(address);
    auto found = hostAllocations.upper_bound(at);
    if (found == hostAllocations.begin()) return 0;
    --found;
    if (at >= found->first + found->second) return 0;
    const uintptr_t page = at & ~uintptr_t(kPage - 1);
    information->BaseAddress = reinterpret_cast<void*>(page);
    information->AllocationBase = reinterpret_cast<void*>(found->first);
    information->AllocationProtect = PAGE_READWRITE;
    information->RegionSize = found->first + found->second - page;
    information->State = MEM_COMMIT;
    information->Protect = PAGE_READWRITE;
    information->Type = MEM_PRIVATE;
    return sizeof(MEMORY_BASIC_INFORMATION);
}

// The console's guest space is one mapping that is always resident.
BOOL K32QueryWorkingSetEx(HANDLE, void* buffer, DWORD size) {
    auto* entries = static_cast<PSAPI_WORKING_SET_EX_INFORMATION*>(buffer);
    const size_t count = size / sizeof(PSAPI_WORKING_SET_EX_INFORMATION);
    for (size_t i = 0; i < count; ++i) {
        MEMORY_BASIC_INFORMATION information{};
        entries[i].VirtualAttributes.Flags = 0;
        if (VirtualQuery(entries[i].VirtualAddress, &information, sizeof(information)) &&
            information.State == MEM_COMMIT) {
            entries[i].VirtualAttributes.Valid = 1;
            entries[i].VirtualAttributes.Win32Protection = information.Protect;
        }
    }
    return TRUE;
}

// Section views exist only for the Windows build's guest space.
HANDLE CreateFileMappingW(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR) {
    fail(ERROR_NOT_SUPPORTED);
    return nullptr;
}

void* MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, SIZE_T) {
    fail(ERROR_NOT_SUPPORTED);
    return nullptr;
}

void* MapViewOfFileEx(HANDLE, DWORD, DWORD, DWORD, SIZE_T, void*) {
    fail(ERROR_NOT_SUPPORTED);
    return nullptr;
}

BOOL UnmapViewOfFile(const void*) { return fail(ERROR_NOT_SUPPORTED); }

// --- Win32 files --------------------------------------------------------------------------

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes, DWORD disposition,
                   DWORD flags, HANDLE templateFile) {
    return CreateFileW(win32Utf8(path).c_str(), access, share, attributes, disposition, flags, templateFile);
}

HANDLE CreateFileW(const char* path, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD flags,
                   HANDLE) {
    if (!path) {
        fail(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    return openFile(path, access, disposition, flags);
}

HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes, DWORD disposition,
                   DWORD flags, HANDLE templateFile) {
    return CreateFileW(path, access, share, attributes, disposition, flags, templateFile);
}

BOOL ReadFile(HANDLE handle, void* buffer, DWORD bytes, DWORD* read, LPOVERLAPPED overlapped) {
    auto file = lookupAs<FileObject>(handle, Kind::File);
    if (!file) return fail(ERROR_INVALID_HANDLE);
    const ssize_t got = overlapped
        ? ::pread(file->fd, buffer, bytes, off_t((uint64_t(overlapped->OffsetHigh) << 32) | overlapped->Offset))
        : ::read(file->fd, buffer, bytes);
    if (got < 0) return fail(errorFromErrno(errno));
    if (read) *read = DWORD(got);
    return TRUE;
}

BOOL WriteFile(HANDLE handle, const void* buffer, DWORD bytes, DWORD* written, LPOVERLAPPED overlapped) {
    auto file = lookupAs<FileObject>(handle, Kind::File);
    if (!file) return fail(ERROR_INVALID_HANDLE);
    const ssize_t put = overlapped
        ? ::pwrite(file->fd, buffer, bytes, off_t((uint64_t(overlapped->OffsetHigh) << 32) | overlapped->Offset))
        : ::write(file->fd, buffer, bytes);
    if (put < 0) return fail(errorFromErrno(errno));
    if (written) *written = DWORD(put);
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE handle) {
    auto file = lookupAs<FileObject>(handle, Kind::File);
    if (!file) return fail(ERROR_INVALID_HANDLE);
    return ::fsync(file->fd) == 0 ? TRUE : fail(errorFromErrno(errno));
}

BOOL GetFileSizeEx(HANDLE handle, LARGE_INTEGER* size) {
    auto file = lookupAs<FileObject>(handle, Kind::File);
    if (!file || !size) return fail(ERROR_INVALID_HANDLE);
    struct stat st {};
    if (::fstat(file->fd, &st) != 0) return fail(errorFromErrno(errno));
    size->QuadPart = st.st_size;
    return TRUE;
}

BOOL SetFilePointerEx(HANDLE handle, LARGE_INTEGER distance, LARGE_INTEGER* position, DWORD method) {
    auto file = lookupAs<FileObject>(handle, Kind::File);
    if (!file) return fail(ERROR_INVALID_HANDLE);
    const int whence = method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END;
    const off_t at = ::lseek(file->fd, off_t(distance.QuadPart), whence);
    if (at < 0) return fail(errorFromErrno(errno));
    if (position) position->QuadPart = at;
    return TRUE;
}

BOOL SetEndOfFile(HANDLE handle) {
    auto file = lookupAs<FileObject>(handle, Kind::File);
    if (!file) return fail(ERROR_INVALID_HANDLE);
    const off_t at = ::lseek(file->fd, 0, SEEK_CUR);
    return at >= 0 && ::ftruncate(file->fd, at) == 0 ? TRUE : fail(errorFromErrno(errno));
}

BOOL MoveFileExW(const char* from, const char* to, DWORD flags) {
    if (!from || !to) return fail(ERROR_INVALID_PARAMETER);
    struct stat st {};
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && ::stat(to, &st) == 0) return fail(ERROR_ALREADY_EXISTS);
    return ::rename(from, to) == 0 ? TRUE : fail(errorFromErrno(errno));
}

BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    return MoveFileExW(win32Utf8(from).c_str(), win32Utf8(to).c_str(), flags);
}

BOOL DeleteFileW(const char* path) {
    if (!path) return fail(ERROR_INVALID_PARAMETER);
    return ::unlink(path) == 0 ? TRUE : fail(errorFromErrno(errno));
}

BOOL DeleteFileW(LPCWSTR path) { return DeleteFileW(win32Utf8(path).c_str()); }

DWORD GetFileAttributesW(const char* path) {
    struct stat st {};
    if (!path || ::stat(path, &st) != 0) {
        lastError() = errorFromErrno(errno);
        return INVALID_FILE_ATTRIBUTES;
    }
    return attributesOf(st);
}

DWORD GetFileAttributesW(LPCWSTR path) { return GetFileAttributesW(win32Utf8(path).c_str()); }

// The console's file system has no sparse flag; unwritten ranges read back as
// zeros either way.
BOOL DeviceIoControl(HANDLE, DWORD, void*, DWORD, void*, DWORD, DWORD* returned, LPOVERLAPPED) {
    if (returned) *returned = 0;
    return TRUE;
}

std::filesystem::path win32MatchPathCase(const std::filesystem::path& base, const std::string& relative) {
    std::filesystem::path current = base;
    size_t start = 0;
    while (start <= relative.size()) {
        size_t end = relative.find('/', start);
        if (end == std::string::npos) end = relative.size();
        const std::string component = relative.substr(start, end - start);
        start = end + 1;
        if (component.empty() || component == ".") continue;
        std::error_code error;
        if (component == ".." || std::filesystem::exists(current / component, error)) {
            current /= component;
            continue;
        }
        bool matched = false;
        for (std::filesystem::directory_iterator it(current, error), last; !error && it != last; it.increment(error)) {
            const std::string name = it->path().filename().string();
            if (sameIgnoringCase(name, component)) {
                current /= name;
                matched = true;
                break;
            }
        }
        if (!matched) {
            // The rest of the path does not exist yet (a file about to be
            // created): keep it as the guest spelled it.
            current /= relative.substr(end - component.size());
            break;
        }
    }
    return current;
}

// --- NT calls ------------------------------------------------------------------------------

NTSTATUS NtCreateFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes, PIO_STATUS_BLOCK io,
                      PLARGE_INTEGER, ULONG, ULONG, ULONG disposition, ULONG options, PVOID, ULONG) {
    std::string path;
    if (!handle || !ntPath(attributes, path)) {
        setIo(io, kInvalidParameter, 0);
        return kInvalidParameter;
    }
    const bool wantDirectory = options & FILE_DIRECTORY_FILE;
    const bool write = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    struct stat st {};
    const bool existed = ::stat(path.c_str(), &st) == 0;
    const bool isDir = existed && S_ISDIR(st.st_mode);
    auto finish = [&](NTSTATUS status, ULONG_PTR information) {
        setIo(io, status, information);
        return status;
    };
    if (existed && disposition == FILE_CREATE) return finish(kNameCollision, FILE_EXISTS);
    if (!existed && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE)) {
        const auto parent = std::filesystem::path(path).parent_path();
        return finish(isDirectory(parent.string()) ? kNameNotFound : kPathNotFound, FILE_DOES_NOT_EXIST);
    }
    if (isDir && (options & FILE_NON_DIRECTORY_FILE)) return finish(kFileIsADirectory, 0);
    if (existed && !isDir && wantDirectory) return finish(kNotADirectory, 0);

    auto file = std::make_shared<FileObject>();
    file->path = path;
    file->deleteOnClose = (options & kFileDeleteOnClose) != 0;
    ULONG_PTR information = existed ? FILE_OPENED : FILE_CREATED;
    if (wantDirectory || isDir) {
        if (!existed && ::mkdir(path.c_str(), 0755) != 0) return finish(statusFromErrno(errno), 0);
        file->fd = ::open(path.c_str(), O_RDONLY);
        file->directory = true;
    } else {
        int mode = write ? O_RDWR : O_RDONLY;
        switch (disposition) {
        case FILE_SUPERSEDE:
            mode |= O_CREAT | O_TRUNC;
            information = existed ? FILE_SUPERSEDED : FILE_CREATED;
            break;
        case FILE_OPEN: break;
        case FILE_CREATE: mode |= O_CREAT | O_EXCL; break;
        case FILE_OPEN_IF: mode |= O_CREAT; break;
        case FILE_OVERWRITE:
            mode |= O_TRUNC;
            information = FILE_OVERWRITTEN;
            break;
        case FILE_OVERWRITE_IF:
            mode |= O_CREAT | O_TRUNC;
            information = existed ? FILE_OVERWRITTEN : FILE_CREATED;
            break;
        default: return finish(kInvalidParameter, 0);
        }
        if ((mode & (O_CREAT | O_TRUNC)) && !write) mode = (mode & ~O_ACCMODE) | O_RDWR;
        file->fd = ::open(path.c_str(), mode, 0644);
    }
    if (file->fd < 0) {
        file->deleteOnClose = false;
        return finish(statusFromErrno(errno), 0);
    }
    *handle = insert(std::move(file));
    return finish(kSuccess, information);
}

NTSTATUS NtReadFile(HANDLE handle, HANDLE event, PIO_APC_ROUTINE apc, PVOID context, PIO_STATUS_BLOCK io,
                    PVOID buffer, ULONG length, PLARGE_INTEGER offset, PULONG) {
    auto file = lookupAs<FileObject>(handle, Kind::File);
    if (!file || file->directory) {
        setIo(io, kInvalidHandle, 0);
        return kInvalidHandle;
    }
    // No offset, or the "current position" sentinels (-2, and -1 which is
    // only meaningful for writes), read at the file pointer.
    const bool atPointer = !offset || offset->QuadPart < 0;
    const off_t at = atPointer ? ::lseek(file->fd, 0, SEEK_CUR) : off_t(offset->QuadPart);
    const ssize_t got = ::pread(file->fd, buffer, length, at);
    if (got < 0) {
        const NTSTATUS status = statusFromErrno(errno);
        setIo(io, status, 0);
        return status;
    }
    ::lseek(file->fd, at + got, SEEK_SET);
    const NTSTATUS status = got == 0 && length ? kEndOfFile : kSuccess;
    setIo(io, status, ULONG_PTR(got));
    signalCompletion(event, apc, context, io);
    return status;
}

NTSTATUS NtWriteFile(HANDLE handle, HANDLE event, PIO_APC_ROUTINE apc, PVOID context, PIO_STATUS_BLOCK io,
                     const void* buffer, ULONG length, PLARGE_INTEGER offset, PULONG) {
    auto file = lookupAs<FileObject>(handle, Kind::File);
    if (!file || file->directory) {
        setIo(io, kInvalidHandle, 0);
        return kInvalidHandle;
    }
    off_t at;
    if (offset && offset->QuadPart == -1) at = ::lseek(file->fd, 0, SEEK_END); // FILE_WRITE_TO_END_OF_FILE
    else if (!offset || offset->QuadPart < 0) at = ::lseek(file->fd, 0, SEEK_CUR);
    else at = off_t(offset->QuadPart);
    const ssize_t put = ::pwrite(file->fd, buffer, length, at);
    if (put < 0) {
        const NTSTATUS status = statusFromErrno(errno);
        setIo(io, status, 0);
        return status;
    }
    ::lseek(file->fd, at + put, SEEK_SET);
    setIo(io, kSuccess, ULONG_PTR(put));
    signalCompletion(event, apc, context, io);
    return kSuccess;
}

NTSTATUS NtQueryInformationFile(HANDLE handle, PIO_STATUS_BLOCK io, PVOID information, ULONG length,
                                FILE_INFORMATION_CLASS kind) {
    auto file = lookupAs<FileObject>(handle, Kind::File);
    if (!file) {
        setIo(io, kInvalidHandle, 0);
        return kInvalidHandle;
    }
    struct stat st {};
    if (::fstat(file->fd, &st) != 0) {
        const NTSTATUS status = statusFromErrno(errno);
        setIo(io, status, 0);
        return status;
    }
    auto* out = static_cast<uint8_t*>(information);
    auto put64 = [&](size_t at, uint64_t value) { std::memcpy(out + at, &value, 8); };
    auto put32 = [&](size_t at, uint32_t value) { std::memcpy(out + at, &value, 4); };
    const uint64_t size = file->directory ? 0 : uint64_t(st.st_size);
    const uint64_t modified = fileTimeOf(st.st_mtim), accessed = fileTimeOf(st.st_atim),
                   changed = fileTimeOf(st.st_ctim);
    ULONG needed;
    switch (int(kind)) {
    case FileBasicInformation: needed = 40; break;
    case FileStandardInformation: needed = 24; break;
    case FileInternalInformation: needed = 8; break;
    case FilePositionInformation: needed = 8; break;
    case FileNetworkOpenInformation: needed = 56; break;
    case FileModeInformation:
    case FileAlignmentInformation:
        // filesystem.cpp answers these two itself when the host does not.
        setIo(io, kNotImplemented, 0);
        return kNotImplemented;
    default:
        setIo(io, kInvalidInfoClass, 0);
        return kInvalidInfoClass;
    }
    if (!out || length < needed) {
        setIo(io, kInfoLengthMismatch, 0);
        return kInfoLengthMismatch;
    }
    std::memset(out, 0, needed);
    switch (int(kind)) {
    case FileBasicInformation:
        put64(0, changed);
        put64(8, accessed);
        put64(16, modified);
        put64(24, changed);
        put32(32, attributesOf(st));
        break;
    case FileStandardInformation:
        put64(0, size);
        put64(8, size);
        put32(16, 1);
        out[20] = file->deleteOnClose;
        out[21] = file->directory;
        break;
    case FileInternalInformation: put64(0, uint64_t(st.st_ino)); break;
    case FilePositionInformation: put64(0, uint64_t(::lseek(file->fd, 0, SEEK_CUR))); break;
    case FileNetworkOpenInformation:
        put64(0, changed);
        put64(8, accessed);
        put64(16, modified);
        put64(24, changed);
        put64(32, size);
        put64(40, size);
        put32(48, attributesOf(st));
        break;
    }
    setIo(io, kSuccess, needed);
    return kSuccess;
}

NTSTATUS NtSetInformationFile(HANDLE handle, PIO_STATUS_BLOCK io, PVOID information, ULONG length,
                              FILE_INFORMATION_CLASS kind) {
    auto file = lookupAs<FileObject>(handle, Kind::File);
    if (!file) {
        setIo(io, kInvalidHandle, 0);
        return kInvalidHandle;
    }
    const auto* in = static_cast<const uint8_t*>(information);
    int64_t value = 0;
    if (length >= 8) std::memcpy(&value, in, 8);
    switch (int(kind)) {
    case FileDispositionInformation:
        if (length < 1) break;
        file->deleteOnClose = in[0] != 0;
        setIo(io, kSuccess, 0);
        return kSuccess;
    case FilePositionInformation:
        if (length < 8 || value < 0) break;
        ::lseek(file->fd, off_t(value), SEEK_SET);
        setIo(io, kSuccess, 0);
        return kSuccess;
    case FileAllocationInformation:
        if (length < 8 || value < 0) break;
        setIo(io, kSuccess, 0);
        return kSuccess;
    case FileEndOfFileInformation:
        if (length < 8 || value < 0) break;
        if (::ftruncate(file->fd, off_t(value)) != 0) {
            const NTSTATUS status = statusFromErrno(errno);
            setIo(io, status, 0);
            return status;
        }
        setIo(io, kSuccess, 0);
        return kSuccess;
    default:
        setIo(io, kInvalidInfoClass, 0);
        return kInvalidInfoClass;
    }
    setIo(io, kInvalidParameter, 0);
    return kInvalidParameter;
}

NTSTATUS NtQueryFullAttributesFile(POBJECT_ATTRIBUTES attributes, PVOID information) {
    std::string path;
    if (!information || !ntPath(attributes, path)) return kInvalidParameter;
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            const auto parent = std::filesystem::path(path).parent_path();
            return isDirectory(parent.string()) ? kNameNotFound : kPathNotFound;
        }
        return statusFromErrno(errno);
    }
    auto* out = static_cast<uint8_t*>(information);
    std::memset(out, 0, 56);
    const uint64_t size = S_ISDIR(st.st_mode) ? 0 : uint64_t(st.st_size);
    const uint64_t times[4] = {fileTimeOf(st.st_ctim), fileTimeOf(st.st_atim), fileTimeOf(st.st_mtim),
                               fileTimeOf(st.st_ctim)};
    std::memcpy(out, times, sizeof(times));
    std::memcpy(out + 32, &size, 8);
    std::memcpy(out + 40, &size, 8);
    const uint32_t attributesValue = attributesOf(st);
    std::memcpy(out + 48, &attributesValue, 4);
    return kSuccess;
}

// FileDirectoryInformation records, one per call (filesystem.cpp asks for a
// single entry). The Xbox's FAT-style volumes list no "." or "..", so neither
// does this.
NTSTATUS NtQueryDirectoryFile(HANDLE handle, HANDLE event, PIO_APC_ROUTINE apc, PVOID context, PIO_STATUS_BLOCK io,
                              PVOID information, ULONG length, FILE_INFORMATION_CLASS kind, BOOLEAN,
                              PUNICODE_STRING mask, BOOLEAN restart) {
    auto file = lookupAs<FileObject>(handle, Kind::File);
    if (!file || !file->directory) {
        setIo(io, kInvalidHandle, 0);
        return kInvalidHandle;
    }
    if (int(kind) != FileDirectoryInformation) {
        setIo(io, kInvalidInfoClass, 0);
        return kInvalidInfoClass;
    }
    const bool firstQuery = !file->listed || restart;
    if (firstQuery) {
        file->listing.clear();
        file->cursor = 0;
        file->listed = true;
        std::wstring pattern = mask && mask->Buffer ? std::wstring(mask->Buffer, mask->Length / 2) : L"*";
        if (pattern.empty()) pattern = L"*";
        if (DIR* dir = ::opendir(file->path.c_str())) {
            while (const dirent* entry = ::readdir(dir)) {
                const std::string name = entry->d_name;
                if (name == "." || name == "..") continue;
                if (wildcardMatch(pattern.c_str(), widen(name.c_str()).c_str())) file->listing.push_back(name);
            }
            ::closedir(dir);
        }
        std::sort(file->listing.begin(), file->listing.end(), [](const std::string& a, const std::string& b) {
            return ::strcasecmp(a.c_str(), b.c_str()) < 0;
        });
    }
    if (file->cursor >= file->listing.size()) {
        const NTSTATUS status = firstQuery && file->listing.empty() ? kNoSuchFile : kNoMoreFiles;
        setIo(io, status, 0);
        signalCompletion(event, apc, context, io);
        return status;
    }
    const std::string name = file->listing[file->cursor];
    const std::wstring wide = widen(name.c_str());
    struct stat st {};
    ::stat((file->path + "/" + name).c_str(), &st);
    const ULONG nameBytes = ULONG(wide.size() * sizeof(wchar_t));
    if (!information || length < 64) {
        setIo(io, kInfoLengthMismatch, 0);
        return kInfoLengthMismatch;
    }
    auto* out = static_cast<uint8_t*>(information);
    std::memset(out, 0, 64);
    const uint64_t size = S_ISDIR(st.st_mode) ? 0 : uint64_t(st.st_size);
    const uint64_t times[4] = {fileTimeOf(st.st_ctim), fileTimeOf(st.st_atim), fileTimeOf(st.st_mtim),
                               fileTimeOf(st.st_ctim)};
    std::memcpy(out + 8, times, sizeof(times));
    std::memcpy(out + 40, &size, 8);
    std::memcpy(out + 48, &size, 8);
    const uint32_t attributesValue = attributesOf(st);
    std::memcpy(out + 56, &attributesValue, 4);
    std::memcpy(out + 60, &nameBytes, 4);
    const ULONG room = length - 64;
    std::memcpy(out + 64, wide.data(), std::min(room, nameBytes));
    NTSTATUS status = kSuccess;
    if (room < nameBytes) status = kBufferOverflow;
    else ++file->cursor;
    setIo(io, status, 64 + std::min(room, nameBytes));
    signalCompletion(event, apc, context, io);
    return status;
}

NTSTATUS NtQueryVolumeInformationFile(HANDLE, PIO_STATUS_BLOCK io, PVOID, ULONG, FS_INFORMATION_CLASS) {
    setIo(io, kNotImplemented, 0);
    return kNotImplemented;
}

NTSTATUS NtFlushBuffersFile(HANDLE handle, PIO_STATUS_BLOCK io) {
    auto file = lookupAs<FileObject>(handle, Kind::File);
    if (!file) {
        setIo(io, kInvalidHandle, 0);
        return kInvalidHandle;
    }
    if (!file->directory) ::fsync(file->fd);
    setIo(io, kSuccess, 0);
    return kSuccess;
}

NTSTATUS NtClose(HANDLE handle) { return CloseHandle(handle) ? kSuccess : kInvalidHandle; }

namespace {
NTSTATUS NTAPI ntSetEvent(HANDLE handle, LONG* previous) {
    auto event = lookupAs<Event>(handle, Kind::Event);
    if (!event) return kInvalidHandle;
    std::lock_guard lock(waitLock);
    if (previous) *previous = event->signaled ? 1 : 0;
    event->signaled = true;
    wakeWaiters(*event);
    return kSuccess;
}
}

ULONG RtlNtStatusToDosError(NTSTATUS status) {
    switch (status) {
    case kSuccess: return ERROR_SUCCESS;
    case kInvalidHandle: return ERROR_INVALID_HANDLE;
    case kNameNotFound: return ERROR_FILE_NOT_FOUND;
    case kPathNotFound: return ERROR_PATH_NOT_FOUND;
    case kNameCollision: return ERROR_ALREADY_EXISTS;
    case kAccessDenied: return ERROR_ACCESS_DENIED;
    case kMediaWriteProtected: return ERROR_WRITE_PROTECT;
    case kDiskFull: return ERROR_DISK_FULL;
    case kEndOfFile: return ERROR_HANDLE_EOF;
    case kNoMoreFiles: return ERROR_NO_MORE_FILES;
    case kNoSuchFile: return ERROR_FILE_NOT_FOUND;
    case kNoMemory: return ERROR_NOT_ENOUGH_MEMORY;
    default: return ERROR_INVALID_PARAMETER;
    }
}

// --- Modules -------------------------------------------------------------------------------

HMODULE GetModuleHandleW(LPCWSTR name) {
    static std::mutex lock;
    static std::unordered_map<std::string, std::unique_ptr<Module>> modules;
    std::string key = name ? win32Utf8(name) : std::string("eboot.bin");
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::lock_guard guard(lock);
    auto& module = modules[key];
    if (!module) module = std::make_unique<Module>(key);
    return module.get();
}

HMODULE LoadLibraryW(LPCWSTR name) { return GetModuleHandleW(name); }

FARPROC GetProcAddress(HMODULE, LPCSTR name) {
    struct Entry {
        const char* name;
        FARPROC function;
    };
    static const Entry table[] = {
        {"NtCreateFile", reinterpret_cast<FARPROC>(&NtCreateFile)},
        {"NtReadFile", reinterpret_cast<FARPROC>(&NtReadFile)},
        {"NtWriteFile", reinterpret_cast<FARPROC>(&NtWriteFile)},
        {"NtQueryInformationFile", reinterpret_cast<FARPROC>(&NtQueryInformationFile)},
        {"NtSetInformationFile", reinterpret_cast<FARPROC>(&NtSetInformationFile)},
        {"NtQueryFullAttributesFile", reinterpret_cast<FARPROC>(&NtQueryFullAttributesFile)},
        {"NtQueryDirectoryFile", reinterpret_cast<FARPROC>(&NtQueryDirectoryFile)},
        {"NtQueryVolumeInformationFile", reinterpret_cast<FARPROC>(&NtQueryVolumeInformationFile)},
        {"NtFlushBuffersFile", reinterpret_cast<FARPROC>(&NtFlushBuffersFile)},
        {"NtClose", reinterpret_cast<FARPROC>(&NtClose)},
        {"NtSetEvent", reinterpret_cast<FARPROC>(&ntSetEvent)},
        {"RtlNtStatusToDosError", reinterpret_cast<FARPROC>(&RtlNtStatusToDosError)},
    };
    if (!name) return nullptr;
    for (const auto& entry : table)
        if (std::strcmp(entry.name, name) == 0) return entry.function;
    // Optional Windows features (MMCSS scheduling, placeholder memory) are
    // absent: callers that probe for them take their fallback.
    lastError() = ERROR_NOT_FOUND;
    return nullptr;
}

DWORD GetModuleFileNameW(HMODULE, LPWSTR buffer, DWORD size) {
    const wchar_t path[] = L"/app0/eboot.bin";
    const DWORD length = DWORD(std::size(path) - 1);
    if (!buffer || !size) return 0;
    const DWORD copied = std::min(length, size - 1);
    std::memcpy(buffer, path, copied * sizeof(wchar_t));
    buffer[copied] = 0;
    if (copied < length) {
        lastError() = ERROR_INSUFFICIENT_BUFFER;
        return size;
    }
    return copied;
}

DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD size) {
    const char* value = name ? std::getenv(win32Utf8(name).c_str()) : nullptr;
    if (!value) {
        lastError() = ERROR_ENVVAR_NOT_FOUND;
        return 0;
    }
    const std::wstring wide = widen(value);
    if (!buffer || size <= wide.size()) return DWORD(wide.size() + 1);
    std::memcpy(buffer, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
    return DWORD(wide.size());
}

void OutputDebugStringA(LPCSTR text) {
    if (text) std::fputs(text, stderr);
}

void OutputDebugStringW(LPCWSTR text) {
    if (text) std::fputs(win32Utf8(text).c_str(), stderr);
}

// --- Strings ---------------------------------------------------------------------------------

std::string win32Utf8(const wchar_t* text) { return text ? utf8(text, std::wcslen(text)) : std::string(); }

std::u16string win32Utf8ToUtf16(std::string_view text) {
    bool valid = true;
    const auto decoded = decodeUtf8(text.data(), text.size(), valid);
    if (!valid) return {};
    std::u16string out;
    for (char32_t code : decoded) {
        if (code >= 0x10000) {
            code -= 0x10000;
            out.push_back(char16_t(0xD800 + (code >> 10)));
            out.push_back(char16_t(0xDC00 + (code & 0x3FF)));
        } else {
            out.push_back(char16_t(code));
        }
    }
    return out;
}

int MultiByteToWideChar(UINT, DWORD flags, const char* text, int bytes, wchar_t* wide, int wideCount) {
    if (!text) return fail(ERROR_INVALID_PARAMETER);
    const size_t length = bytes < 0 ? std::strlen(text) + 1 : size_t(bytes);
    bool valid = true;
    const auto decoded = decodeUtf8(text, length, valid);
    if (!valid && (flags & MB_ERR_INVALID_CHARS)) return fail(1113); // ERROR_NO_UNICODE_TRANSLATION
    if (!wide || wideCount == 0) return int(decoded.size());
    if (size_t(wideCount) < decoded.size()) return fail(ERROR_INSUFFICIENT_BUFFER);
    for (size_t i = 0; i < decoded.size(); ++i) wide[i] = wchar_t(decoded[i]);
    return int(decoded.size());
}

int WideCharToMultiByte(UINT, DWORD, const wchar_t* wide, int wideCount, char* text, int bytes, const char*,
                        BOOL* usedDefault) {
    if (!wide) return fail(ERROR_INVALID_PARAMETER);
    const size_t length = wideCount < 0 ? std::wcslen(wide) + 1 : size_t(wideCount);
    const std::string encoded = utf8(wide, length);
    if (usedDefault) *usedDefault = FALSE;
    if (!text || bytes == 0) return int(encoded.size());
    if (size_t(bytes) < encoded.size()) return fail(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(text, encoded.data(), encoded.size());
    return int(encoded.size());
}

int _wcsicmp(const wchar_t* a, const wchar_t* b) { return compareIgnoringCase(a, b, SIZE_MAX); }

int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t count) { return compareIgnoringCase(a, b, count); }

int _wcsicmp(const char* a, const wchar_t* b) { return _wcsicmp(widen(a).c_str(), b); }

int _wcsnicmp(const char* a, const wchar_t* b, size_t count) { return _wcsnicmp(widen(a).c_str(), b, count); }

int _stricmp(const char* a, const char* b) { return ::strcasecmp(a, b); }

int _strnicmp(const char* a, const char* b, size_t count) { return ::strncasecmp(a, b, count); }

DWORD CharLowerBuffW(LPWSTR text, DWORD length) {
    for (DWORD i = 0; i < length; ++i) text[i] = wchar_t(std::towlower(wint_t(text[i])));
    return length;
}

DWORD CharUpperBuffW(LPWSTR text, DWORD length) {
    for (DWORD i = 0; i < length; ++i) text[i] = wchar_t(std::towupper(wint_t(text[i])));
    return length;
}

// --- Settings files and language -----------------------------------------------------------

// English until the console's own language setting is wired (the system
// service that reports it is not linked into the title yet).
LANGID GetUserDefaultUILanguage() { return 0x0409; }

DWORD GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR fallback, LPWSTR buffer, DWORD size,
                               const char* path) {
    if (!buffer || !size) return 0;
    std::wstring result = fallback ? fallback : L"";
    if (path && section && key) {
        std::ifstream file(path);
        const std::string wantSection = win32Utf8(section), wantKey = win32Utf8(key);
        std::string line;
        bool inSection = false;
        while (std::getline(file, line)) {
            line = trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line.front() == '[' && line.back() == ']') {
                inSection = sameIgnoringCase(trim(line.substr(1, line.size() - 2)), wantSection);
                continue;
            }
            const auto equals = line.find('=');
            if (inSection && equals != std::string::npos && sameIgnoringCase(trim(line.substr(0, equals)), wantKey)) {
                result = widen(trim(line.substr(equals + 1)).c_str());
                break;
            }
        }
    }
    const DWORD copied = std::min<DWORD>(DWORD(result.size()), size - 1);
    std::memcpy(buffer, result.data(), copied * sizeof(wchar_t));
    buffer[copied] = 0;
    return copied;
}

UINT GetPrivateProfileIntW(LPCWSTR section, LPCWSTR key, INT fallback, const char* path) {
    wchar_t text[32]{};
    if (!GetPrivateProfileStringW(section, key, L"", text, 32, path)) return UINT(fallback);
    return UINT(std::wcstol(text, nullptr, 10));
}

BOOL WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, const char* path) {
    if (!path || !section) return fail(ERROR_INVALID_PARAMETER);
    std::vector<std::string> lines;
    {
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line)) lines.push_back(line);
    }
    const std::string wantSection = win32Utf8(section);
    const std::string wantKey = key ? win32Utf8(key) : std::string();
    const std::string entry = wantKey + "=" + (value ? win32Utf8(value) : std::string());
    bool inSection = false, sectionFound = false, written = false;
    size_t insertAt = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string line = trim(lines[i]);
        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            if (inSection && !written) insertAt = i;
            inSection = sameIgnoringCase(trim(line.substr(1, line.size() - 2)), wantSection);
            if (inSection) sectionFound = true;
            continue;
        }
        const auto equals = line.find('=');
        if (inSection && key && equals != std::string::npos && sameIgnoringCase(trim(line.substr(0, equals)), wantKey)) {
            if (value) lines[i] = entry;
            else lines.erase(lines.begin() + long(i));
            written = true;
            break;
        }
    }
    if (!written && key && value) {
        if (!sectionFound) {
            lines.push_back("[" + wantSection + "]");
            lines.push_back(entry);
        } else {
            lines.insert(lines.begin() + long(insertAt), entry);
        }
    }
    std::ofstream file(path, std::ios::trunc);
    if (!file) return fail(errorFromErrno(errno));
    for (const auto& line : lines) file << line << '\n';
    return file.good() ? TRUE : fail(ERROR_WRITE_FAULT);
}

// --- Hashing and random --------------------------------------------------------------------

NTSTATUS BCryptGenRandom(BCRYPT_ALG_HANDLE, PUCHAR buffer, ULONG bytes, ULONG) {
    ::arc4random_buf(buffer, bytes);
    return kSuccess;
}

NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE* algorithm, LPCWSTR id, LPCWSTR, ULONG) {
    if (!algorithm || !id || std::wcscmp(id, BCRYPT_SHA256_ALGORITHM) != 0) return kNotImplemented;
    static int sha256;
    *algorithm = &sha256;
    return kSuccess;
}

NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE, ULONG) { return kSuccess; }

NTSTATUS BCryptHash(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, PUCHAR input, ULONG inputBytes, PUCHAR output,
                    ULONG outputBytes) {
    if (outputBytes != 32) return kInvalidParameter;
    Sha256 hash;
    hash.update(input, inputBytes);
    hash.finish(output);
    return kSuccess;
}

NTSTATUS BCryptCreateHash(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE* hash, PUCHAR, ULONG, PUCHAR, ULONG, ULONG) {
    if (!hash) return kInvalidParameter;
    *hash = new Sha256;
    return kSuccess;
}

NTSTATUS BCryptHashData(BCRYPT_HASH_HANDLE hash, PUCHAR input, ULONG inputBytes, ULONG) {
    if (!hash) return kInvalidHandle;
    static_cast<Sha256*>(hash)->update(input, inputBytes);
    return kSuccess;
}

NTSTATUS BCryptFinishHash(BCRYPT_HASH_HANDLE hash, PUCHAR output, ULONG outputBytes, ULONG) {
    if (!hash || outputBytes != 32) return kInvalidParameter;
    static_cast<Sha256*>(hash)->finish(output);
    return kSuccess;
}

NTSTATUS BCryptDestroyHash(BCRYPT_HASH_HANDLE hash) {
    delete static_cast<Sha256*>(hash);
    return kSuccess;
}

// --- Floating point and exceptions -----------------------------------------------------------

// The CRT's exception-mask bits and the MXCSR mask bits they stand for.
int _controlfp_s(unsigned* current, unsigned value, unsigned mask) {
    static constexpr struct {
        unsigned crt;
        unsigned mxcsr;
    } bits[] = {{_EM_INVALID, 1u << 7},   {_EM_DENORMAL, 1u << 8}, {_EM_ZERODIVIDE, 1u << 9},
                {_EM_OVERFLOW, 1u << 10}, {_EM_UNDERFLOW, 1u << 11}, {_EM_INEXACT, 1u << 12}};
    unsigned csr = _mm_getcsr();
    unsigned word = 0;
    for (const auto& bit : bits)
        if (csr & bit.mxcsr) word |= bit.crt;
    word = (word & ~mask) | (value & mask);
    for (const auto& bit : bits) {
        if (word & bit.crt) csr |= bit.mxcsr;
        else csr &= ~bit.mxcsr;
    }
    _mm_setcsr(csr);
    if (current) *current = word;
    return 0;
}

void RaiseException(DWORD code, DWORD, DWORD, const ULONG_PTR*) {
    std::fprintf(stderr, "[Win32] RaiseException 0x%08X\n", unsigned(code));
    std::fflush(stderr);
    std::abort();
}

// --- XInput -----------------------------------------------------------------------------------

DWORD XInputGetState(DWORD user, XINPUT_STATE* state) {
    if (!state) return ERROR_INVALID_PARAMETER;
    if (user != 0) return ERROR_DEVICE_NOT_CONNECTED;
    static std::mutex lock;
    static DarkRecomp::Native::Ps5Pad::PadState pad;
    std::lock_guard guard(lock);
    if (!DarkRecomp::Native::Ps5Pad::readPad(pad)) return ERROR_DEVICE_NOT_CONNECTED;
    state->dwPacketNumber = pad.packet;
    state->Gamepad.wButtons = pad.buttons;
    state->Gamepad.bLeftTrigger = pad.leftTrigger;
    state->Gamepad.bRightTrigger = pad.rightTrigger;
    state->Gamepad.sThumbLX = pad.leftX;
    state->Gamepad.sThumbLY = pad.leftY;
    state->Gamepad.sThumbRX = pad.rightX;
    state->Gamepad.sThumbRY = pad.rightY;
    return ERROR_SUCCESS;
}

DWORD XInputGetCapabilities(DWORD user, DWORD, XINPUT_CAPABILITIES* capabilities) {
    if (!capabilities) return ERROR_INVALID_PARAMETER;
    XINPUT_STATE state{};
    if (XInputGetState(user, &state) != ERROR_SUCCESS) return ERROR_DEVICE_NOT_CONNECTED;
    *capabilities = {};
    capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
    capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    capabilities->Gamepad.wButtons = 0xF3FF;
    capabilities->Gamepad.bLeftTrigger = 0xFF;
    capabilities->Gamepad.bRightTrigger = 0xFF;
    capabilities->Gamepad.sThumbLX = SHORT(0xFFC0);
    capabilities->Gamepad.sThumbLY = SHORT(0xFFC0);
    capabilities->Gamepad.sThumbRX = SHORT(0xFFC0);
    capabilities->Gamepad.sThumbRY = SHORT(0xFFC0);
    return ERROR_SUCCESS;
}

// Rumble is not wired to the pad yet; the guest's request succeeds silently.
DWORD XInputSetState(DWORD user, XINPUT_VIBRATION* vibration) {
    if (!vibration) return ERROR_INVALID_PARAMETER;
    return user == 0 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

#endif // defined(__PROSPERO__)
