/* The sections the SDK's linker scripts lay out (as tests/stack_fill/image.S). TAIL adds .tail.data, initialised
   data that flash_tail.ld loads into flash after .data, as a chip's script loads its own sections there. */
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
#ifdef TAIL
    .section .tail.data, "aw"
    .space 164, 3
#endif
