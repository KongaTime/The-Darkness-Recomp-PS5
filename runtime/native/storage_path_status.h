#pragma once
#include <filesystem>
#include <system_error>
#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace DarkRecomp::Native::StorageDetail {
inline bool isSymlink(const std::filesystem::path& path, std::error_code& ec) {
#if defined(_WIN32)
    return std::filesystem::is_symlink(path, ec);
#else
    // The console refuses lstat even inside /download0. O_NOFOLLOW checks the
    // final component without following a link; O_NONBLOCK also handles FIFOs
    // without waiting for another process to open them.
    ec.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd >= 0) {
        ::close(fd);
        return false;
    }
    const int error = errno;
    // FreeBSD uses EMLINK for O_NOFOLLOW; Linux uses ELOOP.
    if (error == ELOOP || error == EMLINK) return true;
    ec = std::error_code(error, std::generic_category());
    return false;
#endif
}
}
