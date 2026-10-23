// host_rt.cpp - the runtime pieces the IDE calls, for the screenshot harness: builds go through the
// real compiler (src/cc, RV32 output); running, exporting, sound and the SDK demo are stubs.
#include "../../../firmware/picocalc_studio/src/rt/runner.h"
#include "../../../firmware/picocalc_studio/src/export/export.h"
#include "../../../firmware/picocalc_studio/src/cc/cc.h"
#include "../../../firmware/picocalc_studio/src/sdk/picocalc.h"
#include "../../../firmware/picocalc_studio/src/demo/demo.h"

// ---- building ----

static char *s_exports[1024];
static int s_nexports;

// The firmware's export table (rt/exports_table.S): what a program may call.
static void loadExports() {
  if (s_nexports) return;
  char path[512];
  snprintf(path, sizeof path, "%s/firmware/picocalc_studio/src/rt/exports_table.S", getenv("PICOCALC_REPO"));
  FILE *f = fopen(path, "r");
  if (!f) { fprintf(stderr, "can't open %s\n", path); exit(2); }
  char line[256];
  while (fgets(line, sizeof line, f) && s_nexports < 1024) {
    char *q = strstr(line, ".asciz \"");
    if (!q) continue;
    q += 8;
    if (char *e = strchr(q, '"')) { *e = 0; s_exports[s_nexports++] = strdup(q); }
  }
  fclose(f);
}

static int haveImport(const char *name) {
  for (int i = 0; i < s_nexports; i++)
    if (!strcmp(s_exports[i], name)) return 1;
  return 0;
}

uint8_t *rt_build(const char *const *paths, int n, size_t *size) {
  loadExports();
  const char *incs[] = {"/sd/picocalc-studio/include"};
  CcOptions opt = {incs, 1, nullptr, 0, haveImport};
  CcObject **objs = (CcObject **)calloc(n ? n : 1, sizeof *objs);
  uint8_t *image = nullptr;
  bool ok = true;
  for (int i = 0; ok && i < n; i++) ok = cc_compile(&opt, paths[i], &objs[i]) == 0;
  if (ok && cc_link(objs, n, &opt, &image, size) != 0) image = nullptr;
  free(objs);
  cc_arena_free();
  return image;
}

uint8_t *rt_build_big(const char *const *paths, int n, size_t *size) { return rt_build(paths, n, size); }

const RtBuildMemory *rt_build_memory() {
  static RtBuildMemory m;   // not measured: the PC has room to spare, like PSRAM
  return &m;
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
        files[n++] = strdup(p);
      }
    }
    closedir(d);
  }
  qsort(files, n, sizeof *files, cmpStr);
  return n;
}

// ---- running and exporting: not on the PC ----

int rt_run(const void *, size_t, const RtRunOptions *, RtRunResult *r) {
  memset(r, 0, sizeof *r);
  snprintf(r->err, sizeof r->err, "the screenshot harness doesn't run programs");
  return -1;
}

const char *rt_end_name(RtEnd) { return "?"; }
bool rt_run_cmd(const char *) { return false; }
bool rt_service() { return false; }

long ex_export(const void *, size_t, const char *, const char *, char *err, int errlen) {
  snprintf(err, errlen, "the screenshot harness doesn't export");
  return -1;
}
bool ex_is_app() { return false; }
void ex_run_app() { exit(0); }

void demo_run() {}
