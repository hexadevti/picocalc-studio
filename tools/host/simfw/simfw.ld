/*
 * simfw.ld - memory layout for programs run under rvsim: one flat RAM region at 0x40000000.
 * The emulator's RAM size (--mem, default 64 MB) must cover the image; rvsim puts the stack at
 * the top of RAM and the heap grows from _end up to 1 MB below that initial sp (syscalls.c).
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 64M
}

SECTIONS
{
    .text : {
        KEEP(*(.text.start))
        *(.text.unlikely .text.unlikely.*)
        *(.text .text.* .gnu.linkonce.t.*)
    } > RAM

    .rodata : ALIGN(8) {
        *(.rodata .rodata.* .gnu.linkonce.r.*)
        *(.srodata.cst16) *(.srodata.cst8) *(.srodata.cst4) *(.srodata.cst2) *(.srodata .srodata.*)
        *(.eh_frame_hdr) KEEP(*(.eh_frame)) *(.gcc_except_table .gcc_except_table.*)
    } > RAM

    .preinit_array : ALIGN(4) {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > RAM
    .init_array : ALIGN(4) {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > RAM
    .fini_array : ALIGN(4) {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > RAM

    .data : ALIGN(8) {
        *(.data .data.* .gnu.linkonce.d.*)
        . = ALIGN(8);
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.* .gnu.linkonce.s.*)
    } > RAM

    .bss (NOLOAD) : ALIGN(8) {
        __bss_start = .;
        *(.sbss .sbss.* .gnu.linkonce.sb.*)
        *(.bss .bss.* .gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > RAM

    . = ALIGN(16);
    _end = .;
    PROVIDE(end = .);
    __stack_top = ORIGIN(RAM) + LENGTH(RAM);

    /DISCARD/ : { *(.note.GNU-stack) *(.comment) }
}
