#include "host_filesystem.h"
#include <iostream>
#include <unordered_map>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <mutex>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <cctype>
#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#else
#include "host_filesystem_posix.h"
#include <mutex>
#include <cstdint>
#include "runtime/guest/guest_types.h"
// PS5: the MSVC byteswap intrinsics and SEH the code below uses.
#define _byteswap_ulong(x) __builtin_bswap32(x)
#define _byteswap_ushort(x) __builtin_bswap16(x)
#define _byteswap_uint64(x) __builtin_bswap64(x)
#define __try if (true)
#define __except(x) if (false)
#define EXCEPTION_EXECUTE_HANDLER 1
#define DWORD uint32_t
#define BOOL int
#define TRUE 1
#define FALSE 0
#define ERROR_HANDLE_EOF 38
#define FILE_BEGIN 0
#define FILE_CURRENT 1
struct OVERLAPPED { uint32_t Offset; uint32_t OffsetHigh; };
struct LARGE_INTEGER { int64_t QuadPart; };
#define GetLastError() (errno)
#define CRITICAL_SECTION int
#include <sys/stat.h>
#include <unistd.h>
struct WIN32_FILE_ATTRIBUTE_DATA { uint32_t dwFileAttributes; uint32_t nFileSizeHigh; uint32_t nFileSizeLow; };
#define GetFileExInfoStandard 0
// PS5: Win32 file calls mapped onto the POSIX fd helpers in host_filesystem_posix.h.
inline int ReadFile(int fd, void* buf, uint32_t len, uint32_t* read, OVERLAPPED* ov) {
    uint64_t off = ov ? (((uint64_t)ov->OffsetHigh << 32) | ov->Offset) : DarkRecomp::PosixFile::Tell(fd);
    int64_t n = DarkRecomp::PosixFile::ReadAt(fd, buf, len, off);
    if (n < 0) { *read = 0; return 0; }
    *read = (uint32_t)n;
    if (!ov) DarkRecomp::PosixFile::Seek(fd, off + (uint64_t)n);
    return 1;
}
inline int WriteFile(int fd, const void* buf, uint32_t len, uint32_t* written, OVERLAPPED* ov) {
    uint64_t off = ov ? (((uint64_t)ov->OffsetHigh << 32) | ov->Offset) : DarkRecomp::PosixFile::Tell(fd);
    int64_t n = DarkRecomp::PosixFile::WriteAt(fd, buf, len, off);
    if (n < 0) { *written = 0; return 0; }
    *written = (uint32_t)n;
    if (!ov) DarkRecomp::PosixFile::Seek(fd, off + (uint64_t)n);
    return 1;
}
inline int CloseHandle(int fd) { DarkRecomp::PosixFile::Close(fd); return 1; }
inline int FlushFileBuffers(int fd) { return fsync(fd) == 0; }
inline int SetFilePointerEx(int fd, LARGE_INTEGER pos, LARGE_INTEGER* out, uint32_t) {
    DarkRecomp::PosixFile::Seek(fd, (uint64_t)pos.QuadPart);
    if (out) out->QuadPart = (int64_t)pos.QuadPart;
    return 1;
}
inline int GetFileSizeEx(int fd, LARGE_INTEGER* sz) { sz->QuadPart = (int64_t)DarkRecomp::PosixFile::Size(fd); return 1; }
inline int GetFileAttributesExA(const char* path, int, WIN32_FILE_ATTRIBUTE_DATA* fad) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    uint64_t size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    fad->dwFileAttributes = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
    fad->nFileSizeHigh = (uint32_t)(size >> 32);
    fad->nFileSizeLow = (uint32_t)size;
    return 1;
}
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define OPEN_ALWAYS 4
#define OPEN_EXISTING 3
#define CREATE_ALWAYS 2
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <cctype>
#include <sys/types.h>
#include <unistd.h>
#endif
#include <iostream>
#include <unordered_map>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <mutex>
#include <string>

