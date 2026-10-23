// posix.cpp - newlib's file syscalls on the SD card (SDFS), so the firmware and compiled programs
// use plain stdio/dirent/stat as on the P4. Paths are "/sd/..." (SD_MOUNT); fd 0 reads nothing,
// fd 1/2 go to both serial ports. One recursive lock around every SDFS call: SdFat is not
// thread-safe, and the IDE, the compiler task and a running program all touch files.
//
// arduino-pico's core has weak stubs for these (cores/rp2040/posix.cpp); its VFS library is not
// used, since it has no locking and no directories.
#ifdef ARDUINO_ARCH_RP2040
#include "../../../board.h"
#include <Arduino.h>
#include <SDFS.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#undef errno
extern "C" int *__errno(void);
#define errno (*__errno())

// Compiled programs see these through /sd/picocalc-studio/include (sys/stat.h, dirent.h).
static_assert(sizeof(struct stat) == 88, "struct stat layout");
static_assert(offsetof(struct stat, st_mode) == 4, "struct stat layout");
static_assert(offsetof(struct stat, st_size) == 16, "struct stat layout");
static_assert(offsetof(struct stat, st_mtim) == 40, "struct stat layout");
static_assert(sizeof(struct dirent) == 260 && offsetof(struct dirent, d_name) == 3, "struct dirent layout");

#define MAX_FDS 24
#define FD_BASE 3

static SemaphoreHandle_t s_lock = nullptr;
static File *s_files[MAX_FDS];

