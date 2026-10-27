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
//               offset of each stop point, ascending), then n_funcs x PcxFunc (ascending, names
//               in strtab). Other loaders ignore it.
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
  uint32_t n_funcs;
} PcxDebug;

// A function's code and frame. Its s0 is the sp at entry; the saved ra is at s0 - va - 4, the
// caller's s0 at s0 - va - 8, then the registers of save_x and save_f in register order.
typedef struct {
  uint32_t text_lo, text_hi;  // [text_lo, text_hi)
  uint32_t name;              // offset in strtab
  uint32_t va;                // 32 for a variadic function (its argument registers), else 0
  uint32_t save_x, save_f;    // bit r: x / f register r is saved
} PcxFunc;

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
  const PcxFunc *funcs;    // dbg->n_funcs entries
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

// The function whose code holds a text offset; null if none (or not a debug build).
const PcxFunc *pcx_func_at(const PcxView *v, uint32_t text_off);

// ---- stepping (pcx_dbg.c) ----
// Which stop points stop the program. Breakpoints are kept per stop; the flags the stop points
// check are the breakpoints while running and all set while stepping, and pcx_dbg_check() decides.
//
// A place in the program is a text offset inside its line: a stop point's own offset, or a return
// address minus 1 (inside the call, whose line the caller is on).
//
// Stepping also catches the stepped function's return: its saved ra is patched to point at
// pcx_dbg_ret_tramp (pcx_dbg_ret.S), which calls __pc_dbg_return() with the registers at the
// return site, and goes on to the real return address. Without it, finish would stop in the
// caller's next call (all of a caller's callees share one s0) rather than in the caller.
enum { PCX_DBG_RUN, PCX_DBG_STEP_INTO, PCX_DBG_STEP_OVER, PCX_DBG_STEP_OUT, PCX_DBG_PAUSE };

#define PCX_DBG_RETS 16

typedef struct {
  const PcxView *v;
  uint32_t text;           // address of the module's text
  uint32_t n;              // stop points
  uint8_t *flags;          // __dbg_flags in the module's ram
  uint8_t *bp;             // n bytes: 1 = breakpoint
  volatile int mode;       // PCX_DBG_*
  uint32_t s0;             // where the step started: its frame, file and line
  const char *file;
  int line;
  int caught;              // the step's frame returns through the trampoline
  struct { uint32_t s0, ra; } ret[PCX_DBG_RETS];  // patched frames and their real return address
  int nret;
} PcxDbg;

// One frame of the call stack: a place (see above), its frame pointer and its function.
typedef struct {
  uint32_t at;
  uint32_t s0;
  const PcxFunc *fn;
} PcxFrame;

// The firmware's return trampoline (pcx_dbg_ret.S), and what it calls: the program's runner
// defines __pc_dbg_return(), which calls pcx_dbg_return().
void pcx_dbg_ret_tramp(void);
void __pc_dbg_return(PcxDbgRegs *regs);

// bp: n_stops bytes the caller owns; text: where the module's code is. Clears the breakpoints and
// the flags; mode RUN.
void pcx_dbg_init(PcxDbg *d, const PcxView *v, uint32_t text, uint8_t *ram, uint8_t *bp);
// Sets (on) or clears a breakpoint on `line` of `file` (a path, or its last components). A line
// without a stop point gets the next line that has one. Returns that line, or 0 if none.
int pcx_dbg_break(PcxDbg *d, const char *file, int line, int on);
// At stop point id with frame pointer s0 (x[8]): nonzero if the program stops here.
int pcx_dbg_check(PcxDbg *d, int id, uint32_t s0);
// Leaves the place `at` (frame s0) in mode PCX_DBG_*. at = UINT32_MAX: the program's start.
void pcx_dbg_resume(PcxDbg *d, int mode, uint32_t at, uint32_t s0);
// In __pc_dbg_return: puts the real return address in regs->x[5], where the trampoline jumps.
// Returns 1 if the program stops at the return site (regs->x[5] - text - 1 is the place), 0 if
// not, -1 if the frame was never patched (the program is broken).
int pcx_dbg_return(PcxDbg *d, PcxDbgRegs *regs);
// The call stack from place `at` with frame pointer s0, innermost first: up to max frames, to the
// first function outside the module (the firmware called it). Returns the number of frames.
int pcx_dbg_backtrace(const PcxDbg *d, uint32_t at, uint32_t s0, PcxFrame *f, int max);
// Stops at the next stop point, whatever it is (Break in a debug run).
void pcx_dbg_pause(PcxDbg *d);

#ifdef __cplusplus
}
#endif