namespace DarkRecomp {

#if defined(_WIN32)
using FileHandle = HANDLE;
constexpr FileHandle kInvalidFile = INVALID_HANDLE_VALUE;
#else
using FileHandle = int;
constexpr FileHandle kInvalidFile = -1;
#endif

struct HostFile {
    FileHandle hFile;
    uint64_t fileSize;
    std::string path;
};

static std::unordered_map<uint32_t, HostFile> g_openFiles;
static uint32_t g_nextHandle = 0x1000;
#if defined(_WIN32)
static CRITICAL_SECTION g_fsLock;
#else
// PS5: the Win32 critical-section calls below map onto a recursive mutex.
static std::recursive_mutex g_fsLockImpl;
#define InitializeCriticalSection(cs) ((void)0)
#define EnterCriticalSection(cs) g_fsLockImpl.lock()
#define LeaveCriticalSection(cs) g_fsLockImpl.unlock()
#define g_fsLock g_fsLockImpl
#endif

void InitializeFileSystem() {
    InitializeCriticalSection(&g_fsLock);
    std::cerr << "[FS] Initialized native Xbox 360 File System Bridge." << std::endl;
}

static std::string DecodeGuestUnicodeString(uint32_t bufferPtr, uint16_t byteLen, uint8_t* base) {
    // Xbox 360 guest memory is big-endian: UTF-16 code units are stored BE.
    // Convert to UTF-8/ANSI (game paths are ASCII subset).
    if (!bufferPtr || byteLen == 0) return "";
    // Clamp to sane length to avoid OOB on corrupt guest pointers.
    if (byteLen > 4096) byteLen = 4096;
    uint16_t charCount = byteLen / 2;
    std::string out;
    out.reserve(charCount);
    uint8_t* buf = base + bufferPtr;
    for (uint16_t i = 0; i < charCount; i++) {
        uint16_t be = (static_cast<uint16_t>(buf[i * 2]) << 8) | buf[i * 2 + 1];
        // Fast path: ASCII subset
        if (be < 0x80) {
            out.push_back(static_cast<char>(be));
        } else if (be < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (be >> 6)));
            out.push_back(static_cast<char>(0x80 | (be & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (be >> 12)));
            out.push_back(static_cast<char>(0x80 | ((be >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (be & 0x3F)));
        }
    }
    return out;
}

static std::string DecodeGuestAnsiString(uint32_t bufferPtr, uint16_t byteLen, uint8_t* base) {
    if (!bufferPtr || byteLen == 0) return "";
    if (byteLen > 4096) byteLen = 4096;
    const char* s = reinterpret_cast<const char*>(base + bufferPtr);
    size_t n = 0;
    while (n < byteLen && s[n] != '\0') n++;
    return std::string(s, n);
}

static std::string ExtractGuestPath(uint32_t objectAttributesPtr, uint8_t* base) {
    if (!objectAttributesPtr) return "";
    uint8_t* pAttr = base + objectAttributesPtr;
    // Primary: OBJECT_ATTRIBUTES (32-bit guest):
    //   +0x00 ULONG Length
    //   +0x04 HANDLE RootDirectory
    //   +0x08 PUNICODE_STRING ObjectName
    //   +0x0C ULONG Attributes
    uint32_t namePtr = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pAttr + 8));
    if (namePtr) {
        uint8_t* pName = base + namePtr;
        // UNICODE_STRING (32-bit guest):
        //   +0x00 USHORT Length (bytes, BE)
        //   +0x02 USHORT MaximumLength (BE)
        //   +0x04 PWSTR Buffer (BE)
        uint16_t length = _byteswap_ushort(*reinterpret_cast<uint16_t*>(pName + 0));
        uint32_t bufferPtr = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pName + 4));
        if (bufferPtr && length != 0) {
            std::string uni = DecodeGuestUnicodeString(bufferPtr, length, base);
            if (!uni.empty()) return uni;
        }
    }
    // Fallback: LIBCMT CRT open struct used by sub_828ABF60-family callers
    // (observed: [-3, ANSI_STRING*, 64] at PATTR). The import thunk at
    // 0x829C6054/0x829C60B4 funnels these here. Read ANSI_STRING at +4:
    //   +0x00 USHORT Length (bytes, BE, ANSI chars)
    //   +0x02 USHORT MaximumLength (BE)
    //   +0x04 PCHAR Buffer (BE, guest ANSI bytes)
    __try {
        uint32_t ansiPtr = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pAttr + 4));
        if (ansiPtr) {
            uint8_t* pA = base + ansiPtr;
            uint16_t alen = _byteswap_ushort(*reinterpret_cast<uint16_t*>(pA + 0));
            uint32_t abuf = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pA + 4));
            if (abuf && alen != 0) {
                return DecodeGuestAnsiString(abuf, alen, base);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return "";
}

// Where the game's files are. Relative to the working directory unless the
// title says otherwise: the console starts a title in /, not in its own folder.
static std::filesystem::path g_gameDirectory = "Darkness";

