#pragma once
// The Win32 and NT subset the native runtime uses, for the console.
//
// runtime/native is the Windows build's Xbox 360 kernel layer, the one the game
// boots and plays with. Rather than fork it, the console build compiles the same
// sources against this header (reached through app/ps5/win32/windows.h and its
// siblings), and win32_ps5.cpp implements the subset over the console's POSIX
// layer: kernel objects behind HANDLEs (events, semaphores, waitable timers,
// threads, files), alertable waits and user APCs, critical sections, the
// guest address space's virtual memory, and the NT file calls filesystem.cpp
// resolves through GetProcAddress.
//
// posix_compat.h is the render layer's smaller stand-in; this is a superset.
#if defined(_WIN32)
#error "win32_ps5.h is the console's stand-in for <windows.h>"
#endif

#define DARK_FULL_WIN32_SUBSET 1
#include "posix_compat.h"

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <string_view>

#define WINAPI
#define NTAPI
#define CALLBACK
#define APIENTRY
#define __stdcall
#define __cdecl
#define FORCEINLINE inline __attribute__((always_inline))
#define DECLSPEC_ALIGN(x) __attribute__((aligned(x)))

using BOOL = int;
using BOOLEAN = uint8_t;
using BYTE = uint8_t;
using CHAR = char;
using UCHAR = uint8_t;
using WCHAR = wchar_t;
using SHORT = int16_t;
using USHORT = uint16_t;
using WORD = uint16_t;
using INT = int;
using UINT = unsigned int;
using LONG_PTR = intptr_t;
using ULONG_PTR = uintptr_t;
using UINT_PTR = uintptr_t;
using INT_PTR = intptr_t;
using SSIZE_T = intptr_t;
using DWORD64 = uint64_t;
using DWORD32 = uint32_t;
using ULONG64 = uint64_t;
using UINT64 = uint64_t;
using INT64 = int64_t;
using UINT32 = uint32_t;
using INT32 = int32_t;
using UINT8 = uint8_t;
using LONGLONG = int64_t;
using NTSTATUS = LONG;
using HRESULT = LONG;
using ACCESS_MASK = DWORD;
using PVOID = void*;
using LPWSTR = wchar_t*;
using LPCWSTR = const wchar_t*;
using PCWSTR = const wchar_t*;
using PWSTR = wchar_t*;
using LPSTR = char*;
using LPCSTR = const char*;
using PCSTR = const char*;
using LPBYTE = BYTE*;
using PBYTE = BYTE*;
using PUCHAR = UCHAR*;
using LPDWORD = DWORD*;
using PDWORD = DWORD*;
using PULONG = ULONG*;
using PLONG = LONG*;
using PHANDLE = HANDLE*;
using LPHANDLE = HANDLE*;
using PLARGE_INTEGER = LARGE_INTEGER*;
using HMODULE = void*;
using HINSTANCE = void*;
using FARPROC = void (*)();
using PSIZE_T = SIZE_T*;
using LPLONG = LONG*;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

union ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    uint64_t QuadPart;
};

struct FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
};
using LPFILETIME = FILETIME*;

struct SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};
using LPSYSTEMTIME = SYSTEMTIME*;

struct TIME_ZONE_INFORMATION {
    LONG Bias;
    WCHAR StandardName[32];
    SYSTEMTIME StandardDate;
    LONG StandardBias;
    WCHAR DaylightName[32];
    SYSTEMTIME DaylightDate;
    LONG DaylightBias;
};
struct DYNAMIC_TIME_ZONE_INFORMATION {
    LONG Bias;
    WCHAR StandardName[32];
    SYSTEMTIME StandardDate;
    LONG StandardBias;
    WCHAR DaylightName[32];
    SYSTEMTIME DaylightDate;
    LONG DaylightBias;
    WCHAR TimeZoneKeyName[128];
    BOOLEAN DynamicDaylightTimeDisabled;
};
constexpr DWORD TIME_ZONE_ID_UNKNOWN = 0;
constexpr DWORD TIME_ZONE_ID_STANDARD = 1;
constexpr DWORD TIME_ZONE_ID_DAYLIGHT = 2;
constexpr DWORD TIME_ZONE_ID_INVALID = 0xFFFFFFFFu;

struct SECURITY_ATTRIBUTES {
    DWORD nLength;
    void* lpSecurityDescriptor;
    BOOL bInheritHandle;
};
using LPSECURITY_ATTRIBUTES = SECURITY_ATTRIBUTES*;

struct OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    DWORD Offset;
    DWORD OffsetHigh;
    HANDLE hEvent;
};
using LPOVERLAPPED = OVERLAPPED*;

// --- Errors ------------------------------------------------------------------
constexpr DWORD ERROR_SUCCESS = 0;
constexpr DWORD ERROR_FILE_NOT_FOUND = 2;
constexpr DWORD ERROR_PATH_NOT_FOUND = 3;
constexpr DWORD ERROR_ACCESS_DENIED = 5;
constexpr DWORD ERROR_INVALID_HANDLE = 6;
constexpr DWORD ERROR_NOT_ENOUGH_MEMORY = 8;
constexpr DWORD ERROR_OUTOFMEMORY = 14;
constexpr DWORD ERROR_NO_MORE_FILES = 18;
constexpr DWORD ERROR_WRITE_PROTECT = 19;
constexpr DWORD ERROR_SHARING_VIOLATION = 32;
constexpr DWORD ERROR_HANDLE_EOF = 38;
constexpr DWORD ERROR_NOT_SUPPORTED = 50;
constexpr DWORD ERROR_FILE_EXISTS = 80;
constexpr DWORD ERROR_INVALID_PARAMETER = 87;
constexpr DWORD ERROR_DISK_FULL = 112;
constexpr DWORD ERROR_INSUFFICIENT_BUFFER = 122;
constexpr DWORD ERROR_DIR_NOT_EMPTY = 145;
constexpr DWORD ERROR_BUSY = 170;
constexpr DWORD ERROR_ALREADY_EXISTS = 183;
constexpr DWORD ERROR_ENVVAR_NOT_FOUND = 203;
constexpr DWORD ERROR_MORE_DATA = 234;
constexpr DWORD ERROR_NO_MORE_ITEMS = 259;
constexpr DWORD ERROR_IO_PENDING = 997;
constexpr DWORD ERROR_TIMEOUT = 1460;
constexpr DWORD ERROR_FUNCTION_FAILED = 1627;
constexpr DWORD ERROR_DEVICE_NOT_CONNECTED = 1167;
constexpr DWORD ERROR_CANCELLED = 1223;
constexpr DWORD ERROR_NOT_FOUND = 1168;

