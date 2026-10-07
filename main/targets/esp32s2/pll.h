#pragma once

/* The S2 channel calibration holds a capacitor code for the Wi-Fi channel.
 * Its ROM offset helper changes the SDM word without finding a new code.
 * Search the nine-bit VCO capacitor bank, using RFPLL's voltage-window
 * indication, and hold the middle of the widest usable interval. This is
 * bounded (~11 ms plus I2C overhead); a failed search restores all fields. */
static bool s2_pll_calibrate(void) {
    unsigned mode=rom_chip_i2c_readReg(0x62,1,11);
    unsigned control=rom_chip_i2c_readReg(0x62,1,0);
    unsigned low=rom_chip_i2c_readReg(0x62,1,1);
    unsigned high=rom_chip_i2c_readReg(0x62,1,2);
    rom_chip_i2c_writeReg(0x62,1,11,mode|0x40u);
    rom_chip_i2c_writeReg(0x62,1,0,control|0x80u);
    unsigned run=0,best=0,end=0;
    for(unsigned cap=0;cap<512;cap++) {
        rom_chip_i2c_writeReg(0x62,1,1,cap&255u);
        rom_chip_i2c_writeReg(0x62,1,2,(high&~0x10u)|((cap>>4)&0x10u));
        esp_rom_delay_us(20);
        if(!(rom_chip_i2c_readReg(0x62,1,12)&0x0cu)) {
            if(++run>best){best=run;end=cap;}
        } else run=0;
    }
    if(best) {
        unsigned cap=end-(best-1)/2;
        rom_chip_i2c_writeReg(0x62,1,1,cap&255u);
        rom_chip_i2c_writeReg(0x62,1,2,(high&~0x10u)|((cap>>4)&0x10u));
        esp_rom_delay_us(20);
        return true;
    }
    rom_chip_i2c_writeReg(0x62,1,1,low);
    rom_chip_i2c_writeReg(0x62,1,2,high);
    rom_chip_i2c_writeReg(0x62,1,0,control);
    rom_chip_i2c_writeReg(0x62,1,11,mode);
    return false;
}