// Finds root/relative with each component matched without regard to case.
static bool FindIgnoringCase(const std::filesystem::path& root, const std::string& relative,
                             std::filesystem::path& found) {
    auto lower = [](std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    std::filesystem::path current = root;
    size_t start = 0;
    while (start <= relative.size()) {
        size_t end = relative.find('/', start);
        if (end == std::string::npos) end = relative.size();
        const std::string component = relative.substr(start, end - start);
        start = end + 1;
        if (component.empty() || component == ".") continue;
        std::error_code ec;
        if (std::filesystem::exists(current / component, ec)) {
            current /= component;
            continue;
        }
        const std::string wanted = lower(component);
        bool matched = false;
        for (std::filesystem::directory_iterator it(current, ec), last; !ec && it != last; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (lower(name) == wanted) {
                current /= name;
                matched = true;
                break;
            }
        }
        if (!matched) return false;
    }
    found = current;
    return true;
}

static std::string ResolveGuestPath(const std::string& raw) {
    std::string p = raw;
    // Normalize separators first so prefix matching is stable.
    for (char& c : p) {
        if (c == '\\') c = '/';
    }

    const char* prefixes[] = {
        "/Device/Cdrom0/",
        "Device/Cdrom0/",
        "/Device/Dvd0/",
        "Device/Dvd0/",
        "/Device/Harddisk0/Partition1/",
        "Device/Harddisk0/Partition1/",
        "game:/",
        "game:/",
        "d:/",
        "D:/",
        "devkit:/",
        "e:/",
        nullptr
    };

    for (int i = 0; prefixes[i]; i++) {
        size_t pos = p.find(prefixes[i]);
        if (pos != std::string::npos) {
            p = p.substr(pos + strlen(prefixes[i]));
            break;
        }
    }
    // Also strip a bare "game:" / "d:" style prefix without slash.
    if (p.size() > 5 && (p.compare(0, 5, "game:") == 0 || p.compare(0, 2, "d:") == 0)) {
        size_t colon = p.find(':');
        if (colon != std::string::npos) p = p.substr(colon + 1);
    }

    while (!p.empty() && (p[0] == '/' || p[0] == '\\' || p[0] == ':')) {
        p.erase(0, 1);
    }

    // Try candidates in the game directory, as spelled and then ignoring case:
    // the guest's names are the disc's case-insensitive ones, and the
    // console's file system is case-sensitive.
    const std::filesystem::path roots[] = {
        g_gameDirectory,
        g_gameDirectory / "Content",
        g_gameDirectory / "Content_Eng",
    };
    for (const auto& root : roots) {
        std::error_code ec;
        if (std::filesystem::exists(root / p, ec)) {
            return (root / p).string();
        }
    }
    for (const auto& root : roots) {
        std::filesystem::path found;
        if (FindIgnoringCase(root, p, found)) {
            return found.string();
        }
    }

    // A read of a file that is not there is what the guest reports as a
    // dirty disc, so name it (the first few) for the console's log.
    static int missingLogged = 0;
    if (missingLogged < 32) {
        ++missingLogged;
        std::fprintf(stderr, "[FS] not found: %s (guest %s)\n", p.c_str(), raw.c_str());
        std::fflush(stderr);
    }

    // Return default target even if not yet created (for write/open)
    return (g_gameDirectory / p).string();
}

void SetGameDirectory(const std::filesystem::path& directory) {
    g_gameDirectory = directory;
}

void Hook_NtOpenFile(PPCContext& ctx, uint8_t* base) {
    uint32_t pHandle = ctx.r3.u32;
    uint32_t access = ctx.r4.u32;
    uint32_t pAttr = ctx.r5.u32;
    uint32_t pStatus = ctx.r6.u32;

    std::string rawPath = ExtractGuestPath(pAttr, base);
    std::string resolved = ResolveGuestPath(rawPath);

    if (rawPath.empty()) {
        // Diagnostic: dump OBJECT_ATTRIBUTES + UNICODE_STRING fields so we can
        // see why the path is empty (RootDirectory-relative open? bad offset?).
        // Also dump all PPC arg regs to verify Xbox arg order.
        __try {
            uint8_t* pA = base + pAttr;
            uint32_t lenA = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pA + 0));
            uint32_t root = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pA + 4));
            uint32_t nameP = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pA + 8));
            uint32_t attr = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pA + 12));
            std::cerr << "[FS] NtOpenFile empty path diag: pAttr=0x" << std::hex << pAttr
                      << " Len=0x" << lenA << " Root=0x" << root << " NamePtr=0x" << nameP
                      << " Attr=0x" << attr
                      << " | regs r3=0x" << ctx.r3.u32 << " r4=0x" << ctx.r4.u32
                      << " r5=0x" << ctx.r5.u32 << " r6=0x" << ctx.r6.u32
                      << " r7=0x" << ctx.r7.u32 << " r8=0x" << ctx.r8.u32
                      << " r9=0x" << ctx.r9.u32 << " r10=0x" << ctx.r10.u32
                      << " | mem:";
            for (int i = 0; i < 8; i++) {
                uint32_t w = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pA + i * 4));
                char tmp[16]; snprintf(tmp, sizeof(tmp), " [%d]=0x%x", i, w);
                std::cerr << tmp;
            }
            if (nameP) {
                uint8_t* pN = base + nameP;
                uint16_t ulen = _byteswap_ushort(*reinterpret_cast<uint16_t*>(pN + 0));
                uint16_t umax = _byteswap_ushort(*reinterpret_cast<uint16_t*>(pN + 2));
                uint32_t ubuf = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pN + 4));
                std::cerr << " ULen=" << std::dec << ulen << " UMax=" << umax
                          << " UBuf=0x" << std::hex << ubuf;
                if (ubuf) {
                    std::cerr << " UBytes:";
                    for (int i = 0; i < 16; i++) {
                        char tmp[8]; snprintf(tmp, sizeof(tmp), " %02X", (base + ubuf)[i]);
                        std::cerr << tmp;
                    }
                }
            }
            std::cerr << std::dec << std::endl;
