// pcx_dbg_ret.S - the return trampoline of debug runs (pcx.h, docs/DEBUGGER.md section 3).
//
// Stepping patches the stepped function's saved ra to point here, so its return lands here with
// the caller's registers: the return value in a0/a1 (fa0), sp and s0 restored. Like the stop
// stub that pcc puts in each debug object, it saves every register in a PcxDbgRegs block, calls
// __pc_dbg_return(regs), restores them all and jumps to regs->x[5] (t0, free at a return site),
// which __pc_dbg_return set to the real return address. x[2] holds the sp it was entered with.

#if defined(__riscv)

#if defined(__riscv_flen)
#define FRAME 272   // sizeof(PcxDbgRegs), 16-byte aligned
#else
#define FRAME 128   // x[] only
#endif

  .text
  .globl pcx_dbg_ret_tramp
  .type pcx_dbg_ret_tramp, @function
  .balign 4
pcx_dbg_ret_tramp:
  addi sp, sp, -16
  sw t1, 0(sp)
  addi t1, sp, 16
  andi sp, sp, -16
  addi sp, sp, -FRAME
  sw t1, 8(sp)          // x[2]: the sp at the return site
  lw t1, -16(t1)
  sw x1, 4(sp)
  sw x3, 12(sp)
  sw x4, 16(sp)
  sw x5, 20(sp)
  sw x6, 24(sp)
  sw x7, 28(sp)
  sw x8, 32(sp)
  sw x9, 36(sp)
  sw x10, 40(sp)
  sw x11, 44(sp)
  sw x12, 48(sp)
  sw x13, 52(sp)
  sw x14, 56(sp)
  sw x15, 60(sp)
  sw x16, 64(sp)
  sw x17, 68(sp)
  sw x18, 72(sp)
  sw x19, 76(sp)
  sw x20, 80(sp)
  sw x21, 84(sp)
  sw x22, 88(sp)
  sw x23, 92(sp)
  sw x24, 96(sp)
  sw x25, 100(sp)
  sw x26, 104(sp)
  sw x27, 108(sp)
  sw x28, 112(sp)
  sw x29, 116(sp)
  sw x30, 120(sp)
  sw x31, 124(sp)
#if defined(__riscv_flen)
  .irp f, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
  fsw f\f, (128 + 4 * \f)(sp)
  .endr
  frcsr t1
  sw t1, 256(sp)
#endif
  mv a0, sp
  call __pc_dbg_return
#if defined(__riscv_flen)
  lw t1, 256(sp)
  fscsr t1
  .irp f, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
  flw f\f, (128 + 4 * \f)(sp)
  .endr
#endif
  lw x1, 4(sp)
  lw x3, 12(sp)
  lw x4, 16(sp)
  lw x5, 20(sp)
  lw x6, 24(sp)
  lw x7, 28(sp)
  lw x8, 32(sp)
  lw x9, 36(sp)
  lw x10, 40(sp)
  lw x11, 44(sp)
  lw x12, 48(sp)
  lw x13, 52(sp)
  lw x14, 56(sp)
  lw x15, 60(sp)
  lw x16, 64(sp)
  lw x17, 68(sp)
  lw x18, 72(sp)
  lw x19, 76(sp)
  lw x20, 80(sp)
  lw x21, 84(sp)
  lw x22, 88(sp)
  lw x23, 92(sp)
  lw x24, 96(sp)
  lw x25, 100(sp)
  lw x26, 104(sp)
  lw x27, 108(sp)
  lw x28, 112(sp)
  lw x29, 116(sp)
  lw x30, 120(sp)
  lw x31, 124(sp)
  lw sp, 8(sp)
  jr t0
  .size pcx_dbg_ret_tramp, . - pcx_dbg_ret_tramp

#endif  // __riscv
