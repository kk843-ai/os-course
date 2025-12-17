#define _GNU_SOURCE

#include "vtpc_impl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef VTPC_PAGE_SIZE
#define VTPC_PAGE_SIZE 4096
#endif

#ifndef VTPC_CACHE_PAGES
#define VTPC_CACHE_PAGES 64
#endif

#ifndef VTPC_MAX_FILES
#define VTPC_MAX_FILES 64
#endif

typedef struct {
  int used;
  int os_fd;
  off_t pos;
  off_t size;  // логический размер файла (с учётом данных в кэше)
} vtpc_file_t;

typedef struct {
  int valid;
  int dirty;
  int owner;
  off_t page_no;
  unsigned char data[VTPC_PAGE_SIZE];
} vtpc_page_t;

static vtpc_file_t g_files[VTPC_MAX_FILES];
static vtpc_page_t g_cache[VTPC_CACHE_PAGES];

static int g_rng_inited = 0;

static void rng_init_once(void) {
  if (g_rng_inited) return;
  g_rng_inited = 1;
  unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
  srand(seed);
}

static int alloc_fd(void) {
  for (int i = 0; i < VTPC_MAX_FILES; i++) {
    if (!g_files[i].used) {
      g_files[i].used = 1;
      g_files[i].os_fd = -1;
      g_files[i].pos = 0;
      g_files[i].size = 0;
      return i;
    }
  }
  errno = EMFILE;
  return -1;
}

static void free_fd(int vfd) {
  if (vfd < 0 || vfd >= VTPC_MAX_FILES) return;
  g_files[vfd].used = 0;
  g_files[vfd].os_fd = -1;
  g_files[vfd].pos = 0;
  g_files[vfd].size = 0;
}

static vtpc_page_t* find_page(int vfd, off_t page_no) {
  for (int i = 0; i < VTPC_CACHE_PAGES; i++) {
    if (g_cache[i].valid && g_cache[i].owner == vfd && g_cache[i].page_no == page_no) {
      return &g_cache[i];
    }
  }
  return NULL;
}

static int flush_page(vtpc_page_t* p) {
  if (!p->valid || !p->dirty) return 0;

  int osfd = g_files[p->owner].os_fd;
  if (osfd < 0) {
    errno = EBADF;
    return -1;
  }

  off_t off = p->page_no * (off_t)VTPC_PAGE_SIZE;
  ssize_t w = pwrite(osfd, p->data, VTPC_PAGE_SIZE, off);
  if (w < 0) return -1;
  if (w != VTPC_PAGE_SIZE) {
    errno = EIO;
    return -1;
  }

  p->dirty = 0;
  return 0;
}

static vtpc_page_t* get_slot_for_page(void) {
  for (int i = 0; i < VTPC_CACHE_PAGES; i++) {
    if (!g_cache[i].valid) return &g_cache[i];
  }

  rng_init_once();
  int victim = rand() % VTPC_CACHE_PAGES;
  vtpc_page_t* p = &g_cache[victim];

  if (flush_page(p) != 0) return NULL;

  p->valid = 0;
  p->dirty = 0;
  p->owner = -1;
  p->page_no = 0;
  return p;
}

static vtpc_page_t* load_page(int vfd, off_t page_no) {
  vtpc_page_t* p = find_page(vfd, page_no);
  if (p) return p;

  vtpc_page_t* slot = get_slot_for_page();
  if (!slot) return NULL;

  int osfd = g_files[vfd].os_fd;
  if (osfd < 0) {
    errno = EBADF;
    return NULL;
  }

  off_t off = page_no * (off_t)VTPC_PAGE_SIZE;

  unsigned char tmp[VTPC_PAGE_SIZE];
  ssize_t r = pread(osfd, tmp, VTPC_PAGE_SIZE, off);
  if (r < 0) return NULL;

  if (r < VTPC_PAGE_SIZE) {
    memset(tmp + r, 0, (size_t)(VTPC_PAGE_SIZE - r));
  }

  memcpy(slot->data, tmp, VTPC_PAGE_SIZE);
  slot->valid = 1;
  slot->dirty = 0;
  slot->owner = vfd;
  slot->page_no = page_no;
  return slot;
}

int vtpc_impl_open(const char* path, int mode, int access) {
  if (!path) {
    errno = EINVAL;
    return -1;
  }

  int vfd = alloc_fd();
  if (vfd < 0) return -1;

  int osfd = open(path, mode, access);
  if (osfd < 0) {
    free_fd(vfd);
    return -1;
  }

  struct stat st;
  if (fstat(osfd, &st) != 0) {
    close(osfd);
    free_fd(vfd);
    return -1;
  }

  g_files[vfd].os_fd = osfd;
  g_files[vfd].pos = 0;
  g_files[vfd].size = st.st_size;
  return vfd;
}