#if defined(_WIN32)
            // First empty-path call: dump host stack once to identify the PPC
            // caller (which wrapper builds this struct layout).
            {
                static uint64_t s_fsStackLogged = 0;
                static std::mutex s_fsStackMutex;
                std::lock_guard<std::mutex> lk(s_fsStackMutex);
                if (++s_fsStackLogged == 1) {
                    HANDLE hProcess = GetCurrentProcess();
                    HANDLE hThread = GetCurrentThread();
                    SymInitialize(hProcess, nullptr, TRUE);
                    CONTEXT hc = {};
                    RtlCaptureContext(&hc);
                    STACKFRAME64 sf = {};
                    sf.AddrPC.Offset = hc.Rip;
                    sf.AddrPC.Mode = AddrModeFlat;
                    sf.AddrFrame.Offset = hc.Rbp;
                    sf.AddrFrame.Mode = AddrModeFlat;
                    sf.AddrStack.Offset = hc.Rsp;
                    sf.AddrStack.Mode = AddrModeFlat;
                    std::cerr << "[FS] NtOpenFile caller stack:" << std::endl;
                    for (int frame = 0; frame < 10; frame++) {
                        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProcess, hThread, &sf, &hc,
                                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                            break;
                        if (sf.AddrPC.Offset == 0) break;
                        char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
                        PSYMBOL_INFO pSym = reinterpret_cast<PSYMBOL_INFO>(symBuf);
                        pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
                        pSym->MaxNameLen = 256;
                        DWORD64 disp = 0;
                        std::string nm;
                        if (SymFromAddr(hProcess, sf.AddrPC.Offset, &disp, pSym)) nm = pSym->Name;
                        else { char tmp[32]; snprintf(tmp, sizeof(tmp), "0x%llX", (unsigned long long)sf.AddrPC.Offset); nm = tmp; }
                        std::cerr << "   [F#" << frame << "] " << nm << "+0x" << std::hex << disp << std::dec << std::endl;
                    }
                    std::cerr.flush();
                }
            }
#endif
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::cerr << "[FS] NtOpenFile empty path diag: exception reading guest structs" << std::endl;
        }
    }

    DWORD desiredAccess = GENERIC_READ;
    if (access & 0x0002) desiredAccess |= GENERIC_WRITE;

#if defined(_WIN32)
    HANDLE h = CreateFileA(
        resolved.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
#else
    FileHandle h = PosixFile::Open(resolved, (access & 0x0002) != 0, 1);
#endif

    if (h == kInvalidFile) {
        std::cerr << "[FS] NtOpenFile FAILED: \"" << rawPath << "\" -> \"" << resolved << "\"" << std::endl;
        if (pStatus) {
            *reinterpret_cast<uint32_t*>(base + pStatus + 0) = _byteswap_ulong(0xC0000034); // STATUS_OBJECT_NAME_NOT_FOUND
            *reinterpret_cast<uint32_t*>(base + pStatus + 4) = 0;
        }
        ctx.r3.u64 = 0xC0000034;
        return;
    }

    uint64_t fileSize = 0;
#if defined(_WIN32)
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    fileSize = (uint64_t)sz.QuadPart;
#else
    fileSize = PosixFile::Size(h);
#endif

    EnterCriticalSection(&g_fsLock);
    uint32_t guestHandle = g_nextHandle++;
    g_openFiles[guestHandle] = { h, fileSize, resolved };
    LeaveCriticalSection(&g_fsLock);

    std::cerr << "[FS] NtOpenFile: \"" << rawPath << "\" -> \"" << resolved << "\" (Handle=0x"
              << std::hex << guestHandle << ", Size=" << std::dec << fileSize << " bytes)" << std::endl;

    if (pHandle) {
        *reinterpret_cast<uint32_t*>(base + pHandle) = _byteswap_ulong(guestHandle);
    }
    if (pStatus) {
        *reinterpret_cast<uint32_t*>(base + pStatus + 0) = 0; // STATUS_SUCCESS
        *reinterpret_cast<uint32_t*>(base + pStatus + 4) = _byteswap_ulong(1); // FILE_OPENED
    }
    ctx.r3.u64 = 0;
}

