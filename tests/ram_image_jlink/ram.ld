/* a RAM-only image: the vector table first in RAM, its start named as common_vectors_body.inc.ld names it */
MEMORY
{
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}
ENTRY(entry)
SECTIONS
{
    .vectors : { _LINKER_vectors_start_ = .; KEEP(*(.vectors)) } > ram
    .text : { *(.text*) } > ram
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) }
}
