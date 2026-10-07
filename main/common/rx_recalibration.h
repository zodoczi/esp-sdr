#pragma once

/* Receiver and capture must be stopped. Measures fresh RX DC and loopback IQ
 * coefficients at mhz, then rebuilds the gain tables. Caller restores its
 * channel, filter, RX power and gain settings afterwards. */
void rx_recalibrate(unsigned mhz);
