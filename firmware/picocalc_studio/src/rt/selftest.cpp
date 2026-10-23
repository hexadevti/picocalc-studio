// selftest.cpp - serial "@test [filter]": the compiler test suite, run on the device.
//
// Compiles every /sd/pico-dev/tests/unit/*.c with the on-device compiler, runs it from flash and
// compares its stdout with the matching .expected file; then checks that every
// /sd/pico-dev/tests/errors/*.c is rejected as its "// EXPECT:" header says (see
// tests/errors/README.md). A filter runs only the files whose name contains it. Results go to the
// serial log and to /sd/pico-dev/tests/results.txt. Copy tests/unit and tests/errors there first.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include "../../board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "../hal/hal.h"
#include "../cc/cc.h"
#include "exports.h"
#include "runner.h"
#include "selftest.h"

#define TESTS_DIR "/sd/pico-dev/tests"
#define INCLUDE_DIR "/sd/pico-dev/include"
#ifdef CC_BORROW_FRAMEBUFFER
#define MAX_OUTPUT (8 * 1024)   // no PSRAM: 64 KB in one piece is rarely free after a build
#else
#define MAX_OUTPUT (64 * 1024)
#endif
#define TEST_TIMEOUT_MS 20000

static TaskHandle_t s_task;
static char s_filter[64];
static FILE *s_report;
static int s_pass, s_fail;

static void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void report(const char *fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  hal_log("%s", b);
  if (s_report) { fputs(b, s_report); fputc('\n', s_report); fflush(s_report); }
}

// ---- captured output ----

struct Capture { char *buf; size_t len; };

static ssize_t capWrite(void *cookie, const char *data, size_t n) {
  Capture *c = (Capture *)cookie;
  size_t room = MAX_OUTPUT - c->len;
  size_t k = n < room ? n : room;
  memcpy(c->buf + c->len, data, k);
  c->len += k;
  return (ssize_t)n;
}

// ---- helpers ----

static char *readFile(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
  if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = nullptr; }
  fclose(f);
  if (b) { b[n] = 0; if (len) *len = n; }
  return b;
}

static void stripCr(char *s) {
  char *d = s;
  for (; *s; s++) if (*s != '\r') *d++ = *s;
  *d = 0;
}

