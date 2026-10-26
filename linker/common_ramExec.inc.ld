/* Code written at run time and then executed (chip rp_common/XipControl.hpp: the bootrom's XIP setup function,
   copied out of BOOTRAM): the image's one writable and executable range, an MPU region of its own (chip Mpu.hpp).
   One output section, included by common_ram.ld and common_ram_only.ld. */
.ramExec (NOLOAD) : AT(ADDR(.ramExec)) ALIGN(32) {
    _LINKER_INTERN_ramExec_start_ = .;
    *(.ramExec*)
    . = ALIGN(32);
    _LINKER_INTERN_ramExec_end_ = .;
} > ram
