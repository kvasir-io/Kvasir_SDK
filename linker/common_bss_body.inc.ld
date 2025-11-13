. = ALIGN(4);
_LINKER_INTERN_bss_start_ = .;
. = ALIGN(4);
*(SORT_BY_ALIGNMENT(.bss*))
*(SORT_BY_ALIGNMENT(COMMON))
. = ALIGN(4);
_LINKER_INTERN_bss_end_ = .;
. = ALIGN(4);
