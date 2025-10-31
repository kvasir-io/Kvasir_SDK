/* Shared .data section body - included by common_data_flash.ld and common_data_ram_only.ld */
. = ALIGN(4);
_LINKER_INTERN_data_start_ = .;
. = ALIGN(4);
*(.data*)
. = ALIGN(4);
_LINKER_INTERN_data_end_ = .;
. = ALIGN(4);
