"""Fresh measurements must override reference tunes only during calibration."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

MAIN = Path(__file__).resolve().parents[1] / 'main'


@unittest.skipUnless(shutil.which('cc'), 'Host C compiler unavailable')
class Recalibration(unittest.TestCase):
    def test_h2_packs_each_rf_gain_without_overwriting_phy_parameters(self):
        vendor = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "rx_recalibration.h"
_Alignas(4) unsigned char phy_param[2048];
uint32_t mock_iq_control=0x12345678;
static unsigned writes, measurements, forced=1;
static const uint16_t pairs[]={0x0102,0xfe03,0x04fc,0xfbfa};
void chip_v7_set_chan_ana(unsigned ch) {}
void phy_set_freq(unsigned mhz,int offset) {}
void force_rx_gain(unsigned force,unsigned gain) { forced=force; }
void set_rx_gain_cal_iq(void *out,unsigned debug) {
    assert(!forced);
    memcpy(out,pairs,sizeof(pairs));
    mock_iq_control|=1u<<29;
    measurements++;
}
extern void install_word(uint32_t,uint32_t,unsigned);
void write_gain_mem(uint32_t a,uint32_t b,unsigned index) {
    unsigned group=index%4;
    unsigned iq=((pairs[group]>>1)&0x1f80)|(pairs[group]&127);
    assert(a==0x12345 && (b&0x1fff)==iq);
    assert((b&~0x1fffu)==(0x01402000u|(group<<20)));
    writes++;
}
void set_rx_gain_table(void) {
    assert(!(*(uint32_t *)(phy_param+52)&0x300));
    assert(mock_iq_control==0x12345678);
    for(unsigned group=0;group<4;group++)
        install_word(0x12345,0x01402000u|(group<<20)|0x1fff,group);
}
int main(void) {
    memset(phy_param+60,0xa5,8);
    rx_recalibrate(2413);
    rx_recalibrate(2484);
    assert(measurements==2 && writes==8);
    for(unsigned i=60;i<68;i++) assert(phy_param[i]==0xa5);
}
'''
        reference = '''#include <stdint.h>
extern void write_gain_mem(uint32_t,uint32_t,unsigned);
void install_word(uint32_t a,uint32_t b,unsigned index) { write_gain_mem(a,b,index); }
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path/'sdkconfig.h').write_text('')
            (path/'soc').mkdir()
            (path/'soc/soc.h').write_text(
                '#include <stdint.h>\nextern uint32_t mock_iq_control;\n'
                '#define REG_READ(r) mock_iq_control\n'
                '#define REG_WRITE(r,v) (mock_iq_control=(v))\n')
            (path/'vendor.c').write_text(vendor)
            (path/'reference.c').write_text(reference)
            subprocess.run([
                'cc', '-std=c11', '-DCONFIG_IDF_TARGET_ESP32H2=1',
                '-I'+tmp, '-I'+str(MAIN/'common'), str(MAIN/'common/rx_recalibration.c'),
                str(path/'vendor.c'), str(path/'reference.c'),
                '-Wl,--wrap=chip_v7_set_chan_ana', '-Wl,--wrap=write_gain_mem',
                '-o', str(path/'check')
            ], check=True)
            subprocess.run([str(path/'check')], check=True)

    def test_measurement_frequency_and_cache_invalidation(self):
        vendor = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "rx_recalibration.h"
_Alignas(4) unsigned char phy_param[2048];
static unsigned wanted, tuned, dc, iq, tables;
static uint32_t *flags;
#if CONFIG_IDF_TARGET_ESP32C5
void phy_chip_set_chan_ana(unsigned mhz) { tuned=mhz; phy_param[42]=mhz>4000; }
/* Calls from a separate object exercise the same linker wrapping as libphy. */
extern void reference_tune(unsigned);
void phy_set_rx_gain_cal_dc(unsigned table,unsigned debug,void *rf,void *bb) {
    assert(!(*flags&0x680));
    reference_tune(2432); assert(tuned==wanted); dc++;
}
void phy_set_rx_gain_cal_iq(unsigned a,unsigned b,void *out,unsigned band,unsigned d,unsigned e) {
    reference_tune(5600); assert(tuned==wanted); iq++;
}
void phy_adc_rate_cal_rxdc(void) {}
void phy_set_rx_gain_table(unsigned mhz,unsigned debug) {
    assert((*flags&0x680)==0x480 && mhz==wanted); tables++;
}
#else
void phy_set_channel_rfpll_freq(unsigned mhz,unsigned xtal,unsigned mode) {
    assert(xtal==40 && mode==0); tuned=mhz;
}
extern void reference_tune(unsigned);
unsigned phy_i2c_readReg(unsigned b,unsigned h,unsigned r) { assert(r==12); return 0; }
void phy_i2c_writeReg(unsigned b,unsigned h,unsigned r,unsigned value) { assert(0); }
void esp_rom_delay_us(unsigned us) {}
void phy_rxiq_cal_init(unsigned a,unsigned b,unsigned c) {
    assert(!(*flags&0x680));
    reference_tune(2437); assert(tuned==wanted); iq++;
}
void phy_set_rx_gain_table(unsigned mhz,unsigned debug) {
    assert(!(*flags&0x280));
    reference_tune(2484); assert(tuned==wanted); dc++; tables++;
}
#endif
int main(void) {
    flags=(void *)(phy_param+
#if CONFIG_IDF_TARGET_ESP32C5
        148
#else
        164
#endif
    );
    const unsigned frequencies[]={2413,5340,2413,2150};
    for(unsigned i=0;i<4;i++) {
        wanted=frequencies[i]; *flags=0xffffffff;
        unsigned old_dc=dc;
        rx_recalibrate(wanted);
        assert(dc>old_dc && iq==i+1 && tables==i+1);
        /* Unrelated calibration bits must survive; only RX caches expire. */
        assert((*flags&~0x680u)==(0xffffffffu&~0x680u));
        reference_tune(2437); assert(tuned==2437);
    }
}
'''
        reference = r'''
#if CONFIG_IDF_TARGET_ESP32C5
extern void phy_chip_set_chan_ana(unsigned);
void reference_tune(unsigned mhz) { phy_chip_set_chan_ana(mhz); }
#else
extern void phy_set_channel_rfpll_freq(unsigned,unsigned,unsigned);
void reference_tune(unsigned mhz) { phy_set_channel_rfpll_freq(mhz,40,0); }
#endif
'''
        for chip in ('ESP32C5', 'ESP32C61', 'ESP32S31'):
            with self.subTest(chip=chip), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp)
                (path/'sdkconfig.h').write_text('')
                (path/'soc').mkdir()
                (path/'soc/soc.h').write_text('#define REG_CLR_BIT(r,b) ((void)(r), (void)(b))\n')
                (path/'esp_rom_sys.h').write_text('void esp_rom_delay_us(unsigned);\n')
                (path/'vendor.c').write_text(vendor)
                (path/'reference.c').write_text(reference)
                symbol = ('phy_chip_set_chan_ana' if chip == 'ESP32C5'
                          else 'phy_set_channel_rfpll_freq')
                subprocess.run([
                    'cc', '-std=c11', '-Wall', '-Werror=implicit-function-declaration',
                    '-DCONFIG_IDF_TARGET_'+chip+'=1', '-I'+tmp, '-I'+str(MAIN/'common'),
                    str(MAIN/'common/rx_recalibration.c'), str(path/'vendor.c'),
                    str(path/'reference.c'), '-Wl,--wrap='+symbol, '-o', str(path/'check')
                ], check=True)
                subprocess.run([str(path/'check')], check=True)
