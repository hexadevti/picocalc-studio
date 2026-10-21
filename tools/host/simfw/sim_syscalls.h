/*
 * sim_syscalls.h - ecall interface between simfw (guest) and rvsim (host emulator).
 *
 * a7 = number, a0..a5 = arguments, result in a0; a negative result is -errno (newlib values).
 * Linux riscv numbers where one exists, 2000+ for the directory iteration that Linux does with
 * getdents. Keep in sync with the SYSCALLS enum in tools/host/rvsim/rvsim.c.
 */
#ifndef SIM_SYSCALLS_H
#define SIM_SYSCALLS_H

#define SYS_MKDIRAT        34   /* (dirfd, path, mode) */
#define SYS_UNLINKAT       35   /* (dirfd, path, flags); flags & AT_REMOVEDIR -> rmdir */
#define SYS_RENAMEAT       38   /* (olddirfd, old, newdirfd, new); fails with EEXIST like FATFS */
#define SYS_OPENAT         56   /* (dirfd, path, newlib O_* flags, mode) -> fd */
#define SYS_CLOSE          57   /* (fd) */
#define SYS_LSEEK          62   /* (fd, off, whence) -> new offset */
#define SYS_READ           63   /* (fd, buf, n) */
#define SYS_WRITE          64   /* (fd, buf, n) */
#define SYS_FSTATAT        79   /* (dirfd, path, struct stat *, flags) */
#define SYS_FSTAT          80   /* (fd, struct stat *) */
#define SYS_EXIT           93   /* (status) */
#define SYS_KILL          129   /* (pid, sig) - terminates the emulator for sig != 0 */
#define SYS_GETTIMEOFDAY  169   /* (struct timeval *) host wall clock */
#define SYS_GETPID        172
#define SYS_SIM_OPENDIR  2000   /* (path) -> handle > 0 */
#define SYS_SIM_READDIR  2001   /* (handle, struct dirent *) -> 1 = entry filled, 0 = end */
#define SYS_SIM_CLOSEDIR 2002   /* (handle) */
#define SYS_SIM_REWINDDIR 2003  /* (handle) */

#ifndef __ASSEMBLER__
static inline long sim_syscall(long n, long a, long b, long c, long d)
{
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a7 __asm__("a7") = n;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}
#endif
#endif
