. = ALIGN(8);
_LINKER_INTERN_stack_start_ = .;
. = ALIGN(8);
*(.stackProtector*)
_LINKER_INTERN_stackProtector_end_ = .;
/* The stack takes the RAM the other sections leave: _LINKER_INTERN_stack_extra_ (common.ld) on top of the minimum. */
. = . + ((cmake_min_stack_size + _LINKER_INTERN_stack_extra_) - ( _LINKER_INTERN_stack_extra_ == 0 ? 0 : ((_LINKER_INTERN_stackProtector_end_ - _LINKER_INTERN_stack_start_))));
. = ALIGN(8);
_LINKER_INTERN_stack_end_ = .;
. = ALIGN(8);
