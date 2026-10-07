# ESP32-H2 backend

The `esp32h2` profile targets H2 boards with a 32 MHz crystal and 2 MB or
larger flash. Both native USB Serial/JTAG and optional UART0 (TX GPIO24,
RX GPIO23, default 2 Mbaud) use the shared burst protocol and serial lease.
The firmware and browser installer use the catalog's pinned ESP-IDF revision.

## RF capture

H2 has no Wi-Fi peripheral. Its pinned `librftest.a` implements `adctrig` as
a no-op; the backend instead follows its Bluetooth `bt_adctrig` sequence:

| Function | Address / setting |
| --- | --- |
| RF dump buffer | `0x40820000`, 64 KiB SRAM bank 2 |
| SRAM ownership | `0x60095004`, bits 14:10 = 4 |
| Dump control | `0x600a4c04`; enable 31, trigger 19, done 18 |
| Dump mode | `0x600a4c08`, bits 18:15 set |
| Four-lane selection | `0x600a4c14`: 14, 15, 16, 17 |
| Trigger source | `0x600a20b4`, bit 0 cleared |
| Forced gain | `0x600a2840` |

The heap excludes the entire capture bank, with linker assertions protecting
IRAM/data/BSS. Capture restores SRAM ownership and modified debug registers,
uses a 20 ms timeout, and verifies that every requested word replaced its
sentinel. PHY initialization uses `esp_phy_enable(PHY_MODEM_BT)` followed by
`rftest_open_clk`; it does not initialize Wi-Fi.

`INFO` reports `H2SDR 6 burst 16380`. The dump control's bits 16:15
select hardware subsampling, independently of the analog filter:

| Nominal sample rate | Protocol code | Dump selector | ADC clocks/sample | 16,380-sample capture |
| --- | --- | --- | --- | --- |
| 32 MS/s | 7 | 0 | 1 | ~517 µs |
| 16 MS/s | 6 | 1 | 2 | ~1028 µs |
| 10.667 MS/s | 8 | 2 | 3 | ~1540 µs |
| 6.4 MS/s | 9 | 3 | 5 | ~2563 µs |

Code 8 is 32 MHz / 3; integer protocol fields round it to **10666667 Hz**.
The other rates use 32000000, 16000000 and 6400000 Hz. Existing rate codes
retain their meanings. Capture timing slopes and matching noise-spectrum
features verify the ratios; absolute clock accuracy has not been calibrated.
These modes skip ADC samples without automatic anti-alias filtering. A narrow
feature near −4 MHz folds to approximately +2.4 MHz at 6.4 MS/s in the measured
spectrum. Choose analog bandwidth to suit the desired span; its finite
roll-off does not guarantee alias-free reception.

`CAP16` and `CAP20` use the shared signed IQ8 and packed IQ10 layouts;
`CAP` returns raw 32-bit words. Odd sample counts are supported.

H2's `force_rx_gain` takes two arguments. Its calibrated maximum index is
stored in `phy_param[83]` (66 on the tested board). Releasing forced gain
also leaves `bt_rx_force(1)` active. Gain indices are not dB, and some settings
can saturate the ADC.

## Analog filter

The pinned PHY's `rc_cal` computes four seven-bit RC calibration values;
`i2c_bbtop_init` writes them to BBTOP (`0x67`, host 1) registers 0–3.
Individual sweeps found that **register 0, bits 6:0**, controls the active
capture-path bandwidth. Registers 1–3 had no measurable effect on this path
and are left unchanged. On the tested board all four calibrated values are 45.

`BANDWIDTH <MHz>` accepts **4–11 MHz** in 1 MHz steps, using an approximate
noise-derived full −3 dB width curve. Zero selects the widest setting (RC 0),
not a filter bypass. The interpolation anchors are:

| RC code | 0 | 8 | 16 | 24 | 32 | 48 | 80 | 112 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Approximate full width (MHz) | 11 | 10 | 9 | 8 | 7 | 6 | 5 | 4 |

`LPF <0..127>` selects a raw RC code; `LPF AUTO` restores use of PHY calibration.
`LPF?` reports the selected code (−1 for automatic) and the current calibrated
register value. Each snapshot saves register 0, preserves its high bit, applies
the requested setting, and restores the original value on success or timeout.
Retuning therefore sees the calibrated value. `CAPS` advertises `LPFANA`,
and `LIMITS?` reports `bandwidth: [4,11,1,0]` for the existing viewer control.

Measurements used 24 captures per code at 2300/2412/2484 MHz, gain 35,
32 MS/s, 2048-point Hann-windowed FFTs and median averaged noise spectra.
At 2300 and 2484 MHz the sweeps had no clipping; some 2412 MHz captures
contained strong signals. The approximate range spans ~10.6–10.9 MHz at
RC 0 to ~3.7–3.9 MHz at RC 127. It is not a precision filter calibration or
a guaranteed bandwidth across boards, temperatures and tuning frequencies.

Exact-MHz tuning follows the PHY's channel calibration helper and
`phy_set_freq` for off-channel values. The 100–6000 MHz software attempt
range does not guarantee PLL lock or reception outside the chip's 2.4 GHz
radio band. No sensitivity, absolute gain, or out-of-band response is claimed.

The portable FFT backend supplies 256/512/1024/2048-bin snapshot spectra at
all four rates over both transports, with the RF-gap flag set. Continuous capture
is not advertised. The viewer offers the negotiated rates and analog bandwidth control.

## Board and transport validation

Tested on an ESP32-H2 revision 0.1 with a 32 MHz crystal and 4 MB flash.
Native USB and FT232 UART at 2 Mbaud passed raw captures and every advertised
FFT profile across all four rates. Checks covered IQ8/IQ10/raw words, odd
and maximum sample counts, bandwidth settings, filter restoration, CRCs,
spectrum telemetry, serial leases and UART baud switching. Host regression
tests cover the gain API, rate and bandwidth limits, tuning and filter
preservation. These checks do not establish calibrated RF performance.

A TX/RX-only UART adapter cannot reset the board: hold BOOT, tap RESET,
then release BOOT before flashing, and reset afterward. When both interfaces
are connected, native USB reset controls can enter download mode while data
is flashed over UART. Never assume an adapter's DTR/RTS are wired.
