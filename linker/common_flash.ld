/* Flash-based execution: .text section in flash memory */

SECTIONS {
    . = ORIGIN(flash);

    .vectors : {
        INCLUDE common_vectors_body.inc.ld
    } > flash

    .text : {
        INCLUDE common_text_body.inc.ld
    } > flash

    /* Source-coverage records (the `coverage` variant): per function a descriptor, and the compressed names, read only
       by Kvasir::Coverage and the host. Writable input sections, hence output sections of their own, under the names
       llvm-cov looks for. Empty in every other image. */
    __llvm_prf_data : {
        __start___llvm_prf_data = .;
        KEEP(*(__llvm_prf_data))
        __stop___llvm_prf_data = .;
        /* value profiling is not built: its sections are empty ranges (the runtime refers to them weakly) */
        __start___llvm_prf_vns = .;
        __stop___llvm_prf_vns = .;
        __start___llvm_prf_vtab = .;
        __stop___llvm_prf_vtab = .;
    } > flash
    __llvm_prf_names : {
        __start___llvm_prf_names = .;
        KEEP(*(__llvm_prf_names))
        __stop___llvm_prf_names = .;
    } > flash

    _LINKER_INTERN_data_start_flash_ = .;
}

/* Flash/ROM memory symbols */
_LINKER_INTERN_rom_start_   = ORIGIN(flash);
_LINKER_INTERN_rom_size_    = LENGTH(flash);
_LINKER_INTERN_rom_end_     = _LINKER_INTERN_rom_start_ + _LINKER_INTERN_rom_size_;
