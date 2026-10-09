FILL(0xFFDEFFDE); /* ARM "udf #255" instruction - triggers HardFault if PC goes to uninitialized memory */
. = ALIGN(4);
_LINKER_INTERN_data_start_ = .;
. = ALIGN(4);
*(SORT_BY_ALIGNMENT(.data*))
/* RAM functions (KVASIR_RAM_FUNC_ATTRIBUTES), copied from flash with .data; a RAM-only image puts them in .text */
*(SORT_BY_ALIGNMENT(.ramfunc*))
/* Source-coverage counters (the `coverage` variant, -fprofile-instr-generate): initial values from flash like any
   .data. Empty in every other image: no bytes, no padding (no ALIGN here, the input sections carry theirs). */
__start___llvm_prf_cnts = .;
KEEP(*(__llvm_prf_cnts))
__stop___llvm_prf_cnts = .;
__start___llvm_prf_bits = .;
KEEP(*(__llvm_prf_bits))
__stop___llvm_prf_bits = .;
. = ALIGN(4);
_LINKER_INTERN_data_end_ = .;
. = ALIGN(4);
