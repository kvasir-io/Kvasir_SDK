FILL(0xFFDEFFDE); /* ARM "udf #255" instruction - triggers HardFault if PC goes to uninitialized memory */
. = ALIGN(4);
/* The secondary core's vector table, if there is one. Deliberately no ALIGN() here: the
   table's own 512-byte alignment (VTOR needs the next power of two above its size) comes
   from the input section, so an image without a second core pays no padding at all. */
_LINKER_INTERN_core1_vectors_start_ = .;
KEEP(*(.core1_vectors*))
_LINKER_INTERN_core1_vectors_end_ = .;
*(SORT_BY_ALIGNMENT(.text*))
*(SORT_BY_ALIGNMENT(.rodata*))
. = ALIGN(4);
_LINKER_INTERN_init_array_start_ = .;
. = ALIGN(4);
KEEP(*(SORT_BY_INIT_PRIORITY(.init_array*)))
. = ALIGN(4);
_LINKER_INTERN_init_array_end_ = .;
. = ALIGN(4);
