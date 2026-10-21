// pcc_main.c - host driver for the PicoCalc C compiler (firmware/picocalc_studio/src/cc).
//
//   pcc [-I dir]... [-D name[=value]]... [--exports file] file.c... -o out.pcx
//
// Defaults: -I <repo>/sdcard/pico-dev/include, --exports <repo>/firmware/.../src/rt/exports_table.S
// (both found relative to this executable in <repo>/build/host). Built by build_host.py.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cc.h"

static char *exports[1024];
static int nexports;

static int have_import(const char *name) {
  for (int i = 0; i < nexports; i++)
    if (!strcmp(exports[i], name))
      return 1;
  return 0;
}

// exports_table.S (generated, every export) or exports.txt (one name per line)
static void load_exports(const char *path) {
  FILE *fp = fopen(path, "r");
  if (!fp) {
    fprintf(stderr, "pcc: cannot open %s\n", path);
    exit(2);
  }
  int is_asm = strlen(path) > 2 && !strcmp(path + strlen(path) - 2, ".S");
  char line[256];
  while (fgets(line, sizeof(line), fp) && nexports < 1024) {
    char *p = line;
    if (is_asm) {
      char *q = strstr(line, ".asciz \"");
      if (!q)
        continue;
      q += 8;
      char *e = strchr(q, '"');
      if (e) {
        *e = '\0';
        exports[nexports++] = strdup(q);
      }
      continue;
    }
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '#' || *p == '\n' || *p == '\r' || !*p)
      continue;
    char *e = p;
    while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r' && *e != '#' && *e != '=')
      e++;
    *e = '\0';
    exports[nexports++] = strdup(p);
  }
  fclose(fp);
}

static char *repo_path(const char *argv0, const char *rel) {
  // argv0 = <repo>/build/host/pcc(.exe)
  char *buf = malloc(strlen(argv0) + strlen(rel) + 16);
  strcpy(buf, argv0);
  for (char *p = buf; *p; p++)
    if (*p == '\\')
      *p = '/';
  for (int up = 0; up < 3; up++) {
    char *s = strrchr(buf, '/');
    if (!s) {
      strcpy(buf, "../..");
      break;
    }
    *s = '\0';
  }
  strcat(buf, "/");
  strcat(buf, rel);
  return buf;
}

int main(int argc, char **argv) {
  const char *incs[64], *defs[64], *srcs[64];
  int ninc = 0, ndef = 0, nsrc = 0;
  const char *out = NULL, *exp = NULL;

  for (int i = 1; i < argc; i++) {
    char *a = argv[i];
    if (!strcmp(a, "-o") && i + 1 < argc)
      out = argv[++i];
    else if (!strcmp(a, "--exports") && i + 1 < argc)
      exp = argv[++i];
    else if (!strncmp(a, "-I", 2))
      incs[ninc++] = a[2] ? a + 2 : argv[++i];
    else if (!strncmp(a, "-D", 2))
      defs[ndef++] = a[2] ? a + 2 : argv[++i];
    else if (a[0] == '-') {
      fprintf(stderr, "pcc: unknown option %s\n", a);
      return 2;
    } else
      srcs[nsrc++] = a;
  }
  if (!nsrc || !out) {
    fprintf(stderr, "usage: pcc [-I dir] [-D name[=val]] [--exports file] file.c... -o out.pcx\n");
    return 2;
  }
  incs[ninc++] = repo_path(argv[0], "sdcard/pico-dev/include");
  load_exports(exp ? exp : repo_path(argv[0], "firmware/picocalc_studio/src/rt/exports_table.S"));

  CcOptions opt = {incs, ninc, defs, ndef, have_import};
  CcObject *objs[64];
  for (int i = 0; i < nsrc; i++) {
    if (cc_compile(&opt, srcs[i], &objs[i])) {
      fputs(cc_error(), stderr);
      fputc('\n', stderr);
      return 1;
    }
  }
  uint8_t *image;
  size_t size;
  if (cc_link(objs, nsrc, &opt, &image, &size)) {
    fputs(cc_error(), stderr);
    fputc('\n', stderr);
    return 1;
  }
  FILE *fp = fopen(out, "wb");
  if (!fp || fwrite(image, 1, size, fp) != size) {
    fprintf(stderr, "pcc: cannot write %s\n", out);
    return 1;
  }
  fclose(fp);
  free(image);
  cc_arena_free();
  return 0;
}
