// pcx_dbg.c - debugger stop decisions for debug builds (see pcx.h, docs/DEBUGGER.md section 3).
//
// Portable C99, shared by the firmware's runner and the simulator runner (tools/host/simfw), so
// stepping is tested on the host. No locking: the program task calls pcx_dbg_check() and the
// supervisor, which outranks it on the same core, changes the mode while the program is stopped
// or for a pause.
#include <string.h>
#include "pcx.h"

static int placeLine(const PcxDbg *d, uint32_t at, const char **file) {
  int line = 0;
  *file = NULL;
  pcx_lookup_line(d->v, at, file, &line);
  return line;
}

static int stopLine(const PcxDbg *d, int id, const char **file) { return placeLine(d, d->v->stops[id], file); }

static uint32_t *word(uint32_t addr) { return (uint32_t *)(uintptr_t)addr; }

// The real return address of a frame whose saved ra may be patched; 0 if it isn't known.
static uint32_t realRa(const PcxDbg *d, uint32_t s0, uint32_t ra) {
  if (ra != (uint32_t)(uintptr_t)pcx_dbg_ret_tramp) return ra;
  for (int i = 0; i < d->nret; i++)
    if (d->ret[i].s0 == s0) return d->ret[i].ra;
  return 0;
}

// Makes the function of place `at` (frame s0) return through the trampoline. False if it can't:
// the firmware called it (no return site to show), or the table is full.
static int catchReturn(PcxDbg *d, uint32_t at, uint32_t s0) {
  const PcxFunc *f = pcx_func_at(d->v, at);
  if (!f || !s0) return 0;
  uint32_t *slot = word(s0 - f->va - 4);
  const uint32_t tramp = (uint32_t)(uintptr_t)pcx_dbg_ret_tramp;
  if (*slot == tramp) return realRa(d, s0, tramp) != 0;
  if (*slot - d->text >= d->v->h->text_size) return 0;
  int i = 0;
  while (i < d->nret && d->ret[i].s0 != s0) i++;   // a frame that died without returning (longjmp)
  if (i == PCX_DBG_RETS) return 0;
  if (i == d->nret) d->nret++;
  d->ret[i].s0 = s0;
  d->ret[i].ra = *slot;
  *slot = tramp;
  return 1;
}

// Whether a stop's file is `name`: the same path, or `name` is its last components ("main.c").
static int sameFile(const char *file, const char *name) {
  size_t n = strlen(name), m;
  if (!file) return 0;
  m = strlen(file);
  if (m == n) return !strcmp(file, name);
  return m > n && file[m - n - 1] == '/' && !strcmp(file + m - n, name);
}

void pcx_dbg_init(PcxDbg *d, const PcxView *v, uint32_t text, uint8_t *ram, uint8_t *bp) {
  memset(d, 0, sizeof *d);
  d->v = v;
  d->text = text;
  d->n = v->dbg ? v->dbg->n_stops : 0;
  d->flags = v->dbg ? ram + v->dbg->flags_off : NULL;
  d->bp = bp;
  if (d->n) {
    memset(bp, 0, d->n);
    memset(d->flags, 0, d->n);
  }
  d->mode = PCX_DBG_RUN;
}

int pcx_dbg_break(PcxDbg *d, const char *file, int line, int on) {
  int best = 0;
  for (uint32_t i = 0; i < d->n; i++) {  // the first line at or after `line` that has a stop
    const char *f;
    int l = stopLine(d, (int)i, &f);
    if (l >= line && (!best || l < best) && sameFile(f, file)) best = l;
  }
  if (!best) return 0;
  // Only the line's first stop: a line with several statements (int b = f(a); has two) stops
  // once, as stepping does.
  for (uint32_t i = 0; i < d->n; i++) {
    const char *f;
    if (stopLine(d, (int)i, &f) == best && sameFile(f, file)) {
      d->bp[i] = (uint8_t)(on != 0);
      if (d->mode == PCX_DBG_RUN) d->flags[i] = d->bp[i];
      break;
    }
  }
  return best;
}

int pcx_dbg_check(PcxDbg *d, int id, uint32_t s0) {
  const char *file;
  int line;
  if (id < 0 || (uint32_t)id >= d->n) return 0;
  if (d->bp[id]) return 1;
  switch (d->mode) {
    case PCX_DBG_PAUSE: return 1;
    case PCX_DBG_STEP_INTO:
      line = stopLine(d, id, &file);
      return s0 != d->s0 || line != d->line || file != d->file;
    // The stack grows down: a caller's frame has a higher s0. When the step's frame returns
    // through the trampoline, pcx_dbg_return() stops in the caller; otherwise (the firmware called
    // it) the first stop in a shallower frame does.
    case PCX_DBG_STEP_OVER:
      if (s0 != d->s0) return !d->caught && s0 > d->s0;
      line = stopLine(d, id, &file);
      return line != d->line || file != d->file;
    case PCX_DBG_STEP_OUT: return !d->caught && s0 > d->s0;
    default: return 0;
  }
}

void pcx_dbg_resume(PcxDbg *d, int mode, uint32_t at, uint32_t s0) {
  d->s0 = s0;
  d->file = NULL;
  d->line = at != UINT32_MAX ? placeLine(d, at, &d->file) : 0;
  d->caught = mode != PCX_DBG_RUN && at != UINT32_MAX && catchReturn(d, at, s0);
  d->mode = mode;
  if (!d->n) return;
  if (mode == PCX_DBG_RUN) memcpy(d->flags, d->bp, d->n);
  else memset(d->flags, 1, d->n);
}

void pcx_dbg_pause(PcxDbg *d) {
  d->mode = PCX_DBG_PAUSE;
  if (d->n) memset(d->flags, 1, d->n);
}

int pcx_dbg_return(PcxDbg *d, PcxDbgRegs *regs) {
  const uint32_t s0 = regs->x[2];   // the epilogue left sp at the returning frame's s0
  uint32_t ra = 0;
  // Frames at or below this one are gone: this one, and any a longjmp skipped.
  int k = 0;
  for (int i = 0; i < d->nret; i++) {
    if (d->ret[i].s0 == s0) ra = d->ret[i].ra;
    if (d->ret[i].s0 > s0) d->ret[k++] = d->ret[i];
  }
  d->nret = k;
  if (!ra) return -1;
  regs->x[5] = ra;
  if (d->mode == PCX_DBG_PAUSE) return 1;
  return d->mode != PCX_DBG_RUN && s0 == d->s0;
}

int pcx_dbg_backtrace(const PcxDbg *d, uint32_t at, uint32_t s0, PcxFrame *f, int max) {
  int n = 0;
  while (n < max) {
    const PcxFunc *fn = pcx_func_at(d->v, at);
    f[n].at = at;
    f[n].s0 = s0;
    f[n++].fn = fn;
    if (!fn) break;
    const uint32_t ra = realRa(d, s0, *word(s0 - fn->va - 4));
    const uint32_t up = *word(s0 - fn->va - 8);
    if (!ra || ra - d->text - 1 >= d->v->h->text_size || up <= s0) break;
    at = ra - d->text - 1;
    s0 = up;
  }
  return n;
}
