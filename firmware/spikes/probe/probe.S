# Phase-0 probe routine, copied verbatim into flash and executed from an mmap'd page.
# int probe(int a, int (*cb)(int), int k) = cb(a) + k + IMM + DATA_WORD
# - jalr into firmware: calls back across the mapping
# - auipc + lw: reads a constant through the same mapping (rodata next to code)
# IMM differs between v1 (1) and v2 (7) so a stale cache line shows up as the wrong result.
    .option norvc
    .text
    .globl probe_v1
probe_v1:
    addi sp, sp, -16
    sw   ra, 12(sp)
    sw   s0, 8(sp)
    mv   s0, a2
    jalr a1
    add  a0, a0, s0
    addi a0, a0, 1
1:  auipc t0, %pcrel_hi(data_v1)
    lw   t1, %pcrel_lo(1b)(t0)
    add  a0, a0, t1
    lw   s0, 8(sp)
    lw   ra, 12(sp)
    addi sp, sp, 16
    ret
data_v1: .word 1000
    .globl probe_v2
probe_v2:
    addi sp, sp, -16
    sw   ra, 12(sp)
    sw   s0, 8(sp)
    mv   s0, a2
    jalr a1
    add  a0, a0, s0
    addi a0, a0, 7
2:  auipc t0, %pcrel_hi(data_v2)
    lw   t1, %pcrel_lo(2b)(t0)
    add  a0, a0, t1
    lw   s0, 8(sp)
    lw   ra, 12(sp)
    addi sp, sp, 16
    ret
data_v2: .word 2000
