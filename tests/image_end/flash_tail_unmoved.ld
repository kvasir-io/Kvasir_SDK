/* flash_tail.ld without the image end line: a chip script that forgot it. The symbol stays at .data's end, and
   cmake/tools/elf_image.py must refuse the image that has something in .tail. */
MEMORY{
    flash  (xr ) : ORIGIN = 0x10000000, LENGTH = 64K
    ram    (xrw) : ORIGIN = 0x20000000, LENGTH = 16K
    bank   (xrw) : ORIGIN = 0x20004000, LENGTH = 4K
}
INCLUDE common_flash.ld
INCLUDE common_ram.ld
SECTIONS {
    .tail : AT(_LINKER_INTERN_data_end_flash_) {
        . = ALIGN(4);
        *(.tail.data*)
        . = ALIGN(4);
    } > bank
}
INCLUDE common.ld
