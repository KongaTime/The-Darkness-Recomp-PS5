#include "runtime/native/storage_path_status.h"
#include <cassert>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

int main() {
    namespace fs = std::filesystem;
    using DarkRecomp::Native::StorageDetail::isSymlink;
    char pattern[] = "/tmp/darkness-storage-XXXXXX";
    const char* root = mkdtemp(pattern);
    assert(root);
    const fs::path dir(root), file = dir / "metadata", link = dir / "link";
    int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0); close(fd);
    std::error_code ec;
    assert(!isSymlink(file, ec) && !ec);
    assert(!isSymlink(dir, ec) && !ec);
    assert(!isSymlink(dir / "absent", ec) && ec == std::errc::no_such_file_or_directory);
    assert(symlink(file.c_str(), link.c_str()) == 0);
    assert(isSymlink(link, ec) && !ec);
    assert(unlink(file.c_str()) == 0);
    assert(isSymlink(link, ec) && !ec); // Dangling links also remain rejected.
    assert(unlink(link.c_str()) == 0);
    const fs::path fifo = dir / "fifo";
    assert(mkfifo(fifo.c_str(), 0600) == 0);
    assert(!isSymlink(fifo, ec) && !ec);
    assert(unlink(fifo.c_str()) == 0 && rmdir(root) == 0);
    std::puts("Storage paths: regular files, directories, missing paths, symlinks and FIFOs pass.");
}
