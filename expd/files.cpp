#include "files.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace expd {
namespace {

bool fail(std::string* error, const std::string& what) {
  *error = what + ": " + std::strerror(errno);
  return false;
}

bool sync_to_disk(int fd) {
#ifdef F_FULLFSYNC
  // On macOS, fsync() only hands the data to the drive, which can keep it in its own cache.
  // F_FULLFSYNC also flushes that cache. Fall back to fsync() where it isn't supported.
  if (fcntl(fd, F_FULLFSYNC) == 0) return true;
#endif
  return fsync(fd) == 0;
}

}  // namespace

bool write_file_atomically(const std::string& path, std::string_view data, mode_t mode,
                           std::string* error) {
  // Same directory as the target, because rename() can't cross volumes (EXDEV).
  std::string tmp = path + ".tmp";
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) return fail(error, "open " + tmp);
  fchmod(fd, mode);  // open() applied the umask

  size_t written = 0;
  while (written < data.size()) {
    ssize_t n = write(fd, data.data() + written, data.size() - written);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      int saved = errno;
      close(fd);
      unlink(tmp.c_str());
      errno = saved;
      return fail(error, "write " + tmp);
    }
    written += size_t(n);
  }

  // The data must reach the disk before the name points at it. Otherwise a crash can leave
  // `path` naming a file whose contents were never written.
  if (!sync_to_disk(fd)) {
    int saved = errno;
    close(fd);
    unlink(tmp.c_str());
    errno = saved;
    return fail(error, "fsync " + tmp);
  }
  close(fd);

  if (rename(tmp.c_str(), path.c_str()) != 0) {
    int saved = errno;
    unlink(tmp.c_str());
    errno = saved;
    return fail(error, "rename " + tmp);
  }

  // Sync the directory too, so the rename itself survives a crash.
  std::string dir = path.substr(0, path.find_last_of('/') + 1);
  int dir_fd = open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (dir_fd >= 0) {
    fsync(dir_fd);
    close(dir_fd);
  }
  return true;
}

bool read_file(const std::string& path, std::string* out) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  out->clear();
  char buf[4096];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      close(fd);
      return false;
    }
    if (n == 0) break;
    out->append(buf, size_t(n));
  }
  close(fd);
  return true;
}

}  // namespace expd
