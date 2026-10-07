#include <assert.h>
#include <stdbool.h>
#include <string.h>

static unsigned regs[16], original[16], first, last, probes, writes;
static int forced = -1;
static bool unstable;

unsigned phy_i2c_readReg(unsigned b, unsigned h, unsigned r) {
    assert(b == 0x62 && h == 1 && r < 16);
    if (r != 12) return regs[r];
    if (forced >= 0) return (unsigned)forced << 2;
    unsigned cap = regs[1] | ((regs[2] & 64u) << 2);
    unsigned status = cap < first ? 1 : cap > last ? 2 : 0;
    if (unstable && probes > 22) status = 3;
    return status << 2;
}

void phy_i2c_writeReg(unsigned b, unsigned h, unsigned r, unsigned v) {
    assert(b == 0x62 && h == 1 && v <= 255);
    assert(r == 1 || r == 2 || r == 11);
    if (r == 2) assert((v & ~64u) == (original[2] & ~64u));
    if (r == 11) assert((v & ~64u) == (original[11] & ~64u));
    regs[r] = v;
    writes++;
}

void esp_rom_delay_us(unsigned us) { assert(us == 50); probes++; }
#include "rx_pll_cal.h"

static void reset(unsigned lo, unsigned hi, unsigned initial) {
    memset(regs, 0, sizeof(regs));
    regs[1] = initial & 255;
    regs[2] = 0xbb | ((initial >> 2) & 64);
    regs[11] = 0x24;
    memcpy(original, regs, sizeof(regs));
    first = lo; last = hi; probes = writes = 0; forced = -1; unstable = false;
}

int main(void) {
    /* Exercise both nine-bit boundaries and windows narrower than the
     * hardware examples. Start outside each window to require recovery. */
    for (unsigned lo = 0; lo < 512; lo++) {
        for (unsigned width = 1; width <= 12 && lo + width <= 512; width++) {
            reset(lo, lo + width - 1, lo ? 0 : 511);
            assert(rx_pll_recover(2200));
            unsigned cap = regs[1] | ((regs[2] & 64u) << 2);
            assert(cap == lo + (width - 1) / 2);
            assert(regs[11] == (original[11] | 64));
            assert(probes <= 25);
        }
    }
    reset(200, 208, 204);
    assert(rx_pll_recover(2200) && writes == 0); /* Already calibrated. */
    reset(200, 208, 0);
    assert(rx_pll_recover(2412) && writes == 0); /* Nominal band untouched. */
    for (int status = 1; status <= 3; status++) {
        reset(200, 208, 0); forced = status;
        assert(!rx_pll_recover(2200));
        assert(memcmp(regs, original, sizeof(regs)) == 0 && probes <= 25);
    }
    /* A one-code window that changes during the final stability check must
     * restore the complete original capacitor/manual-mode state. */
    reset(0, 0, 511); unstable = true;
    assert(!rx_pll_recover(2200));
    assert(memcmp(regs, original, sizeof(regs)) == 0);
    return 0;
}