#define HRESULT_FROM_WIN32(x) \
    ((HRESULT)(x) <= 0 ? (HRESULT)(x) : (HRESULT)(((x) & 0x0000FFFF) | (7 << 16) | 0x80000000))
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
constexpr HRESULT S_OK = 0;
constexpr HRESULT S_FALSE = 1;
constexpr HRESULT E_FAIL = HRESULT(0x80004005u);
constexpr HRESULT E_INVALIDARG = HRESULT(0x80070057u);
constexpr HRESULT E_OUTOFMEMORY = HRESULT(0x8007000Eu);
constexpr HRESULT E_NOTIMPL = HRESULT(0x80004001u);
constexpr HRESULT E_POINTER = HRESULT(0x80004003u);

DWORD GetLastError();
void SetLastError(DWORD error);

// --- Handles and waits ---------------------------------------------------------
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(intptr_t(-1)))
constexpr DWORD INFINITE = 0xFFFFFFFFu;
constexpr DWORD WAIT_OBJECT_0 = 0;
constexpr DWORD WAIT_ABANDONED = 0x80;
constexpr DWORD WAIT_ABANDONED_0 = 0x80;
constexpr DWORD WAIT_IO_COMPLETION = 0xC0;
constexpr DWORD WAIT_TIMEOUT = 258;
constexpr DWORD WAIT_FAILED = 0xFFFFFFFFu;
constexpr DWORD MAXIMUM_WAIT_OBJECTS = 64;
constexpr DWORD STILL_ACTIVE = 259;
constexpr DWORD DUPLICATE_CLOSE_SOURCE = 1;
constexpr DWORD DUPLICATE_SAME_ACCESS = 2;
constexpr DWORD SYNCHRONIZE = 0x00100000u;
constexpr DWORD EVENT_ALL_ACCESS = 0x1F0003u;
constexpr DWORD SEMAPHORE_ALL_ACCESS = 0x1F0003u;
constexpr DWORD TIMER_ALL_ACCESS = 0x1F0003u;
constexpr DWORD THREAD_ALL_ACCESS = 0x1FFFFFu;
constexpr DWORD CREATE_EVENT_MANUAL_RESET = 1;
constexpr DWORD CREATE_EVENT_INITIAL_SET = 2;
constexpr DWORD CREATE_WAITABLE_TIMER_MANUAL_RESET = 1;
constexpr DWORD CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 2;

BOOL CloseHandle(HANDLE handle);
BOOL DuplicateHandle(HANDLE sourceProcess, HANDLE source, HANDLE targetProcess, HANDLE* target,
                     DWORD access, BOOL inherit, DWORD options);
DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds);
DWORD WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL alertable);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD milliseconds);
DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD milliseconds,
                               BOOL alertable);
DWORD SleepEx(DWORD milliseconds, BOOL alertable);
BOOL SwitchToThread();

HANDLE CreateEventW(LPSECURITY_ATTRIBUTES attributes, BOOL manualReset, BOOL initialState, LPCWSTR name);
HANDLE CreateEventExW(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name, DWORD flags, DWORD access);
BOOL SetEvent(HANDLE event);
BOOL ResetEvent(HANDLE event);
BOOL PulseEvent(HANDLE event);
HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES attributes, LONG initial, LONG maximum, LPCWSTR name);
HANDLE CreateSemaphoreExW(LPSECURITY_ATTRIBUTES attributes, LONG initial, LONG maximum, LPCWSTR name,
                          DWORD flags, DWORD access);
BOOL ReleaseSemaphore(HANDLE semaphore, LONG count, LONG* previous);
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES attributes, BOOL initialOwner, LPCWSTR name);
BOOL ReleaseMutex(HANDLE mutex);

using PTIMERAPCROUTINE = void (*)(void* argument, DWORD lowValue, DWORD highValue);
HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES attributes, BOOL manualReset, LPCWSTR name);
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name, DWORD flags, DWORD access);
BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER* dueTime, LONG period, PTIMERAPCROUTINE routine,
                      void* argument, BOOL resume);
BOOL CancelWaitableTimer(HANDLE timer);

// --- Threads -----------------------------------------------------------------
using LPTHREAD_START_ROUTINE = DWORD (*)(void* parameter);
using PAPCFUNC = void (*)(ULONG_PTR parameter);
constexpr DWORD CREATE_SUSPENDED = 0x4;
constexpr DWORD STACK_SIZE_PARAM_IS_A_RESERVATION = 0x10000;
constexpr int THREAD_PRIORITY_LOWEST = -2;
constexpr int THREAD_PRIORITY_BELOW_NORMAL = -1;
constexpr int THREAD_PRIORITY_NORMAL = 0;
constexpr int THREAD_PRIORITY_ABOVE_NORMAL = 1;
constexpr int THREAD_PRIORITY_HIGHEST = 2;
constexpr int THREAD_PRIORITY_TIME_CRITICAL = 15;
constexpr int THREAD_PRIORITY_IDLE = -15;

HANDLE CreateThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stackSize, LPTHREAD_START_ROUTINE start,
                    void* parameter, DWORD flags, DWORD* threadId);
DWORD ResumeThread(HANDLE thread);
DWORD SuspendThread(HANDLE thread);
BOOL GetExitCodeThread(HANDLE thread, DWORD* exitCode);
[[noreturn]] void ExitThread(DWORD exitCode);
BOOL SetThreadPriority(HANDLE thread, int priority);
int GetThreadPriority(HANDLE thread);
DWORD_PTR SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask);
DWORD GetThreadId(HANDLE thread);
DWORD GetCurrentProcessId();
DWORD QueueUserAPC(PAPCFUNC routine, HANDLE thread, ULONG_PTR parameter);
[[noreturn]] void ExitProcess(UINT exitCode);

struct GROUP_AFFINITY {
    ULONG_PTR Mask;
    WORD Group;
    WORD Reserved[3];
};
BOOL GetThreadGroupAffinity(HANDLE thread, GROUP_AFFINITY* affinity);

