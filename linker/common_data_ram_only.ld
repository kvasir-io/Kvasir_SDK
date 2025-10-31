/* Data section for RAM-only execution (no flash copy needed) */

SECTIONS {
    /* Initialized data - no AT() needed, already in RAM! */
    .data : {
        INCLUDE common_data_body.inc.ld
    } > ram

    /* Flash symbols set to zero for RAM-only (kept for compatibility) */
    _LINKER_INTERN_data_start_flash_ = 0;
    _LINKER_INTERN_data_end_flash_ = 0;
}
