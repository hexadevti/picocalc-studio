// ide.cpp - see ide.h. A project is a folder of .c/.h files; every .c in it is compiled and
// linked. Building saves all changed files first. The last project, file and line are kept in
// /sd/picocalc-studio/ide.cfg so the IDE reopens them.
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "ide.h"
#include "doc.h"
#include "editor.h"
#include "complete.h"
#include "../ui/ui.h"
#include "../sdk/sdk_internal.h"
#include "../hal/hal.h"
#include "../rt/runner.h"
#include "../cc/cc.h"
#include "../demo/demo.h"
#include "../export/export.h"
#include "../../version.h"

extern "C" char *cc_normpath(char *path);   // cc/alloc.c

#define ROOT_DIR     "/sd/picocalc-studio"
#define PROJECTS_DIR "/sd/picocalc-studio/projects"
#define EXAMPLES_DIR "/sd/picocalc-studio/examples"
#define INCLUDE_DIR  "/sd/picocalc-studio/include"
#define LIB_DIR      "/sd/picocalc-studio/lib"      // LIB_DIR/<x>/: the sources of the library <x.h>
#define CFG_PATH     "/sd/picocalc-studio/ide.cfg"
#define APPS_DIR     "/sd/picocalc-studio/apps"   // default export folder (F5)
#define MAX_DOCS     12
#define MAX_ITEMS    96
#define MAX_REFS     48
#define MAX_OTHER    32      // files of other projects a project includes
#define MAX_BACK     16
#define OUT_MAX      (16 * 1024)
#define PANEL_ROWS   7
#define HINT_COLOR   RGB(230, 230, 160)
#define REF_COLOR    RGB(120, 200, 230)


static jmp_buf s_jmp;
static char s_proj[160];                 // open project folder, "" if none
static Doc *s_docs[MAX_DOCS];
static int s_ndocs, s_cur;
static char s_out[OUT_MAX + 1];          // build and program output
static int s_outLen;
static bool s_panel;                     // output panel under the editor: build, errors, program output
static char s_msg[100];                  // status bar message
static color_t s_msgColor;
static char s_errPath[192];              // line marked red: last error or crash
static int s_errLine = -1;
static char s_find[64];
static char s_repl[64];
static char s_battery[16];
static uint32_t s_batteryAt;

struct Place {
  char path[192];
  int y, x;
};
static Place s_back[MAX_BACK];           // where F9 (or opening a reference) went from, for Esc
static int s_nback;
static Place s_lastEdit;                 // where the last edit left the cursor, for F8

static void pushBack(const Doc *d) {
  if (s_nback == MAX_BACK) memmove(s_back, s_back + 1, --s_nback * sizeof *s_back);
  Place &p = s_back[s_nback++];
  snprintf(p.path, sizeof p.path, "%s", d->path);
  p.y = d->cy;
  p.x = d->cx;
}

// ---- small helpers ----

static void setMsg(color_t c, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_msg, sizeof s_msg, fmt, ap);
  va_end(ap);
  s_msgColor = c;
}