// CPU sets: the console reports none, so callers keep their default topology.
enum CPU_SET_INFORMATION_TYPE { CpuSetInformation };
struct SYSTEM_CPU_SET_INFORMATION {
    DWORD Size;
    CPU_SET_INFORMATION_TYPE Type;
    union {
        struct {
            DWORD Id;
            WORD Group;
            BYTE LogicalProcessorIndex;
            BYTE CoreIndex;
            BYTE LastLevelCacheIndex;
            BYTE NumaNodeIndex;
            BYTE EfficiencyClass;
            union {
                BYTE AllFlags;
                struct {
                    BYTE Parked : 1;
                    BYTE Allocated : 1;
                    BYTE AllocatedToTargetProcess : 1;
                    BYTE RealTime : 1;
                    BYTE ReservedFlags : 4;
                };
            };
            union {
                DWORD Reserved;
                BYTE SchedulingClass;
            };
            DWORD64 AllocationTag;
        } CpuSet;
    };
};
BOOL GetSystemCpuSetInformation(SYSTEM_CPU_SET_INFORMATION* information, ULONG length, ULONG* returned,
                                HANDLE process, ULONG flags);

// --- Critical sections and interlocked ----------------------------------------
// The runtime publishes RecursionCount to the guest (RtlEnterCriticalSection),
// so the owner keeps it current as Windows does.
struct CRITICAL_SECTION {
    void* implementation;
    LONG LockCount;
    LONG RecursionCount;
    HANDLE OwningThread;
};
using LPCRITICAL_SECTION = CRITICAL_SECTION*;
void InitializeCriticalSection(CRITICAL_SECTION* section);
BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION* section, DWORD spinCount);
void DeleteCriticalSection(CRITICAL_SECTION* section);
void EnterCriticalSection(CRITICAL_SECTION* section);
BOOL TryEnterCriticalSection(CRITICAL_SECTION* section);
void LeaveCriticalSection(CRITICAL_SECTION* section);