void Hook_NtCreateFile(PPCContext& ctx, uint8_t* base) {
    uint32_t pHandle = ctx.r3.u32;
    uint32_t access = ctx.r4.u32;
    uint32_t pAttr = ctx.r5.u32;
    uint32_t pStatus = ctx.r6.u32;
    uint32_t disposition = ctx.r10.u32;

    std::string rawPath = ExtractGuestPath(pAttr, base);
    std::string resolved = ResolveGuestPath(rawPath);

    DWORD dwDisposition = OPEN_ALWAYS;
    if (disposition == 1) dwDisposition = OPEN_EXISTING;
    else if (disposition == 2) dwDisposition = CREATE_ALWAYS;

    DWORD desiredAccess = GENERIC_READ;
    if (access & 0x0002) desiredAccess |= GENERIC_WRITE;

#if defined(_WIN32)
    HANDLE h = CreateFileA(
        resolved.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        dwDisposition,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
#else
    FileHandle h = PosixFile::Open(resolved, (access & 0x0002) != 0, disposition);
#endif

    if (h == kInvalidFile) {
        std::cerr << "[FS] NtCreateFile FAILED: \"" << rawPath << "\" -> \"" << resolved << "\"" << std::endl;
        if (pStatus) {
            *reinterpret_cast<uint32_t*>(base + pStatus + 0) = _byteswap_ulong(0xC0000034);
            *reinterpret_cast<uint32_t*>(base + pStatus + 4) = 0;
        }
        ctx.r3.u64 = 0xC0000034;
        return;
    }

    uint64_t fileSize = 0;
#if defined(_WIN32)
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    fileSize = (uint64_t)sz.QuadPart;
#else
    fileSize = PosixFile::Size(h);
#endif

    EnterCriticalSection(&g_fsLock);
    uint32_t guestHandle = g_nextHandle++;
    g_openFiles[guestHandle] = { h, fileSize, resolved };
    LeaveCriticalSection(&g_fsLock);

    std::cerr << "[FS] NtCreateFile: \"" << rawPath << "\" -> \"" << resolved << "\" (Handle=0x"
              << std::hex << guestHandle << ", Size=" << std::dec << fileSize << " bytes)" << std::endl;

    if (pHandle) {
        *reinterpret_cast<uint32_t*>(base + pHandle) = _byteswap_ulong(guestHandle);
    }
    if (pStatus) {
        *reinterpret_cast<uint32_t*>(base + pStatus + 0) = 0;
        *reinterpret_cast<uint32_t*>(base + pStatus + 4) = _byteswap_ulong(1);
    }
    ctx.r3.u64 = 0;
}

void Hook_NtReadFile(PPCContext& ctx, uint8_t* base) {
    uint32_t handle = ctx.r3.u32;
    uint32_t pStatus = ctx.r7.u32;
    uint32_t pBuffer = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    uint32_t pOffset = ctx.r10.u32;

    EnterCriticalSection(&g_fsLock);
    auto it = g_openFiles.find(handle);
    if (it == g_openFiles.end()) {
        LeaveCriticalSection(&g_fsLock);
        if (pStatus) {
            *reinterpret_cast<uint32_t*>(base + pStatus + 0) = _byteswap_ulong(0xC0000008); // STATUS_INVALID_HANDLE
            *reinterpret_cast<uint32_t*>(base + pStatus + 4) = 0;
        }
        ctx.r3.u64 = 0xC0000008;
        return;
    }
    HostFile file = it->second;
    LeaveCriticalSection(&g_fsLock);

    DWORD bytesRead = 0;
    BOOL ok = FALSE;

#if defined(_WIN32)
    if (pOffset != 0) {
        uint64_t offset = _byteswap_uint64(*reinterpret_cast<uint64_t*>(base + pOffset));
        OVERLAPPED ov = {};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        ok = ReadFile(file.hFile, base + pBuffer, length, &bytesRead, &ov);
    } else {
        ok = ReadFile(file.hFile, base + pBuffer, length, &bytesRead, nullptr);
    }
#else
    // PS5: positioned read. Without an offset, or with NT's "use the file
    // pointer" offset (-2; any negative one), the guest reads at the file's
    // current position, which the handle tracks on this side.
    const int64_t requested = pOffset != 0
        ? static_cast<int64_t>(_byteswap_uint64(*reinterpret_cast<uint64_t*>(base + pOffset)))
        : -2;
    const bool atPointer = requested < 0;
    const uint64_t offset = atPointer ? PosixFile::Tell(file.hFile) : static_cast<uint64_t>(requested);
    const int64_t got = PosixFile::ReadAt(file.hFile, base + pBuffer, length, offset);
    if (got < 0) {
        // Report the failure instead of an empty success: a loader reading a
        // count from a failed read loops on whatever was in its buffer.
        const int error = errno;
        static int failuresLogged = 0;
        if (failuresLogged < 32) {
            ++failuresLogged;
            std::fprintf(stderr, "[FS] read failed: \"%s\" offset=%llu len=%u errno=%d\n", file.path.c_str(),
                         static_cast<unsigned long long>(offset), length, error);
        }
        if (pStatus) {
            *reinterpret_cast<uint32_t*>(base + pStatus + 0) = _byteswap_ulong(0xC0000185); // STATUS_IO_DEVICE_ERROR
            *reinterpret_cast<uint32_t*>(base + pStatus + 4) = 0;
        }
        ctx.r3.u64 = 0xC0000185;
        return;
    }
    bytesRead = static_cast<DWORD>(got);
    if (atPointer) PosixFile::Seek(file.hFile, offset + bytesRead);
    if (bytesRead == 0 && length != 0) {
        // NtReadFile at or past the end of the file: STATUS_END_OF_FILE, which
        // is how the guest's readers stop.
        if (pStatus) {
            *reinterpret_cast<uint32_t*>(base + pStatus + 0) = _byteswap_ulong(0xC0000011);
            *reinterpret_cast<uint32_t*>(base + pStatus + 4) = 0;
        }
        ctx.r3.u64 = 0xC0000011;
        return;
    }
    ok = TRUE;
#endif

    if (!ok && bytesRead == 0) {
        DWORD err = GetLastError();
        if (err == ERROR_HANDLE_EOF) {
            if (pStatus) {
                *reinterpret_cast<uint32_t*>(base + pStatus + 0) = _byteswap_ulong(0xC0000011); // STATUS_END_OF_FILE
                *reinterpret_cast<uint32_t*>(base + pStatus + 4) = 0;
            }
            ctx.r3.u64 = 0xC0000011;
            return;
        }
    }

    if (pStatus) {
        *reinterpret_cast<uint32_t*>(base + pStatus + 0) = 0; // STATUS_SUCCESS
        *reinterpret_cast<uint32_t*>(base + pStatus + 4) = _byteswap_ulong(bytesRead);
    }
    {
        static uint64_t s_readLogged = 0;
        static CRITICAL_SECTION s_readLock;
        static bool s_readLockInit = false;
        if (!s_readLockInit) { InitializeCriticalSection(&s_readLock); s_readLockInit = true; }
        EnterCriticalSection(&s_readLock);
        bool log = (++s_readLogged <= 3);
        LeaveCriticalSection(&s_readLock);
        if (log) {
            EnterCriticalSection(&g_fsLock);
            std::string nm = "?";
            auto it2 = g_openFiles.find(handle);
            if (it2 != g_openFiles.end()) nm = it2->second.path;
            LeaveCriticalSection(&g_fsLock);
            std::cerr << "[FS] NtReadFile # " << s_readLogged << " handle=0x" << std::hex << handle
                      << " guestBuf=0x" << pBuffer << std::dec << " len=" << length
                      << " got=" << bytesRead << " \"" << nm << "\"" << std::endl;
            std::cerr.flush();
        }
    }
    ctx.r3.u64 = 0;
}

void Hook_NtReadFileScatter(PPCContext& ctx, uint8_t* base) {
    uint32_t handle = ctx.r3.u32;
    uint32_t pStatus = ctx.r7.u32;
    uint32_t pSegmentArray = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    uint32_t pOffset = ctx.r10.u32;

    EnterCriticalSection(&g_fsLock);
    auto it = g_openFiles.find(handle);
    if (it == g_openFiles.end()) {
        LeaveCriticalSection(&g_fsLock);
        ctx.r3.u64 = 0xC0000008;
        return;
    }
    HostFile file = it->second;
    LeaveCriticalSection(&g_fsLock);

    uint64_t currentOffset = 0;
    if (pOffset) {
        currentOffset = _byteswap_uint64(*reinterpret_cast<uint64_t*>(base + pOffset));
    }

    uint32_t bytesRemaining = length;
    uint32_t segmentIdx = 0;
    uint32_t totalBytesRead = 0;

    while (bytesRemaining > 0) {
        uint32_t pageAddr = _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + pSegmentArray + (segmentIdx * 8)));
        if (!pageAddr) break;

        DWORD chunk = (std::min)(bytesRemaining, 4096u);
        DWORD readThisChunk = 0;
        OVERLAPPED ov = {};
        ov.Offset = static_cast<DWORD>(currentOffset);
        ov.OffsetHigh = static_cast<DWORD>(currentOffset >> 32);

        BOOL ok = ReadFile(file.hFile, base + pageAddr, chunk, &readThisChunk, &ov);
        if (!ok || readThisChunk == 0) break;

        totalBytesRead += readThisChunk;
        currentOffset += readThisChunk;
        bytesRemaining -= readThisChunk;
        segmentIdx++;
    }

    if (pStatus) {
        *reinterpret_cast<uint32_t*>(base + pStatus + 0) = 0;
        *reinterpret_cast<uint32_t*>(base + pStatus + 4) = _byteswap_ulong(totalBytesRead);
    }
    ctx.r3.u64 = 0;
}

