// The C11 threads layer RADV's archive carries was built against the console's
// <threads.h>, and Mesa's code that calls it reads Mesa's own constants.
//
// The two disagree about what a successful call returns:
//
//   console <threads.h> (FreeBSD):   thrd_busy 1, thrd_error 2, thrd_nomem 3,
//                                    thrd_success 4, thrd_timedout 5
//   Mesa's c11/threads.h:            thrd_success 0, thrd_timedout 1,
//                                    thrd_error 2, thrd_busy 3, thrd_nomem 4
//
// So the layer answers 4 where its caller reads 0, and every check in Mesa that
// says "this failed" fires on success. The first one is the instance's own
// mutex: vk_instance_init tests `mtx_init(...) != 0`, the console's layer
// answered thrd_success (4), and RADV returned VK_ERROR_INITIALIZATION_FAILED
// from vk_instance.c:187 - measured on the console on 2026-10-25, with the same
// title's probe line showing `mtx_init(1) = 4`.
//
// The link wraps the calls that answer with one of those codes, so the layer
// keeps its own behaviour and only the answer is translated. The console has no
// loader to patch a driver in, and rebuilding RADV is not this port's business:
// the title fixes the boundary it owns.
#include <time.h>

namespace {

// The console's code in, Mesa's code out.
int mesaThreadCode(int console) {
    switch (console) {
    case 4: return 0; // thrd_success
    case 5: return 1; // thrd_timedout
    case 2: return 2; // thrd_error
    case 1: return 3; // thrd_busy
    case 3: return 4; // thrd_nomem
    default: return console;
    }
}

} // namespace

// mtx_t is pthread_mutex_t and cnd_t is pthread_cond_t for that build, and the
// wrappers never look inside them; thrd_t is the console's pthread pointer.
extern "C" int __real_mtx_init(void* mutex, int type);
extern "C" int __wrap_mtx_init(void* mutex, int type) { return mesaThreadCode(__real_mtx_init(mutex, type)); }

extern "C" int __real_mtx_lock(void* mutex);
extern "C" int __wrap_mtx_lock(void* mutex) { return mesaThreadCode(__real_mtx_lock(mutex)); }

extern "C" int __real_mtx_timedlock(void* mutex, const struct timespec* until);
extern "C" int __wrap_mtx_timedlock(void* mutex, const struct timespec* until) {
    return mesaThreadCode(__real_mtx_timedlock(mutex, until));
}

extern "C" int __real_mtx_trylock(void* mutex);
extern "C" int __wrap_mtx_trylock(void* mutex) { return mesaThreadCode(__real_mtx_trylock(mutex)); }

extern "C" int __real_mtx_unlock(void* mutex);
extern "C" int __wrap_mtx_unlock(void* mutex) { return mesaThreadCode(__real_mtx_unlock(mutex)); }

extern "C" int __real_cnd_init(void* condition);
extern "C" int __wrap_cnd_init(void* condition) { return mesaThreadCode(__real_cnd_init(condition)); }

extern "C" int __real_cnd_wait(void* condition, void* mutex);
extern "C" int __wrap_cnd_wait(void* condition, void* mutex) {
    return mesaThreadCode(__real_cnd_wait(condition, mutex));
}

extern "C" int __real_cnd_timedwait(void* condition, void* mutex, const struct timespec* until);
extern "C" int __wrap_cnd_timedwait(void* condition, void* mutex, const struct timespec* until) {
    return mesaThreadCode(__real_cnd_timedwait(condition, mutex, until));
}

extern "C" int __real_cnd_signal(void* condition);
extern "C" int __wrap_cnd_signal(void* condition) { return mesaThreadCode(__real_cnd_signal(condition)); }

extern "C" int __real_cnd_broadcast(void* condition);
extern "C" int __wrap_cnd_broadcast(void* condition) {
    return mesaThreadCode(__real_cnd_broadcast(condition));
}

extern "C" int __real_thrd_create(void* thread, int (*start)(void*), void* argument);
extern "C" int __wrap_thrd_create(void* thread, int (*start)(void*), void* argument) {
    return mesaThreadCode(__real_thrd_create(thread, start, argument));
}

extern "C" int __real_thrd_join(void* thread, int* result);
extern "C" int __wrap_thrd_join(void* thread, int* result) {
    return mesaThreadCode(__real_thrd_join(thread, result));
}

extern "C" int __real_thrd_detach(void* thread);
extern "C" int __wrap_thrd_detach(void* thread) { return mesaThreadCode(__real_thrd_detach(thread)); }

// realpath: the console's sandbox refuses it (EPERM) for the title's own
// folder, and the native runtime normalizes every path it is given through
// std::filesystem::weakly_canonical, which calls it: the first console run of
// that runtime stopped at "in canonical: Operation not permitted
// [/app0/Darkness]" (2026-10-25). The title's paths contain no symbolic links,
// so when the console refuses the call the path is resolved lexically - made
// absolute, "." and ".." folded - and must exist, as realpath requires.
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" char* __real_realpath(const char* path, char* resolved);
extern "C" char* __wrap_realpath(const char* path, char* resolved) {
    if (char* result = __real_realpath(path, resolved)) return result;
    if (!path || (errno != EPERM && errno != EACCES && errno != ENOSYS)) return nullptr;
    std::string absolute;
    if (path[0] != '/') {
        char cwd[PATH_MAX];
        absolute = ::getcwd(cwd, sizeof cwd) ? cwd : "";
        absolute += '/';
    }
    absolute += path;
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= absolute.size()) {
        size_t end = absolute.find('/', start);
        if (end == std::string::npos) end = absolute.size();
        const std::string part = absolute.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    std::string normal;
    for (const auto& part : parts) normal += "/" + part;
    if (normal.empty()) normal = "/";
    struct stat st {};
    if (::stat(normal.c_str(), &st) != 0) return nullptr; // errno from stat
    if (normal.size() >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return nullptr;
    }
    if (resolved) {
        std::memcpy(resolved, normal.c_str(), normal.size() + 1);
        return resolved;
    }
    return ::strdup(normal.c_str());
}

// stdout: the platform relays a title's stderr to the kernel log, and nothing
// reads its stdout. The native runtime and the renderer report through printf
// and puts, so the link sends those to stderr as well.
#include <cstdarg>
#include <cstdio>

extern "C" int __wrap_printf(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    return written;
}

extern "C" int __wrap_vprintf(const char* format, va_list arguments) {
    return std::vfprintf(stderr, format, arguments);
}

extern "C" int __wrap_puts(const char* text) {
    const int written = std::fputs(text, stderr);
    std::fputc('\n', stderr);
    return written < 0 ? written : written + 1;
}

extern "C" int __wrap_putchar(int character) { return std::fputc(character, stderr); }