inline LONG InterlockedIncrement(volatile LONG* value) { return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST); }
inline LONG InterlockedDecrement(volatile LONG* value) { return __atomic_sub_fetch(value, 1, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedIncrement64(volatile LONG64* value) { return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedDecrement64(volatile LONG64* value) { return __atomic_sub_fetch(value, 1, __ATOMIC_SEQ_CST); }
inline LONG InterlockedExchange(volatile LONG* target, LONG value) {
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}
inline LONG64 InterlockedExchange64(volatile LONG64* target, LONG64 value) {
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}
inline char InterlockedExchange8(volatile char* target, char value) {
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}
inline LONG InterlockedCompareExchange(volatile LONG* target, LONG exchange, LONG comparand) {
    __atomic_compare_exchange_n(target, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
inline LONG64 InterlockedCompareExchange64(volatile LONG64* target, LONG64 exchange, LONG64 comparand) {
    __atomic_compare_exchange_n(target, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
inline LONG InterlockedExchangeAdd(volatile LONG* target, LONG value) {
    return __atomic_fetch_add(target, value, __ATOMIC_SEQ_CST);
}
inline LONG64 InterlockedExchangeAdd64(volatile LONG64* target, LONG64 value) {
    return __atomic_fetch_add(target, value, __ATOMIC_SEQ_CST);
}
inline LONG InterlockedOr(volatile LONG* target, LONG value) { return __atomic_fetch_or(target, value, __ATOMIC_SEQ_CST); }
inline LONG InterlockedAnd(volatile LONG* target, LONG value) { return __atomic_fetch_and(target, value, __ATOMIC_SEQ_CST); }
inline void* InterlockedExchangePointer(void* volatile* target, void* value) {
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}
inline void* InterlockedCompareExchangePointer(void* volatile* target, void* exchange, void* comparand) {
    __atomic_compare_exchange_n(target, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
inline void MemoryBarrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
inline void YieldProcessor() { __builtin_ia32_pause(); }
#define _ReadWriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define _mm_pause_compat() __builtin_ia32_pause()

// --- Time ---------------------------------------------------------------------
BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency);
void GetSystemTimeAsFileTime(FILETIME* time);
BOOL FileTimeToSystemTime(const FILETIME* time, SYSTEMTIME* system);
BOOL SystemTimeToFileTime(const SYSTEMTIME* system, FILETIME* time);
DWORD GetTimeZoneInformation(TIME_ZONE_INFORMATION* information);
DWORD GetDynamicTimeZoneInformation(DYNAMIC_TIME_ZONE_INFORMATION* information);
DWORD GetTickCount();

// --- Virtual memory -------------------------------------------------------------
constexpr DWORD MEM_COMMIT = 0x1000;
constexpr DWORD MEM_RESERVE = 0x2000;
constexpr DWORD MEM_DECOMMIT = 0x4000;
constexpr DWORD MEM_RELEASE = 0x8000;
constexpr DWORD MEM_FREE = 0x10000;
constexpr DWORD MEM_PRIVATE = 0x20000;
constexpr DWORD MEM_MAPPED = 0x40000;
constexpr DWORD MEM_RESET = 0x80000;
constexpr DWORD MEM_TOP_DOWN = 0x100000;
constexpr DWORD MEM_LARGE_PAGES = 0x20000000;
constexpr DWORD MEM_RESERVE_PLACEHOLDER = 0x40000;
constexpr DWORD MEM_REPLACE_PLACEHOLDER = 0x4000;
constexpr DWORD MEM_PRESERVE_PLACEHOLDER = 0x2;
constexpr DWORD MEM_COALESCE_PLACEHOLDERS = 0x1;
constexpr DWORD PAGE_NOACCESS = 0x01;
constexpr DWORD PAGE_READONLY = 0x02;
constexpr DWORD PAGE_READWRITE = 0x04;
constexpr DWORD PAGE_WRITECOPY = 0x08;
constexpr DWORD PAGE_EXECUTE = 0x10;
constexpr DWORD PAGE_EXECUTE_READ = 0x20;
constexpr DWORD PAGE_EXECUTE_READWRITE = 0x40;
constexpr DWORD PAGE_EXECUTE_WRITECOPY = 0x80;
constexpr DWORD PAGE_GUARD = 0x100;
constexpr DWORD PAGE_NOCACHE = 0x200;
constexpr DWORD PAGE_WRITECOMBINE = 0x400;
constexpr DWORD FILE_MAP_WRITE = 0x2;
constexpr DWORD FILE_MAP_READ = 0x4;
constexpr DWORD FILE_MAP_ALL_ACCESS = 0xF001F;
constexpr DWORD SEC_COMMIT = 0x8000000;
constexpr DWORD SEC_RESERVE = 0x4000000;

void* VirtualAlloc(void* address, SIZE_T size, DWORD type, DWORD protection);
BOOL VirtualFree(void* address, SIZE_T size, DWORD type);
BOOL VirtualProtect(void* address, SIZE_T size, DWORD protection, DWORD* oldProtection);
HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES attributes, DWORD protection, DWORD sizeHigh,
                          DWORD sizeLow, LPCWSTR name);
void* MapViewOfFile(HANDLE mapping, DWORD access, DWORD offsetHigh, DWORD offsetLow, SIZE_T bytes);
void* MapViewOfFileEx(HANDLE mapping, DWORD access, DWORD offsetHigh, DWORD offsetLow, SIZE_T bytes,
                      void* address);
BOOL UnmapViewOfFile(const void* address);

struct PSAPI_WORKING_SET_EX_BLOCK {
    ULONG_PTR Flags;
    struct {
        ULONG_PTR Valid : 1;
        ULONG_PTR ShareCount : 3;
        ULONG_PTR Win32Protection : 11;
        ULONG_PTR Shared : 1;
        ULONG_PTR Node : 6;
        ULONG_PTR Locked : 1;
        ULONG_PTR LargePage : 1;
    };
};
struct PSAPI_WORKING_SET_EX_INFORMATION {
    void* VirtualAddress;
    PSAPI_WORKING_SET_EX_BLOCK VirtualAttributes;
};
BOOL K32QueryWorkingSetEx(HANDLE process, void* buffer, DWORD size);

// --- Files ----------------------------------------------------------------------
constexpr DWORD GENERIC_READ = 0x80000000u;
constexpr DWORD GENERIC_WRITE = 0x40000000u;
constexpr DWORD GENERIC_ALL = 0x10000000u;
constexpr DWORD FILE_SHARE_READ = 1;
constexpr DWORD FILE_SHARE_WRITE = 2;
constexpr DWORD FILE_SHARE_DELETE = 4;
constexpr DWORD CREATE_NEW = 1;
constexpr DWORD CREATE_ALWAYS = 2;
constexpr DWORD OPEN_EXISTING = 3;
constexpr DWORD OPEN_ALWAYS = 4;
constexpr DWORD TRUNCATE_EXISTING = 5;
constexpr DWORD FILE_ATTRIBUTE_READONLY = 0x1;
constexpr DWORD FILE_ATTRIBUTE_HIDDEN = 0x2;
constexpr DWORD FILE_ATTRIBUTE_DIRECTORY = 0x10;
constexpr DWORD FILE_ATTRIBUTE_ARCHIVE = 0x20;
constexpr DWORD FILE_ATTRIBUTE_NORMAL = 0x80;
constexpr DWORD FILE_FLAG_WRITE_THROUGH = 0x80000000u;
constexpr DWORD FILE_FLAG_NO_BUFFERING = 0x20000000u;
constexpr DWORD FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000u;
constexpr DWORD FILE_FLAG_BACKUP_SEMANTICS = 0x02000000u;
constexpr DWORD MOVEFILE_REPLACE_EXISTING = 0x1;
constexpr DWORD MOVEFILE_WRITE_THROUGH = 0x8;
constexpr DWORD FILE_BEGIN = 0;
constexpr DWORD FILE_CURRENT = 1;
constexpr DWORD FILE_END = 2;

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes,
                   DWORD disposition, DWORD flags, HANDLE templateFile);
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes,
                   DWORD disposition, DWORD flags, HANDLE templateFile);
BOOL ReadFile(HANDLE file, void* buffer, DWORD bytes, DWORD* read, LPOVERLAPPED overlapped);
BOOL WriteFile(HANDLE file, const void* buffer, DWORD bytes, DWORD* written, LPOVERLAPPED overlapped);
BOOL FlushFileBuffers(HANDLE file);
BOOL GetFileSizeEx(HANDLE file, LARGE_INTEGER* size);
BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, LARGE_INTEGER* position, DWORD method);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL DeleteFileW(LPCWSTR path);

// --- Strings, environment and modules -------------------------------------------
constexpr UINT CP_ACP = 0;
constexpr UINT CP_UTF8 = 65001;
constexpr DWORD MB_ERR_INVALID_CHARS = 0x8;
constexpr DWORD WC_ERR_INVALID_CHARS = 0x80;
int MultiByteToWideChar(UINT codePage, DWORD flags, const char* text, int bytes, wchar_t* wide, int wideCount);
int WideCharToMultiByte(UINT codePage, DWORD flags, const wchar_t* wide, int wideCount, char* text, int bytes,
                        const char* defaultChar, BOOL* usedDefault);
DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD size);
DWORD GetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size);
HMODULE GetModuleHandleW(LPCWSTR name);
HMODULE LoadLibraryW(LPCWSTR name);
FARPROC GetProcAddress(HMODULE module, LPCSTR name);
void OutputDebugStringA(LPCSTR text);
void OutputDebugStringW(LPCWSTR text);

int _wcsicmp(const wchar_t* a, const wchar_t* b);
int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t count);
int _stricmp(const char* a, const char* b);
int _strnicmp(const char* a, const char* b, size_t count);
inline void* _aligned_malloc(size_t size, size_t alignment) {
    return ::aligned_alloc(alignment, ((size + alignment - 1) / alignment) * alignment);
}
inline void _aligned_free(void* pointer) { ::free(pointer); }
// Paths are UTF-8 on the console, so the runtime's wide comparisons of a
// path's native string against a wide literal compare after widening.
int _wcsicmp(const char* a, const wchar_t* b);
int _wcsnicmp(const char* a, const wchar_t* b, size_t count);
inline void _lock_file(FILE* file) { flockfile(file); }
inline void _unlock_file(FILE* file) { funlockfile(file); }

// --- Structured exceptions ----------------------------------------------------
// The console has no SEH. The runtime's handlers guard host-side reads of
// guest memory; on the console those reads stay inside the one committed 4 GB
// guest mapping, so the __try body runs and the __except body never does.
#define __try if (true)
#define __except(filter) else if (false)
#define __finally
#define GetExceptionCode() DWORD(0)
#define GetExceptionInformation() static_cast<EXCEPTION_POINTERS*>(nullptr)
constexpr LONG EXCEPTION_CONTINUE_EXECUTION = -1;
constexpr DWORD EXCEPTION_NONCONTINUABLE = 1;
[[noreturn]] void RaiseException(DWORD code, DWORD flags, DWORD argumentCount, const ULONG_PTR* arguments);

// --- NT (winternl.h) --------------------------------------------------------------
struct UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};
using PUNICODE_STRING = UNICODE_STRING*;
struct OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};
using POBJECT_ATTRIBUTES = OBJECT_ATTRIBUTES*;
constexpr ULONG OBJ_CASE_INSENSITIVE = 0x40;
#define InitializeObjectAttributes(p, n, a, r, s) \
    do {                                          \
        (p)->Length = sizeof(OBJECT_ATTRIBUTES);  \
        (p)->RootDirectory = (r);                 \
        (p)->Attributes = (a);                    \
        (p)->ObjectName = (n);                    \
        (p)->SecurityDescriptor = (s);            \
        (p)->SecurityQualityOfService = nullptr;  \
    } while (0)

