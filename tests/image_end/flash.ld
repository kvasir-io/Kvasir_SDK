/* A chip with flash that loads nothing after .data (shaped like chip_rp2350/linker/chip.ld): the image end is the
   common.ld default, .data's load end */
MEMORY{
    flash  (xr ) : ORIGIN = 0x10000000, LENGTH = 64K
    ram    (xrw) : ORIGIN = 0x20000000, LENGTH = 16K
}
INCLUDE common_flash.ld
INCLUDE common_ram.ld
INCLUDE common.ld
