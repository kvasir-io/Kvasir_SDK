/* the smallest flash image with the shape the IMAGE_CRC patcher meets: a section of its own in front with a gap
   after it (RP .boot2), .text with .rodata and the descriptor (linker/common_text_body.inc.ld), .data loaded after
   .text */
MEMORY
{
    flash (rx)  : ORIGIN = 0x10000000, LENGTH = 1M
    ram   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}
ENTRY(entry)
SECTIONS
{
    .boot2 : { KEEP(*(.boot2)) } > flash
    .text ORIGIN(flash) + 0x100 : {
        KEEP(*(.text.entry)) *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        KEEP(*(.kvasir_image_crc))
    } > flash
    .data : { *(.data*) } > ram AT > flash
    .ramimage : { KEEP(*(.ramimage)) } > ram
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } > ram
    /DISCARD/ : { *(.ARM.exidx*) }
}
