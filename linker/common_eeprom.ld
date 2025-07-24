SECTIONS {
    .eeprom : {
        FILL(0xFFFFFFFF);
        KEEP(*(.eeprom*))
    } > eeprom
}
