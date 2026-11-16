/* The sections the SDK's linker scripts lay out, with sizes that are not multiples of 8 (alignment is part of what
   test_stack_fill.py checks). BOOT2 adds a section no script names, as chip_rp2040's RAM-only image has; ALIGNED a
   2 KiB-aligned .bss input. */
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
#ifdef ALIGNED
    /* a DMA ring aligned to its size, as a UART RX ring: up to 2 KiB of padding before .bss that no SIZEOF counts */
    .section .bss.ring, "aw", %nobits
    .balign 2048
    .space 2048
#endif
#ifdef BOOT2
    .section .boot2, "ax"
    .space 256, 2
#endif
