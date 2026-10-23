// runner.cpp - program task, supervision, exit paths and per-run resource tracking.
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../../board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "../hal/hal.h"
#include "../sdk/picocalc.h"
#include "../sdk/sdk_internal.h"
#include "../cc/cc.h"
#include "exports.h"
#include "module.h"
#include "../ide/doc.h"
#include "runner.h"
#include "selftest.h"

#define INCLUDE_DIR "/sd/pico-dev/include"
#ifndef PROGRAM_STACK
#define PROGRAM_STACK (32 * 1024)
#endif
#ifndef BUILD_STACK
#define BUILD_STACK (32 * 1024)
#endif
#define BREAK_GRACE_MS 500

#define BIT_DONE  1u
#define BIT_BREAK 2u

// State of the one running program. Written by the runner task before it signals BIT_DONE, read
// by the supervisor after.
static struct {
  TaskHandle_t task, supervisor;
  jmp_buf jmp;
  int argc;
  char **argv;
  FILE *out;
  volatile bool breakReq;
  int status;
  RtEnd end;
} g;

static sdk_exit_fn s_prevExit;

static bool inProgram() { return g.task && xTaskGetCurrentTaskHandle() == g.task; }

[[noreturn]] static void endProgram(RtEnd end, int status) {
  g.end = end;
  g.status = status;
  longjmp(g.jmp, 1);
}

// ---- exit paths: exported to programs in place of the firmware's own ----

static void progExit(int status) {
  if (inProgram()) endProgram(RT_END_EXIT, status);
  abort();
}

static void progAbort() {
  if (inProgram()) endProgram(RT_END_ABORT, 134);
  abort();
}

// sys_exit(), and Break seen by the program's own key pump. Other tasks (the demo menu) keep the
// handler they had.
static void sdkExitHandler() {
  if (inProgram()) endProgram(g.breakReq ? RT_END_BREAK : RT_END_EXIT, 0);
  s_prevExit();
}

// Keyboard task: Break pressed while a program runs.
static void onBreak() {
  if (!g.task) return;
  g.breakReq = true;
  xTaskNotify(g.supervisor, BIT_BREAK, eSetBits);
}

// ---- tracked memory ----
// Every block the program allocates carries a header linking it into one list, so whatever is
// left at the end can be freed. The IDF heap only guarantees 4-byte alignment, as does this.

#define BLOCK_MAGIC 0xB10CB10Cu
struct Block {
  Block *prev, *next;
  uint32_t magic;
  uint32_t size;
};
static Block s_blocks = {&s_blocks, &s_blocks, 0, 0};

static void link(Block *b) {
  b->prev = &s_blocks;
  b->next = s_blocks.next;
  s_blocks.next->prev = b;
  s_blocks.next = b;
}
static void unlink(Block *b) {
  b->prev->next = b->next;
  b->next->prev = b->prev;
}

static Block *blockOf(void *p) {
  Block *b = (Block *)p - 1;
  if (((uintptr_t)p & 3) || !(esp_ptr_internal(b) || esp_ptr_external_ram(b)) || b->magic != BLOCK_MAGIC) {
    if (inProgram()) endProgram(RT_END_BAD_FREE, 134);
    return nullptr;
  }
  return b;
}

static void *tMalloc(size_t n) {
  if (n > 0x7FFFFFF0u) return nullptr;
  Block *b = (Block *)malloc(sizeof(Block) + n);
  if (!b) return nullptr;
  b->magic = BLOCK_MAGIC;
  b->size = n;
  link(b);
  return b + 1;
}

static void tFree(void *p) {
  if (!p) return;
  Block *b = blockOf(p);
  if (!b) return;
  unlink(b);
  b->magic = 0;
  free(b);
}

static void *tCalloc(size_t n, size_t size) {
  if (size && n > 0x7FFFFFF0u / size) return nullptr;
  void *p = tMalloc(n * size);
  if (p) memset(p, 0, n * size);
  return p;
}

static void *tRealloc(void *p, size_t n) {
  if (!p) return tMalloc(n);
  if (!n) { tFree(p); return nullptr; }
  Block *b = blockOf(p);
  if (!b || n > 0x7FFFFFF0u) return nullptr;
  unlink(b);
  Block *nb = (Block *)realloc(b, sizeof(Block) + n);
  if (!nb) { link(b); return nullptr; }
  nb->size = n;
  link(nb);
  return nb + 1;
}

