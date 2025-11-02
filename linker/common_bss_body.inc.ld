. = ALIGN(4);
_LINKER_INTERN_bss_start_ = .;
. = ALIGN(4);
*(.bss*)
*(COMMON)
. = ALIGN(4);
_LINKER_INTERN_bss_end_ = .;
. = ALIGN(4);
