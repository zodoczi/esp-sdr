# ESP32-C2 backend

The `esp32c2` profile targets ESP8684 boards with a 26 MHz crystal, 2 MB or
larger flash, and UART0 (TX GPIO20, RX GPIO19). It uses the catalog's pinned
ESP-IDF revision. A board with a 40 MHz crystal needs a matching rebuild;
`tools/export_web_firmware.py` records `xtal_mhz` so the browser installer can
reject a crystal mismatch before writing.

## RF capture

The capture sequence comes from `adctrig` in the pinned C2 `librftest.a`,
not from the C3 register map:

| Function | Address / setting |
| --- | --- |
| RF dump buffer | `0x3fcc0000`, 32 KiB |
| SRAM ownership | `0x600c1018`, usage bits 2:0 = 2 |
| Dump control | `0x6004bc04`; enable bit 31, trigger bit 19, done bit 18 |
| Dump mode | `0x6004bc08`, bits 18:15 set |
| Four-lane order | `0x6004bc14`: 0, 1, 2, 3 in six-bit fields |
| Trigger source | `0x600460b4`, bit 0 cleared |
| Clock enable | `0x60026014`, bit 29 set |
| AGC / forced gain | `0x6004a02c` |

The heap guard excludes the complete 128 KiB SRAM bank beginning at
`0x3fcc0000`, while allowing IDF to handle revision-specific ROM reservations.
The FFT workspace occupies `0x3fcc8000..0x3fcce820` and is quiescent during
capture; stacks, heap allocations and Wi-Fi packet buffers stay below the bank.
The original 32 KiB-only reservation allowed capture-related corruption of PHY
function pointers and UART heap objects. Small Wi-Fi packet-buffer counts
leave enough heap for the receiver without reducing the maximum FFT size. The
linker guard prevents IRAM, data and BSS from overlapping it. A critical
section surrounds the bounded hardware handoff; ownership, trigger, mode,
lane selection and clock settings are restored after success or timeout.
Every requested word starts with a sentinel and must be overwritten before
any data is returned.

`INFO` reports `C2SDR 6 burst 8190`. Rate indices 0/1/6 select nominal 80/40/16 MS/s.
Dump-clock selectors 0/1/2 produce these rates; selector 3 also measures
16 MS/s and is not advertised separately. Other protocol rates are rejected. `CAP16` packs signed eight-bit I/Q and `CAP20`
packs signed ten-bit I/Q, including odd sample counts. Hardware AGC and
manual gain use the calibrated PHY table (indices 0–79 on the tested board).
Exact-MHz tuning attempts use channel calibration followed by `phy_set_freq`
for off-channel frequencies, as on the other backends. The 100–6000 MHz
software range does not guarantee PLL lock or reception.

The portable spectrum backend advertises only UART snapshot profiles,
256/512/1024/2048 bins at 80/40/16 MS/s, with the RF-gap flag set. It supports the
shared detector, DC and statistics commands. No continuous mode is claimed.

## Analog bandwidth

`BANDWIDTH 12..20` selects an approximate two-sided bandwidth in MHz.
The conservative noise-derived curve uses capacitor codes 32/40/48/56/63
for 20/17/14/13/12 MHz. `BANDWIDTH 0` selects code 0; `LPF AUTO` restores
PHY calibration, and `LPF 0..63` exposes the raw capacitor code.
BBTOP block 0x67, host 1, registers 4/5 control the I/Q filter capacitors.
The upper bits and calibrated bytes are preserved and restored after every
capture, including a timeout. The initial board calibration was 29/29.

Measurements used 24 captures per code, gain 75 and 80 MS/s at 2300 and
2484 MHz. Widths use a 2–4 MHz noise reference away from the DC pedestal.
Strong traffic clipped some 2412 MHz captures, so those were excluded from
the curve. Low capacitor codes have a non-flat response; the numeric range
is deliberately conservative, not a precision filter specification.
Subsampling does not automatically apply an anti-alias filter.

## Board and transport validation

Tested on an ESP8684H revision 1.0 with a 26 MHz crystal and 4 MB flash.
Raw IQ8/IQ10 captures and all advertised FFT profiles were checked across
sample rates and bandwidth settings, including odd sample counts and filter
restoration. Host regression tests cover commands, filter preservation,
capability export and SRAM-bank exclusion.

The CH340 connection can lose bytes even at 1 Mbaud. Use the viewer's
**Switch to 1 Mbaud** warning and CRC recovery; these checks do not establish
lossless UART delivery. The default remains 2 Mbaud, and `BAUD 1000000`
is session-only.