static char *tStrdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *d = (char *)tMalloc(n);
  if (d) memcpy(d, s, n);
  return d;
}

// ---- tracked files and directories ----

#define MAX_FILES 32
#define MAX_DIRS 16
static FILE *s_files[MAX_FILES];
static DIR *s_dirs[MAX_DIRS];

static FILE *tFopen(const char *path, const char *mode) {
  FILE *f = fopen(path, mode);
  if (f)
    for (int i = 0; i < MAX_FILES; i++)
      if (!s_files[i]) { s_files[i] = f; break; }
  return f;
}

static int tFclose(FILE *f) {
  for (int i = 0; i < MAX_FILES; i++)
    if (f && s_files[i] == f) s_files[i] = nullptr;
  return fclose(f);
}

static DIR *tOpendir(const char *path) {
  DIR *d = opendir(path);
  if (d)
    for (int i = 0; i < MAX_DIRS; i++)
      if (!s_dirs[i]) { s_dirs[i] = d; break; }
  return d;
}

static int tClosedir(DIR *d) {
  for (int i = 0; i < MAX_DIRS; i++)
    if (d && s_dirs[i] == d) s_dirs[i] = nullptr;
  return closedir(d);
}

static void *resolveOverride(const char *name) {
  static const struct { const char *name; void *addr; } table[] = {
    {"abort", (void *)progAbort},   {"calloc", (void *)tCalloc},     {"closedir", (void *)tClosedir},
    {"exit", (void *)progExit},     {"fclose", (void *)tFclose},     {"fopen", (void *)tFopen},
    {"free", (void *)tFree},        {"malloc", (void *)tMalloc},     {"opendir", (void *)tOpendir},
    {"realloc", (void *)tRealloc},  {"strdup", (void *)tStrdup},
  };
  for (auto &e : table)
    if (!strcmp(name, e.name)) return e.addr;
  return nullptr;
}

// What the program left behind. After a forced delete the task may have died holding a file's
// lock, so files are only forgotten then, not closed (closing could hang).
static void releaseAll(bool killed, RtRunResult *r) {
  while (s_blocks.next != &s_blocks) {
    Block *b = s_blocks.next;
    unlink(b);
    r->freed_blocks++;
    r->freed_bytes += b->size;
    b->magic = 0;
    free(b);
  }
  for (int i = 0; i < MAX_FILES; i++)
    if (s_files[i]) {
      if (!killed) fclose(s_files[i]);
      s_files[i] = nullptr;
      r->closed_files++;
    }
  for (int i = 0; i < MAX_DIRS; i++)
    if (s_dirs[i]) {
      if (!killed) closedir(s_dirs[i]);
      s_dirs[i] = nullptr;
      r->closed_files++;
    }
}

// ---- the program task ----

static void runnerTask(void *arg) {
  RtModule *m = (RtModule *)arg;
  FILE *console = stdout;
  if (g.out) stdout = g.out;
  if (!setjmp(g.jmp)) {
    if (m->main) {
      g.status = m->main(g.argc, g.argv);
      g.end = RT_END_RETURN;
    } else {
      if (m->setup) m->setup();
      for (;;) {
        if (m->loop) m->loop();
        else sys_delay(100);
        sys_yield();
      }
    }
  }
  fflush(stdout);
  stdout = console;
  xTaskNotify(g.supervisor, BIT_DONE, eSetBits);
  for (;;) vTaskDelay(portMAX_DELAY);   // the supervisor deletes this task
}

const char *rt_end_name(RtEnd end) {
  switch (end) {
    case RT_END_RETURN: return "returned";
    case RT_END_EXIT: return "exit";
    case RT_END_ABORT: return "abort";
    case RT_END_BREAK: return "stopped (Break)";
    case RT_END_KILLED: return "killed (Break, no SDK call)";
    case RT_END_TIMEOUT: return "timeout";
    case RT_END_BAD_FREE: return "free() of a bad pointer";
  }
  return "?";
}