static int cmpName(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// Sorted *.c files in dir that match the filter; caller frees each name and the array.
static int listSources(const char *dir, char ***out) {
  DIR *d = opendir(dir);
  int n = 0, cap = 0;
  char **names = nullptr;
  if (d) {
    while (struct dirent *e = readdir(d)) {
      size_t l = strlen(e->d_name);
      if (l < 3 || strcmp(e->d_name + l - 2, ".c") || (s_filter[0] && !strstr(e->d_name, s_filter))) continue;
      if (n == cap) names = (char **)realloc(names, (cap = cap ? cap * 2 : 32) * sizeof *names);
      names[n++] = strdup(e->d_name);
    }
    closedir(d);
  }
  if (n) qsort(names, n, sizeof *names, cmpName);
  *out = names;
  return n;
}

#ifdef CC_BORROW_FRAMEBUFFER
// No PSRAM: builds borrow the framebuffer, so the suite runs while the IDE waits (rt_service).
static volatile bool s_pending;
static uint8_t *build(const char *path, size_t *size) { return rt_build_big(&path, 1, size); }
#else
static uint8_t *build(const char *path, size_t *size) { return rt_build(&path, 1, size); }
#endif

// ---- unit tests ----

static void unitTest(const char *name) {
  char path[160], exp[160], err[160];
  snprintf(path, sizeof path, TESTS_DIR "/unit/%s", name);
  snprintf(exp, sizeof exp, "%.*s.expected", (int)(strlen(path) - 2), path);
  hal_breadcrumb("@test %s", name);

  uint32_t t0 = hal_millis();
  size_t size;
  uint8_t *image = build(path, &size);
  uint32_t tc = hal_millis() - t0;
  if (!image) { report("FAIL %s: compile: %s", name, cc_error()); s_fail++; return; }

  Capture cap = {(char *)heap_caps_malloc(MAX_OUTPUT + 1, MALLOC_CAP_SPIRAM), 0};
  cookie_io_functions_t io = {nullptr, capWrite, nullptr, nullptr};
  FILE *out = cap.buf ? fopencookie(&cap, "w", io) : nullptr;
  if (!out) { report("FAIL %s: out of memory", name); free(cap.buf); free(image); s_fail++; return; }
  setvbuf(out, nullptr, _IOFBF, 1024);
  char *argv[2] = {path, nullptr};
  RtRunOptions opt = {1, argv, out, false, TEST_TIMEOUT_MS};
  RtRunResult r;
  int loaded = rt_run(image, size, &opt, &r);
  // After a forced stop the program may have died inside a write to `out`: leave it open.
  if (loaded || (r.end != RT_END_KILLED && r.end != RT_END_TIMEOUT)) fclose(out);
  free(image);
  if (loaded) { report("FAIL %s: load: %s", name, r.err); free(cap.buf); s_fail++; return; }
  const uint32_t tr = r.ms;
  const int rc = r.end == RT_END_RETURN || r.end == RT_END_EXIT ? r.status : -1;
  cap.buf[cap.len] = 0;

  char *want = nullptr;
#ifdef CC_BORROW_FRAMEBUFFER
  // No PSRAM: a test whose output depends on it has a <name>.nopsram.expected.
  char alt[176];
  snprintf(alt, sizeof alt, "%.*s.nopsram.expected", (int)(strlen(path) - 2), path);
  want = readFile(alt, nullptr);
#endif
  if (!want) want = readFile(exp, nullptr);
  if (want) stripCr(want);
  bool ok = false;
  if (!want) report("FAIL %s: no %s", name, exp);
  else if (rc != 0) report("FAIL %s: %s, status %d", name, rt_end_name(r.end), r.status);
  else if (strcmp(cap.buf, want)) {
    int line = 1;
    const char *a = cap.buf, *b = want;
    while (*a && *a == *b) { if (*a == '\n') line++; a++; b++; }
    const char *ae = strchr(a, '\n'), *be = strchr(b, '\n');
    report("FAIL %s: output differs at line %d", name, line);
    report("  got:  %.*s", ae ? (int)(ae - a) : 120, a);
    report("  want: %.*s", be ? (int)(be - b) : 120, b);
  } else {
    report("PASS %s  (%u bytes, compile %lu ms, run %lu ms%s)", name, (unsigned)size, (unsigned long)tc,
           (unsigned long)tr, r.freed_blocks || r.closed_files ? ", left allocations/files released" : "");
    ok = true;
  }
  if (ok) s_pass++;
  else s_fail++;
  free(want);
  free(cap.buf);
}

// ---- must-fail tests ----

// The EXPECT patterns are case-insensitive regexes with only |, ( ) and literals: expand them into
// the literal alternatives and look for each as a substring.
static int expandAlts(const char *p, const char *end, char out[][64], int max);

static int expandSeq(const char *p, const char *end, char out[][64], int max) {
  int n = 1;
  out[0][0] = 0;
  while (p < end) {
    if (*p == '(') {
      int depth = 1;
      const char *q = p + 1;
      while (q < end && depth) { if (*q == '(') depth++; else if (*q == ')') depth--; q++; }
      char sub[16][64];
      int k = expandAlts(p + 1, q - 1, sub, 16);
      int m = 0;
      char (*next)[64] = (char (*)[64])malloc(32 * 64);   // not static: builds need the RAM
      if (!next) return n;
      for (int i = 0; i < n; i++)
        for (int j = 0; j < k && m < 32; j++) snprintf(next[m++], 64, "%s%s", out[i], sub[j]);
      n = m < max ? m : max;
      for (int i = 0; i < n; i++) strcpy(out[i], next[i]);
      free(next);
      p = q;
    } else {
      for (int i = 0; i < n; i++) {
        size_t l = strlen(out[i]);
        if (l < 63) { out[i][l] = *p; out[i][l + 1] = 0; }
      }
      p++;
    }
  }
  return n;
}

static int expandAlts(const char *p, const char *end, char out[][64], int max) {
  int n = 0;
  while (p <= end && n < max) {
    int depth = 0;
    const char *q = p;
    while (q < end && (depth || *q != '|')) { if (*q == '(') depth++; else if (*q == ')') depth--; q++; }
    char seq[16][64];
    int k = expandSeq(p, q, seq, 16);
    for (int i = 0; i < k && n < max; i++) strcpy(out[n++], seq[i]);
    p = q + 1;
  }
  return n;
}

static bool matches(const char *msg, const char *pattern) {
  char alts[32][64];
  int n = expandAlts(pattern, pattern + strlen(pattern), alts, 32);
  for (int i = 0; i < n; i++)
    if (alts[i][0] && strcasestr(msg, alts[i])) return true;
  return false;
}

static void errorTest(const char *name) {
  char path[160];
  snprintf(path, sizeof path, TESTS_DIR "/errors/%s", name);
  hal_breadcrumb("@test %s", name);
  char *src = readFile(path, nullptr);
  char stage[16] = "", lines[32] = "", pattern[128] = "";
  const char *h = src ? strstr(src, "EXPECT:") : nullptr;
  if (!h || sscanf(h, "EXPECT: %15s line=%31s match=%127[^\r\n]", stage, lines, pattern) != 3) {
    report("FAIL %s: no EXPECT header", name);
    free(src);
    s_fail++;
    return;
  }
  free(src);

  size_t size;
  uint8_t *image = build(path, &size);
  if (image) {
    free(image);
    report("FAIL %s: compiled, but must fail", name);
    s_fail++;
    return;
  }
  const char *msg = cc_error();
  // Which stage failed: link errors come from cc_link(), which never says "file:line:col".
  // Not checked beyond the message and the line, as on the host.
  bool lineOk = !strcmp(lines, "0");
  const char *at = strstr(msg, name);
  int got = at && at[strlen(name)] == ':' ? atoi(at + strlen(name) + 1) : 0;
  for (char *tok = strtok(lines, "|"); tok && !lineOk; tok = strtok(nullptr, "|")) lineOk = atoi(tok) == got;
  const char *nl = strchr(msg, '\n');
  if (!lineOk) report("FAIL %s: reported line %d, want %s: %.*s", name, got, lines, nl ? (int)(nl - msg) : 200, msg);
  else if (!matches(msg, pattern)) report("FAIL %s: message doesn't match /%s/: %.*s", name, pattern, nl ? (int)(nl - msg) : 200, msg);
  else { report("PASS %s  (%.*s)", name, nl ? (int)(nl - msg) : 200, msg); s_pass++; return; }
  s_fail++;
}

// ---- task ----

static void runSuite() {
  s_pass = s_fail = 0;
  size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  uint32_t t0 = hal_millis();
  s_report = fopen(TESTS_DIR "/results.txt", "w");
  report("=== @test%s%s: on-device compiler suite ===", s_filter[0] ? " " : "", s_filter);

  const char *dirs[] = {TESTS_DIR "/unit", TESTS_DIR "/errors"};
  for (int d = 0; d < 2; d++) {
    char **names;
    int n = listSources(dirs[d], &names);
    if (!n && !s_filter[0]) report("no tests in %s", dirs[d]);
    for (int i = 0; i < n; i++) {
      if (d == 0) unitTest(names[i]);
      else errorTest(names[i]);
      free(names[i]);
      vTaskDelay(1);
    }
    free(names);
  }
  hal_breadcrumb("@test done");
  report("=== %d passed, %d failed, %lu ms; heap %u KB free (was %u KB) ===", s_pass, s_fail,
         (unsigned long)(hal_millis() - t0), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) >> 10),
         (unsigned)(heap0 >> 10));
  if (s_report) fclose(s_report);
  s_report = nullptr;
}

