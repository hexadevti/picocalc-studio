// asm_rv32.h - RV32IMFA machine-code emitter used by codegen_rv32.c (and patch helpers for link.c).
//
// Code goes straight into a CcObject's text section. Branch targets are per-function labels
// (ints from asm_new_label) resolved by asm_end_function(); references to symbols become
// CcReloc entries the linker resolves.
#pragma once
#include <stdint.h>
#include "cc.h"

// Integer registers
enum {
  ZERO = 0, RA = 1, SP = 2, GP = 3, TP = 4, T0 = 5, T1 = 6, T2 = 7, S0 = 8, S1 = 9,
  A0 = 10, A1, A2, A3, A4, A5, A6, A7,
  S2 = 18, S11 = 27,
  T3 = 28, T4, T5, T6,
};
// Floating-point registers
enum { FT0 = 0, FT1 = 1, FA0 = 10 };

// Section helpers
void sect_append(CcSection *s, const void *data, uint32_t n);
void sect_align(CcSection *s, uint32_t align);

// Begins emitting into obj's text section (call once per object).
void asm_init(CcObject *obj);
uint32_t asm_pc(void);  // current text offset
void emit32(uint32_t insn);

// Labels (valid within one function)
int asm_new_label(void);
void asm_bind(int label);
void asm_end_function(void);  // resolves fixups; errors if a label is unbound

// Two-pass functions: asm_begin_function() marks the start; asm_rollback() discards everything
// emitted since (text, labels, fixups, relocations) and starts the second pass, which uses the
// first pass's label positions to emit short forward branches. The second pass must emit the
// same branches and labels in the same order, with code that is nowhere longer.
void asm_begin_function(void);
void asm_rollback(void);

// The last instruction, if it can be rewritten: nothing branches to the current pc.
uint32_t *asm_last_insn(void);

// Encoders
void rv_r(int f7, int rs2, int rs1, int f3, int rd, int op);
void rv_i(int imm, int rs1, int f3, int rd, int op);
void rv_s(int imm, int rs2, int rs1, int f3, int op);

#define OP_LOAD 0x03
#define OP_LOADFP 0x07
#define OP_IMM 0x13
#define OP_AUIPC 0x17
#define OP_STORE 0x23
#define OP_STOREFP 0x27
#define OP_AMO 0x2f
#define OP_OP 0x33
#define OP_LUI 0x37
#define OP_FP 0x53
#define OP_BRANCH 0x63
#define OP_JALR 0x67
#define OP_JAL 0x6f

// Common instructions
void rv_addi(int rd, int rs, int imm);       // imm must fit in 12 bits
void rv_li(int rd, int32_t imm);
void rv_mv(int rd, int rs);
void rv_add(int rd, int a, int b);
void rv_sub(int rd, int a, int b);
void rv_op(int f7, int f3, int rd, int a, int b);  // OP opcode, e.g. rv_op(0,7,..) = and
void rv_opi(int f3, int rd, int rs, int imm);      // OP-IMM (andi/ori/xori/slti/sltiu)
void rv_shi(int f3, int f7, int rd, int rs, int sh);  // slli/srli/srai
// Load/store with any offset (uses T5 when the offset does not fit in 12 bits).
// width: 1,2,4; is_unsigned selects lbu/lhu.
void rv_load(int rd, int base, int32_t off, int width, int is_unsigned);
void rv_store(int rs, int base, int32_t off, int width);
void rv_flw(int fd, int base, int32_t off);
void rv_fsw(int fs, int base, int32_t off);
void rv_addr(int rd, int base, int32_t off);  // rd = base + off (any off)
void rv_fp(int f7, int rs2, int rs1, int rm, int rd);  // OP-FP

// Branch if (a cond b) to label. cond is the B-type funct3 (BEQ..BGEU).
enum { BEQ = 0, BNE = 1, BLT = 4, BGE = 5, BLTU = 6, BGEU = 7 };
void rv_branch(int cond, int a, int b, int label);
void rv_jump(int label);
void rv_label_addr(int rd, int label);  // rd = address of label (auipc + addi)

// Symbol references (emit auipc+I-type pair with a CC_R_PAIR relocation)
void rv_sym_addr(int rd, int sym, int32_t addend, int line);  // auipc rd; addi rd, rd
// auipc rd only: the next instruction must be a load/store at offset 0 from rd.
void rv_sym_hi(int rd, int sym, int32_t addend, int line);
void rv_call_sym(int sym, int line);                         // auipc ra; jalr ra, ra

// Patches an auipc + (I or S-type) pair at p for a pc-relative displacement.
void rv_patch_pair(uint8_t *p, int32_t pcrel);
