// cc.h - PicoCalc C compiler: public interface.
//
// Typical build of a multi-file program:
//   CcOptions opt = { .include_paths = paths, .n_include_paths = n };
//   CcObject *objs[N];
//   for each file: if (cc_compile(&opt, file, &objs[i])) -> show cc_error()
//   if (cc_link(objs, N, &opt, &image, &size)) -> show cc_error()
//   cc_arena_free();            // all compiler memory except the linked image
//   ... load image (rt/pcx.h), free(image)
//
// The compiler is not reentrant: one build at a time.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char **include_paths;   // searched for #include <...> (and after the file's dir for "...")
  int n_include_paths;
  const char **defines;         // "NAME" or "NAME=value"
  int n_defines;
  // Link: names the firmware exports; returns nonzero if `name` can be imported.
  int (*have_import)(const char *name);
} CcOptions;

// In-memory object file (one translation unit). Lives in the compiler arena.
enum { CC_TEXT, CC_RODATA, CC_DATA, CC_BSS, CC_NSECT };
enum { CC_R_PAIR, CC_R_ABS32 };  // same encodings as PCX_R_PAIR / PCX_R_ABS32

typedef struct {
  char *name;
  int sect;            // CC_TEXT.. or -1 if undefined (external reference)
  uint32_t offset, size;
  uint8_t is_global;   // visible to other objects
  uint8_t is_common;   // tentative definition (int x;): duplicates merge
  int line;            // first reference/definition line, for diagnostics
} CcSym;

typedef struct {
  int sect;            // section the relocation patches
  uint32_t offset;
  int type;            // CC_R_*
  int sym;             // index into syms
  int32_t addend;
  int line;            // source line of the reference (0 if unknown)
} CcReloc;

typedef struct {
  uint32_t text_off;
  int file;            // index into CcObject.files
  int line;
} CcLine;

typedef struct {
  uint8_t *data;
  uint32_t size, cap, align;
} CcSection;

typedef struct {
  char *path;          // main source file
  CcSection sect[CC_NSECT];
  CcSym *syms;
  int nsyms, capsyms;
  CcReloc *relocs;
  int nrelocs, caprelocs;
  CcLine *lines;
  int nlines, caplines;
  char **files;        // source files the line table refers to
  int nfiles, capfiles;
} CcObject;

// Compiles one C file. Returns 0, or -1 with the message in cc_error().
int cc_compile(const CcOptions *opt, const char *path, CcObject **out);

// Links objects into a .pcx image (malloc'ed, caller frees). Returns 0 or -1.
int cc_link(CcObject **objs, int n, const CcOptions *opt, uint8_t **image, size_t *size);

// The last error: "file:line:col: error: message\n  source\n  ^\n" or "error: message".
const char *cc_error(void);

// Frees every allocation the compiler made (objects included).
void cc_arena_free(void);
size_t cc_arena_used(void);

// Optional hooks.
extern char *(*cc_read_file_hook)(const char *path, size_t *len);  // in-memory sources
extern void (*cc_diag_hook)(const char *msg);                      // warnings (default: stderr)

#ifdef __cplusplus
}
#endif
