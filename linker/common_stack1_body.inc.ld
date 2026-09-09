/* The secondary core's stack. Sized by CORE1_STACK_SIZE (cmake_core1_stack_size), which is
   0 unless the project asks for a second core; then this is an empty section with two
   symbols and nothing else changes. It sits right after .stack, so a core 1 overflow runs
   into core 0's stack top - which is why SecondaryCore arms MSPLIM before anything else. */
. = ALIGN(8);
_LINKER_INTERN_stack1_start_ = .;
. = . + cmake_core1_stack_size;
. = ALIGN(8);
_LINKER_INTERN_stack1_end_ = .;
