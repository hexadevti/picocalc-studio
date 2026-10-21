/* dirent.h - PicoCalc C library (struct dirent matches the firmware's ESP-IDF newlib, 260 bytes) */
#ifndef _DIRENT_H
#define _DIRENT_H

#include <sys/types.h>

struct dirent {
    ino_t d_ino;            /* 0 */
    unsigned char d_type;   /* 2: DT_* */
    char d_name[256];       /* 3: zero-terminated file name */
};

typedef struct __pc_DIR DIR;    /* opaque */

#define MAXNAMLEN  255
#define DT_UNKNOWN 0
#define DT_REG     1
#define DT_DIR     2

DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

#endif
