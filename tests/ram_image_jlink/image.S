/* a RAM image's vector table (word 0 the initial SP, word 1 the reset entry) at _LINKER_vectors_start_ (ram.ld);
   -DBAD_SP: an SP that is not word-aligned */
    .syntax unified
    .thumb

    .section .vectors,"a",%progbits
#if defined(BAD_SP)
    .word 0x20001002
#else
    .word 0x20001000
#endif
    .word entry          /* a Thumb function symbol: bit 0 set */

    .section .text,"ax",%progbits
    .globl entry
    .type entry,%function
entry:
    b entry
