FILL(0xFFDEFFDE); /* ARM "udf #255" instruction - triggers HardFault if PC goes to uninitialized memory */
. = ALIGN(4);
_LINKER_INTERN_data_start_ = .;
. = ALIGN(4);
*(SORT_BY_ALIGNMENT(.data*))
/* RAM functions (KVASIR_RAM_FUNC_ATTRIBUTES), copied from flash with .data; a RAM-only image puts them in .text */
*(SORT_BY_ALIGNMENT(.ramfunc*))
. = ALIGN(4);
_LINKER_INTERN_data_end_ = .;
. = ALIGN(4);