void Hook_NtWriteFile(PPCContext& ctx, uint8_t* base) {
    uint32_t handle = ctx.r3.u32;
    uint32_t pStatus = ctx.r7.u32;
    uint32_t pBuffer = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    uint32_t pOffset = ctx.r10.u32;

    EnterCriticalSection(&g_fsLock);
    auto it = g_openFiles.find(handle);
    if (it == g_openFiles.end()) {
        LeaveCriticalSection(&g_fsLock);
        ctx.r3.u64 = 0xC0000008;
        return;
    }
    HostFile file = it->second;
    LeaveCriticalSection(&g_fsLock);

    DWORD bytesWritten = 0;
    BOOL ok = FALSE;
    if (pOffset) {
        uint64_t offset = _byteswap_uint64(*reinterpret_cast<uint64_t*>(base + pOffset));
        OVERLAPPED ov = {};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        ok = WriteFile(file.hFile, base + pBuffer, length, &bytesWritten, &ov);
    } else {
        ok = WriteFile(file.hFile, base + pBuffer, length, &bytesWritten, nullptr);
    }

    if (pStatus) {
        *reinterpret_cast<uint32_t*>(base + pStatus + 0) = ok ? 0 : 0xC0000001;
        *reinterpret_cast<uint32_t*>(base + pStatus + 4) = _byteswap_ulong(bytesWritten);
    }
    ctx.r3.u64 = ok ? 0 : 0xC0000001;
}

