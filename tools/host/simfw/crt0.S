/*
 * crt0.S - simfw entry point. rvsim enters at _start with sp below the argv block at the top of
 * RAM, a0 = argc and a1 = argv. Sets gp, clears .bss, enables the FPU and hands over to
 * __simfw_cstart (syscalls.c), which runs the constructors and calls exit(main(argc, argv)).
 */
    .section .text.start, "ax"
    .globl _start
    .type _start, @function
_start:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    bnez    sp, 1f                  /* no stack from the loader: use the linker's */
    la      sp, __stack_top
1:  mv      s0, a0
    mv      s1, a1
    la      t0, __bss_start
    la      t1, __bss_end
2:  bgeu    t0, t1, 3f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       2b
3:  li      t0, 0x2000              /* mstatus.FS = initial: FPU on (as the ESP-IDF startup does) */
    csrs    mstatus, t0
    fscsr   zero                    /* round to nearest, flags clear */
    mv      a0, s0
    mv      a1, s1
    mv      a2, sp
    call    __simfw_cstart
    ebreak                          /* not reached */
    .size _start, . - _start
