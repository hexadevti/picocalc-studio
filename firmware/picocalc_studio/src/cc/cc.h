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
#include <stdbool.h>
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

// Nonzero: the target has no F extension (RP2350's Hazard3). float values then live in integer
// registers (ilp32 ABI) and arithmetic goes through libgcc's __addsf3 etc. Defaults to what the
// compiler itself was built for; the host tools set it with -msoft-float.
extern int cc_soft_float;

// Nonzero: debug build. Every statement starts with a stop point (docs/DEBUGGER.md): a check of
// its byte in __dbg_flags that calls the firmware's __pc_dbg_stop() when set. The image gets a
// PcxDebug trailer (rt/pcx.h). 0 (the default) emits exactly the code of a normal build.
extern int cc_debug;

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
  uint32_t offset;
  int32_t addend;
  uint32_t sym : 24;   // index into syms
  uint32_t sect : 4;   // section the relocation patches
  uint32_t type : 4;   // CC_R_*
  int line;            // source line of the reference (0 if unknown)
} CcReloc;

#define CC_RELOC_BLOCK 512  // relocations per block of CcObject.relocs (8 KB)

typedef struct {
  uint32_t text_off;
  int file;            // index into CcObject.files
  int line;
} CcLine;

typedef struct {
  uint8_t *data;
  uint32_t size;
} CcPart;

// A section's bytes [0, base) are in parts[], finished; data holds [base, size), in cap bytes.
// (Only .text is split: its code is not touched again once the function is done.)
typedef struct {
  uint8_t *data;
  uint32_t size, cap, align;
  uint32_t base;
  CcPart *parts;
  int nparts;
} CcSection;

typedef struct {
  char *path;          // main source file
  CcSection sect[CC_NSECT];
  CcSym *syms;
  int nsyms, capsyms;
  CcReloc **relocs;    // relocation i is relocs[i / CC_RELOC_BLOCK][i % CC_RELOC_BLOCK]
  int nrelocs, caprelocs;
  CcLine *lines;
  int nlines, caplines;
  char **files;        // source files the line table refers to
  int nfiles, capfiles;
  uint32_t *stops;     // cc_debug: text offset of each stop point; its index is its flag's
  int nstops, capstops;  // offset from __dbg_flags
} CcObject;

// Compiles one C file. Returns 0, or -1 with the message in cc_error().
int cc_compile(const CcOptions *opt, const char *path, CcObject **out);

// Links objects into a .pcx image (malloc'ed, caller frees). Returns 0 or -1. With memory lent
// (cc_arena_lend()), the image is put there if it fits: cc_arena_is_lent() tells, and then the
// caller copies it out before taking the memory back, and does not free it.
int cc_link(CcObject **objs, int n, const CcOptions *opt, uint8_t **image, size_t *size);

// The last error: "file:line:col: error: message\n  source\n  ^\n" or "error: message".
const char *cc_error(void);

// Frees every allocation the compiler made (objects included).
void cc_arena_free(void);
size_t cc_arena_used(void);
size_t cc_arena_peak(void);  // most chunk memory held at once since cc_arena_free()
// Lets the compiler take its arena chunks and grown arrays from buf (until cc_arena_lend(NULL, 0))
// before the heap. Every arena must be empty both times: lend before a compile and take back
// after cc_arena_free().
void cc_arena_lend(void *buf, size_t n);
bool cc_arena_is_lent(const void *p);  // whether p is in the memory lent now
size_t cc_lent_peak(void);             // most of it in use at once since cc_arena_lend()
// Bytes of stack a compile may use below cc_compile(): code nested deeper is an error instead of an
// overflow. 0 (the default) means no limit.
extern size_t cc_stack_limit;

// Optional hooks.
extern char *(*cc_read_file_hook)(const char *path, size_t *len);  // in-memory sources
extern void (*cc_diag_hook)(const char *msg);                      // warnings (default: stderr)
// Called before an out-of-memory error: what was asked for (a chunk, array or image of n bytes),
// and the free bytes and biggest free block of the lent memory.
extern void (*cc_oom_hook)(const char *what, size_t n, size_t lent_free, size_t lent_largest);

#ifdef __cplusplus
}
#endif
