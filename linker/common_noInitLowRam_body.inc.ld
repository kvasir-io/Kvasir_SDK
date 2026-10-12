. = ALIGN(4);
_LINKER_INTERN_noInitLowRam_start_ = .;
/* uc_log's boot words first: at the start of RAM they have the same address in every image of a chip, so the printer
   can compare their count across a reflash (uc_log detail/RttEpoch.hpp). The boot ROMs leave the start of RAM alone:
   the RP2040's runs its second stage in SRAM5 (SRAM5_BASE 0x20041000, RP2040 datasheet 2.8.1), the RP2350's keeps its
   state in its own boot RAM (RP2350 datasheet 4.3). The RP2040 datasheet does not state the boot ROM's stack. */
KEEP(*(.noInitLowRam.uc_log_boot_words))
. = ALIGN(4);
*(.noInitLowRam*)
. = ALIGN(4);
_LINKER_INTERN_noInitLowRam_end_ = .;
. = ALIGN(4);
