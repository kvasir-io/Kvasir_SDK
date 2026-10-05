/* The sections the SDK's linker scripts lay out, with sizes that are not multiples of 8 (alignment is part of what
   test_stack_fill.py checks). BOOT2 adds a section no script names, as chip_rp2040's RAM-only image has. */
    .syntax unified
    .thumb
    .section .core_vectors, "a"
    .word _LINKER_stack_end_
    .word ResetISR
    .text
    .globl ResetISR
    .type ResetISR, %function
    .thumb_func
ResetISR:
    b ResetISR
    .space 1234
    .data
    .space 301, 1
    .bss
    .space 777
    .section .noInit, "aw", %nobits
    .space 45
    .section .stackProtector, "aw", %nobits
    .space 32
#ifdef BOOT2
    .section .boot2, "ax"
    .space 256, 2
#endif
