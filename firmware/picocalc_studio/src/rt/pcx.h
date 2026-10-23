// pcx.h - PicoCalc executable module (.pcx): the linker's output and the loader's input.
//
// A module is position independent apart from its relocation list. It has two blocks:
//   text  code, placed anywhere executable (flash on the device, RAM in the simulator)
//   ram   rodata + data (initialized, data_size bytes) followed by bss (bss_size bytes, zeroed)
// Calls between functions are already resolved (text is contiguous), so the relocations only
// cover references from one block to the other block or to a firmware export.
//
// File layout (all fields little endian):
//   PcxHeader
//   text        text_size bytes
//   data        data_size bytes
//   imports     n_imports x uint32_t: offset of the name in strtab
//   relocs      n_relocs x PcxReloc
//   lines       n_lines x PcxLine (sorted by text offset)
//   files       n_files x uint32_t: offset of the source file name in strtab
//   strtab      strtab_size bytes of NUL-terminated names
//   [debug]     debug builds only, 4-byte aligned: PcxDebug, then n_stops x uint32_t (the text
//               offset of each stop point, ascending). Other loaders ignore it.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCX_MAGIC 0x31584350u   // "PCX1"
#define PCX_VERSION 1
#define PCX_NO_ENTRY 0xffffffffu

typedef struct {
  uint32_t magic, version;
  uint32_t text_size, data_size, bss_size;
  uint32_t ram_align;                         // required alignment of the ram block (power of 2)
  uint32_t n_imports, n_relocs, n_lines, n_files, strtab_size;
  uint32_t entry_main, entry_setup, entry_loop; // text offsets, PCX_NO_ENTRY if absent
} PcxHeader;

// Where a relocation applies: (where & PCX_W_RAM) selects the block, the rest is the offset.
#define PCX_W_RAM 0x80000000u
#define PCX_W_OFFSET(w) ((w) & 0x0fffffffu)
#define PCX_W_TYPE(w) (((w) >> 28) & 7u)
enum {
  PCX_R_PAIR = 0,   // auipc rd, hi20 at `where`; I-type (addi/jalr/load) using rd at where+4
  PCX_R_ABS32 = 1,  // 32-bit word at `where` = target address
};

// What a relocation points at: kind in the top byte, import index in the low 24 bits.
#define PCX_T_KIND(t) ((t) >> 24)
#define PCX_T_INDEX(t) ((t) & 0xffffffu)
enum { PCX_T_TEXT = 0, PCX_T_RAM = 1, PCX_T_IMPORT = 2 };

typedef struct {
  uint32_t where;   // block bit | type << 28 | offset
  uint32_t target;  // kind << 24 | import index
  int32_t addend;   // offset in the target block (or added to the import's address)
} PcxReloc;

typedef struct {
  uint32_t text_off;  // first instruction of the statement
  uint16_t file;      // index into the file table
  uint16_t line;
} PcxLine;

// Debug builds (docs/DEBUGGER.md). Stop point i checks the byte at ram + flags_off + i.
#define PCX_DEBUG_MAGIC 0x44584350u  // "PCXD"
typedef struct {
  uint32_t magic;
  uint32_t n_stops;
  uint32_t flags_off;  // offset of __dbg_flags in the ram block (bss)
} PcxDebug;

// What a stop point's stub hands __pc_dbg_stop(): every register as it was at the stop point.
// x[2] is its sp, x[5] (t0) the address just after its jal; x[0] is unused. f[] and fcsr are only
// saved on targets with an F extension. Changes made here are loaded back when the stop returns.
typedef struct {
  uint32_t x[32];
  uint32_t f[32];
  uint32_t fcsr;
} PcxDbgRegs;

// Parsed view of a module image held in memory (pointers into the buffer).
typedef struct {
  const PcxHeader *h;
  const uint8_t *text, *data;
  const uint32_t *imports;
  const PcxReloc *relocs;
  const PcxLine *lines;
  const uint32_t *files;
  const char *strtab;
  const PcxDebug *dbg;     // null unless a debug build
  const uint32_t *stops;   // dbg->n_stops entries
} PcxView;

// Checks the header and the table bounds. Returns 0, or -1 with a message in err.
int pcx_parse(const void *buf, size_t len, PcxView *v, char *err, int errlen);

// Resolves imports and applies relocations. text_buf is a writable copy of the text that will
// execute at text_addr (text_buf may equal text_addr); ram is the ram block (data_size +
// bss_size bytes, ram_align aligned), which this function fills (data copied, bss zeroed).
// resolve() returns a firmware symbol's address or NULL. Returns 0, or -1 with a message.
int pcx_relocate(const PcxView *v, uint8_t *text_buf, uint32_t text_addr, uint8_t *ram,
                 void *(*resolve)(const char *name), char *err, int errlen);

// Maps a text offset to its source location; returns 0 if not found.
int pcx_lookup_line(const PcxView *v, uint32_t text_off, const char **file, int *line);

// The stop point a stop came from, given the text offset its stub returns to (x[5] minus the
// text address); -1 if none.
int pcx_stop_index(const PcxView *v, uint32_t ret_off);

// ---- stepping (pcx_dbg.c) ----
// Which stop points stop the program. Breakpoints are kept per stop; the flags the stop points
// check are the breakpoints while running and all set while stepping, and pcx_dbg_check() decides.
enum { PCX_DBG_RUN, PCX_DBG_STEP_INTO, PCX_DBG_STEP_OVER, PCX_DBG_STEP_OUT, PCX_DBG_PAUSE };

typedef struct {
  const PcxView *v;
  uint32_t n;              // stop points
  uint8_t *flags;          // __dbg_flags in the module's ram
  uint8_t *bp;             // n bytes: 1 = breakpoint
  volatile int mode;       // PCX_DBG_*
  uint32_t s0;             // where the step started: its frame, file and line
  const char *file;
  int line;
} PcxDbg;

// bp: n_stops bytes the caller owns. Clears the breakpoints and the flags; mode RUN.
void pcx_dbg_init(PcxDbg *d, const PcxView *v, uint8_t *ram, uint8_t *bp);
// Sets (on) or clears a breakpoint on `line` of `file` (a path, or its last components). A line
// without a stop point gets the next line that has one. Returns that line, or 0 if none.
int pcx_dbg_break(PcxDbg *d, const char *file, int line, int on);
// At stop point id with frame pointer s0 (x[8]): nonzero if the program stops here.
int pcx_dbg_check(PcxDbg *d, int id, uint32_t s0);
// Leaves stop point id (frame s0) in mode PCX_DBG_*.
void pcx_dbg_resume(PcxDbg *d, int mode, int id, uint32_t s0);
// Stops at the next stop point, whatever it is (Break in a debug run).
void pcx_dbg_pause(PcxDbg *d);

#ifdef __cplusplus
}
#endif