static const char *baseName(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

static bool isDir(const char *path) {
  struct stat st;
  return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static int cmpStr(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static Doc *cur() { return s_ndocs ? s_docs[s_cur] : nullptr; }

static int editRows() {
  const int h = SCREEN_H - 2 * UI_BAR_H - (s_panel ? PANEL_ROWS * UI_CH + 4 : 0);
  return h / UI_CH;
}

// ---- output ----

static void outAppend(const char *s, size_t n) {
  if (n > OUT_MAX / 2) { s += n - OUT_MAX / 2; n = OUT_MAX / 2; }
  if (s_outLen + n > OUT_MAX) {   // drop the older half, at a line start
    const char *keep = s_out + s_outLen - OUT_MAX / 2;
    const char *nl = (const char *)memchr(keep, '\n', s_out + s_outLen - keep);
    if (nl) keep = nl + 1;
    s_outLen = (int)(s_out + s_outLen - keep);
    memmove(s_out, keep, s_outLen);
  }
  memcpy(s_out + s_outLen, s, n);
  s_outLen += (int)n;
  s_out[s_outLen] = 0;
}

static void outPrintf(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  outAppend(buf, n < (int)sizeof buf ? n : sizeof buf - 1);
}

static void outClear() { s_outLen = 0; s_out[0] = 0; }

static void diagHook(const char *msg) {
  outAppend(msg, strlen(msg));
  if (*msg && msg[strlen(msg) - 1] != '\n') outAppend("\n", 1);
}

static ssize_t capWrite(void *, const char *buf, size_t n) {
  outAppend(buf, n);
  return n;
}

// ---- settings ----

static char s_exportDir[128] = APPS_DIR;   // where F5 last exported to

static void cfgSave() {
  Doc *d = cur();
  if (!s_proj[0]) return;
  if (FILE *f = fopen(CFG_PATH, "w")) {
    fprintf(f, "project=%s\nfile=%s\nline=%d\nexportdir=%s\n", s_proj, d ? d->path : "", d ? d->cy + 1 : 1,
            s_exportDir);
    fclose(f);
  }
}

static void cfgLoad(char *proj, char *file, int *line) {
  proj[0] = file[0] = 0;
  *line = 1;
  FILE *f = fopen(CFG_PATH, "r");
  if (!f) return;
  char l[220];
  while (fgets(l, sizeof l, f)) {
    l[strcspn(l, "\r\n")] = 0;
    if (!strncmp(l, "project=", 8)) snprintf(proj, 160, "%s", l + 8);
    else if (!strncmp(l, "file=", 5)) snprintf(file, 192, "%s", l + 5);
    else if (!strncmp(l, "line=", 5)) *line = atoi(l + 5);
    else if (!strncmp(l, "exportdir=", 10) && l[10]) snprintf(s_exportDir, sizeof s_exportDir, "%s", l + 10);
  }
  fclose(f);
}

// ---- documents ----

static int findDoc(const char *path) {
  for (int i = 0; i < s_ndocs; i++)
    if (!strcmp(s_docs[i]->path, path)) return i;
  return -1;
}

static void closeDoc(int i) {
  doc_free(s_docs[i]);
  memmove(&s_docs[i], &s_docs[i + 1], (s_ndocs - i - 1) * sizeof *s_docs);
  s_ndocs--;
  if (s_cur >= i && s_cur > 0) s_cur--;
}

static bool inProject(const char *path) {
  const size_t l = strlen(s_proj);
  return !strncmp(path, s_proj, l) && path[l] == '/';
}

// An example opened as it is (not a copy): it can be built and run, not changed.
static bool isExample() { return !strncmp(s_proj, EXAMPLES_DIR "/", sizeof EXAMPLES_DIR); }

// A reference file: outside the project (a header or library source F9 went to).
static bool isRef(const Doc *d) { return !inProject(d->path); }

static void readOnlyMsg(const Doc *d) {
  if (isRef(d)) setMsg(YELLOW, "%s is a library file: read-only", baseName(d->path));
  else setMsg(YELLOW, "Examples are read-only: \"+ New from example\" copies one");
}

// Opens (or switches to) a file. A relative path is inside the project; a file outside it (a
// library header F9 went to) is read-only, and so is all of an example.
static bool openFile(const char *path) {
  char full[192];
  if (path[0] == '/') snprintf(full, sizeof full, "%s", path);
  else snprintf(full, sizeof full, "%s/%s", s_proj, path);
  int i = findDoc(full);
  if (i >= 0) { s_cur = i; return true; }
  if (s_ndocs == MAX_DOCS) {   // make room: drop an unchanged file that isn't shown
    for (i = 0; i < s_ndocs; i++)
      if (i != s_cur && !s_docs[i]->modified) { closeDoc(i); break; }
    if (s_ndocs == MAX_DOCS) { setMsg(RED, "Too many changed files open: save first"); return false; }
  }
  Doc *d = doc_open(full);
  if (!d) {   // say why: doc_open() can't tell
    struct stat st;
    FILE *f = fopen(full, "rb");
    const int err = f ? 0 : errno;
    if (f) fclose(f);
    char why[80];
    if (stat(full, &st)) snprintf(why, sizeof why, "not found");
    else if (err) snprintf(why, sizeof why, "%s", strerror(err));
    else snprintf(why, sizeof why, "out of memory for %ld bytes, %u KB free", (long)st.st_size,
                  (unsigned)(hal_free_heap() >> 10));
    hal_log("open %s failed: %s", full, why);
    setMsg(RED, "Can't open %s: %s", baseName(full), why);
    return false;
  }
  d->readOnly = !inProject(full) || isExample();
  s_docs[s_ndocs] = d;
  s_cur = s_ndocs++;
  return true;
}

static void reindex();
static int otherFiles(char **out);

static bool saveAll() {
  for (int i = 0; i < s_ndocs; i++)
    if (s_docs[i]->modified && !doc_save(s_docs[i])) {
      setMsg(RED, "Can't save %s", baseName(s_docs[i]->path));
      return false;
    }
  reindex();   // the saved text is the project's names now
  return true;
}

static bool anyModified() {
  for (int i = 0; i < s_ndocs; i++)
    if (s_docs[i]->modified) return true;
  return false;
}

// Closes the project, asking about unsaved changes. False if the user cancelled.
static bool closeProject() {
  if (anyModified()) {
    char q[96];
    snprintf(q, sizeof q, "Save changes to %s? (Y/N)", baseName(s_proj));
    const int a = ui_ask(q, "yn");
    if (a == KEY_ESC) return false;
    if (a == 'y' && !saveAll()) return false;
  }
  while (s_ndocs) closeDoc(s_ndocs - 1);
  s_cur = 0;
  s_proj[0] = 0;
  s_errPath[0] = 0;
  s_nback = 0;
  ac_project(INCLUDE_DIR, LIB_DIR, "");
  ac_extra(nullptr, 0);
  return true;
}

// Opens a project folder and `file` in it, or its main.c / first source file.
static bool openProject(const char *dir, const char *file) {
  snprintf(s_proj, sizeof s_proj, "%s", dir);
  reindex();
  if (file && *file && openFile(file)) return true;
  char main[192];
  snprintf(main, sizeof main, "%s/main.c", dir);
  if (openFile(main)) return true;
  char *files[1];
  if (rt_list_sources(dir, files, 1)) {
    const bool ok = openFile(files[0]);
    free(files[0]);
    if (ok) return true;
  }
  // An empty project: start its main.c.
  Doc *d = doc_new(main, "#include <picocalc.h>\n\nvoid setup(void)\n{\n}\n\nvoid loop(void)\n{\n}\n");
  if (!d) { s_proj[0] = 0; return false; }
  d->modified = true;
  s_docs[s_ndocs] = d;
  s_cur = s_ndocs++;
  return true;
}

// ---- location parsing ----

// "path:line[:col]..." -> path, 1-based line and col (col 0 if absent). False if there's no line.
static bool parseLoc(const char *s, char *path, int pathSize, int *line, int *col) {
  const char *eol = strchr(s, '\n');
  if (!eol) eol = s + strlen(s);
  for (const char *p = strchr(s, ':'); p && p < eol; p = strchr(p + 1, ':')) {
    if (!isdigit((unsigned char)p[1])) continue;
    char *e;
    *line = (int)strtol(p + 1, &e, 10);
    *col = *e == ':' && isdigit((unsigned char)e[1]) ? (int)strtol(e + 1, nullptr, 10) : 0;
    const int n = (int)(p - s) < pathSize - 1 ? (int)(p - s) : pathSize - 1;
    memcpy(path, s, n);
    path[n] = 0;
    return *line > 0;
  }
  return false;
}

// Opens the file of "path:line:col: ..." at that place and marks the line.
static bool jumpTo(const char *loc) {
  char path[192];
  int line, col;
  if (!parseLoc(loc, path, sizeof path, &line, &col)) return false;
  if (!openFile(path)) return false;
  Doc *d = cur();
  ed_goto(d, line - 1, col > 0 ? col - 1 : doc_indent(d, line - 1 < d->n ? line - 1 : d->n - 1), editRows());
  snprintf(s_errPath, sizeof s_errPath, "%s", d->path);
  s_errLine = line - 1;
  return true;
}

// ---- build and run ----

static void drawAll();

// Saves, then compiles and links every .c of the project. Null on errors (shown and jumped to).
static uint8_t *build(size_t *size) {
  if (!saveAll()) return nullptr;
  s_errPath[0] = 0;
  outClear();
  char *files[32 + MAX_OTHER];
  int n = rt_list_sources(s_proj, files, 32);
  if (!n) { setMsg(RED, "No .c files in %s", baseName(s_proj)); return nullptr; }
  char *other[MAX_OTHER];   // and the .c files of other projects it includes
  const int no = otherFiles(other);
  for (int i = 0; i < no; i++) {
    const size_t l = strlen(other[i]);
    if (l > 2 && !strcmp(other[i] + l - 2, ".c")) files[n++] = other[i];
    else free(other[i]);
  }
  setMsg(YELLOW, "Building...");
  drawAll();
  outPrintf("Building %s: %d file%s\n", baseName(s_proj), n, n == 1 ? "" : "s");
  hal_breadcrumb("build %s", s_proj);
  cc_diag_hook = diagHook;
  const uint32_t t0 = sys_millis();
  uint8_t *image = rt_build_big((const char *const *)files, n, size);
  const uint32_t ms = sys_millis() - t0;
  cc_diag_hook = nullptr;
  for (int i = 0; i < n; i++) free(files[i]);
  hal_breadcrumb("ide");
  if (const RtBuildMemory *m = rt_build_memory(); m->valid)
    outPrintf("Memory: %u KB free, %u KB for the build (block %u KB)\n"
              "  compiler peak %u KB, framebuffer %u of %u KB\n"
              "  after: %u KB free (block %u KB)\n",
              (unsigned)(m->heapBefore >> 10), (unsigned)(m->heapParked >> 10),
              (unsigned)(m->largestParked >> 10), (unsigned)(m->compilerPeak >> 10),
              (unsigned)(m->lentPeak >> 10), (unsigned)(m->lentSize >> 10),
              (unsigned)(m->heapAfter >> 10), (unsigned)(m->largestAfter >> 10));
  if (!image) {
    const char *e = cc_error();
    outPrintf("%s\n", e);
    hal_log("build %s failed: %s", s_proj, e);
    const char *msg = strstr(e, "error: ");
    const char *eol = strchr(e, '\n');
    const char *m = msg && (!eol || msg < eol) ? msg + 7 : e;
    const int len = (int)(eol && eol > m ? eol - m : strlen(m));
    jumpTo(e);
    setMsg(RED, "%.*s", len, m);
    s_panel = true;
    return nullptr;
  }
  outPrintf("OK: %u bytes, %lu ms\n", (unsigned)*size, (unsigned long)ms);
  hal_log("build %s: %u bytes, %lu ms", s_proj, (unsigned)*size, (unsigned long)ms);
  setMsg(GREEN, "Build OK: %u bytes, %lu ms", (unsigned)*size, (unsigned long)ms);
  s_panel = true;   // with the memory status; any key hides it
  return image;
}

// F4, or Ctrl-F4 for a debug run (docs/DEBUGGER.md): a debug build that stops at the breakpoints
// of the open files (Ctrl-B) and on Break.
static void run(bool debug) {
  size_t size;
  cc_debug = debug;
  uint8_t *image = build(&size);
  cc_debug = 0;
  if (!image) return;
  cfgSave();   // a crash reboots: come back to this project
  static RtBreakpoint bps[MAX_DOCS * DOC_MAX_BREAKS];
  int nbps = 0;
  for (int i = 0; debug && i < s_ndocs; i++)
    for (int j = 0; j < s_docs[i]->nbp; j++) bps[nbps++] = {s_docs[i]->path, s_docs[i]->bp[j] + 1};
  if (debug) outPrintf("Debugging %s: %d breakpoint%s\n", baseName(s_proj), nbps, nbps == 1 ? "" : "s");
  else outPrintf("Running %s\n", baseName(s_proj));
  const int out0 = s_outLen;
  cookie_io_functions_t io = {nullptr, capWrite, nullptr, nullptr};
  FILE *out = fopencookie(nullptr, "w", io);
  if (out) setvbuf(out, nullptr, _IOLBF, 256);
  char name[64];
  snprintf(name, sizeof name, "%s", baseName(s_proj));
  char *argv[2] = {name, nullptr};
  RtRunOptions opt = {1, argv, out, true, 0, debug, bps, nbps};
  RtRunResult r;
  hal_breadcrumb("run %s", s_proj);
  const int rc = rt_run(image, size, &opt, &r);
  hal_breadcrumb("ide");
  // After a forced stop the program may have died inside a write to `out`: leave it open.
  if (out && (rc || (r.end != RT_END_KILLED && r.end != RT_END_TIMEOUT))) fclose(out);
  free(image);
  const bool printed = s_outLen > out0;
  if (rc) {
    outPrintf("Can't load the program: %s\n", r.err);
    setMsg(RED, "Can't load: %s", r.err);
    s_panel = true;
    return;
  }
  bool bad = false;
  switch (r.end) {
    case RT_END_RETURN: case RT_END_EXIT:
      setMsg(r.status ? YELLOW : GREEN, "Program ended, status %d (%lu ms)", r.status, (unsigned long)r.ms);
      break;
    case RT_END_BREAK: setMsg(LIGHTGRAY, "Program stopped (Break)"); break;
    case RT_END_KILLED: setMsg(YELLOW, "Program stopped by force: it didn't respond to Break"); bad = true; break;
    case RT_END_ABORT: setMsg(RED, "Program called abort()"); bad = true; break;
    case RT_END_BAD_FREE: setMsg(RED, "Program freed a bad pointer"); bad = true; break;
    case RT_END_TIMEOUT: setMsg(YELLOW, "Program timed out"); bad = true; break;
  }
  outPrintf("%s\n", s_msg);
  if (r.err[0]) outPrintf("%s\n", r.err);
  if (r.freed_blocks || r.closed_files)
    outPrintf("Released what it left: %d blocks (%u bytes), %d files\n", r.freed_blocks,
              (unsigned)r.freed_bytes, r.closed_files);
  hal_log("run %s: %s, status %d, %lu ms", s_proj, rt_end_name(r.end), r.status, (unsigned long)r.ms);
  if (printed || bad) s_panel = true;
}

// Creates dir and its missing parents.
static bool mkdirs(const char *dir) {
  char p[160];
  snprintf(p, sizeof p, "%s", dir);
  for (char *s = p + 1; *s; s++)
    if (*s == '/') { *s = 0; mkdir(p, 0777); *s = '/'; }
  return !mkdir(p, 0777) || isDir(p);
}

// Asks for the export folder (prefilled with the last one; empty means APPS_DIR), builds, then
// writes the firmware with the program in its export slot to <folder>/<project>.bin (.uf2 on the
// RP2350).
static void exportApp() {
  char dir[128];
  snprintf(dir, sizeof dir, "%s", s_exportDir);
  if (!ui_input("Export", "Export folder (empty: " APPS_DIR "):", dir, sizeof dir)) return;
  size_t len = strlen(dir);
  while (len > 1 && dir[len - 1] == '/') dir[--len] = 0;
  if (!dir[0]) snprintf(dir, sizeof dir, "%s", APPS_DIR);
  if (dir[0] != '/') { ui_message("Use a full path, like " APPS_DIR); return; }
  if (!mkdirs(dir)) { ui_message("Can't create the folder (is the SD card in?)"); return; }
  snprintf(s_exportDir, sizeof s_exportDir, "%s", dir);
  cfgSave();
  size_t size;
  uint8_t *image = build(&size);
  if (!image) return;
  const char *name = baseName(s_proj);
  char path[200], err[160];
#ifdef ARDUINO_ARCH_RP2040
  snprintf(path, sizeof path, "%s/%s.uf2", dir, name);   // ex_export() names it so anyway
#else
  snprintf(path, sizeof path, "%s/%s.bin", dir, name);
#endif
  setMsg(YELLOW, "Exporting %s...", path);
  drawAll();
  hal_breadcrumb("export %s", s_proj);
  const uint32_t t0 = sys_millis();
  const long n = ex_export(image, size, name, path, err, sizeof err);
  const uint32_t ms = sys_millis() - t0;
  hal_breadcrumb("ide");
  free(image);
  if (n < 0) {
    outPrintf("Export failed: %s\n", err);
    hal_log("export %s failed: %s", s_proj, err);
    setMsg(RED, "Export failed: %s", err);
    s_panel = true;
    return;
  }
  outPrintf("Exported %s: %ld KB, %lu ms\nFlash it from PicoMultiLoader (F10: back to the loader).\n",
            path, n / 1024, (unsigned long)ms);
  hal_log("export %s: %s, %ld bytes, %lu ms", s_proj, path, n, (unsigned long)ms);
  setMsg(GREEN, "Exported %s (%ld KB)", path, n / 1024);
}

// ---- screens ----

static void newProject() {
  char name[33] = "";
  if (!ui_input("New project", "Name (letters, digits, _ and -):", name, sizeof name) || !name[0]) return;
  for (char *p = name; *p; p++)
    if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') { ui_message("Use letters, digits, _ and - only."); return; }
  char dir[160], path[200];
  mkdir(ROOT_DIR, 0777);
  mkdir(PROJECTS_DIR, 0777);
  snprintf(dir, sizeof dir, PROJECTS_DIR "/%s", name);
  if (isDir(dir)) { ui_message("That project already exists."); return; }
  if (mkdir(dir, 0777)) { ui_message("Can't create the folder (is the SD card in?)"); return; }
  snprintf(path, sizeof path, "%s/project.cfg", dir);
  if (FILE *f = fopen(path, "w")) { fprintf(f, "name=%s\nentry=setup_loop\n", name); fclose(f); }
  snprintf(path, sizeof path, "%s/main.c", dir);
  if (FILE *f = fopen(path, "w")) {
    fprintf(f,
            "/* %s */\n#include <picocalc.h>\n\nvoid setup(void)\n{\n    gfx_clear(BLACK);\n"
            "    gfx_text(10, 10, \"Hello from %s!\", WHITE);\n    gfx_text(10, 30, \"Esc: back to the IDE\", GRAY);\n"
            "    gfx_flush();\n}\n\nvoid loop(void)\n{\n    if (kbd_get() == KEY_ESC)\n        sys_exit();\n}\n",
            name, name);
    fclose(f);
  }
  if (closeProject()) { openProject(dir, nullptr); cfgSave(); }
}

// Copies the files of folder `from` into the new folder `to`.
static bool copyDir(const char *from, const char *to) {
  DIR *d = opendir(from);
  if (!d) return false;
  if (mkdir(to, 0777)) { closedir(d); return false; }
  bool ok = true;
  static char buf[4096];
  while (struct dirent *e = readdir(d)) {
    char src[256], dst[256];
    snprintf(src, sizeof src, "%s/%s", from, e->d_name);
    snprintf(dst, sizeof dst, "%s/%s", to, e->d_name);
    if (e->d_name[0] == '.' || isDir(src)) continue;
    FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : nullptr;
    size_t n;
    while (out && (n = fread(buf, 1, sizeof buf, in)) > 0)
      if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    if (!out || fclose(out)) ok = false;
    if (in) fclose(in);
  }
  closedir(d);
  return ok;
}

// Copies an example into /sd/picocalc-studio/projects under a name the user picks, then opens the copy.
static void copyExample(const char *example) {
  char name[33], src[160], dir[160];
  snprintf(name, sizeof name, "%s", example);
  snprintf(src, sizeof src, EXAMPLES_DIR "/%s", example);
  mkdir(ROOT_DIR, 0777);
  mkdir(PROJECTS_DIR, 0777);
  for (;;) {
    if (!ui_input("New from example", "Copy the example as project:", name, sizeof name) || !name[0]) return;
    snprintf(dir, sizeof dir, PROJECTS_DIR "/%s", name);
    if (!isDir(dir)) break;
    ui_message("That project already exists: pick another name.");
  }
  if (!copyDir(src, dir)) { ui_message("Can't copy the example (is the SD card in?)"); return; }
  if (closeProject()) { openProject(dir, nullptr); cfgSave(); }
}

// "+ New from example": picks an example to copy (the open one, if an example is open).
static void newFromExample() {
  static char names[MAX_ITEMS][48];
  static const char *items[MAX_ITEMS];
  int n = 0, sel = 0;
  if (DIR *d = opendir(EXAMPLES_DIR)) {
    while (struct dirent *e = readdir(d)) {
      char p[160];
      snprintf(p, sizeof p, EXAMPLES_DIR "/%s", e->d_name);
      if (n == MAX_ITEMS || e->d_name[0] == '.' || !isDir(p)) continue;
      snprintf(names[n++], sizeof names[0], "%s", e->d_name);
    }
    closedir(d);
  }
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && strcasecmp(names[j - 1], names[j]) > 0; j--) {
      char t[48];
      memcpy(t, names[j], 48); memcpy(names[j], names[j - 1], 48); memcpy(names[j - 1], t, 48);
    }
  for (int i = 0; i < n; i++) {
    items[i] = names[i];
    if (isExample() && !strcmp(names[i], baseName(s_proj))) sel = i;
  }
  const int i = ui_list("New from example", items, n, &sel, "Enter: copy into a new project  Esc: back", nullptr);
  if (i >= 0) copyExample(names[i]);
}

// The folders of dir, sorted by name, into paths and labels from n on. Returns the new n.
static int listProjects(const char *dir, char (*paths)[128], char (*labels)[64], int n) {
  const int first = n;
  if (DIR *d = opendir(dir)) {
    while (struct dirent *e = readdir(d)) {
      if (n == MAX_ITEMS || e->d_name[0] == '.') continue;
      snprintf(paths[n], 128, "%s/%s", dir, e->d_name);
      if (!isDir(paths[n])) continue;
      snprintf(labels[n], 64, "  %s", e->d_name);
      n++;
    }
    closedir(d);
  }
  for (int i = first + 1; i < n; i++)
    for (int j = i; j > first && strcasecmp(labels[j - 1], labels[j]) > 0; j--) {
      char t[128];
      memcpy(t, paths[j], 128); memcpy(paths[j], paths[j - 1], 128); memcpy(paths[j - 1], t, 128);
      memcpy(t, labels[j], 64); memcpy(labels[j], labels[j - 1], 64); memcpy(labels[j - 1], t, 64);
    }
  return n;
}

// The project list: "My files" (the projects in /sd/picocalc-studio/projects), then the examples, which
// open read-only. Returns when a project is open or the user goes back to an open one.
static void pickProject() {
  static char paths[MAX_ITEMS][128];
  static char labels[MAX_ITEMS][64];
  static const char *items[MAX_ITEMS];
  for (;;) {
    int n = 0, sel = 0;
    snprintf(labels[n], sizeof labels[n], "+ New project");
    paths[n++][0] = 0;
    snprintf(labels[n], sizeof labels[n], "+ New from example");
    paths[n++][0] = 0;
    snprintf(labels[n], sizeof labels[n], "-- My files --");
    paths[n++][0] = 0;
    const int mine = n;
    n = listProjects(PROJECTS_DIR, paths, labels, n);
    if (n < MAX_ITEMS - 1) {
      snprintf(labels[n], sizeof labels[n], "-- Examples --");
      paths[n++][0] = 0;
      const int examples = n;
      n = listProjects(EXAMPLES_DIR, paths, labels, n);
      if (n == examples) n--;   // no examples: no heading either
    }
    for (int i = 0; i < n; i++) {
      items[i] = labels[i];
      if (s_proj[0] && !strcmp(paths[i], s_proj)) sel = i;
    }
    if (!sel && n > mine) sel = mine;   // the first of my projects
    int key = 0;
    const int i = ui_list("Projects", items, n, &sel, "Enter: open  F9: SDK demo  F10: loader  Esc: back", &key);
    if (i == -1) { if (s_ndocs) return; continue; }
    if (i == -2) {
      if (key == KEY_F9) { demo_run(); sdk_reset(); }
      continue;
    }
    if (i <= 1) {
      if (i) newFromExample();
      else newProject();
      if (s_ndocs) return;
      continue;
    }
    if (!paths[i][0]) continue;   // a heading
    if (!strcmp(paths[i], s_proj)) return;
    char dir[160];
    snprintf(dir, sizeof dir, "%s", paths[i]);
    if (!closeProject()) continue;
    if (openProject(dir, nullptr)) { cfgSave(); return; }
    ui_message("Can't open that project.");
  }
}

static void newFile() {
  char name[48] = "";
  if (!ui_input("New file", "File name (.c or .h):", name, sizeof name) || !name[0]) return;
  if (!strchr(name, '.')) strcat(name, ".c");
  char path[192];
  snprintf(path, sizeof path, "%s/%s", s_proj, name);
  struct stat st;
  if (!stat(path, &st)) { ui_message("That file already exists."); return; }
  const size_t l = strlen(name);
  const bool header = l > 2 && !strcmp(name + l - 2, ".h");
  FILE *f = fopen(path, "w");
  if (!f) { ui_message("Can't create the file."); return; }
  fputs(header ? "#pragma once\n\n" : "#include <picocalc.h>\n\n", f);
  fclose(f);
  reindex();
  openFile(path);
}

static void deleteFile(const char *name) {
  char q[96], path[192];
  snprintf(q, sizeof q, "Delete %s? (Y/N)", name);
  if (ui_ask(q, "yn") != 'y') return;
  snprintf(path, sizeof path, "%s/%s", s_proj, name);
  const int di = findDoc(path);
  if (di >= 0) closeDoc(di);
  if (remove(path)) ui_message("Can't delete the file.");
  reindex();
}

// "#include <name>" or "#include "name"" in s[0, len): the name goes into out, *quoted says which.
// False if it isn't one.
static bool includeName(const char *s, int len, char *out, int size, bool *quoted) {
  int i = 0;
  while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
  if (i == len || s[i] != '#') return false;
  for (i++; i < len && (s[i] == ' ' || s[i] == '\t'); i++) {}
  if (len - i < 7 || strncmp(s + i, "include", 7)) return false;
  for (i += 7; i < len && (s[i] == ' ' || s[i] == '\t'); i++) {}
  if (i == len || (s[i] != '<' && s[i] != '"')) return false;
  const char close = s[i] == '<' ? '>' : '"';
  *quoted = close == '"';
  const int a = ++i;
  while (i < len && s[i] != close) i++;
  if (i == len || i == a || i - a >= size) return false;
  memcpy(out, s + a, i - a);
  out[i - a] = 0;
  return true;
}

// Calls fn(arg, line, len) for each line of a file: an open one's text as edited, else as saved.
static void eachLine(const char *path, void (*fn)(void *, const char *, int), void *arg) {
  const int di = findDoc(path);
  if (di >= 0) {
    const Doc *d = s_docs[di];
    for (int y = 0; y < d->n; y++) fn(arg, d->ln[y].s, d->ln[y].len);
  } else if (FILE *f = fopen(path, "r")) {
    char line[256];
    while (fgets(line, sizeof line, f)) fn(arg, line, (int)strcspn(line, "\r\n"));
    fclose(f);
  }
}

// The project's .c/.h files, full paths, malloc'ed.
static int projectSources(char **out, int max) {
  int n = 0;
  if (DIR *dp = opendir(s_proj)) {
    while (struct dirent *e = readdir(dp)) {
      const size_t l = strlen(e->d_name);
      if (l < 3 || e->d_name[l - 2] != '.' || (e->d_name[l - 1] != 'c' && e->d_name[l - 1] != 'h') || n == max) continue;
      char p[256];
      snprintf(p, sizeof p, "%s/%s", s_proj, e->d_name);
      out[n++] = strdup(p);
    }
    closedir(dp);
  }
  return n;
}

struct PathList { char **v; int n, max; const char *file; };

static void addPath(PathList *l, const char *path) {
  for (int i = 0; i < l->n; i++)
    if (!strcmp(l->v[i], path)) return;
  if (l->n < l->max) l->v[l->n++] = strdup(path);
}

// Another project's header that `#include` line s[0, len) of l->file brings in: quoted, found
// next to the file (as the compiler looks, so "../other/x.h"), outside this project and the
// include folder. Its .c (other/x.c), which builds with the project, comes too.
static void otherInclude(void *arg, const char *s, int len) {
  PathList *l = (PathList *)arg;
  char name[96], p[192];
  bool quoted;
  if (!includeName(s, len, name, sizeof name, &quoted) || !quoted || name[0] == '/') return;
  const char *slash = strrchr(l->file, '/');
  snprintf(p, sizeof p, "%.*s/%s", slash ? (int)(slash - l->file) : 0, l->file, name);
  cc_normpath(p);
  struct stat st;
  if (inProject(p) || !strncmp(p, INCLUDE_DIR "/", sizeof INCLUDE_DIR) || stat(p, &st) || S_ISDIR(st.st_mode)) return;
  addPath(l, p);
  const size_t n = strlen(p);
  if (n > 2 && !strcmp(p + n - 2, ".h")) {
    p[n - 1] = 'c';
    if (!stat(p, &st)) addPath(l, p);
  }
}

// The other projects' files this one uses: the headers it includes as "../other/x.h", the .c
// next to each, and what those include in turn. Full paths, malloc'ed.
static int otherFiles(char **out) {
  PathList l = {out, 0, MAX_OTHER, nullptr};
  if (!s_proj[0]) return 0;
  char *files[MAX_ITEMS];
  const int nf = projectSources(files, MAX_ITEMS);
  for (int i = 0; i < nf; i++) {
    l.file = files[i];
    eachLine(files[i], otherInclude, &l);
    free(files[i]);
  }
  for (int i = 0; i < l.n; i++) {   // l grows while it is read
    l.file = l.v[i];
    eachLine(l.v[i], otherInclude, &l);
  }
  return l.n;
}

// The project's names for completion and F9, with the other projects' files it uses.
static void reindex() {
  ac_project(INCLUDE_DIR, LIB_DIR, s_proj);
  char *other[MAX_OTHER];
  const int n = otherFiles(other);
  ac_extra(other, n);
  for (int i = 0; i < n; i++) free(other[i]);
}

static void addRef(char **refs, int *n, const char *path) {
  for (int i = 0; i < *n; i++)
    if (!strcmp(refs[i], path)) return;
  if (*n < MAX_REFS) refs[(*n)++] = strdup(path);
}

struct RefList { char **refs; int *n; };

// An #include of a project file: the header it names, if it is in the include folder, and its
// library's sources (LIB_DIR/<name without .h>/).
static void addInclude(void *arg, const char *s, int len) {
  char **refs = ((RefList *)arg)->refs;
  int *n = ((RefList *)arg)->n;
  char name[96], path[192];
  struct stat st;
  bool quoted;
  if (!includeName(s, len, name, sizeof name, &quoted)) return;
  snprintf(path, sizeof path, "%s/%s", s_proj, name);
  if (!stat(path, &st)) return;   // the project's own
  snprintf(path, sizeof path, INCLUDE_DIR "/%s", name);
  if (stat(path, &st) || S_ISDIR(st.st_mode)) return;
  addRef(refs, n, path);
  const size_t l = strlen(name);
  if (l > 2 && !strcmp(name + l - 2, ".h")) name[l - 2] = 0;
  snprintf(path, sizeof path, LIB_DIR "/%s", name);
  if (DIR *dp = opendir(path)) {
    while (struct dirent *e = readdir(dp)) {
      const size_t m = strlen(e->d_name);
      if (e->d_name[0] == '.' || !((m > 2 && !strcmp(e->d_name + m - 2, ".c")) || (m > 4 && !strcmp(e->d_name + m - 4, ".cpp"))))
        continue;
      char src[256];
      snprintf(src, sizeof src, "%s/%s", path, e->d_name);
      addRef(refs, n, src);
    }
    closedir(dp);
  }
}

// The order of the references: a library source (LIB_DIR/x/file.c) right after its header
// (INCLUDE_DIR/x.h), so it sorts as "INCLUDE_DIR/x.h" + '\1' + "file.c"; another project's x.c
// right after its x.h the same way.
static void refKey(const char *p, char *key, int size) {
  if (!strncmp(p, LIB_DIR "/", sizeof LIB_DIR)) {
    const char *lib = p + sizeof LIB_DIR, *slash = strchr(lib, '/');
    if (slash) {
      snprintf(key, size, INCLUDE_DIR "/%.*s.h\1%s", (int)(slash - lib), lib, slash + 1);
      return;
    }
  }
  const size_t l = strlen(p);
  if (l > 2 && !strcmp(p + l - 2, ".c")) snprintf(key, size, "%.*sh\1c", (int)(l - 1), p);
  else snprintf(key, size, "%s", p);
}

// Another project's file as the project names it: "../other/x.h". Null if it isn't one.
static const char *otherName(const char *p, char *buf, int size) {
  const char *slash = strrchr(s_proj, '/');
  if (!slash || strncmp(p, s_proj, slash + 1 - s_proj)) return nullptr;
  snprintf(buf, size, "../%s", p + (slash + 1 - s_proj));
  return buf;
}

static int cmpRef(const void *a, const void *b) {
  char ka[256], kb[256];
  refKey(*(char *const *)a, ka, sizeof ka);
  refKey(*(char *const *)b, kb, sizeof kb);
  return strcasecmp(ka, kb);
}

// The files outside the project it refers to: the headers its .c/.h files include (as edited)
// with their library sources, the other projects' files it uses, and the other files open in the
// editor (F9 went there). Full paths, malloc'ed, sorted.
static int listRefs(char **refs) {
  int n = 0;
  RefList rl = {refs, &n};
  char *files[MAX_ITEMS];
  const int nf = projectSources(files, MAX_ITEMS);
  for (int i = 0; i < nf; i++) {
    eachLine(files[i], addInclude, &rl);
    free(files[i]);
  }
  char *other[MAX_OTHER];
  const int no = otherFiles(other);
  for (int i = 0; i < no; i++) {
    addRef(refs, &n, other[i]);
    free(other[i]);
  }
  for (int i = 0; i < s_ndocs; i++)
    if (!inProject(s_docs[i]->path)) addRef(refs, &n, s_docs[i]->path);
  qsort(refs, n, sizeof *refs, cmpRef);
  return n;
}

// Esc in the editor: the project's files, then the files it refers to.
static void pickFile() {
  static char names[MAX_ITEMS][64];
  static char labels[MAX_ITEMS][72];
  static const char *items[MAX_ITEMS];
  for (;;) {
    char *list[MAX_ITEMS];
    int nf = 0;
    if (DIR *d = opendir(s_proj)) {
      while (struct dirent *e = readdir(d)) {
        const size_t l = strlen(e->d_name);
        if (nf == MAX_ITEMS - 2 || e->d_name[0] == '.' || (l > 4 && !strcmp(e->d_name + l - 4, ".tmp"))) continue;
        char p[256];
        snprintf(p, sizeof p, "%s/%s", s_proj, e->d_name);
        if (isDir(p)) continue;
        list[nf++] = strdup(e->d_name);
      }
      closedir(d);
    }
    qsort(list, nf, sizeof *list, cmpStr);
    int n = 0, sel = 2;
    snprintf(labels[n++], sizeof labels[0], "< Projects");
    snprintf(labels[n++], sizeof labels[0], "+ New file");
    for (int i = 0; i < nf; i++, n++) {
      snprintf(names[n], sizeof names[n], "%s", list[i]);
      char p[256];
      snprintf(p, sizeof p, "%s/%s", s_proj, list[i]);
      const int di = findDoc(p);
      snprintf(labels[n], sizeof labels[n], "%s %s", di >= 0 && s_docs[di]->modified ? "*" : " ", list[i]);
      if (cur() && !strcmp(cur()->path, p)) sel = n;
      free(list[i]);
    }
    const int filesEnd = n;   // then "External references files" and the files it lists
    char *refs[MAX_REFS];
    const int nr = listRefs(refs);
    int nrShown = 0;
    if (nr && n + 1 < MAX_ITEMS) {
      snprintf(labels[n++], sizeof labels[0], "-- External references files --");
      for (; nrShown < nr && n < MAX_ITEMS; nrShown++, n++) {
        const int di = findDoc(refs[nrShown]);
        const char *r = refs[nrShown];
        const bool inc = !strncmp(r, INCLUDE_DIR "/", sizeof INCLUDE_DIR);
        const bool lib = !strncmp(r, LIB_DIR "/", sizeof LIB_DIR);   // shown as <x>/file.c
        char buf[160];
        const char *other = inc || lib ? nullptr : otherName(r, buf, sizeof buf);   // "../other/x.h"
        const size_t rl = strlen(r);
        const bool src = lib || (other && rl > 2 && !strcmp(r + rl - 2, ".c"));      // under its header
        snprintf(labels[n], sizeof labels[n], inc ? "%s <%s>" : src ? "%s   %s" : "%s %s",
                 di >= 0 && s_docs[di]->modified ? "*" : " ",
                 inc ? r + sizeof INCLUDE_DIR : lib ? r + sizeof LIB_DIR : other ? other : r);
        if (cur() && !strcmp(cur()->path, r)) sel = n;
      }
    }
    for (int i = 0; i < n; i++) items[i] = labels[i];
    char title[80];
    snprintf(title, sizeof title, "Files - %s", baseName(s_proj));
    int key = 0;
    const bool unsaved = anyModified();   // F3 shown only when there is something to save
    const int i = ui_list(title, items, n, &sel,
                          isExample() ? "Enter: open  Esc: editor  (example: read-only)"
                          : unsaved   ? "Enter: open  Del: delete  F3: save all  Esc: editor"
                                      : "Enter: open  Del: delete  Esc: editor", &key);
    char ref[192] = "";
    if (i > filesEnd) snprintf(ref, sizeof ref, "%s", refs[i - filesEnd - 1]);
    for (int r = 0; r < nr; r++) free(refs[r]);
    if (i == -1) return;
    if (i == 0) { pickProject(); return; }
    if (i == 1) {
      if (isExample()) { setMsg(YELLOW, "Examples are read-only: \"+ New from example\" copies one"); return; }
      newFile();
      return;
    }
    if (i == -2) {
      if (key == KEY_DEL && sel >= 2 && sel < filesEnd && !isExample()) deleteFile(names[sel]);   // not a reference
      else if (key == KEY_F3 && unsaved) {
        if (saveAll()) { setMsg(GREEN, "Saved"); cfgSave(); }
        else ui_message(s_msg);
      }
      continue;
    }
    if (i == filesEnd) continue;   // the "External references files" line
    Doc *from = cur();
    if (openFile(ref[0] ? ref : names[i]) && ref[0] && from && from != cur()) pushBack(from);   // Esc comes back
    cfgSave();
    return;
  }
}

static const char kHelp[] =
    PCS_NAME " " PCS_VERSION "\n"
    "\n"
    "F1   this help\n"
    "F2   mark mode on/off (select with the arrows)\n"
    "F3   build (saves all files first)\n"
    "F4   build and run; Break stops the program\n"
    "Ctrl-F4  debug: build and run, stopping at the\n"
    "     breakpoints (Ctrl-B on a line sets/clears\n"
    "     one) and on Break. Stopped: F4 runs on,\n"
    "     Down next line, Right step into, Left\n"
    "     step out, Esc ends the program\n"
    "F5   export <folder>/<project>.bin (.uf2 on the\n"
    "     Pico 2): a firmware that runs the program,\n"
    "     for PicoMultiLoader. Asks for the folder;\n"
    "     the default is /sd/picocalc-studio/apps\n"
    "F6   full build/program output (closing it also\n"
    "     hides the output panel)\n"
    "F7   output panel on/off: the last lines of the\n"
    "     build (memory use, errors) and of the\n"
    "     program's output. F3/F4/F5 show it, any\n"
    "     other key hides it\n"
    "F8   back to the last edit (any file)\n"
    "F9   go to the declaration of the name at the\n"
    "     cursor: a local, a parameter, a function,\n"
    "     macro, type, global or struct member, in the\n"
    "     project or the headers\n"
    "F10  back to PicoMultiLoader (Shift-F5 too, on\n"
    "     any screen)\n"
    "Esc  clear the selection, else the files of the\n"
    "     project (Esc there: back to the editor).\n"
    "     Under External references files: the\n"
    "     headers the project includes and other\n"
    "     files F9 opened\n"
    "\n"
    "Using another project's code: #include\n"
    "\"../other/x.h\" in a file. x.h then shows\n"
    "under External references files with x.c (if\n"
    "there is one), F9 and Ctrl-Space know their\n"
    "names, and x.c is built with the project.\n"
    "\n"
    "Reference files (headers, files outside the\n"
    "project; REF in the status bar) are read-only:\n"
    "F9   go to a declaration, deeper\n"
    "Esc  back to where you came from; with\n"
    "     nothing to go back to, the files\n"
    "F2, Ctrl-A, Ctrl-C, Ctrl-F, Ctrl-G as usual.\n"
    "F3/F4/F5 work from the project's own files.\n"
    "\n"
    "Editing\n"
    "Ctrl-S  save all     Ctrl-G  go to line\n"
    "Ctrl-Z  undo         Ctrl-Y  redo\n"
    "Ctrl-F  find         Ctrl-N  find next\n"
    "Ctrl-R  replace, from the cursor down (Y/N/A)\n"
    "Ctrl-K  delete line  Ctrl-D  duplicate line\n"
    "Home    line start (again: column 1)\n"
    "Ctrl-Home/End  start/end of the file\n"
    "Ctrl-Left/Right  previous/next word\n"
    "\n"
    "Completion\n"
    "Ctrl-Space lists the names that start with\n"
    "the word before the cursor (all of them if\n"
    "there is none). Up/Down choose, Tab or Enter\n"
    "insert, Space inserts it and a space, Esc\n"
    "closes; typing narrows the list.\n"
    "The status bar shows the chosen name's\n"
    "declaration. Names come from C, the headers\n"
    "in /sd/picocalc-studio/include and the project's\n"
    "files.\n"
    "\n"
    "Selecting\n"
    "Alt + arrows/Home/End select. Shift-Up/Down\n"
    "page; Alt-Shift-Up/Down select a page.\n"
    "Or mark mode: F2, move the cursor, F2 again.\n"
    "The cursor is orange while\n"
    "marking. Ctrl-A selects everything.\n"
    "Ctrl-C  copy         Ctrl-X  cut\n"
    "Ctrl-V  paste. With nothing selected, copy and\n"
    "cut take the whole line, and pasting it puts it\n"
    "above the cursor line.\n"
    "Typing replaces the selection. Tab/Alt-Tab\n"
    "indent/unindent the selected lines.\n"
    "\n"
    "Projects are folders in /sd/picocalc-studio/projects.\n"
    "Every .c file in the folder is compiled and\n"
    "linked. A program has setup() and loop(), or\n"
    "main().\n"
    "Headers: /sd/picocalc-studio/include (picocalc.h is the\n"
    "SDK). printf() output shows in the output panel.\n"
    "\n"
    "If a program crashes, the IDE reopens at the\n"
    "line where it crashed.\n"
    "\n"
    "In the project list: My files are your\n"
    "projects. Examples open read-only (build and\n"
    "run them); \"+ New from example\" copies one\n"
    "into a new project to change. F9 there: SDK\n"
    "demo.\n";

// Shows a text file (a header). Large ones are cut.
static void viewFile(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) { ui_message("Can't open the file."); return; }
  const size_t max = 96 * 1024;
  char *buf = (char *)heap_caps_malloc(max + 1, MALLOC_CAP_SPIRAM);
  if (!buf) { fclose(f); ui_message("Out of memory."); return; }
  const size_t n = fread(buf, 1, max, f);
  fclose(f);
  size_t o = 0;   // drop CRs, tabs become one space
  for (size_t i = 0; i < n; i++)
    if (buf[i] != '\r') buf[o++] = buf[i] == '\t' ? ' ' : buf[i];
  buf[o] = 0;
  ui_text_view(baseName(path), buf);
  heap_caps_free(buf);
}

// F1: the key help, or one of the SDK headers.
static void helpMenu() {
  static char names[MAX_ITEMS][40];
  static char labels[MAX_ITEMS][56];
  static const char *items[MAX_ITEMS];
  int n = 0, sel = 0;
  snprintf(labels[n++], sizeof labels[0], "Keys and how the IDE works");
  snprintf(names[n], sizeof names[0], "picocalc.h");
  snprintf(labels[n++], sizeof labels[0], "picocalc.h (the SDK: graphics, keys...)");
  char *list[MAX_ITEMS];
  int nf = 0;
  if (DIR *d = opendir(INCLUDE_DIR)) {
    while (struct dirent *e = readdir(d)) {
      const size_t l = strlen(e->d_name);
      if (nf == MAX_ITEMS - 2 || l < 3 || strcmp(e->d_name + l - 2, ".h") || !strcmp(e->d_name, "picocalc.h")) continue;
      list[nf++] = strdup(e->d_name);
    }
    closedir(d);
  }
  qsort(list, nf, sizeof *list, cmpStr);
  for (int i = 0; i < nf; i++, n++) {
    snprintf(names[n], sizeof names[0], "%s", list[i]);
    snprintf(labels[n], sizeof labels[0], "%s", list[i]);
    free(list[i]);
  }
  for (int i = 0; i < n; i++) items[i] = labels[i];
  for (;;) {
    const int i = ui_list("Help", items, n, &sel, "Enter: show  Esc: back", nullptr);
    if (i < 0) return;
    if (i == 0) { ui_text_view("Keys", kHelp); continue; }
    char path[128];
    snprintf(path, sizeof path, INCLUDE_DIR "/%s", names[i]);
    viewFile(path);
  }
}

// ---- main screen ----

static void drawPanel(int y) {
  gfx_fill_rect(0, y, SCREEN_W, PANEL_ROWS * UI_CH + 4, RGB(8, 8, 12));
  gfx_hline(0, y, SCREEN_W, UI_DIM);
  // the last PANEL_ROWS lines of the output
  const char *p = s_out + s_outLen;
  if (p > s_out && p[-1] == '\n') p--;
  int rows = 0;
  while (p > s_out && rows < PANEL_ROWS) {
    p--;
    if (*p == '\n') rows++;
  }
  if (*p == '\n') p++;
  char line[UI_COLS + 1];
  for (int r = 0; r < PANEL_ROWS && *p; r++) {
    const char *eol = strchr(p, '\n');
    const int len = (int)(eol ? eol - p : strlen(p));
    const int n = len < UI_COLS ? len : UI_COLS;
    memcpy(line, p, n);
    line[n] = 0;
    color_t c = UI_TEXT;
    if (strstr(line, "error")) c = RGB(255, 110, 110);
    else if (strstr(line, "warning")) c = YELLOW;
    gfx_text(2, y + 3 + r * UI_CH, line, c);
    p = eol ? eol + 1 : p + len;
  }
}

static void drawAll() {
  Doc *d = cur();
  const int rows = editRows();
  char title[96];
  snprintf(title, sizeof title, "%s / %s%s", baseName(s_proj), d ? baseName(d->path) : "",
           d && d->readOnly ? " (read-only)" : d && d->modified ? " *" : "");
  gfx_font(FONT_6X10);
  ui_title(title, s_battery);
  if (d) ed_draw(d, UI_BAR_H, rows, !strcmp(s_errPath, d->path) ? s_errLine : -1);
  const int yEnd = UI_BAR_H + rows * UI_CH;
  gfx_fill_rect(0, yEnd, SCREEN_W, SCREEN_H - UI_BAR_H - yEnd, UI_BG);
  if (s_panel) drawPanel(SCREEN_H - UI_BAR_H - PANEL_ROWS * UI_CH - 4);
  if (d) ac_draw(d, UI_BAR_H, rows);
  // compact position, so the key hint (41 characters) fits beside it
  char pos[24];
  snprintf(pos, sizeof pos, "L%d C%d", d ? d->cy + 1 : 0, d ? d->cx + 1 : 0);
  const char *hint = ac_hint();
  const char *hintPos = hint && strlen(hint) > 40 ? nullptr : pos;   // a long declaration takes the whole bar
  if (s_msg[0]) ui_status(s_msg, pos, s_msgColor);
  else if (ac_active()) ui_status(hint ? hint : "Tab/Enter/Space: insert  Esc: close", hint ? hintPos : pos, HINT_COLOR);
  else if (d && d->marking) ui_status("MARK: move to select  F2 done  Esc cancel", pos, ORANGE);
  else if (d && d->readOnly && ed_has_sel(d)) ui_status("SEL: ^C copy  Esc clear", pos, REF_COLOR);
  else if (d && isRef(d)) ui_status("REF  F9 go to decl  Esc back  ^F find", pos, REF_COLOR);
  else if (d && d->readOnly) ui_status("EXAMPLE F1 help F3 build F4 run Esc files", pos, REF_COLOR);
  else if (d && ed_has_sel(d)) ui_status("SEL: ^C copy ^X cut ^V paste  Esc clear", pos, UI_TEXT);
  else ui_status("F1 help F2 mark F3 build F4 run Esc files", pos, UI_DIM);
  gfx_flush();
}

static void updateBattery() {
  bool charging = false;
  const int pct = sys_battery(&charging);
  if (pct < 0) s_battery[0] = 0;
  else snprintf(s_battery, sizeof s_battery, "Bat. %d%%%s", pct, charging ? "+" : "");
  s_batteryAt = sys_millis() + 20000;
}

// Waits for a key; KEY_NONE when the screen needs redrawing instead (an @run, the battery).
static int waitKey() {
  for (;;) {
    const int k = ui_key();
    if (k != KEY_NONE) return k;
    if (rt_service()) return KEY_NONE;
    if ((int32_t)(sys_millis() - s_batteryAt) >= 0) { updateBattery(); return KEY_NONE; }
    sys_delay(10);
  }
}

static void onBreak() { longjmp(s_jmp, 1); }

// F10 / Shift-F5 anywhere: back to PicoMultiLoader, offering to save changed files first.
static void toLoader() {
  if (anyModified()) {
    const int a = ui_ask("Save changes before going to the loader? Y/N", "yn");
    if (a == KEY_ESC) return;
    if (a == 'y' && !saveAll()) { ui_message(s_msg); return; }
  }
  cfgSave();
  sys_exit_to_loader();
}

// A one-line selection goes into buf (to search for it); otherwise buf is left alone.
static void selectionText(Doc *d, char *buf, int size) {
  if (!d || !ed_has_sel(d) || d->ay != d->cy) return;
  const int x0 = d->ax < d->cx ? d->ax : d->cx, x1 = d->ax < d->cx ? d->cx : d->ax;
  const int n = x1 - x0 < size - 1 ? x1 - x0 : size - 1;
  memcpy(buf, d->ln[d->cy].s + x0, n);
  buf[n] = 0;
}

// Ctrl-R: replaces matches from the cursor (or the selection) to the end of the file, asking
// about each one.
static void replace(Doc *d, int rows) {
  if (!d) return;
  if (d->readOnly) { readOnlyMsg(d); return; }
  selectionText(d, s_find, sizeof s_find);
  if (!ui_input("Replace", "Text to find:", s_find, sizeof s_find) || !s_find[0]) return;
  if (!ui_input("Replace", "Replace with:", s_repl, sizeof s_repl)) return;
  int y = d->cy, x = d->cx;
  if (ed_has_sel(d) && (d->ay < y || (d->ay == y && d->ax < x))) { y = d->ay; x = d->ax; }
  int count = 0;
  bool all = false;
  while (ed_find_at(d, s_find, rows, y, x) && !(d->ay < y || (d->ay == y && d->ax < x))) {   // not wrapped
    int a = 'a';
    if (!all) {
      const int r = d->cy - d->top;   // keep the match clear of the question box
      if (r >= 10 && r <= 15) d->top = d->cy - 3;
      drawAll();
      a = ui_ask("Replace this one? Y/N, A: all the rest", "yna");
      if (a == KEY_ESC) break;
    }
    if (a != 'n') {
      if (!all) doc_begin(d);   // "all the rest" is undone in one go
      all = a == 'a';
      ed_replace_sel(d, s_repl, rows);
      count++;
    }
    y = d->cy;
    x = d->cx;
  }
  ed_unselect(d);
  setMsg(count ? GREEN : YELLOW, "%d replaced", count);
}

static bool wordChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

// The column of the whole word w in line y nearest to col, or col if it isn't there (columns read
// from a file on the card don't count the tabs the editor expands).
static int nearestWord(const Doc *d, int y, int col, const char *w) {
  const DocLine *l = &d->ln[y];
  const int n = (int)strlen(w);
  int best = col;
  for (int x = 0; x + n <= l->len; x++)
    if (!memcmp(l->s + x, w, n) && (x == 0 || !wordChar(l->s[x - 1])) && (x + n == l->len || !wordChar(l->s[x + n])) &&
        (best == col || abs(x - col) < abs(best - col)))
      best = x;
  return best;
}

// F9: to the declaration of the name at the cursor, in the project or a header; Esc comes back
// from a header.
static void gotoDecl(Doc *d, int rows) {
  if (!d) return;
  char name[64], path[192];
  int line, col;
  if (!ac_find_decl(s_docs, s_ndocs, d, name, sizeof name, path, sizeof path, &line, &col)) {
    if (name[0]) setMsg(YELLOW, "No declaration of %s found", name);
    else setMsg(YELLOW, "F9: put the cursor on a name");
    return;
  }
  if (!openFile(path)) return;   // d stays open: openFile drops only a file that isn't shown
  pushBack(d);
  Doc *t = cur();
  if (line >= t->n) line = t->n - 1;
  ed_goto(t, line, nearestWord(t, line, col, name), rows);
  setMsg(GREEN, "%s: %s line %d  %s", name, baseName(t->path), line + 1, t->readOnly ? "Esc back" : "F8: last edit");
}

static void goBack(int rows) {
  if (!s_nback) { setMsg(UI_DIM, "Nothing to go back to (F9 goes to a declaration)"); return; }
  const Place &p = s_back[--s_nback];
  if (!openFile(p.path)) return;
  Doc *t = cur();
  ed_goto(t, p.y < t->n ? p.y : t->n - 1, p.x, rows);
}

// F8: to where the last edit left the cursor.
static void goLastEdit(int rows) {
  if (!s_lastEdit.path[0]) { setMsg(UI_DIM, "No edits yet"); return; }
  if (!openFile(s_lastEdit.path)) return;
  Doc *t = cur();
  ed_goto(t, s_lastEdit.y < t->n ? s_lastEdit.y : t->n - 1, s_lastEdit.x, rows);
}

// Changes with every edit (typing may only grow the last undo record).
static uint64_t editStamp(const Doc *d) {
  if (!d) return 0;
  const DocUndo *r = d->nu ? &d->u[d->nu - 1] : nullptr;
  return (uint64_t)d->nu << 48 ^ (uint64_t)d->nt << 32 ^ (uint64_t)d->n << 16 ^
         (r ? (uint32_t)(r->n * 31 + r->y * 7 + r->x) : 0);
}

static void doCommand(int k);

static void command(int k) {
  Doc *d = cur();
  const uint64_t stamp = editStamp(d);
  doCommand(k);
  if (d && cur() == d && editStamp(d) != stamp) {
    snprintf(s_lastEdit.path, sizeof s_lastEdit.path, "%s", d->path);
    s_lastEdit.y = d->cy;
    s_lastEdit.x = d->cx;
  }
}

static void doCommand(int k) {
  Doc *d = cur();
  const bool panel = s_panel;
  s_panel = false;   // any key hides the output panel (F3-F5 and F7 show it); the key still counts
  const int rows = editRows();
  if (d && ac_key(d, k, rows)) return;   // the completion list takes it
  // F6-F10 are Shift+F1-F5 on this keyboard, so they come with Shift
  if ((k & 0xFF) >= KEY_F6 && (k & 0xFF) <= KEY_F10 && !(k & (UI_CTRL | UI_ALT))) k &= ~UI_SHIFT;
  if (d && d->readOnly && isRef(d)) {   // a reference file: viewing keys only
    switch (k) {
      case KEY_F3: case KEY_F4: case KEY_F4 | UI_CTRL: case KEY_F5:
        setMsg(YELLOW, "Reference file: Esc back to the project to build");
        return;
      case KEY_ESC:
        if (d->marking || ed_has_sel(d)) ed_unselect(d);
        else if (s_nback) goBack(rows);
        else pickFile();
        return;
    }
  }
  switch (k) {
    case KEY_F1: helpMenu(); break;
    case KEY_F2: if (d) ed_mark(d); break;
    case KEY_F3: { size_t size; free(build(&size)); break; }
    case KEY_F4: run(false); break;
    case KEY_F4 | UI_CTRL: run(true); break;
    case KEY_F5: exportApp(); break;
    case KEY_F6:
      ui_text_view("Output", s_outLen ? s_out : "(no output yet)");
      s_panel = false;
      break;
    case KEY_F7:
      if (!s_outLen) setMsg(UI_DIM, "No output yet (F3 builds)");
      else s_panel = !panel;
      break;
    case KEY_F8: goLastEdit(rows); break;
    case KEY_F9: gotoDecl(d, rows); break;
    case KEY_ESC:
      if (d && (d->marking || ed_has_sel(d))) ed_unselect(d);
      else pickFile();
      break;
    case 's' | UI_CTRL:
      if (saveAll()) setMsg(GREEN, "Saved");
      cfgSave();
      break;
    case 'g' | UI_CTRL: {
      char buf[12] = "";
      if (ui_input("Go to line", "Line number:", buf, sizeof buf) && atoi(buf) > 0) ed_goto(d, atoi(buf) - 1, 0, rows);
      break;
    }
    case 'f' | UI_CTRL:
      selectionText(d, s_find, sizeof s_find);
      if (!ui_input("Find", "Text to find:", s_find, sizeof s_find) || !s_find[0]) break;
      // fall through
    case 'n' | UI_CTRL:
      if (s_find[0] && !ed_find(d, s_find, rows)) setMsg(YELLOW, "\"%s\" not found", s_find);
      break;
    case 'r' | UI_CTRL: replace(d, rows); break;
    case 'b' | UI_CTRL:
      if (!d) break;
      if (!doc_toggle_break(d, d->cy)) setMsg(YELLOW, "Too many breakpoints in this file (%d)", DOC_MAX_BREAKS);
      else if (doc_has_break(d, d->cy)) setMsg(UI_DIM, "Breakpoint on line %d (Ctrl-F4 debugs)", d->cy + 1);
      else setMsg(UI_DIM, "Breakpoint off: line %d", d->cy + 1);
      break;
    case ' ' | UI_CTRL:
      if (d && d->readOnly) readOnlyMsg(d);
      else if (d && !ac_open(s_docs, s_ndocs, d, rows)) setMsg(UI_DIM, "No names to complete");
      break;
    default:
      if (d && ed_key(d, k, rows)) ac_after_key(d, k);
      else if (d && d->readOnly && !(k & UI_ALT) && (k & 0xFF) != KEY_ESC) readOnlyMsg(d);
      break;
  }
}

static void openCrash(const char *where, const char *why, const char *lastProj) {
  char path[192];
  int line, col;
  outClear();
  outPrintf("%s\n", why ? why : where);   // the report already names the place
  s_panel = true;
  if (!parseLoc(where, path, sizeof path, &line, &col)) return;
  char dir[160];
  if (strchr(path, '/')) {
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = 0;
  } else {
    snprintf(dir, sizeof dir, "%s", lastProj);
  }
  if (!dir[0] || !openProject(dir, path)) return;
  jumpTo(where);
  setMsg(RED, "The program crashed here");
}

void ide_run(const char *crashWhere, const char *crashWhy) {
  sdk_set_exit_handler(onBreak);
  ui_loader_hook = toLoader;
  updateBattery();
  {
    char proj[160], file[192];
    int line;
    cfgLoad(proj, file, &line);
    if (crashWhere) openCrash(crashWhere, crashWhy, proj);
    if (!s_ndocs && proj[0] && isDir(proj) && openProject(proj, file)) ed_goto(cur(), line - 1, 0, editRows());
  }
  if (setjmp(s_jmp)) sdk_reset();   // Break in the IDE: back to the editor
  for (;;) {
    if (!s_ndocs) { pickProject(); continue; }
    drawAll();
    const int k = waitKey();
    if (k == KEY_NONE) continue;
    s_msg[0] = 0;
    command(k);
  }
}
