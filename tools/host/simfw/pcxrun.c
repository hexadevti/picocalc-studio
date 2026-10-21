/*
 * pcxrun.c - runs a .pcx module under rvsim (guest program, linked with simfw).
 *
 *   rvsim [--sd DIR] build/host/pcxrun.elf /sd/path/prog.pcx [args...]
 *
 * The module path is a guest path, i.e. under /sd (rvsim maps /sd/... to the --sd directory).
 * Loads the module with the firmware's loader (firmware/picocalc_studio/src/rt/pcx_load.c),
 * resolving imports against pcxrun_exports.S (tools/gen_exports.py --sim-out: the exports.txt
 * libc/libm/libgcc symbols; the picocalc.h SDK functions do not exist here). Then:
 *   main present:   exit(main(argc, argv)) with argv[0] = the module path, argv[1..] = args
 *   setup/loop:     setup(), then loop() forever (the program ends with exit(), or --max-insns);
 *                   setup() without loop() exits with status 0 when it returns
 * Load errors print "pcxrun: <message>" on stderr and exit with status 127.
 */
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcx.h"

struct RtExport {
    const char *name;
    void *addr;
};
extern const struct RtExport rt_exports[];
extern const uint32_t rt_exports_count;

static void *resolve(const char *name)
{
    uint32_t lo = 0, hi = rt_exports_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = strcmp(name, rt_exports[mid].name);
        if (!c) return rt_exports[mid].addr;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "pcxrun: ");
    fprintf(stderr, msg, arg);
    fprintf(stderr, "\n");
    exit(127);
}

int main(int argc, char **argv)
{
    char err[160];
    PcxView v;
    if (argc < 2) die("usage: pcxrun module.pcx [args...]%s", "");

    FILE *f = fopen(argv[1], "rb");
    if (!f) die("cannot open %s", argv[1]);
    long len = -1;
    if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET)) die("cannot read %s", argv[1]);
    void *image = malloc(len ? (size_t)len : 1);   /* malloc is 8-byte aligned */
    if (!image) die("out of memory reading %s", argv[1]);
    if (fread(image, 1, (size_t)len, f) != (size_t)len) die("cannot read %s", argv[1]);
    fclose(f);

    if (pcx_parse(image, (size_t)len, &v, err, sizeof err)) die("%s", err);
    const PcxHeader *h = v.h;

    /* rvsim has no memory protection: text runs from the heap */
    uint8_t *text = memalign(4, h->text_size ? h->text_size : 4);
    size_t ram_size = (size_t)h->data_size + h->bss_size;
    uint8_t *ram = memalign(h->ram_align < 4 ? 4 : h->ram_align, ram_size ? ram_size : 4);
    if (!text || !ram) die("out of memory loading %s", argv[1]);
    memcpy(text, v.text, h->text_size);
    if (pcx_relocate(&v, text, (uint32_t)(uintptr_t)text, ram, resolve, err, sizeof err)) die("%s", err);
    __asm__ volatile("fence.i" ::: "memory");

    if (h->entry_main != PCX_NO_ENTRY) {
        int (*m)(int, char **) = (int (*)(int, char **))(void *)(text + h->entry_main);
        int rc = m(argc - 1, argv + 1);
        fflush(stdout);
        exit(rc);
    }
    if (h->entry_setup != PCX_NO_ENTRY) {
        void (*setup)(void) = (void (*)(void))(void *)(text + h->entry_setup);
        setup();
        fflush(stdout);
        if (h->entry_loop == PCX_NO_ENTRY) exit(0);
        void (*loop)(void) = (void (*)(void))(void *)(text + h->entry_loop);
        for (;;) loop();
    }
    die("%s has no main() and no setup()/loop()", argv[1]);
    return 127;
}
