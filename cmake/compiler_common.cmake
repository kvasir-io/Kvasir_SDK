
set(
  compliler_common_flags
  -ffunction-sections
  -fdata-sections
  -fno-common
  -fomit-frame-pointer
  -fmerge-all-constants
  -fstack-protector-strong
  -ffast-math
  -Wno-unused-macros
)

set(
  compliler_common_cxx_flags
  -std=c++2b
  -fno-exceptions
  -fno-unwind-tables
  -fno-use-cxa-atexit
  -fno-rtti
  -fno-threadsafe-statics
  -fstrict-enums
)

set(
  compliler_common_c_flags
  -std=c2x
)

set(compliler_common_asm_flags)

set(
  linker_common_flags
  --static
  --Bstatic
  --gc-sections
  --entry=ResetISR
)
