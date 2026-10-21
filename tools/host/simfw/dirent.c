/*
 * dirent.c - opendir/readdir/closedir for simfw, on top of rvsim's directory ecalls.
 *
 * Layouts come from the toolchain's ESP-IDF <sys/dirent.h>: struct dirent is
 * { ino_t d_ino (u16); uint8_t d_type; char d_name[256]; } (260 bytes), DT_REG = 1, DT_DIR = 2,
 * and DIR is a 4-byte header { u16 dd_vfs_idx; u16 dd_rsv; } that the VFS implementation extends.
 * rvsim writes the dirent itself, so the offsets are pinned below. "." and ".." are not listed
 * (as with ESP-IDF's FAT VFS).
 */
#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include "sim_syscalls.h"

_Static_assert(offsetof(struct dirent, d_ino) == 0 && sizeof(ino_t) == 2, "dirent layout");
_Static_assert(offsetof(struct dirent, d_type) == 2 && offsetof(struct dirent, d_name) == 3, "dirent layout");
_Static_assert(sizeof(struct dirent) == 260 && DT_REG == 1 && DT_DIR == 2, "dirent layout");

typedef struct {
    DIR base;               /* must be first: the DIR * handed out points here */
    long handle;            /* rvsim directory handle */
    struct dirent entry;    /* storage returned by readdir() */
} SimDir;

static int fail(long v) { errno = (int)-v; return -1; }

DIR *opendir(const char *path)
{
    long h = sim_syscall(SYS_SIM_OPENDIR, (long)path, 0, 0, 0);
    if (h < 0) { fail(h); return NULL; }
    SimDir *d = calloc(1, sizeof *d);
    if (!d) { sim_syscall(SYS_SIM_CLOSEDIR, h, 0, 0, 0); errno = ENOMEM; return NULL; }
    d->handle = h;
    return &d->base;
}

struct dirent *readdir(DIR *dir)
{
    SimDir *d = (SimDir *)dir;
    if (!d) { errno = EBADF; return NULL; }
    long r = sim_syscall(SYS_SIM_READDIR, d->handle, (long)&d->entry, 0, 0);
    if (r < 0) fail(r);
    return r == 1 ? &d->entry : NULL;
}

int readdir_r(DIR *dir, struct dirent *entry, struct dirent **result)
{
    SimDir *d = (SimDir *)dir;
    long r = sim_syscall(SYS_SIM_READDIR, d->handle, (long)entry, 0, 0);
    if (r < 0) { *result = NULL; return (int)-r; }
    *result = r == 1 ? entry : NULL;
    return 0;
}

void rewinddir(DIR *dir)
{
    if (dir) sim_syscall(SYS_SIM_REWINDDIR, ((SimDir *)dir)->handle, 0, 0, 0);
}

int closedir(DIR *dir)
{
    SimDir *d = (SimDir *)dir;
    if (!d) { errno = EBADF; return -1; }
    long r = sim_syscall(SYS_SIM_CLOSEDIR, d->handle, 0, 0, 0);
    free(d);
    return r < 0 ? fail(r) : 0;
}
