/* .text section for RAM-only execution */

SECTIONS {
    . = ORIGIN(ram);
    .text : {
        INCLUDE common_text_body.inc.ld
    } > ram
}