static void testTask(void *) {
  runSuite();
  s_task = nullptr;
  vTaskDelete(nullptr);
}

bool rt_selftest_service() {
#ifdef CC_BORROW_FRAMEBUFFER
  if (!s_pending) return false;
  runSuite();
  s_pending = false;
  return true;
#else
  return false;
#endif
}

static bool onSerialCmd(const char *line) {
  if (rt_run_cmd(line)) return true;
  if (strncmp(line, "@test", 5) || (line[5] && line[5] != ' ')) return false;
  if (s_task) { hal_log("@test is already running"); return true; }
  const char *f = line + 5;
  while (*f == ' ') f++;
#ifdef CC_BORROW_FRAMEBUFFER
  if (s_pending) { hal_log("@test is already running"); return true; }
  snprintf(s_filter, sizeof s_filter, "%s", f);
  s_pending = true;
  hal_log("@test: queued (runs when the IDE or demo menu is idle)");
  return true;
#endif
  snprintf(s_filter, sizeof s_filter, "%s", f);
  // The compiler recurses deeply and the tests have big frames: a large stack, in internal RAM
  // (flash writes disable the cache, so it can't live in PSRAM). Core 1, away from the idle-task
  // watchdog on core 0.
  if (xTaskCreatePinnedToCore(testTask, "cctest", 64 * 1024, nullptr, 1, &s_task, 1) != pdPASS &&
      xTaskCreatePinnedToCore(testTask, "cctest", 40 * 1024, nullptr, 1, &s_task, 1) != pdPASS) {
    s_task = nullptr;
    hal_log("@test: cannot start the test task");
  }
  return true;
}

// Also routes "@run" to the runner.
void rt_selftest_install() { hal_set_serial_cmd_hook(onSerialCmd); }
