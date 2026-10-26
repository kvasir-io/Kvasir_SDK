/* an image every post-link step passes (linked with tests/elf_image/flash.ld): a reset entry of the naked shape, a
   chip's own code section no tool names (.chip_code: the listing must disassemble it, read from the ELF), loaded data
   (.data: in the listing too) and a section that is not loaded (.kvasir_meta: not in it) */
    .syntax unified
    .thumb

    .section .vectors,"a",%progbits
    .word 0x20001000
    .word ResetISR       /* a Thumb function symbol: bit 0 set */

    .section .text.entry,"ax",%progbits
    .globl entry
    .globl ResetISR
    .type ResetISR,%function
entry:
ResetISR:
    cpsid i
    bl body
body:
    b body
    /* check_reset_entry reads 64 bytes from the entry */
    .space 64

    .section .chip_code,"ax",%progbits
    .globl chip_routine
    .type chip_routine,%function
chip_routine:
    bx lr

    .section .data.value,"aw",%progbits
    .word 0xD47AD47A

    .section .kvasir_meta,"",%progbits
    .word 0xBADBADBA
