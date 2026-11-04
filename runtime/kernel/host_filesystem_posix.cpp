#include "host_filesystem_posix.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace DarkRecomp::PosixFile {

// Win32 dispositions the guest passes: 1 = OPEN_EXISTING, 2 = CREATE_ALWAYS,
// anything else = OPEN_ALWAYS (matching Hook_NtCreateFile's mapping).
int Open(const std::string& path, bool write, uint32_t disposition) {
    int flags = write ? O_RDWR : O_RDONLY;
    if (disposition == 1) {
        // OPEN_EXISTING: no flags to add.
    } else if (disposition == 2) {
        flags |= O_CREAT | O_TRUNC;
    } else {
        flags |= O_CREAT;
    }
    return ::open(path.c_str(), flags, 0644);
}

int64_t ReadAt(int fd, void* buffer, uint32_t length, uint64_t offset) {
    return ::pread(fd, buffer, length, static_cast<off_t>(offset));
}

int64_t WriteAt(int fd, const void* buffer, uint32_t length, uint64_t offset) {
    return ::pwrite(fd, buffer, length, static_cast<off_t>(offset));
}

uint64_t Size(int fd) {
    struct stat st {};
    if (fstat(fd, &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_size);
}

uint64_t Tell(int fd) {
    off_t position = lseek(fd, 0, SEEK_CUR);
    return position < 0 ? 0 : static_cast<uint64_t>(position);
}

void Seek(int fd, uint64_t position) {
    lseek(fd, static_cast<off_t>(position), SEEK_SET);
}

void Close(int fd) {
    if (fd >= 0) ::close(fd);
}

} // namespace DarkRecomp::PosixFile