namespace {
struct Lock {
  Lock()  { if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
  ~Lock() { if (s_lock) xSemaphoreGiveRecursive(s_lock); }
};
}

void rpPosixInit() {
  if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
}

// "/sd/a/b" -> "/a/b", "/sd" -> "/"; NULL for paths outside the card.
static const char *fsPath(const char *p) {
  static const size_t n = sizeof(SD_MOUNT) - 1;
  if (!p || strncmp(p, SD_MOUNT, n) || (p[n] && p[n] != '/')) return nullptr;
  return p[n] ? p + n : "/";
}

static File *fileOf(int fd) {
  const int i = fd - FD_BASE;
  return i >= 0 && i < MAX_FDS ? s_files[i] : nullptr;
}

static void consoleWrite(const void *buf, size_t n) {
  Serial1.write((const uint8_t *)buf, n);
  if (Serial) Serial.write((const uint8_t *)buf, n);
}

extern "C" {

int _open(const char *path, int flags, int mode) {
  (void)mode;
  const char *p = fsPath(path);
  if (!p) { errno = ENOENT; return -1; }
  Lock l;
  int slot = 0;
  while (slot < MAX_FDS && s_files[slot]) slot++;
  if (slot == MAX_FDS) { errno = EMFILE; return -1; }
  const int acc = flags & O_ACCMODE;
  const bool exists = SDFS.exists(p);
  if ((flags & O_CREAT) && (flags & O_EXCL) && exists) { errno = EEXIST; return -1; }
  if (!exists && !(flags & O_CREAT)) { errno = ENOENT; return -1; }
  const char *m;
  if (acc == O_RDONLY)              m = "r";
  else if (flags & O_APPEND)        m = acc == O_RDWR ? "a+" : "a";
  else if ((flags & O_TRUNC) || !exists) m = acc == O_RDWR ? "w+" : "w";
  else                              m = "r+";
  File f = SDFS.open(p, m);
  if (!f) { errno = exists ? EACCES : ENOENT; return -1; }
  s_files[slot] = new File(f);
  return slot + FD_BASE;
}

int _close(int fd) {
  File *f = fileOf(fd);
  if (!f) { errno = EBADF; return -1; }
  Lock l;
  f->close();
  delete f;
  s_files[fd - FD_BASE] = nullptr;
  return 0;
}

int _read(int fd, char *buf, int len) {
  if (fd == 0) return 0;
  File *f = fileOf(fd);
  if (!f) { errno = EBADF; return -1; }
  Lock l;
  const int n = f->read((uint8_t *)buf, (size_t)len);
  if (n < 0) { errno = EIO; return -1; }
  return n;
}

ssize_t _write(int fd, const void *buf, size_t len) {
  if (fd == 1 || fd == 2) { consoleWrite(buf, len); return (ssize_t)len; }
  File *f = fileOf(fd);
  if (!f) { errno = EBADF; return -1; }
  Lock l;
  const size_t n = f->write((const uint8_t *)buf, len);
  if (n != len && !n) { errno = ENOSPC; return -1; }
  return (ssize_t)n;
}

int _lseek(int fd, int off, int whence) {
  File *f = fileOf(fd);
  if (!f) { errno = EBADF; return -1; }
  Lock l;
  long base = whence == SEEK_CUR ? (long)f->position() : whence == SEEK_END ? (long)f->size() : 0;
  const long to = base + off;
  if (to < 0 || !f->seek((uint32_t)to, SeekSet)) { errno = EINVAL; return -1; }
  return (int)to;
}

static void fillStat(struct stat *st, bool dir, size_t size, time_t t) {
  memset(st, 0, sizeof *st);
  st->st_mode = dir ? (S_IFDIR | 0777) : (S_IFREG | 0666);
  st->st_nlink = 1;
  st->st_size = (off_t)size;
  st->st_blksize = 512;
  st->st_atim.tv_sec = st->st_mtim.tv_sec = st->st_ctim.tv_sec = t;
}

int _fstat(int fd, struct stat *st) {
  if (fd >= 0 && fd <= 2) {
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFCHR;
    return 0;
  }
  File *f = fileOf(fd);
  if (!f) { errno = EBADF; return -1; }
  Lock l;
  fillStat(st, f->isDirectory(), f->size(), f->getLastWrite());
  return 0;
}

int _stat(const char *path, struct stat *st) {
  const char *p = fsPath(path);
  if (!p) { errno = ENOENT; return -1; }
  Lock l;
  if (!strcmp(p, "/")) { fillStat(st, true, 0, 0); return 0; }
  File f = SDFS.open(p, "r");
  if (!f) { errno = ENOENT; return -1; }
  fillStat(st, f.isDirectory(), f.isDirectory() ? 0 : f.size(), f.getLastWrite());
  f.close();
  return 0;
}

int _isatty(int fd) { return fd >= 0 && fd <= 2; }

int _unlink(const char *path) {
  const char *p = fsPath(path);
  if (!p) { errno = ENOENT; return -1; }
  Lock l;
  if (!SDFS.remove(p)) { errno = ENOENT; return -1; }
  return 0;
}

int _link(const char *, const char *) { errno = ENOSYS; return -1; }

// newlib's remove() is unlink(); a directory needs rmdir() as in POSIX.
int rename(const char *from, const char *to) {
  const char *a = fsPath(from), *b = fsPath(to);
  if (!a || !b) { errno = ENOENT; return -1; }
  Lock l;
  if (!SDFS.exists(a)) { errno = ENOENT; return -1; }
  if (SDFS.exists(b)) {   // POSIX: replace the target
    File t = SDFS.open(b, "r");
    const bool dir = t && t.isDirectory();
    t.close();
    if (dir ? !SDFS.rmdir(b) : !SDFS.remove(b)) { errno = EEXIST; return -1; }
  }
  if (!SDFS.rename(a, b)) { errno = EIO; return -1; }
  return 0;
}

int mkdir(const char *path, mode_t) {
  const char *p = fsPath(path);
  if (!p) { errno = ENOENT; return -1; }
  Lock l;
  if (SDFS.exists(p)) { errno = EEXIST; return -1; }
  if (!SDFS.mkdir(p)) { errno = ENOENT; return -1; }
  return 0;
}

int rmdir(const char *path) {
  const char *p = fsPath(path);
  if (!p) { errno = ENOENT; return -1; }
  Lock l;
  if (!SDFS.rmdir(p)) { errno = ENOTEMPTY; return -1; }
  return 0;
}

struct __pc_DIR {
  Dir dir;
  struct dirent ent;
};

DIR *opendir(const char *path) {
  const char *p = fsPath(path);
  if (!p) { errno = ENOENT; return nullptr; }
  Lock l;
  if (strcmp(p, "/")) {
    File f = SDFS.open(p, "r");
    const bool dir = f && f.isDirectory();
    f.close();
    if (!dir) { errno = ENOENT; return nullptr; }
  }
  DIR *d = new (std::nothrow) DIR;
  if (!d) { errno = ENOMEM; return nullptr; }
  d->dir = SDFS.openDir(p);
  return d;
}

struct dirent *readdir(DIR *d) {
  if (!d) { errno = EBADF; return nullptr; }
  Lock l;
  if (!d->dir.next()) return nullptr;
  memset(&d->ent, 0, sizeof d->ent);
  String name = d->dir.fileName();
  const char *s = name.c_str(), *slash = strrchr(s, '/');
  snprintf(d->ent.d_name, sizeof d->ent.d_name, "%s", slash ? slash + 1 : s);
  d->ent.d_type = d->dir.isDirectory() ? DT_DIR : DT_REG;
  return &d->ent;
}

void rewinddir(DIR *d) {
  if (!d) return;
  Lock l;
  d->dir.rewind();
}

int closedir(DIR *d) {
  if (!d) { errno = EBADF; return -1; }
  Lock l;
  delete d;   // the Dir's destructor closes it
  return 0;
}

}  // extern "C"

#endif  // ARDUINO_ARCH_RP2040