static int runImage(const void *image, size_t size, const RtRunOptions *opt, RtRunResult *r) {
  memset(r, 0, sizeof *r);
  if (g.task) { snprintf(r->err, sizeof r->err, "a program is already running"); return -1; }
  static RtModule m;
  if (rt_module_load(image, size, &m, resolveOverride, r->err, sizeof r->err)) return -1;
  if (!m.main && !m.setup && !m.loop) {
    rt_module_unload(&m);
    snprintf(r->err, sizeof r->err, "no main() or setup()/loop()");
    return -1;
  }

  static char *defArgv[] = {(char *)"program", nullptr};
  g.argc = opt->argv ? opt->argc : 1;
  g.argv = opt->argv ? opt->argv : defArgv;
  g.out = opt->out;
  g.breakReq = false;
  g.status = 0;
  g.end = RT_END_RETURN;
  g.supervisor = xTaskGetCurrentTaskHandle();
  xTaskNotifyStateClear(nullptr);
  ulTaskNotifyValueClear(nullptr, 0xFFFFFFFFu);

  if (opt->foreground) sdk_reset();
  s_prevExit = sdk_set_exit_handler(sdkExitHandler);
  // Above the program, on the same core: wakes at once on Break and can delete it while it is
  // not running.
  const UBaseType_t prio = uxTaskPriorityGet(nullptr);
  if (prio < 2) vTaskPrioritySet(nullptr, 2);
  const uint32_t t0 = hal_millis();
  TaskHandle_t task;
  if (xTaskCreatePinnedToCore(runnerTask, "program", PROGRAM_STACK, &m, 1, &task, xPortGetCoreID()) != pdPASS) {
    vTaskPrioritySet(nullptr, prio);
    sdk_set_exit_handler(s_prevExit);
    rt_module_unload(&m);
    snprintf(r->err, sizeof r->err, "cannot start the program task");
    return -1;
  }
  g.task = task;
  hal_set_break_hook(onBreak);

  bool done = false;
  RtEnd stop = RT_END_KILLED;
  for (;;) {
    uint32_t bits = 0;
    TickType_t wait = portMAX_DELAY;
    if (opt->timeout_ms) {
      const int32_t left = (int32_t)(t0 + opt->timeout_ms - hal_millis());
      if (left <= 0) { stop = RT_END_TIMEOUT; break; }
      wait = pdMS_TO_TICKS(left);
    }
    xTaskNotifyWait(0, 0xFFFFFFFFu, &bits, wait);
    if (bits & BIT_DONE) { done = true; break; }
    if (bits & BIT_BREAK) {
      // The program's next kbd/sys call sees the queued Break and exits through sdkExitHandler.
      const uint32_t until = hal_millis() + BREAK_GRACE_MS;
      while (!done && (int32_t)(until - hal_millis()) > 0) {
        bits = 0;
        xTaskNotifyWait(0, 0xFFFFFFFFu, &bits, pdMS_TO_TICKS(until - hal_millis() + 1));
        done = bits & BIT_DONE;
      }
      break;
    }
  }
  hal_set_break_hook(nullptr);
  vTaskDelete(task);   // parked after BIT_DONE, or stuck
  g.task = nullptr;
  r->ms = hal_millis() - t0;
  if (done) {
    r->end = g.end;
    r->status = g.status;
  } else {
    r->end = stop;
    r->status = -1;
  }
  sdk_set_exit_handler(s_prevExit);
  vTaskPrioritySet(nullptr, prio);
  releaseAll(!done, r);
  rt_module_unload(&m);
  if (opt->foreground) sdk_reset();
  return 0;
}

// ---- building ----

static int haveImport(const char *name) { return rt_find_export(name) != nullptr; }

static size_t s_buildPeak;   // cc_arena_peak() of the last rt_build()

uint8_t *rt_build(const char *const *paths, int n, size_t *size) {
  const char *incs[] = {INCLUDE_DIR};
  CcOptions opt = {incs, 1, nullptr, 0, haveImport};
  CcObject **objs = (CcObject **)calloc(n ? n : 1, sizeof *objs);
  uint8_t *image = nullptr;
  bool ok = objs != nullptr;
  for (int i = 0; ok && i < n; i++) ok = cc_compile(&opt, paths[i], &objs[i]) == 0;
  if (ok && cc_link(objs, n, &opt, &image, size) != 0) image = nullptr;
  free(objs);
  s_buildPeak = cc_arena_peak();
  cc_arena_free();
  return image;
}

// ---- @run ----

static char s_runPath[128];
static int s_runTimes;
static volatile bool s_runPending;

