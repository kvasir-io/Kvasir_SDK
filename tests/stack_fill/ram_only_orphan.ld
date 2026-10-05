/* A RAM-only image with a section of the chip's own in ram, as chip_rp2040/linker/chip_ram_only.ld's .boot2: the chip
   names it (an orphan cannot be SIZEOF'd by GNU ld) and tells the stack its size before the INCLUDE */
MEMORY{
    ram    (xrw) : ORIGIN = 0x20000000, LENGTH = 32K
}
SECTIONS {
    .boot2 : {
        *(.boot2)
    } > ram
} INSERT AFTER .vectors;
_LINKER_INTERN_chip_ram_other_size_ = SIZEOF(.boot2);
INCLUDE common_ram_only.ld
INCLUDE common.ld
