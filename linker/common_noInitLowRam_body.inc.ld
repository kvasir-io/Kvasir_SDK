. = ALIGN(4);
_LINKER_INTERN_noInitLowRam_start_ = .;
. = ALIGN(4);
*(.noInitLowRam*)
. = ALIGN(4);
_LINKER_INTERN_noInitLowRam_end_ = .;
. = ALIGN(4);
