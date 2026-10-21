/* sys/stat.h - PicoCalc C library (struct stat layout matches the firmware's newlib, 88 bytes) */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <sys/types.h>
#include <time.h>

struct stat {
    dev_t st_dev;               /*  0 */
    ino_t st_ino;               /*  2 */
    mode_t st_mode;             /*  4 */
    nlink_t st_nlink;           /*  8 */
    uid_t st_uid;               /* 10 */
    gid_t st_gid;               /* 12 */
    dev_t st_rdev;              /* 14 */
    off_t st_size;              /* 16 */
    struct timespec st_atim;    /* 24 */
    struct timespec st_mtim;    /* 40 */
    struct timespec st_ctim;    /* 56 */
    blksize_t st_blksize;       /* 72 */
    blkcnt_t st_blocks;         /* 76 */
    long st_spare4[2];          /* 80 */
};

#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

#define _IFMT   0170000
#define _IFDIR  0040000
#define _IFCHR  0020000
#define _IFBLK  0060000
#define _IFREG  0100000
#define _IFLNK  0120000
#define _IFSOCK 0140000
#define _IFIFO  0010000

#define S_IFMT   _IFMT
#define S_IFDIR  _IFDIR
#define S_IFCHR  _IFCHR
#define S_IFBLK  _IFBLK
#define S_IFREG  _IFREG
#define S_IFLNK  _IFLNK
#define S_IFSOCK _IFSOCK
#define S_IFIFO  _IFIFO

#define S_ISUID 0004000
#define S_ISGID 0002000
#define S_ISVTX 0001000
#define S_IRWXU 0000700
#define S_IRUSR 0000400
#define S_IWUSR 0000200
#define S_IXUSR 0000100
#define S_IRWXG 0000070
#define S_IRGRP 0000040
#define S_IWGRP 0000020
#define S_IXGRP 0000010
#define S_IRWXO 0000007
#define S_IROTH 0000004
#define S_IWOTH 0000002
#define S_IXOTH 0000001
#define S_IREAD  S_IRUSR
#define S_IWRITE S_IWUSR
#define S_IEXEC  S_IXUSR

#define S_ISBLK(m)  (((m) & _IFMT) == _IFBLK)
#define S_ISCHR(m)  (((m) & _IFMT) == _IFCHR)
#define S_ISDIR(m)  (((m) & _IFMT) == _IFDIR)
#define S_ISFIFO(m) (((m) & _IFMT) == _IFIFO)
#define S_ISREG(m)  (((m) & _IFMT) == _IFREG)
#define S_ISLNK(m)  (((m) & _IFMT) == _IFLNK)
#define S_ISSOCK(m) (((m) & _IFMT) == _IFSOCK)

int stat(const char *restrict path, struct stat *restrict st);
int mkdir(const char *path, mode_t mode);

#endif
