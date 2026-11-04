#pragma once
#include <cstdint>
#include <string>

// POSIX file handle helpers for the PS5 build of host_filesystem.cpp.
// They mirror the Win32 calls the Windows build makes, so the Hook_Nt*
// implementations keep the same guest-visible behavior.
namespace DarkRecomp::PosixFile {

// Open with the Win32 disposition semantics the guest requests.
// Returns -1 on failure.
int Open(const std::string& path, bool write, uint32_t disposition);

// Positioned read/write (the equivalent of OVERLAPPED offsets).
// Return the byte count, or -1 on error.
int64_t ReadAt(int fd, void* buffer, uint32_t length, uint64_t offset);
int64_t WriteAt(int fd, const void* buffer, uint32_t length, uint64_t offset);

// File size in bytes, or 0 when unknown.
uint64_t Size(int fd);

// Current file position, and a seek to an absolute position (the sequential
// read path uses these where Win32 tracks the position on the handle).
uint64_t Tell(int fd);
void Seek(int fd, uint64_t position);

void Close(int fd);

} // namespace DarkRecomp::PosixFile
