FILL(0xFFDEFFDE); /* ARM "udf #255" instruction - triggers HardFault if PC goes to uninitialized memory */
. = ALIGN(4);
_LINKER_INTERN_data_start_ = .;
. = ALIGN(4);
*(SORT_BY_ALIGNMENT(.data*))
. = ALIGN(4);
_LINKER_INTERN_data_end_ = .;
. = ALIGN(4);
