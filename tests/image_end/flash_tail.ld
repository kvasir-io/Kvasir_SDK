/* A chip that loads a section of its own into flash after .data (shaped like chip_rp2040/linker/chip.ld's scratch
   banks and .core_reset_guard): a second RAM region, .tail's load image after .data's, and the image end moved past
   it before common.ld. Without TAIL in the image .tail is empty. */
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
_LINKER_INTERN_image_end_flash_ = MAX(_LINKER_INTERN_data_end_flash_, LOADADDR(.tail) + SIZEOF(.tail));
INCLUDE common.ld