void Hook_NtClose(PPCContext& ctx, uint8_t* base) {
    uint32_t handle = ctx.r3.u32;
    EnterCriticalSection(&g_fsLock);
    auto it = g_openFiles.find(handle);
    if (it != g_openFiles.end()) {
        CloseHandle(it->second.hFile);
        g_openFiles.erase(it);
    }
    LeaveCriticalSection(&g_fsLock);
    ctx.r3.u64 = 0;
}

void Hook_NtSetInformationFile(PPCContext& ctx, uint8_t* base) {
    uint32_t handle = ctx.r3.u32;
    uint32_t pStatus = ctx.r4.u32;
    uint32_t pInfo = ctx.r5.u32;
    uint32_t length = ctx.r6.u32;
    uint32_t infoClass = ctx.r7.u32;

    EnterCriticalSection(&g_fsLock);
    auto it = g_openFiles.find(handle);
    if (it == g_openFiles.end()) {
        LeaveCriticalSection(&g_fsLock);
        ctx.r3.u64 = 0xC0000008;
        return;
    }
    HostFile file = it->second;
    LeaveCriticalSection(&g_fsLock);

    // Class 14: FilePositionInformation (NT's numbering; 4 is the basic
    // information, which carries no position).
    uint32_t status = 0;
    if (infoClass == 14 && length >= 8 && pInfo) {
        uint64_t newPos = _byteswap_uint64(*reinterpret_cast<uint64_t*>(base + pInfo));
        LARGE_INTEGER li;
        li.QuadPart = newPos;
        SetFilePointerEx(file.hFile, li, nullptr, FILE_BEGIN);
    } else if (infoClass != 13 && infoClass != 19 && infoClass != 20) {
        // 13 (disposition), 19 (allocation) and 20 (end of file) are the save
        // path's; they have no effect on the read-only game folder.
        status = 0xC0000003; // STATUS_INVALID_INFO_CLASS
        std::fprintf(stderr, "[FS] set information class %u not supported (\"%s\")\n", infoClass,
                     file.path.c_str());
    }

    if (pStatus) {
        *reinterpret_cast<uint32_t*>(base + pStatus + 0) = _byteswap_ulong(status);
        *reinterpret_cast<uint32_t*>(base + pStatus + 4) = 0;
    }
    ctx.r3.u64 = status;
}