struct IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
};
using PIO_STATUS_BLOCK = IO_STATUS_BLOCK*;
using PIO_APC_ROUTINE = void (*)(PVOID context, PIO_STATUS_BLOCK status, ULONG reserved);

enum FILE_INFORMATION_CLASS : int {
    FileDirectoryInformation = 1,
    FileBothDirectoryInformation = 3,
    FileBasicInformation = 4,
    FileStandardInformation = 5,
    FileInternalInformation = 6,
    FileRenameInformation = 10,
    FileDispositionInformation = 13,
    FilePositionInformation = 14,
    FileModeInformation = 16,
    FileAlignmentInformation = 17,
    FileAllocationInformation = 19,
    FileEndOfFileInformation = 20,
    FileNetworkOpenInformation = 34,
};
enum FS_INFORMATION_CLASS : int {
    FileFsVolumeInformation = 1,
    FileFsSizeInformation = 3,
    FileFsDeviceInformation = 4,
    FileFsAttributeInformation = 5,
    FileFsFullSizeInformation = 7,
};

constexpr ULONG FILE_SUPERSEDE = 0;
constexpr ULONG FILE_OPEN = 1;
constexpr ULONG FILE_CREATE = 2;
constexpr ULONG FILE_OPEN_IF = 3;
constexpr ULONG FILE_OVERWRITE = 4;
constexpr ULONG FILE_OVERWRITE_IF = 5;
constexpr ULONG FILE_DIRECTORY_FILE = 0x1;
constexpr ULONG FILE_WRITE_THROUGH = 0x2;
constexpr ULONG FILE_SEQUENTIAL_ONLY = 0x4;
constexpr ULONG FILE_NO_INTERMEDIATE_BUFFERING = 0x8;
constexpr ULONG FILE_SYNCHRONOUS_IO_ALERT = 0x10;
constexpr ULONG FILE_SYNCHRONOUS_IO_NONALERT = 0x20;
constexpr ULONG FILE_NON_DIRECTORY_FILE = 0x40;
constexpr ULONG FILE_OPEN_FOR_BACKUP_INTENT = 0x4000;
constexpr ULONG FILE_SUPERSEDED = 0;
constexpr ULONG FILE_OPENED = 1;
constexpr ULONG FILE_CREATED = 2;
constexpr ULONG FILE_OVERWRITTEN = 3;
constexpr ULONG FILE_EXISTS = 4;
constexpr ULONG FILE_DOES_NOT_EXIST = 5;
constexpr DWORD FILE_READ_DATA = 0x1;
constexpr DWORD FILE_LIST_DIRECTORY = 0x1;
constexpr DWORD FILE_WRITE_DATA = 0x2;
constexpr DWORD FILE_APPEND_DATA = 0x4;
constexpr DWORD FILE_READ_ATTRIBUTES = 0x80;
constexpr DWORD FILE_WRITE_ATTRIBUTES = 0x100;
constexpr DWORD FILE_READ_EA = 0x8;
constexpr DWORD FILE_WRITE_EA = 0x10;
constexpr DWORD FILE_EXECUTE = 0x20;
constexpr DWORD FILE_DELETE_CHILD = 0x40;
constexpr DWORD WRITE_DAC = 0x40000;
constexpr DWORD WRITE_OWNER = 0x80000;
constexpr DWORD FILE_ALL_ACCESS = 0x1F01FF;
constexpr DWORD DELETE = 0x10000;
constexpr DWORD READ_CONTROL = 0x20000;
constexpr DWORD FILE_GENERIC_READ = 0x120089;
constexpr DWORD FILE_GENERIC_WRITE = 0x120116;

// The NT calls filesystem.cpp resolves from "ntdll.dll" through
// GetProcAddress; GetProcAddress hands back these.
NTSTATUS NtCreateFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes, PIO_STATUS_BLOCK io,
                      PLARGE_INTEGER allocation, ULONG fileAttributes, ULONG share, ULONG disposition,
                      ULONG options, PVOID ea, ULONG eaLength);
NTSTATUS NtReadFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, PVOID apcContext, PIO_STATUS_BLOCK io,
                    PVOID buffer, ULONG length, PLARGE_INTEGER offset, PULONG key);
NTSTATUS NtWriteFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, PVOID apcContext, PIO_STATUS_BLOCK io,
                     const void* buffer, ULONG length, PLARGE_INTEGER offset, PULONG key);
NTSTATUS NtQueryInformationFile(HANDLE file, PIO_STATUS_BLOCK io, PVOID information, ULONG length,
                                FILE_INFORMATION_CLASS kind);
NTSTATUS NtSetInformationFile(HANDLE file, PIO_STATUS_BLOCK io, PVOID information, ULONG length,
                              FILE_INFORMATION_CLASS kind);
NTSTATUS NtQueryFullAttributesFile(POBJECT_ATTRIBUTES attributes, PVOID information);
NTSTATUS NtQueryDirectoryFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apc, PVOID apcContext,
                              PIO_STATUS_BLOCK io, PVOID information, ULONG length,
                              FILE_INFORMATION_CLASS kind, BOOLEAN single, PUNICODE_STRING mask,
                              BOOLEAN restart);
