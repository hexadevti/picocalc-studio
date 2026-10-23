// dirent.h - directory reading on the SD card (RP2350 build only). The toolchain's newlib has none;
// posix.cpp implements it on SDFS. struct dirent has the layout of ESP-IDF's, which compiled
// programs see through /sd/pico-dev/include/dirent.h.
#pragma once
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dirent {
  ino_t d_ino;
  unsigned char d_type;   // DT_*
  char d_name[256];
};

typedef struct __pc_DIR DIR;

#define MAXNAMLEN  255
#define DT_UNKNOWN 0
#define DT_REG     1
#define DT_DIR     2

DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
void rewinddir(DIR *d);

#ifdef __cplusplus
}
#endif