void Hook_NtQueryInformationFile(PPCContext& ctx, uint8_t* base) {
    uint32_t handle = ctx.r3.u32;
    uint32_t pStatus = ctx.r4.u32;
    uint32_t pInfo = ctx.r5.u32;
    uint32_t length = ctx.r6.u32;
    uint32_t infoClass = ctx.r7.u32;

    EnterCriticalSection(&g_fsLock);
    auto it = g_openFiles.find(handle);
    if (it == g_openFiles.end()) {
        LeaveCriticalSection(&g_fsLock);
        ctx.r3.u64 = 0xC0000008;
        return;
    }
    HostFile file = it->second;
    LeaveCriticalSection(&g_fsLock);

    // NT's information classes, with the layouts runtime/native/filesystem.cpp
    // writes. This bridge used to answer 4 as the position (4 is the basic
    // information; the position is 14) and every other class with an empty
    // success, so a reader asking for the size read whatever was in its buffer.
    LARGE_INTEGER sz;
    GetFileSizeEx(file.hFile, &sz);
    const uint64_t size = static_cast<uint64_t>(sz.QuadPart);
#if defined(_WIN32)
    LARGE_INTEGER zero = {}, cur = {};
    SetFilePointerEx(file.hFile, zero, &cur, FILE_CURRENT);
    const uint64_t position = static_cast<uint64_t>(cur.QuadPart);
#else
    const uint64_t position = PosixFile::Tell(file.hFile);
#endif
    constexpr uint32_t kNormal = 0x80; // FILE_ATTRIBUTE_NORMAL
    uint8_t* out = pInfo ? base + pInfo : nullptr;
    auto put64 = [&](uint32_t offset, uint64_t value) {
        *reinterpret_cast<uint64_t*>(out + offset) = _byteswap_uint64(value);
    };
    auto put32 = [&](uint32_t offset, uint32_t value) {
        *reinterpret_cast<uint32_t*>(out + offset) = _byteswap_ulong(value);
    };

    uint32_t needed = 0;
    switch (infoClass) {
        case 4: needed = 40; break;  // FileBasicInformation
        case 5: needed = 24; break;  // FileStandardInformation
        case 6: needed = 8; break;   // FileInternalInformation
        case 14: needed = 8; break;  // FilePositionInformation
        case 16: needed = 4; break;  // FileModeInformation
        case 17: needed = 4; break;  // FileAlignmentInformation
        case 34: needed = 56; break; // FileNetworkOpenInformation
        default: break;
    }
    uint32_t status = 0;
    if (needed == 0) {
        status = 0xC0000003; // STATUS_INVALID_INFO_CLASS
        std::fprintf(stderr, "[FS] query information class %u not supported (\"%s\")\n", infoClass,
                     file.path.c_str());
    } else if (!out || length < needed) {
        status = 0xC0000004; // STATUS_INFO_LENGTH_MISMATCH
    } else {
        memset(out, 0, needed);
        switch (infoClass) {
            case 4: put32(32, kNormal); break;
            case 5:
                put64(0, size); // AllocationSize
                put64(8, size); // EndOfFile
                put32(16, 1);   // NumberOfLinks
                break;
            case 6: put64(0, handle); break;
            case 14: put64(0, position); break;
            case 16: break;
            case 17: break;
            case 34:
                put64(32, size); // AllocationSize
                put64(40, size); // EndOfFile
                put32(48, kNormal);
                break;
        }
    }

    if (pStatus) {
        *reinterpret_cast<uint32_t*>(base + pStatus + 0) = _byteswap_ulong(status);
        *reinterpret_cast<uint32_t*>(base + pStatus + 4) = _byteswap_ulong(status == 0 ? needed : 0);
    }
    ctx.r3.u64 = status;
}

void Hook_NtQueryFullAttributesFile(PPCContext& ctx, uint8_t* base) {
    uint32_t pAttr = ctx.r3.u32;
    uint32_t pInfo = ctx.r4.u32;

    std::string rawPath = ExtractGuestPath(pAttr, base);
    std::string resolved = ResolveGuestPath(rawPath);

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(resolved.c_str(), GetFileExInfoStandard, &fad)) {
        ctx.r3.u64 = 0xC0000034; // STATUS_OBJECT_NAME_NOT_FOUND
        return;
    }

    if (pInfo) {
        uint8_t* out = base + pInfo;
        memset(out, 0, 0x34);
        uint64_t sz = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        *reinterpret_cast<uint64_t*>(out + 0x20) = _byteswap_uint64(sz); // AllocationSize
        *reinterpret_cast<uint64_t*>(out + 0x28) = _byteswap_uint64(sz); // EndOfFile
        *reinterpret_cast<uint32_t*>(out + 0x30) = _byteswap_ulong(fad.dwFileAttributes);
    }
    ctx.r3.u64 = 0;
}

void Hook_NtFlushBuffersFile(PPCContext& ctx, uint8_t* base) {
    uint32_t handle = ctx.r3.u32;
    EnterCriticalSection(&g_fsLock);
    auto it = g_openFiles.find(handle);
    if (it != g_openFiles.end()) {
        FlushFileBuffers(it->second.hFile);
    }
    LeaveCriticalSection(&g_fsLock);
    ctx.r3.u64 = 0;
}

} // namespace DarkRecomp