NTSTATUS NtQueryVolumeInformationFile(HANDLE file, PIO_STATUS_BLOCK io, PVOID information, ULONG length,
                                      FS_INFORMATION_CLASS kind);
NTSTATUS NtFlushBuffersFile(HANDLE file, PIO_STATUS_BLOCK io);
NTSTATUS NtClose(HANDLE handle);
ULONG RtlNtStatusToDosError(NTSTATUS status);

// --- Random (bcrypt.h) ------------------------------------------------------------
using BCRYPT_ALG_HANDLE = void*;
constexpr ULONG BCRYPT_USE_SYSTEM_PREFERRED_RNG = 2;
NTSTATUS BCryptGenRandom(BCRYPT_ALG_HANDLE algorithm, PUCHAR buffer, ULONG bytes, ULONG flags);

// --- Debug help (dbghelp.h) -------------------------------------------------------
// No symbol server on the console: the runtime's crash backtrace prints
// addresses only.
struct SYMBOL_INFO {
    ULONG SizeOfStruct;
    ULONG TypeIndex;
    ULONG64 Reserved[2];
    ULONG Index;
    ULONG Size;
    ULONG64 ModBase;
    ULONG Flags;
    ULONG64 Value;
    ULONG64 Address;
    ULONG Register;
    ULONG Scope;
    ULONG Tag;
    ULONG NameLen;
    ULONG MaxNameLen;
    CHAR Name[1];
};
constexpr ULONG MAX_SYM_NAME = 2000;
inline BOOL SymInitialize(HANDLE, PCSTR, BOOL) { return FALSE; }
inline BOOL SymFromAddr(HANDLE, DWORD64, DWORD64*, SYMBOL_INFO*) { return FALSE; }

// --- XInput (Xinput.h) ----------------------------------------------------------
// The native input layer reads controllers through these three calls; on the
// console they read the pad (runtime/native/input_ps5.cpp) as user 0.
constexpr DWORD XUSER_MAX_COUNT = 4;
constexpr DWORD XINPUT_FLAG_GAMEPAD = 1;
constexpr BYTE XINPUT_DEVTYPE_GAMEPAD = 1;
constexpr BYTE XINPUT_DEVSUBTYPE_GAMEPAD = 1;
constexpr WORD XINPUT_CAPS_FFB_SUPPORTED = 0x0001;
constexpr WORD XINPUT_CAPS_WIRELESS = 0x0002;
constexpr WORD XINPUT_GAMEPAD_DPAD_UP = 0x0001;
constexpr WORD XINPUT_GAMEPAD_DPAD_DOWN = 0x0002;
constexpr WORD XINPUT_GAMEPAD_DPAD_LEFT = 0x0004;
constexpr WORD XINPUT_GAMEPAD_DPAD_RIGHT = 0x0008;
constexpr WORD XINPUT_GAMEPAD_START = 0x0010;
constexpr WORD XINPUT_GAMEPAD_BACK = 0x0020;
constexpr WORD XINPUT_GAMEPAD_LEFT_THUMB = 0x0040;
constexpr WORD XINPUT_GAMEPAD_RIGHT_THUMB = 0x0080;
constexpr WORD XINPUT_GAMEPAD_LEFT_SHOULDER = 0x0100;
constexpr WORD XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200;
constexpr WORD XINPUT_GAMEPAD_A = 0x1000;
constexpr WORD XINPUT_GAMEPAD_B = 0x2000;
constexpr WORD XINPUT_GAMEPAD_X = 0x4000;
constexpr WORD XINPUT_GAMEPAD_Y = 0x8000;
constexpr SHORT XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE = 7849;
constexpr SHORT XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE = 8689;
constexpr BYTE XINPUT_GAMEPAD_TRIGGER_THRESHOLD = 30;
struct XINPUT_GAMEPAD {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};
struct XINPUT_STATE {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
};
struct XINPUT_VIBRATION {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
};
struct XINPUT_CAPABILITIES {
    BYTE Type;
    BYTE SubType;
    WORD Flags;
    XINPUT_GAMEPAD Gamepad;
    XINPUT_VIBRATION Vibration;
};
DWORD XInputGetState(DWORD user, XINPUT_STATE* state);
DWORD XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* capabilities);
DWORD XInputSetState(DWORD user, XINPUT_VIBRATION* vibration);

// --- Windows and messages ---------------------------------------------------------
// The console has no windows. The native input layer still names the window
// messages and virtual keys it would translate; with no window attached it
// never receives one, and its controller path is the only live one.
struct HWND__;
using HWND = HWND__*;
using WPARAM = UINT_PTR;
using LPARAM = LONG_PTR;
using LRESULT = LONG_PTR;
struct POINT { LONG x, y; };
struct RECT { LONG left, top, right, bottom; };
constexpr UINT WM_NCDESTROY = 0x0082;
constexpr UINT WM_SETFOCUS = 0x0007;
constexpr UINT WM_KILLFOCUS = 0x0008;
constexpr UINT WM_ACTIVATEAPP = 0x001C;
constexpr UINT WM_KEYDOWN = 0x0100;
constexpr UINT WM_KEYUP = 0x0101;
constexpr UINT WM_SYSKEYDOWN = 0x0104;
constexpr UINT WM_SYSKEYUP = 0x0105;
constexpr UINT WM_MOUSEMOVE = 0x0200;
constexpr UINT WM_LBUTTONDOWN = 0x0201;
constexpr UINT WM_LBUTTONUP = 0x0202;
constexpr UINT WM_RBUTTONDOWN = 0x0204;
constexpr UINT WM_RBUTTONUP = 0x0205;
constexpr UINT WM_MBUTTONDOWN = 0x0207;
constexpr UINT WM_MBUTTONUP = 0x0208;
constexpr UINT WM_MOUSEWHEEL = 0x020A;
constexpr UINT WM_XBUTTONDOWN = 0x020B;
constexpr UINT WM_XBUTTONUP = 0x020C;
constexpr UINT WM_CAPTURECHANGED = 0x0215;
constexpr int WHEEL_DELTA = 120;
constexpr WORD XBUTTON1 = 0x0001;
constexpr WORD XBUTTON2 = 0x0002;
#define GET_WHEEL_DELTA_WPARAM(w) (short(WORD(((DWORD_PTR)(w) >> 16) & 0xffff)))
#define GET_XBUTTON_WPARAM(w) (WORD(((DWORD_PTR)(w) >> 16) & 0xffff))
#define LOWORD(l) (WORD(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) (WORD((((DWORD_PTR)(l)) >> 16) & 0xffff))
constexpr unsigned VK_LBUTTON = 0x01;
constexpr unsigned VK_RBUTTON = 0x02;
constexpr unsigned VK_MBUTTON = 0x04;
constexpr unsigned VK_XBUTTON1 = 0x05;
constexpr unsigned VK_XBUTTON2 = 0x06;
constexpr unsigned VK_BACK = 0x08;
constexpr unsigned VK_TAB = 0x09;
constexpr unsigned VK_RETURN = 0x0D;
constexpr unsigned VK_SHIFT = 0x10;
constexpr unsigned VK_CONTROL = 0x11;
constexpr unsigned VK_MENU = 0x12;
constexpr unsigned VK_ESCAPE = 0x1B;
constexpr unsigned VK_SPACE = 0x20;
constexpr unsigned VK_LEFT = 0x25;
constexpr unsigned VK_UP = 0x26;
constexpr unsigned VK_RIGHT = 0x27;
constexpr unsigned VK_DOWN = 0x28;
constexpr unsigned VK_F1 = 0x70;
constexpr unsigned VK_F4 = 0x73;
constexpr unsigned VK_F5 = 0x74;
constexpr unsigned VK_F6 = 0x75;
constexpr unsigned VK_LSHIFT = 0xA0;
constexpr unsigned VK_RSHIFT = 0xA1;
constexpr unsigned VK_LCONTROL = 0xA2;
constexpr unsigned VK_RCONTROL = 0xA3;

