FILL(0xFFDEFFDE); /* ARM "udf #255" instruction - triggers HardFault if PC goes to uninitialized memory */
. = ALIGN(4);
_LINKER_vectors_start_ = .;   /* the boot core's vector table, for VTOR */
KEEP(*(.core_vectors*))
/* bounded for IMAGE_CRC: on the RP2350 this is the picobin block loop, which picotool seal/sign rewrite after the
   link (chip_rp2350 TARGET_IMAGE_CRC_EXCLUDE) */
_LINKER_INTERN_after_vectors_start_ = .;
KEEP(*(.after_vectors*))
_LINKER_INTERN_after_vectors_end_ = .;
. = ALIGN(4);
