#include "vtpc.h"

#include "vtpc_impl.h"

int vtpc_open(const char* path, int mode, int access) {
  return vtpc_impl_open(path, mode, access);
}

int vtpc_close(int fd) {
  return vtpc_impl_close(fd);
}

ssize_t vtpc_read(int fd, void* buf, size_t count) {
  return vtpc_impl_read(fd, buf, count);
}

ssize_t vtpc_write(int fd, const void* buf, size_t count) {
  return vtpc_impl_write(fd, buf, count);
}

off_t vtpc_lseek(int fd, off_t offset, int whence) {
  return vtpc_impl_lseek(fd, offset, whence);
}

int vtpc_fsync(int fd) {
  return vtpc_impl_fsync(fd);
}