int rt_run(const void *image, size_t size, const RtRunOptions *opt, RtRunResult *r) {
#ifdef CC_BORROW_FRAMEBUFFER
  // No PSRAM: the program gets the room of the saved docs too, read back when it ends.
  doc_park_all();
  const int rc = runImage(image, size, opt, r);
  doc_unpark_all();
  return rc;
#else
  return runImage(image, size, opt, r);
#endif
}

bool rt_run_cmd(const char *line) {
  if (strncmp(line, "@run ", 5)) return false;
  if (s_runPending || g.task) { hal_log("@run: busy"); return true; }
  char path[128];
  int times = 1;
  if (sscanf(line + 5, "%127s %d", path, &times) < 1) { hal_log("usage: @run <file.c | dir> [times]"); return true; }
  snprintf(s_runPath, sizeof s_runPath, "%s", path);
  s_runTimes = times < 1 ? 1 : times;
  s_runPending = true;
  hal_log("@run %s: queued (runs when the IDE or demo menu is idle)", path);
  return true;
}

static void logMultiline(const char *s) {
  while (*s) {
    const char *nl = strchr(s, '\n');
    int n = nl ? (int)(nl - s) : (int)strlen(s);
    hal_log("%.*s", n, s);
    s += n + (nl ? 1 : 0);
  }
}

// The compiler recurses deeply: build on a 64 KB stack in internal RAM (flash writes disable
// the cache) rather than on the caller's.
struct BuildJob { const char *const *paths; int n; size_t size; uint8_t *image; TaskHandle_t waiter; };

static void buildTask(void *arg) {
  BuildJob *j = (BuildJob *)arg;
  // The compiler stops at nesting deeper than this; the rest is for rt_build, FatFS and errors.
  cc_stack_limit = BUILD_STACK - 8 * 1024;
  j->image = rt_build(j->paths, j->n, &j->size);
  cc_stack_limit = 0;
  xTaskNotifyGive(j->waiter);
  // The waiter deletes this task: that frees the stack at once, where a self-delete leaves it to
  // the idle task, and the program task about to be created may need the room.
  vTaskSuspend(nullptr);
}

#ifdef CC_BORROW_FRAMEBUFFER
// The biggest block malloc() can give now, to within 1 KB.
static size_t largestBlock() {
  size_t lo = 0, hi = heap_caps_get_free_size(MALLOC_CAP_8BIT) >> 10;
  while (lo < hi) {
    const size_t mid = (lo + hi + 1) / 2;
    void *p = malloc(mid << 10);
    if (p) { free(p); lo = mid; }
    else hi = mid - 1;
  }
  return lo;
}
#endif

#ifdef CC_BORROW_FRAMEBUFFER
static void oomHook(const char *what, size_t n, size_t lentFree, size_t lentLargest) {
  hal_log("build: out of memory for a %u-byte %s; heap %u free, largest block %u KB; lent %u free, "
          "largest %u", (unsigned)n, what, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
          (unsigned)largestBlock(), (unsigned)lentFree, (unsigned)lentLargest);
}
#endif

static RtBuildMemory s_buildMem;

const RtBuildMemory *rt_build_memory() { return &s_buildMem; }

