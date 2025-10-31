/* .text section for flash-based execution */

SECTIONS {
    . = ORIGIN(flash);
    .text : {
        INCLUDE common_text_body.inc.ld
    } > flash
}
