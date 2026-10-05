/* A chip with flash, shaped like chip_stm32f413/linker/chip.ld */
MEMORY{
    flash  (xr ) : ORIGIN = 0x08000000, LENGTH = 64K
    ram    (xrw) : ORIGIN = 0x20000000, LENGTH = 16K
}
INCLUDE common_flash.ld
INCLUDE common_ram.ld
INCLUDE common.ld
