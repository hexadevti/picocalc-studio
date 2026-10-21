/* t18: file I/O on the SD card - fopen modes, fwrite/fread, fseek/ftell, fgets/fputs, fprintf/fscanf,
 * rename/remove, mkdir, directory listing with <dirent.h>. Leaves no files behind.
 * TEST_DIR is overridden by the host harness; on the device it lives on /sd. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#ifndef TEST_DIR
#define TEST_DIR "/sd/pico-dev/tests/tmp"
#endif

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int make_dir(const char *p)
{
#ifdef _WIN32
    return mkdir(p);
#else
    return mkdir(p, 0777);
#endif
}

int main(void)
{
    char path[128], path2[128];
    make_dir(TEST_DIR);

    snprintf(path, sizeof path, "%s/data.bin", TEST_DIR);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("open for write failed\n"); return 1; }
    unsigned char block[256];
    for (int i = 0; i < 256; i++) block[i] = (unsigned char)i;
    size_t w = 0;
    for (int i = 0; i < 16; i++) w += fwrite(block, 1, sizeof block, f);
    fclose(f);
    printf("wrote %u\n", (unsigned)w);

    f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 1000, SEEK_SET);
    int c = fgetc(f);
    fseek(f, -1, SEEK_END);
    int last = fgetc(f);
    printf("size %ld at1000 %d last %d eof %d\n", size, c, last, fgetc(f) == EOF);
    rewind(f);
    unsigned sum = 0;
    size_t r;
    while ((r = fread(block, 1, 100, f)) > 0)
        for (size_t i = 0; i < r; i++) sum += block[i];
    printf("sum %u\n", sum);
    fclose(f);

    snprintf(path2, sizeof path2, "%s/text.txt", TEST_DIR);
    f = fopen(path2, "w");
    fprintf(f, "line %d\n", 1);
    fputs("second line\n", f);
    fprintf(f, "%s %d %.2f\n", "values", 42, 1.5);
    fclose(f);
    f = fopen(path2, "a");
    fputs("appended\n", f);
    fclose(f);

    f = fopen(path2, "r");
    char line[64];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        printf("read[%d] %s\n", n++, line);
    }
    fclose(f);

    f = fopen(path2, "r");
    char word[16];
    int num = 0;
    fgets(line, sizeof line, f);
    fgets(line, sizeof line, f);
    int got = fscanf(f, "%15s %d", word, &num);
    printf("scan %d %s %d\n", got, word, num);
    fclose(f);

    char renamed[128];
    snprintf(renamed, sizeof renamed, "%s/renamed.txt", TEST_DIR);
    remove(renamed);   /* left over from an earlier, interrupted run */
    printf("rename %d\n", rename(path2, renamed));
    printf("missing %d\n", fopen(path2, "r") == NULL);

    DIR *d = opendir(TEST_DIR);
    char *names[16];
    int count = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) && count < 16)
        if (e->d_name[0] != '.') names[count++] = strdup(e->d_name);
    if (d) closedir(d);
    qsort(names, (size_t)count, sizeof names[0], cmp_str);
    printf("dir");
    for (int i = 0; i < count; i++) { printf(" %s", names[i]); free(names[i]); }
    printf("\n");

    struct stat st;
    int sr = stat(path, &st);
    printf("stat %d size_ok %d isdir %d\n", sr, sr == 0 && (long)st.st_size == size, S_ISDIR(st.st_mode) != 0);

    printf("remove %d %d\n", remove(path), remove(renamed));
    printf("gone %d\n", fopen(path, "rb") == NULL);
    return 0;
}