constexpr DWORD TIMER_QUERY_STATE = 0x0001;
constexpr DWORD TIMER_MODIFY_STATE = 0x0002;
using PSYSTEM_CPU_SET_INFORMATION = SYSTEM_CPU_SET_INFORMATION*;

// --- Remaining constants and CRT names ---------------------------------------------
constexpr DWORD FILE_ATTRIBUTE_TEMPORARY = 0x100;
constexpr DWORD FILE_FLAG_DELETE_ON_CLOSE = 0x04000000u;
constexpr DWORD ERROR_INVALID_DATA = 13;
constexpr DWORD THREAD_TERMINATE = 0x0001;
constexpr DWORD THREAD_SUSPEND_RESUME = 0x0002;
constexpr DWORD THREAD_GET_CONTEXT = 0x0008;
constexpr DWORD THREAD_SET_CONTEXT = 0x0010;
constexpr DWORD THREAD_QUERY_INFORMATION = 0x0040;
constexpr DWORD THREAD_SET_INFORMATION = 0x0020;
constexpr DWORD THREAD_QUERY_LIMITED_INFORMATION = 0x0800;

// x87/SSE exception masks, as the CRT names them; only the SSE state matters
// on x86-64, and it is the MXCSR mask bits.
constexpr unsigned _MCW_EM = 0x0008001Fu;
constexpr unsigned _EM_INVALID = 0x10u;
constexpr unsigned _EM_DENORMAL = 0x80000u;
constexpr unsigned _EM_ZERODIVIDE = 0x08u;
constexpr unsigned _EM_OVERFLOW = 0x04u;
constexpr unsigned _EM_UNDERFLOW = 0x02u;
constexpr unsigned _EM_INEXACT = 0x01u;
int _controlfp_s(unsigned* current, unsigned value, unsigned mask);

template <size_t N>
inline int sprintf_s(char (&buffer)[N], const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(buffer, N, format, arguments);
    va_end(arguments);
    return written;
}
inline int sprintf_s(char* buffer, size_t size, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return written;
}
// CPU cycles the calling thread has used; the console reports its CPU time in
// nanoseconds, which serves the same relative measurements.
BOOL QueryThreadCycleTime(HANDLE thread, ULONG64* cycles);

// --- More virtual keys and the keyboard names the binding editor shows --------------
constexpr unsigned VK_CAPITAL = 0x14;
constexpr unsigned VK_PRIOR = 0x21;
constexpr unsigned VK_NEXT = 0x22;
constexpr unsigned VK_END = 0x23;
constexpr unsigned VK_HOME = 0x24;
constexpr unsigned VK_SNAPSHOT = 0x2C;
constexpr unsigned VK_INSERT = 0x2D;
constexpr unsigned VK_DELETE = 0x2E;
constexpr unsigned VK_NUMPAD0 = 0x60;
constexpr unsigned VK_NUMPAD9 = 0x69;
constexpr unsigned VK_MULTIPLY = 0x6A;
constexpr unsigned VK_ADD = 0x6B;
constexpr unsigned VK_SUBTRACT = 0x6D;
constexpr unsigned VK_DECIMAL = 0x6E;
constexpr unsigned VK_DIVIDE = 0x6F;
constexpr unsigned VK_F2 = 0x71;
constexpr unsigned VK_F8 = 0x77;
constexpr unsigned VK_F24 = 0x87;
constexpr unsigned VK_NUMLOCK = 0x90;
constexpr unsigned VK_SCROLL = 0x91;
constexpr unsigned VK_OEM_1 = 0xBA;
constexpr unsigned VK_OEM_PLUS = 0xBB;
constexpr unsigned VK_OEM_COMMA = 0xBC;
constexpr unsigned VK_OEM_MINUS = 0xBD;
constexpr unsigned VK_OEM_PERIOD = 0xBE;
constexpr unsigned VK_OEM_2 = 0xBF;
constexpr unsigned VK_OEM_3 = 0xC0;
constexpr unsigned VK_OEM_4 = 0xDB;
constexpr unsigned VK_OEM_5 = 0xDC;
constexpr unsigned VK_OEM_6 = 0xDD;
constexpr unsigned VK_OEM_7 = 0xDE;
constexpr unsigned VK_OEM_102 = 0xE2;
constexpr UINT MAPVK_VK_TO_VSC = 0;
constexpr UINT MAPVK_VSC_TO_VK = 1;
// No keyboard layout on the console: scan codes are unknown, and a key's name
// is left to the binding editor's own fallback.
inline UINT MapVirtualKeyW(UINT, UINT) { return 0; }
inline int GetKeyNameTextW(LONG, LPWSTR buffer, int size) {
    if (buffer && size > 0) buffer[0] = 0;
    return 0;
}

