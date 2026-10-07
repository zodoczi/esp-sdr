ASSERT(_iram_end <= 0x403a0000, "C2 RF dump overlaps IRAM")
ASSERT(_data_end <= 0x3fcc0000, "C2 RF dump overlaps data")
ASSERT(_bss_end <= 0x3fcc0000, "C2 RF dump overlaps BSS")
SECTIONS {
    .c2_spectrum 0x3fcc8000 (NOLOAD) : ALIGN(16) {
        *(.c2_spectrum)
    }
}
ASSERT(SIZEOF(.c2_spectrum) <= 0x8000, "C2 spectrum workspace exceeds reserved bank slot")
