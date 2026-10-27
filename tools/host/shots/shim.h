// shim.h - force-included (-include) into every firmware source the screenshot harness builds
// natively with MinGW. Maps the device's "/sd/..." paths to the folder in $PICOCALC_SD, and fills
// in the few POSIX bits Windows lacks.
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <io.h>       // declare mkdir(path) here, before the macro below
#include <direct.h>
#ifdef __cplusplus
#include <string>     // libstdc++ headers use remove/rename names
#include <vector>
#include <deque>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdio>
#endif

#ifdef __cplusplus
extern "C" {
#endif
const char *host_path(const char *path, char *buf, size_t n);   // "/sd/x" -> "$PICOCALC_SD/x"
FILE *host_fopen(const char *path, const char *mode);
DIR *host_opendir(const char *path);
int host_stat(const char *path, struct stat *st);
int host_mkdir(const char *path);
int host_remove(const char *path);
int host_rename(const char *from, const char *to);
#ifdef __cplusplus
}
#endif

#undef stat
#define fopen(p, m) host_fopen(p, m)
#define opendir(p) host_opendir(p)
#define stat(p, s) host_stat(p, s)
#define mkdir(p, m) host_mkdir(p)
#define remove(p) host_remove(p)
#define rename(a, b) host_rename(a, b)

// ide.cpp captures program output with fopencookie; the harness doesn't run programs.
#ifdef __cplusplus
typedef ssize_t (*host_cookie_write)(void *, const char *, size_t);
typedef struct { void *read; host_cookie_write write; void *seek; void *close; } cookie_io_functions_t;
static inline FILE *fopencookie(void *, const char *, cookie_io_functions_t) { return nullptr; }
#endif