// --- Slim reader/writer locks --------------------------------------------------------
struct SRWLOCK {
    void* Ptr;
};
#define SRWLOCK_INIT {nullptr}
void InitializeSRWLock(SRWLOCK* lock);
void AcquireSRWLockExclusive(SRWLOCK* lock);
void ReleaseSRWLockExclusive(SRWLOCK* lock);
void AcquireSRWLockShared(SRWLOCK* lock);
void ReleaseSRWLockShared(SRWLOCK* lock);
BOOLEAN TryAcquireSRWLockExclusive(SRWLOCK* lock);

// --- Language and settings files -------------------------------------------------------
using LANGID = WORD;
#define PRIMARYLANGID(id) (WORD(id) & 0x3ff)
constexpr WORD LANG_ENGLISH = 0x09;
constexpr WORD LANG_GERMAN = 0x07;
constexpr WORD LANG_FRENCH = 0x0c;
constexpr WORD LANG_SPANISH = 0x0a;
constexpr WORD LANG_ITALIAN = 0x10;
// The console's system language, in Windows' LANGID form.
LANGID GetUserDefaultUILanguage();
DWORD GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR fallback, LPWSTR buffer, DWORD size,
                               const char* path);
BOOL WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, const char* path);
UINT GetPrivateProfileIntW(LPCWSTR section, LPCWSTR key, INT fallback, const char* path);

// --- Hashing (bcrypt.h) ---------------------------------------------------------------
#define BCRYPT_SHA256_ALGORITHM L"SHA256"
NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE* algorithm, LPCWSTR id, LPCWSTR implementation,
                                     ULONG flags);
NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE algorithm, ULONG flags);
NTSTATUS BCryptHash(BCRYPT_ALG_HANDLE algorithm, PUCHAR secret, ULONG secretBytes, PUCHAR input,
                    ULONG inputBytes, PUCHAR output, ULONG outputBytes);

// --- Device control (winioctl.h) --------------------------------------------------------
constexpr DWORD FSCTL_SET_SPARSE = 0x000900C4;
// The console's file system has no sparse flag; files read back zeros in
// unwritten ranges either way, so the request succeeds without effect.
BOOL DeviceIoControl(HANDLE device, DWORD code, void* input, DWORD inputBytes, void* output, DWORD outputBytes,
                     DWORD* returned, LPOVERLAPPED overlapped);

// --- UTF-8 paths ------------------------------------------------------------------------
// std::filesystem::path::c_str() is UTF-8 on the console, where the runtime
// passes it to these wide calls.
HANDLE CreateFileW(const char* path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes,
                   DWORD disposition, DWORD flags, HANDLE templateFile);
BOOL MoveFileExW(const char* from, const char* to, DWORD flags);
BOOL DeleteFileW(const char* path);
constexpr DWORD ERROR_WRITE_FAULT = 29;
constexpr DWORD ERROR_READ_FAULT = 30;
inline size_t strnlen_s(const char* text, size_t limit) { return text ? ::strnlen(text, limit) : 0; }
// Mixed wide and UTF-8 arguments (a std::wstring temporary beside a path).
std::string win32Utf8(const wchar_t* text);
inline std::string win32Utf8(const char* text) { return text ? std::string(text) : std::string(); }
template <class From, class To>
inline BOOL MoveFileExW(const From* from, const To* to, DWORD flags) {
    return MoveFileExW(win32Utf8(from).c_str(), win32Utf8(to).c_str(), flags);
}
constexpr DWORD INVALID_FILE_ATTRIBUTES = 0xFFFFFFFFu;
constexpr DWORD FILE_ATTRIBUTE_REPARSE_POINT = 0x400;
DWORD GetFileAttributesW(LPCWSTR path);
DWORD GetFileAttributesW(const char* path);
BOOL SetEndOfFile(HANDLE file);
DWORD CharLowerBuffW(LPWSTR text, DWORD length);
DWORD CharUpperBuffW(LPWSTR text, DWORD length);
constexpr DWORD ERROR_NOT_READY = 21;
constexpr DWORD ERROR_NO_SUCH_USER = 1317;
using BCRYPT_HASH_HANDLE = void*;
NTSTATUS BCryptCreateHash(BCRYPT_ALG_HANDLE algorithm, BCRYPT_HASH_HANDLE* hash, PUCHAR object, ULONG objectBytes,
                          PUCHAR secret, ULONG secretBytes, ULONG flags);
NTSTATUS BCryptHashData(BCRYPT_HASH_HANDLE hash, PUCHAR input, ULONG inputBytes, ULONG flags);
NTSTATUS BCryptFinishHash(BCRYPT_HASH_HANDLE hash, PUCHAR output, ULONG outputBytes, ULONG flags);
NTSTATUS BCryptDestroyHash(BCRYPT_HASH_HANDLE hash);

// --- Console paths -------------------------------------------------------------------
#include <filesystem>
// The title's folder (/app0) is read-only; saves, the cache: drive and the
// runtime's own files live in the title's writable application-data mount.
inline constexpr const char* kConsoleDataRoot = "/download0/darkness-ps5";
// base / relative ('/'-separated), with each component that exists matched
// without regard to case, as the guest's disc and Windows' file system do.
// Components past the first missing one are kept as given.
std::filesystem::path win32MatchPathCase(const std::filesystem::path& base, const std::string& relative);
// UTF-8 to the guest's UTF-16 (empty on invalid input, as MB_ERR_INVALID_CHARS).
std::u16string win32Utf8ToUtf16(std::string_view text);
// The guest address space whose Win32 state (reserved, committed, protection)
// VirtualAlloc/VirtualFree/VirtualProtect/VirtualQuery keep. It starts
// reserved and inaccessible, as a fresh Windows reservation does.
void win32RegisterGuestSpace(void* base, uint64_t size);
void win32UnregisterGuestSpace(void* base);

// --- The four posix_compat.h leaves to this layer ---------------------------------------
// Thread ids are small, unique and stable for a thread's life, as Windows'
// are (the runtime stores them in guest thread objects).
DWORD GetCurrentThreadId();
// The pseudo-handle for the calling thread, which DuplicateHandle turns into a
// real one.
inline HANDLE GetCurrentThread() { return reinterpret_cast<HANDLE>(intptr_t(-2)); }
BOOL GetProcessAffinityMask(HANDLE process, DWORD_PTR* processMask, DWORD_PTR* systemMask);
SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION information, SIZE_T length);
