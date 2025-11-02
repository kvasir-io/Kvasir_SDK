FILL(0xFFDEFFDE); /* ARM "udf #255" instruction - triggers HardFault if PC goes to uninitialized memory */
. = ALIGN(4);
KEEP(*(.core_vectors*))
KEEP(*(.after_vectors*))
. = ALIGN(4);
