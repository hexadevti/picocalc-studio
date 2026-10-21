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

// Parsed view of a module image held in memory (pointers into the buffer).
typedef struct {
  const PcxHeader *h;
  const uint8_t *text, *data;
  const uint32_t *imports;
  const PcxReloc *relocs;
  const PcxLine *lines;
  const uint32_t *files;
  const char *strtab;
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

#ifdef __cplusplus
}
#endif
