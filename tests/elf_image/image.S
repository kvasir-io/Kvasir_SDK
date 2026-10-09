/* the sections elf_image.py has to sort: loaded ones (by no name list), NOLOAD, non-ALLOC, .eeprom */
    .syntax unified
    .thumb

    .section .vectors,"a",%progbits
    .word 0x20001000
    .word entry + 1

    .section .text.entry,"ax",%progbits
    .globl entry
    .type entry,%function
entry:
    b entry
#if defined(BIG)
    /* past 64 KiB from address 0: llvm-objcopy's hex switches to type 02 records */
    .fill 0x11000, 1, 0xA5
#endif

    .section .rodata.table,"a",%progbits
    .word 0x11223344, 0x55667788

    .section .data.value,"aw",%progbits
    .word 0xD47AD47A

    /* an initialised RAM section no list names - the trap the name list fell into */
    .section .trap_data,"aw",%progbits
    .word 0x7A5EC7ED, 0x01020304

    .section .eeprom,"aw",%progbits
    .word 0xEE00EE00, 0xEE11EE11

    .section .noload_buffer,"aw",%nobits
    .space 64

    .section .bss.zero,"aw",%nobits
    .space 16

    /* metadata at address 0 that is never flashed (kvasir_ram_funcs' shape) */
    .section .kvasir_meta,"",%progbits
    .word 0xBADBADBA
