ASSERT(_iram_end <= 0x40820000, "H2 RF dump overlaps IRAM")
ASSERT(_data_end <= 0x40820000, "H2 RF dump overlaps data")
ASSERT(_bss_end <= 0x40820000, "H2 RF dump overlaps BSS")
