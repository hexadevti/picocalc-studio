/* stress_alloc - leaks on purpose so the runtime's per-run cleanup can be checked.
 * Allocates heap blocks, opens files, starts a tone and draws, then exits WITHOUT freeing
 * anything. The harness (@stress serial command or manual run/stop) repeats it 50x and checks
 * that sys_free_heap() at start returns to the same baseline every run.
 * Output line: "stress run baseline <free at start> peak_used <bytes> leaked_blocks <n>" */
#include <picocalc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define BLOCKS 64
#define FILES 4
#define TMP_DIR SD_ROOT "/picocalc-studio/tests/tmp_stress"   /* not tests/tmp: t18 lists that */

void setup(void)
{
    size_t baseline = sys_free_heap();
    sys_seed(12345);

    void *blocks[BLOCKS];
    size_t total = 0;
    for (int i = 0; i < BLOCKS; i++) {
        size_t sz = 64 + sys_random() % 8192;
        blocks[i] = malloc(sz);
        if (blocks[i]) { memset(blocks[i], i, sz); total += sz; }
    }
    /* realloc churn so the allocator has fragmented state to clean up */
    for (int i = 0; i < BLOCKS; i += 3) {
        void *n = realloc(blocks[i], 16384);
        if (n) blocks[i] = n;
    }
    void *big = malloc(256 * 1024);   /* lands in PSRAM */
    if (big) memset(big, 0xAA, 256 * 1024);

    FILE *files[FILES];
    mkdir(TMP_DIR, 0777);
    for (int i = 0; i < FILES; i++) {
        char path[64];
        snprintf(path, sizeof path, TMP_DIR "/stress%d.tmp", i);
        files[i] = fopen(path, "w");
        if (files[i]) fprintf(files[i], "leaked file %d\n", i);   /* never closed */
    }

    snd_tone(440, 5000);   /* still playing when we exit */
    gfx_clear(NAVY);
    gfx_printf(10, 10, WHITE, "baseline %lu", (unsigned long)baseline);
    gfx_printf(10, 26, WHITE, "allocated %lu + 256K", (unsigned long)total);
    gfx_flush();

    size_t used = baseline - sys_free_heap();
    printf("stress run baseline %lu peak_used %lu leaked_blocks %d\n", (unsigned long)baseline, (unsigned long)used,
           BLOCKS + 1);
    sys_log("stress baseline=%lu\n", (unsigned long)baseline);
    sys_exit();   /* the runtime must free blocks, big, files[] and stop the tone */
}

void loop(void) {}
