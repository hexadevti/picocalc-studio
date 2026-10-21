/*
 * syscalls.c - newlib system layer for simfw (guest side, built with riscv32-esp-elf-gcc).
 *
 * The ESP toolchain's newlib 4.3 is configured with __DYNAMIC_REENT__ (every _REENT goes through
 * __getreent(), which ESP-IDF supplies per FreeRTOS task) and _REENT_SMALL, and its libc.a
 * contains the reentrant wrappers (_write_r -> _write, ...) but none of the low-level calls.
 * Like ESP-IDF's vfs/syscalls.c, we define the _*_r functions directly, so the libc wrappers
 * (which would need _write, _open, ...) are never pulled in. The retargetable lock functions
 * (__retarget_lock_*) are left to libc's built-in no-op versions: there is a single thread.
 * Every call becomes an ecall to rvsim (sim_syscalls.h).
 */
#include <errno.h>
#include <fcntl.h>
#include <reent.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>
#include "sim_syscalls.h"

/* The emulator fills struct stat / struct timeval with these offsets (rvsim.c put_stat). */
_Static_assert(sizeof(struct stat) == 88, "struct stat layout");
_Static_assert(offsetof(struct stat, st_mode) == 4 && offsetof(struct stat, st_size) == 16 &&
               offsetof(struct stat, st_atim) == 24 && offsetof(struct stat, st_mtim) == 40 &&
               offsetof(struct stat, st_ctim) == 56 && offsetof(struct stat, st_blksize) == 72,
               "struct stat layout");
_Static_assert(sizeof(struct timeval) == 16 && offsetof(struct timeval, tv_usec) == 8, "timeval");

#define STACK_RESERVE (1024 * 1024)     /* heap stops this far below the initial sp */

extern char _end[];
static char *heap_ptr, *heap_base, *heap_limit;

/* Single "task": the global reent from libc's impure.c (ESP-IDF returns the task's own). */
struct _reent *__getreent(void)
{
    return _impure_ptr;
}

/* rvsim returns -errno on failure: store it in the reent and return -1. */
static long ret(struct _reent *r, long v)
{
    if ((unsigned long)v > (unsigned long)-4096L) {
        r->_errno = (int)-v;
        return -1;
    }
    return v;
}
#define SC(r, n, a, b, c, d) ret((r), sim_syscall((n), (long)(a), (long)(b), (long)(c), (long)(d)))

int _open_r(struct _reent *r, const char *path, int flags, int mode)
{ return (int)SC(r, SYS_OPENAT, AT_FDCWD, path, flags, mode); }
int _close_r(struct _reent *r, int fd) { return (int)SC(r, SYS_CLOSE, fd, 0, 0, 0); }
_ssize_t _read_r(struct _reent *r, int fd, void *buf, size_t n) { return SC(r, SYS_READ, fd, buf, n, 0); }
_ssize_t _write_r(struct _reent *r, int fd, const void *buf, size_t n) { return SC(r, SYS_WRITE, fd, buf, n, 0); }
_off_t _lseek_r(struct _reent *r, int fd, _off_t off, int whence) { return SC(r, SYS_LSEEK, fd, off, whence, 0); }
int _fstat_r(struct _reent *r, int fd, struct stat *st) { return (int)SC(r, SYS_FSTAT, fd, st, 0, 0); }
int _stat_r(struct _reent *r, const char *path, struct stat *st)
{ return (int)SC(r, SYS_FSTATAT, AT_FDCWD, path, st, 0); }
int _unlink_r(struct _reent *r, const char *path) { return (int)SC(r, SYS_UNLINKAT, AT_FDCWD, path, 0, 0); }
int _rename_r(struct _reent *r, const char *from, const char *to)
{ return (int)SC(r, SYS_RENAMEAT, AT_FDCWD, from, AT_FDCWD, to); }
int _mkdir_r(struct _reent *r, const char *path, int mode)
{ return (int)SC(r, SYS_MKDIRAT, AT_FDCWD, path, mode, 0); }
int _link_r(struct _reent *r, const char *a, const char *b) { (void)a; (void)b; r->_errno = ENOSYS; return -1; }
int _getpid_r(struct _reent *r) { (void)r; return (int)sim_syscall(SYS_GETPID, 0, 0, 0, 0); }
int _kill_r(struct _reent *r, int pid, int sig) { return (int)SC(r, SYS_KILL, pid, sig, 0, 0); }

int _isatty_r(struct _reent *r, int fd)
{
    struct stat st;
    return _fstat_r(r, fd, &st) == 0 && S_ISCHR(st.st_mode);
}
int _isatty(int fd) { return _isatty_r(_REENT, fd); }

int _gettimeofday_r(struct _reent *r, struct timeval *tv, void *tz)
{
    (void)tz;
    return (int)SC(r, SYS_GETTIMEOFDAY, tv, 0, 0, 0);
}

/* clock()/times(): the emulator's time CSR (1 us ticks, derived from the instruction count). */
clock_t _times_r(struct _reent *r, struct tms *t)
{
    uint32_t lo, hi, hi2;
    (void)r;
    do {
        __asm__ volatile("rdtimeh %0" : "=r"(hi));
        __asm__ volatile("rdtime %0" : "=r"(lo));
        __asm__ volatile("rdtimeh %0" : "=r"(hi2));
    } while (hi != hi2);
    uint64_t us = (uint64_t)hi << 32 | lo;
    clock_t c = (clock_t)(us / (1000000 / CLOCKS_PER_SEC));
    if (t) { t->tms_utime = c; t->tms_stime = 0; t->tms_cutime = 0; t->tms_cstime = 0; }
    return c;
}

/* malloc's heap: from _end up to STACK_RESERVE below the stack pointer seen at startup. */
void *_sbrk_r(struct _reent *r, ptrdiff_t incr)
{
    char *old = heap_ptr;
    if (incr < 0 ? heap_ptr + incr < heap_base : heap_limit - heap_ptr < incr) {
        r->_errno = ENOMEM;
        return (void *)-1;
    }
    heap_ptr += incr;
    return old;
}

void _exit(int status)
{
    for (;;) sim_syscall(SYS_EXIT, status, 0, 0, 0);
}

/* POSIX calls newlib leaves to the system (ESP-IDF provides them in its VFS layer). */
int mkdir(const char *path, mode_t mode) { return _mkdir_r(_REENT, path, (int)mode); }
int rmdir(const char *path) { return (int)SC(_REENT, SYS_UNLINKAT, AT_FDCWD, path, AT_REMOVEDIR, 0); }

/* C runtime start, called by crt0 after .bss is cleared. main() comes from the program. */
extern void (*__preinit_array_start[])(void), (*__preinit_array_end[])(void);
extern void (*__init_array_start[])(void), (*__init_array_end[])(void);
extern void (*__fini_array_start[])(void), (*__fini_array_end[])(void);
extern int main(int argc, char **argv);
void __simfw_cstart(int argc, char **argv, char *sp);

static void run_fini_array(void)
{
    for (size_t n = (size_t)(__fini_array_end - __fini_array_start); n; n--) __fini_array_start[n - 1]();
}

void __simfw_cstart(int argc, char **argv, char *sp)
{
    heap_base = heap_ptr = (char *)(((uintptr_t)_end + 15) & ~(uintptr_t)15);
    heap_limit = sp - STACK_RESERVE;
    for (void (**f)(void) = __preinit_array_start; f < __preinit_array_end; f++) (*f)();
    for (void (**f)(void) = __init_array_start; f < __init_array_end; f++) (*f)();
    atexit(run_fini_array);
    exit(main(argc, argv));
}
