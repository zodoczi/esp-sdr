"""Exercise production radio setup across manual/AGC/retune transitions."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('cc'), 'Host C compiler unavailable')
class StreamingGain(unittest.TestCase):
    def test_restore_agc_defaults_after_manual_gain(self):
        source = (Path(__file__).resolve().parents[1] /
                  'main/targets/esp32s31/streaming/receiver.c').read_text()
        setup = source[source.index('static void prepare_rx(void) {'):
                       source.index('static void IRAM_ATTR diagnostic_start')]
        stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct { unsigned frequency_hz, rate, gain, bandwidth, dc_correction, agc; } receiver_config_t;
static unsigned init = 0x12345678, threshold = 0xabcdef01;
static const unsigned original_init = 0x12345678, original_threshold = 0xabcdef01;
static unsigned forced, selected, saturation, receiver_gain_max;
static int mirror = -1;
static uint32_t params[128];
#define phy_param ((unsigned char *)params)
#define REG_READ(r) ((r)==0x20107094 ? init : (r)==0x2010713c ? threshold : 71u<<8)
#define REG_WRITE(r,v) do { if((r)==0x20107094) init=(v); else { assert((r)==0x2010713c); threshold=(v); } } while(0)
static void phy_pbus_workmode(void) { forced=0; }
static void phy_pbus_xpd_tx_off(void) {}
static void phy_pbus_xpd_rx_on(unsigned on) {}
static void phy_set_rxclk_en(unsigned on) {}
static void phy_set_txclk_en(unsigned on) {}
static void burst_gain_mirror(int gain) { mirror=gain; }
static void s31_tune(unsigned mhz) {
    assert(mirror==-1); /* Never calibrate through a manually mirrored slot. */
    assert(init==original_init && threshold==original_threshold);
}
static void phy_loopback_mode_en(unsigned on) {}
static void phy_bb_bss_cbw40_dig(unsigned on) {}
static void phy_bb_cbw_chan_cfg(unsigned on) {}
static void phy_wifi_fbw_sel(unsigned on) {}
static void phy_pbus_debugmode(void) {}
static void phy_set_rx_gain_table(unsigned mhz, unsigned debug) { assert(mirror==-1); }
static void phy_rfrx_sat_rst(unsigned on) { saturation=on; }
static void phy_force_rx_gain(unsigned force, unsigned gain) { forced=force; selected=gain; }
static unsigned receiver_bandwidth(const receiver_config_t *c) { return 20000000; }
static unsigned rx_bandwidth_dcap(unsigned mhz) { return 20; }
static unsigned phy_i2c_readReg(unsigned b, unsigned h, unsigned r) { return 0; }
static void phy_i2c_writeReg(unsigned b, unsigned h, unsigned r, unsigned v) {}
'''
        check = r'''
int main(void) {
    receiver_config_t c = {2412000000u, 16000000, 40, 0, 1, 0};
    for(unsigned pass=0; pass<4; pass++) {
        c.frequency_hz += 1000000;
        c.gain = 10 + pass*20;
        c.agc = 0;
        radio_configure(&c);
        assert(forced && selected==c.gain && !saturation && mirror==(int)c.gain);
        assert(((init>>2)&127)==c.gain && ((threshold>>18)&127)==c.gain);
        c.agc = 1;
        radio_configure(&c);
        assert(!forced && saturation && mirror==-1);
        assert(init==original_init && threshold==original_threshold);
    }
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'gain.c'
            path.write_text(stub+setup+check)
            binary = Path(tmp)/'gain'
            subprocess.run(['cc', '-std=c11', str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