int vtpc_impl_close(int fd) {
  if (fd < 0 || fd >= VTPC_MAX_FILES || !g_files[fd].used) {
    errno = EBADF;
    return -1;
  }

  for (int i = 0; i < VTPC_CACHE_PAGES; i++) {
    if (g_cache[i].valid && g_cache[i].owner == fd) {
      if (flush_page(&g_cache[i]) != 0) return -1;
    }
  }

  // применяем логический размер к реальному файлу
  if (ftruncate(g_files[fd].os_fd, g_files[fd].size) != 0) return -1;

  // очищаем страницы fd
  for (int i = 0; i < VTPC_CACHE_PAGES; i++) {
    if (g_cache[i].valid && g_cache[i].owner == fd) {
      g_cache[i].valid = 0;
      g_cache[i].dirty = 0;
      g_cache[i].owner = -1;
      g_cache[i].page_no = 0;
    }
  }

  int rc = close(g_files[fd].os_fd);
  free_fd(fd);
  return rc;
}

ssize_t vtpc_impl_read(int fd, void* buf, size_t count) {
  if (fd < 0 || fd >= VTPC_MAX_FILES || !g_files[fd].used) {
    errno = EBADF;
    return -1;
  }
  if (!buf && count != 0) {
    errno = EINVAL;
    return -1;
  }

  size_t done = 0;
  unsigned char* out = (unsigned char*)buf;

  while (done < count) {
    off_t pos = g_files[fd].pos;

    // EOF по логическому size
    if (pos >= g_files[fd].size) break;

    off_t page_no = pos / (off_t)VTPC_PAGE_SIZE;
    size_t in_page = (size_t)(pos % (off_t)VTPC_PAGE_SIZE);
    size_t to_copy = VTPC_PAGE_SIZE - in_page;
    if (to_copy > (count - done)) to_copy = count - done;

    off_t remain = g_files[fd].size - pos;
    if ((off_t)to_copy > remain) to_copy = (size_t)remain;

    vtpc_page_t* p = load_page(fd, page_no);
    if (!p) return (done == 0) ? -1 : (ssize_t)done;

    memcpy(out + done, p->data + in_page, to_copy);
    done += to_copy;
    g_files[fd].pos += (off_t)to_copy;

    if (to_copy == 0) break;
  }

  return (ssize_t)done;
}

ssize_t vtpc_impl_write(int fd, const void* buf, size_t count) {
  if (fd < 0 || fd >= VTPC_MAX_FILES || !g_files[fd].used) {
    errno = EBADF;
    return -1;
  }
  if (!buf && count != 0) {
    errno = EINVAL;
    return -1;
  }

  size_t done = 0;
  const unsigned char* in = (const unsigned char*)buf;

  while (done < count) {
    off_t pos = g_files[fd].pos;
    off_t page_no = pos / (off_t)VTPC_PAGE_SIZE;
    size_t in_page = (size_t)(pos % (off_t)VTPC_PAGE_SIZE);
    size_t to_copy = VTPC_PAGE_SIZE - in_page;
    if (to_copy > (count - done)) to_copy = count - done;

    vtpc_page_t* p = load_page(fd, page_no);
    if (!p) return (done == 0) ? -1 : (ssize_t)done;

    memcpy(p->data + in_page, in + done, to_copy);
    p->dirty = 1;

    done += to_copy;
    g_files[fd].pos += (off_t)to_copy;

    if (g_files[fd].pos > g_files[fd].size) g_files[fd].size = g_files[fd].pos;
  }

  return (ssize_t)done;
}

off_t vtpc_impl_lseek(int fd, off_t offset, int whence) {
  if (fd < 0 || fd >= VTPC_MAX_FILES || !g_files[fd].used) {
    errno = EBADF;
    return (off_t)-1;
  }
  if (whence != SEEK_SET) {
    errno = EINVAL;
    return (off_t)-1;
  }
  if (offset < 0) {
    errno = EINVAL;
    return (off_t)-1;
  }

  g_files[fd].pos = offset;
  return offset;
}

int vtpc_impl_fsync(int fd) {
  if (fd < 0 || fd >= VTPC_MAX_FILES || !g_files[fd].used) {
    errno = EBADF;
    return -1;
  }

  for (int i = 0; i < VTPC_CACHE_PAGES; i++) {
    if (g_cache[i].valid && g_cache[i].owner == fd) {
      if (flush_page(&g_cache[i]) != 0) return -1;
    }
  }

  // применяем размер до fsync
  if (ftruncate(g_files[fd].os_fd, g_files[fd].size) != 0) return -1;

  return fsync(g_files[fd].os_fd);
}
