FILL(0xFFDEFFDE); /* ARM "udf #255" instruction - triggers HardFault if PC goes to uninitialized memory */
. = ALIGN(4);
*(SORT_BY_ALIGNMENT(.text*))
*(SORT_BY_ALIGNMENT(.rodata*))
. = ALIGN(4);
_LINKER_INTERN_init_array_start_ = .;
. = ALIGN(4);
KEEP(*(SORT_BY_INIT_PRIORITY(.init_array*)))
. = ALIGN(4);
_LINKER_INTERN_init_array_end_ = .;
. = ALIGN(4);
