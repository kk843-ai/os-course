#pragma once

#include <sys/types.h>

int vtpc_impl_open(const char* path, int mode, int access);
int vtpc_impl_close(int fd);

ssize_t vtpc_impl_read(int fd, void* buf, size_t count);
ssize_t vtpc_impl_write(int fd, const void* buf, size_t count);

off_t vtpc_impl_lseek(int fd, off_t offset, int whence);
int vtpc_impl_fsync(int fd);