uint8_t *rt_build_big(const char *const *paths, int n, size_t *size) {
#ifdef CC_BORROW_FRAMEBUFFER
  cc_oom_hook = oomHook;
  // The lines of saved docs are read back from the card afterwards, and the room goes to the build.
  const size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  doc_park_all();
  const size_t heap1 = heap_caps_get_free_size(MALLOC_CAP_8BIT), big1 = largestBlock();
  // No PSRAM: the compiler also gets the framebuffer while the caller waits. The screen keeps its
  // picture; the framebuffer comes back cleared, and the caller redraws.
  color_t *fb = gfx_framebuffer();
  const size_t fbSize = SCREEN_W * SCREEN_H * sizeof(color_t);
  cc_arena_lend(fb, fbSize);
#endif
  BuildJob j = {paths, n, 0, nullptr, xTaskGetCurrentTaskHandle()};
  TaskHandle_t task;
  if (xTaskCreatePinnedToCore(buildTask, "ccbuild", BUILD_STACK, &j, uxTaskPriorityGet(nullptr), &task,
                              xPortGetCoreID()) != pdPASS) {
    j.image = rt_build(paths, n, &j.size);
  } else {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (eTaskGetState(task) != eSuspended) vTaskDelay(1);
    vTaskDelete(task);
  }
#ifdef CC_BORROW_FRAMEBUFFER
  const bool lent = j.image && cc_arena_is_lent(j.image);
  cc_arena_lend(nullptr, 0);
  // The image is in the framebuffer, or among the compiler's heap chunks, now free: move it to the
  // bottom of the heap (through the framebuffer) so it doesn't split the room the program needs.
  if (j.image && j.size <= fbSize) {
    memmove(fb, j.image, j.size);
    if (!lent) free(j.image);
    if ((j.image = (uint8_t *)malloc(j.size))) memcpy(j.image, fb, j.size);
    else hal_log("build: out of memory for the %u-byte image", (unsigned)j.size);
  }
  memset(fb, 0, fbSize);
  const size_t lentPeak = cc_lent_peak();
  doc_unpark_all();
  RtBuildMemory &m = s_buildMem;
  m = {true, heap0, heap1, big1 << 10, s_buildPeak, lentPeak, fbSize,
       heap_caps_get_free_size(MALLOC_CAP_8BIT), largestBlock() << 10};
  // Every build, so a near miss shows before it becomes an out-of-memory error.
  hal_log("build memory (%s): heap %u KB free, %u KB after parking docs (largest block %u KB); "
          "compiler peak %u KB, %u of the %u KB framebuffer; after: heap %u KB free (largest block "
          "%u KB) with the %u-byte image", j.image ? "ok" : "failed", (unsigned)(m.heapBefore >> 10),
          (unsigned)(m.heapParked >> 10), (unsigned)(m.largestParked >> 10),
          (unsigned)(m.compilerPeak >> 10), (unsigned)(m.lentPeak >> 10), (unsigned)(m.lentSize >> 10),
          (unsigned)(m.heapAfter >> 10), (unsigned)(m.largestAfter >> 10), (unsigned)j.size);
#endif
  *size = j.size;
  return j.image;
}

static int cmpStr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int rt_list_sources(const char *dir, char **files, int max) {
  int n = 0;
  if (DIR *d = opendir(dir)) {
    while (struct dirent *e = readdir(d)) {
      size_t l = strlen(e->d_name);
      if (l > 2 && !strcmp(e->d_name + l - 2, ".c") && n < max) {
        char p[192];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (!(files[n] = strdup(p))) break;
        n++;
      }
    }
    closedir(d);
  }
  qsort(files, n, sizeof *files, cmpStr);
  return n;
}

bool rt_service() {
  if (rt_selftest_service()) return true;
  if (!s_runPending) return false;
  // A directory is a project: every .c in it.
  char *files[32];
  int n = 0;
  struct stat st;
  if (!stat(s_runPath, &st) && S_ISDIR(st.st_mode)) n = rt_list_sources(s_runPath, files, 32);
  else files[n++] = strdup(s_runPath);

  hal_breadcrumb("@run %s (build)", s_runPath);
  const uint32_t t0 = hal_millis();
  size_t size = 0;
  uint8_t *image = n ? rt_build_big((const char *const *)files, n, &size) : nullptr;
  if (!n) hal_log("@run %s: no .c files", s_runPath);
  else if (!image) { hal_log("@run %s: build failed", s_runPath); logMultiline(cc_error()); }
  else hal_log("@run %s: %d file(s), %u bytes, built in %lu ms", s_runPath, n, (unsigned)size,
               (unsigned long)(hal_millis() - t0));
  for (int i = 0; i < n; i++) free(files[i]);

  const size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  for (int i = 0; image && i < s_runTimes; i++) {
    hal_breadcrumb("@run %s", s_runPath);
    RtRunOptions opt = {0, nullptr, nullptr, true, 0};
    RtRunResult r;
    if (rt_run(image, size, &opt, &r)) { hal_log("@run: %s", r.err); break; }
    hal_log("run %d/%d: %s, status %d, %lu ms; freed %d blocks (%u bytes), closed %d; heap %u (start %u)",
            i + 1, s_runTimes, rt_end_name(r.end), r.status, (unsigned long)r.ms, r.freed_blocks,
            (unsigned)r.freed_bytes, r.closed_files, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
            (unsigned)heap0);
  }
  free(image);
  hal_breadcrumb("menu");
  s_runPending = false;
  return true;
}
