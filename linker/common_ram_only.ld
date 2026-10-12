/* RAM-only execution: all sections in RAM, no flash copy needed */

SECTIONS {
    . = ORIGIN(ram);

    .vectors : {
        INCLUDE common_vectors_body.inc.ld
    } > ram

    /* MPU region boundaries (chip Mpu.hpp): a region starts and ends on 32 bytes, so the read-only vectors, the
       writable low RAM, the read-only code and the writable rest each begin on a boundary of their own. The
       sections' own ALIGN: lld places a section of a MEMORY region at the region's cursor, not at `.`. */
    .noInitLowRam (NOLOAD) : AT(ADDR(.noInitLowRam)) ALIGN(32) {
        INCLUDE common_noInitLowRam_body.inc.ld
    } > ram

    .text : ALIGN(32) {
        _LINKER_INTERN_text_start_ = .;
        INCLUDE common_text_body.inc.ld
        /* RAM functions (KVASIR_RAM_FUNC_ATTRIBUTES): here they are in RAM already, so they go with the code rather
           than with .data (common_data_body.inc.ld, which the first match leaves without them) - read-only, and
           covered by an IMAGE_CRC check of the running image. */
        . = ALIGN(4);
        *(SORT_BY_ALIGNMENT(.ramfunc*))
        /* the code's MPU region ends here: .data must not share its last 32 bytes */
        . = ALIGN(32);
        _LINKER_INTERN_text_end_ = .;
    } > ram

    /* Code written at run time and then executed (chip bootrom_functions.hpp: the bootrom's XIP setup function, copied
       out of BOOTRAM): the image's one writable and executable range, an MPU region of its own (chip Mpu.hpp) */
    .ramExec (NOLOAD) : AT(ADDR(.ramExec)) ALIGN(32) {
        _LINKER_INTERN_ramExec_start_ = .;
        *(.ramExec*)
        . = ALIGN(32);
        _LINKER_INTERN_ramExec_end_ = .;
    } > ram

    .data : {
        INCLUDE common_data_body.inc.ld
    } > ram

    .bss (NOLOAD) : AT(ADDR(.bss)) {
        INCLUDE common_bss_body.inc.ld
    } > ram

    .noInit (NOLOAD) : AT(ADDR(.noInit)) {
        INCLUDE common_noInit_body.inc.ld
    } > ram

    .stack (NOLOAD) : AT(ADDR(.stack)) {
        INCLUDE common_stack_body.inc.ld
    } > ram

    .stack1 (NOLOAD) : AT(ADDR(.stack1)) {
        INCLUDE common_stack1_body.inc.ld
    } > ram

    .heap (NOLOAD) : AT(ADDR(.heap)) {
        INCLUDE common_heap_body.inc.ld
    } > ram
}

/* The MPU's regions (chip Mpu.hpp): the vectors with the picobin block end where the low RAM begins; the code is
   [_LINKER_text_start_, _LINKER_text_end_), .ramExec right behind it. All on 32 bytes. */
_LINKER_vectors_end_ = ADDR(.noInitLowRam);
_LINKER_text_start_ = _LINKER_INTERN_text_start_;
_LINKER_text_end_ = _LINKER_INTERN_text_end_;

/* Flash symbols set to zero for RAM-only (kept for compatibility) */
_LINKER_INTERN_data_start_flash_ = 0;
_LINKER_INTERN_data_end_flash_ = 0;

/* No flash memory in RAM-only configuration */
_LINKER_INTERN_rom_start_   = 0;
_LINKER_INTERN_rom_size_    = 0;
_LINKER_INTERN_rom_end_     = 0;

/* The stack fills what RAM is left (common_stack_body.inc.ld): the sizes of every other section in region ram.
   A chip whose script puts a section of its own into ram (chip_rp2040's RAM-only .boot2) sets
   _LINKER_INTERN_chip_ram_other_size_ BEFORE it includes this file. */
PROVIDE(_LINKER_INTERN_chip_ram_other_size_ = 0);
_LINKER_INTERN_ram_other_size_ = SIZEOF(.vectors) + SIZEOF(.noInitLowRam) + SIZEOF(.text) + SIZEOF(.ramExec) + SIZEOF(.data) + SIZEOF(.bss) + SIZEOF(.noInit) + SIZEOF(.stack1)
                               + _LINKER_INTERN_chip_ram_other_size_;

ASSERT( (_LINKER_vectors_end_ % 32) == 0, "ERROR: MPU region boundary: the vectors' end is not 32-byte aligned");
ASSERT( (_LINKER_text_start_ % 32) == 0, "ERROR: MPU region boundary: the code's start is not 32-byte aligned");
ASSERT( (_LINKER_text_end_ % 32) == 0, "ERROR: MPU region boundary: the code's end is not 32-byte aligned");
