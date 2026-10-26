/* A RAM-only image, shaped like chip_rp2350/linker/chip_ram_only.ld */
MEMORY{
    ram    (xrw) : ORIGIN = 0x20000000, LENGTH = 16K
}
INCLUDE common_ram_only.ld
INCLUDE common.ld
