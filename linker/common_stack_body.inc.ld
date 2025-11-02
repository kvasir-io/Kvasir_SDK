. = ALIGN(8);
_LINKER_INTERN_stack_start_ = .;
. = ALIGN(8);
*(.stackProtector*)
_LINKER_INTERN_stackProtector_end_ = .;
. = . + ((cmake_min_stack_size + cmake_stack_size_extra) - ( cmake_stack_size_extra == 0 ? 0 : ((_LINKER_INTERN_stackProtector_end_ - _LINKER_INTERN_stack_start_))));
. = ALIGN(8);
_LINKER_INTERN_stack_end_ = .;
. = ALIGN(8);
