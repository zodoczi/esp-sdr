#pragma once
extern void phy_set_chanfreq(unsigned mhz, unsigned mode);
/* Fresh per-frequency RX measurements are installed before channel setup.
 * The C5 channel API accepts MHz directly; do not choose a nearby channel. */
static void c5_set_chan(unsigned mhz, unsigned mode) {
    phy_set_chanfreq(mhz, mode);
}
#define phy_chip_set_chan c5_set_chan
