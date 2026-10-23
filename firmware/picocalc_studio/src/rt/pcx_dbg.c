// pcx_dbg.c - debugger stop decisions for debug builds (see pcx.h, docs/DEBUGGER.md section 3).
//
// Portable C99, shared by the firmware's runner and the simulator runner (tools/host/simfw), so
// stepping is tested on the host. No locking: the program task calls pcx_dbg_check() and the
// supervisor, which outranks it on the same core, changes the mode while the program is stopped
// or for a pause.
#include <string.h>
#include "pcx.h"

static int stopLine(const PcxDbg *d, int id, const char **file) {
  int line = 0;
  *file = NULL;
  pcx_lookup_line(d->v, d->v->stops[id], file, &line);
  return line;
}

// Whether a stop's file is `name`: the same path, or `name` is its last components ("main.c").
static int sameFile(const char *file, const char *name) {
  size_t n = strlen(name), m;
  if (!file) return 0;
  m = strlen(file);
  if (m == n) return !strcmp(file, name);
  return m > n && file[m - n - 1] == '/' && !strcmp(file + m - n, name);
}

void pcx_dbg_init(PcxDbg *d, const PcxView *v, uint8_t *ram, uint8_t *bp) {
  memset(d, 0, sizeof *d);
  d->v = v;
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
    case PCX_DBG_STEP_OVER:   // the stack grows down: a caller's frame has a higher s0
      if (s0 != d->s0) return s0 > d->s0;
      line = stopLine(d, id, &file);
      return line != d->line || file != d->file;
    // Any shallower frame. All of a caller's callees share one s0, so when the caller has no stop
    // left after the call, finish stops in the next call it makes (docs/DEBUGGER.md).
    case PCX_DBG_STEP_OUT: return s0 > d->s0;
    default: return 0;
  }
}

void pcx_dbg_resume(PcxDbg *d, int mode, int id, uint32_t s0) {
  d->s0 = s0;
  d->line = id >= 0 && (uint32_t)id < d->n ? stopLine(d, id, &d->file) : 0;
  d->mode = mode;
  if (!d->n) return;
  if (mode == PCX_DBG_RUN) memcpy(d->flags, d->bp, d->n);
  else memset(d->flags, 1, d->n);
}

void pcx_dbg_pause(PcxDbg *d) {
  d->mode = PCX_DBG_PAUSE;
  if (d->n) memset(d->flags, 1, d->n);
}
