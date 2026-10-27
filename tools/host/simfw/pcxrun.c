/*
 * pcxrun.c - runs a .pcx module under rvsim (guest program, linked with simfw).
 *
 *   rvsim [--sd DIR] build/host/pcxrun.elf [--dbg all|trace|SCRIPT] /sd/path/prog.pcx [args...]
 *
 * The module path is a guest path, i.e. under /sd (rvsim maps /sd/... to the --sd directory).
 * Loads the module with the firmware's loader (firmware/picocalc_studio/src/rt/pcx_load.c),
 * resolving imports against pcxrun_exports.S (tools/gen_exports.py --sim-out: the exports.txt
 * libc/libm/libgcc symbols; the picocalc.h SDK functions do not exist here). Then:
 *   main present:   exit(main(argc, argv)) with argv[0] = the module path, argv[1..] = args
 *   setup/loop:     setup(), then loop() forever (the program ends with exit(), or --max-insns);
 *                   setup() without loop() exits with status 0 when it returns
 * Load errors print "pcxrun: <message>" on stderr and exit with status 127.
 *
 * Debug builds (pcc -g, docs/DEBUGGER.md) run as usual with every stop point off. --dbg turns
 * them all on: `all` only counts the stops (printed on stderr at exit), `trace` also prints
 * "stop file:line" on stderr at each. Either way stdout must match a normal build's.
 *
 * --dbg SCRIPT (a guest path) drives the debugger the way the device's stopped view does, through
 * pcx_dbg.c. One command per line, '#' comments:
 *   break FILE:LINE / delete FILE:LINE   set / clear a breakpoint ("@break FILE:LINE" on stdout)
 *   run, continue                        run to the next breakpoint
 *   next, step, finish                   step over / into / out
 *   bt                                   the call stack: "@bt FUNC FILE:LINE" per frame
 * Commands up to the first run command apply before the program starts; then they run at each
 * stop, which prints "@stop FILE:LINE" on stdout, between the program's output (a stop where a
 * stepped function returned to its caller prints "@return FILE:LINE"). Out of commands, the
 * program runs to the end with its breakpoints off.
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

static PcxView s_view;
static uint32_t s_text;
static int s_trace;
static unsigned long s_nstops;
static FILE *s_script;
static PcxDbg s_dbg;
static uint32_t s_at, s_s0;   /* where the program is stopped (bt) */

static void die(const char *msg, const char *arg);

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Runs script commands up to the next one that resumes the program; returns its PCX_DBG_* mode. */
static int script_commands(void)
{
    char line[160], file[128];
    int n;
    while (s_script && fgets(line, sizeof line, s_script)) {
        char *c = line + strspn(line, " \t");
        c[strcspn(c, "#\r\n")] = 0;
        for (n = (int)strlen(c); n && (c[n - 1] == ' ' || c[n - 1] == '\t'); n--) c[n - 1] = 0;
        if (!*c) continue;
        if (!strcmp(c, "run") || !strcmp(c, "continue")) return PCX_DBG_RUN;
        if (!strcmp(c, "next")) return PCX_DBG_STEP_OVER;
        if (!strcmp(c, "step")) return PCX_DBG_STEP_INTO;
        if (!strcmp(c, "finish")) return PCX_DBG_STEP_OUT;
        if (!strcmp(c, "bt")) {
            PcxFrame f[16];
            int nf = pcx_dbg_backtrace(&s_dbg, s_at, s_s0, f, 16);
            for (int k = 0; k < nf; k++) {
                const char *file = "?";
                int l = 0;
                pcx_lookup_line(&s_view, f[k].at, &file, &l);
                printf("@bt %s %s:%d\n", f[k].fn ? s_view.strtab + f[k].fn->name : "?", base_name(file), l);
            }
            continue;
        }
        int on = !strncmp(c, "break ", 6);
        if ((on || !strncmp(c, "delete ", 7)) && sscanf(c + (on ? 6 : 7), "%127[^:]:%d", file, &n) == 2) {
            int at = pcx_dbg_break(&s_dbg, file, n, on);
            if (!at) die("no stop point at or after %s", c);
            if (on) printf("@break %s:%d\n", base_name(file), at);
            continue;
        }
        die("bad script command: %s", c);
    }
    s_script = NULL;   /* out of commands: run to the end */
    memset(s_dbg.bp, 0, s_dbg.n);
    return PCX_DBG_RUN;
}

void __pc_dbg_stop(PcxDbgRegs *regs)
{
    int i = pcx_stop_index(&s_view, regs->x[5] - s_text);
    if (i < 0 || regs->x[5] - s_text != s_view.stops[i] + 16)
        die("stop from an unknown place%s", "");
    if ((regs->x[2] & 3) || regs->x[2] < (uint32_t)(uintptr_t)regs)
        die("stop with a bad sp%s", "");
    if (s_dbg.v) {
        const char *file;
        int line;
        if (!pcx_dbg_check(&s_dbg, i, regs->x[8])) return;
        if (!pcx_lookup_line(&s_view, s_view.stops[i], &file, &line)) die("stop without a line%s", "");
        printf("@stop %s:%d\n", base_name(file), line);
        s_at = s_view.stops[i];
        s_s0 = regs->x[8];
        pcx_dbg_resume(&s_dbg, script_commands(), s_at, s_s0);
        return;
    }
    s_nstops++;
    if (s_trace) {
        const char *file;
        int line;
        if (pcx_lookup_line(&s_view, s_view.stops[i], &file, &line))
            fprintf(stderr, "stop %s:%d\n", file, line);
        else
            fprintf(stderr, "stop %u\n", (unsigned)i);
    }
}

void __pc_dbg_return(PcxDbgRegs *regs)
{
    const char *file;
    int line;
    int r = pcx_dbg_return(&s_dbg, regs);
    if (r < 0) die("return through the trampoline from an unknown frame%s", "");
    if (!r) return;
    s_at = regs->x[5] - s_text - 1;
    s_s0 = regs->x[8];
    if (!pcx_lookup_line(&s_view, s_at, &file, &line)) die("return without a line%s", "");
    printf("@return %s:%d\n", base_name(file), line);
    pcx_dbg_resume(&s_dbg, script_commands(), s_at, s_s0);
}

static void report_stops(void)
{
    fprintf(stderr, "pcxrun: %lu stops\n", s_nstops);
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
    const char *dbg = NULL;
    if (argc > 2 && !strcmp(argv[1], "--dbg")) {
        dbg = argv[2];
        s_trace = !strcmp(dbg, "trace");
        argc -= 2;
        argv += 2;
    }
    if (argc < 2) die("usage: pcxrun [--dbg all|trace|SCRIPT] module.pcx [args...]%s", "");

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
    s_view = v;
    s_text = (uint32_t)(uintptr_t)text;
    if (dbg) {
        if (!v.dbg) die("%s is not a debug build (pcc -g)", argv[1]);
        if (!strcmp(dbg, "all") || s_trace) {
            memset(ram + v.dbg->flags_off, 1, v.dbg->n_stops);
            atexit(report_stops);
        } else {
            uint8_t *bp = malloc(v.dbg->n_stops ? v.dbg->n_stops : 1);
            if (!bp || !(s_script = fopen(dbg, "r"))) die("cannot open %s", dbg);
            pcx_dbg_init(&s_dbg, &s_view, s_text, ram, bp);
            pcx_dbg_resume(&s_dbg, script_commands(), UINT32_MAX, 0);
        }
    }

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
