# On-chip spectra

`CAPS` advertises `SPEC SPECN SPECCAPS` only on enabled targets. Query
`SPECINFO?` before enabling a control. Its reply is `SPECINFO <JSON>`:

```json
{"continuous":false,"transports":["USB","UART"],"profiles":[[80000000,0,256,1,1,1],[80000000,0,512,1,1,0]]}
```

C2 supports UART snapshot spectra at 80/40/16 MS/s (codes 0/1/6),
with 256/512/1024/2048 bins at each rate.

H2 uses rate codes 7/6/8/9 for nominal 32/16/10.667/6.4 MS/s. Code 8 is
32 MHz / 3, represented as 10666667 Hz in integer protocol fields. Existing
codes 0–7 retain their meanings. H2 provides snapshot spectra at
256/512/1024/2048 bins on all four rates over USB or UART.

Each profile contains `[sample_rate_hz, rate_code, fft_bins, stride,
units_per_frame, continuous]`. The last field is optional for compatibility;
five-field profiles inherit the top-level `continuous` value. Capabilities
are specific to the connection: C6/C61 use snapshot FFT on UART and
continuous 256-bin capture on native USB. S31 supports continuous capture at
all advertised FFT sizes and rates on native USB, with snapshots over UART.
S3 spectra require native USB.
Unsupported controls stay hidden. Existing S3 firmware without `SPECCAPS`
uses the original S3 compatibility profiles.

`SPEC` always processes as many FFTs as its capture and transport allow.
Completed powers accumulate while the previous frame is being transmitted;
once output is available, it emits and clears that batch. There is no fixed
frame timer, rolling history, or host request. The legacy stride and batch
arguments remain in the command for compatibility, but no longer limit SPEC
processing. Mean versus max-hold remains selectable.

Send `SPEC <milliseconds> <stride> <units_per_frame> <detector> <rate_code>
<fft_bins> [stats]`. When `CAPS` includes `SPECSTAT`, append `1` to receive
statistics alongside spectra (omitting it preserves the original protocol). Zero milliseconds runs until a stop byte; detector 0 means mean
power and 1 means maximum power. Use the parameters from the chosen profile.
Portable snapshots require stride 1. Incomplete FFTs are excluded from batches.

The start reply is `SPEC <fft_bins> <sample_rate_hz> <unit_pairs> <MHz>`.
Binary frames follow. All integers are little-endian:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `SPC1` |
| 4 | 4 | Frame sequence |
| 8 | 8 | First sample index |
| 16 | 4 | Samples spanned by the frame |
| 20 | 2 | Completed FFTs represented |
| 22 | 1 | Flags: bit 0 maximum detector; bit 1 skipped processing; bit 2 prior output drop; bit 3 snapshot gaps |
| 23 | 1 | Gain metadata from the first I/Q word |
| 24 | 2 | Saturating cumulative frame-drop count |
| 26 | 1 | log2(FFT bins) |
| 27 | 1 | dB code multiplier (2) |
| 28 | N | Power codes in natural FFT order |
| 28+N | 4 | CRC32 of header and bins |

A power code represents `20*log10(power)` in the normalized Q15 FFT domain,
clamped to 0–255. The viewer converts this to dBFS using the Hann-window
normalization. Zero is a quantized floor, not evidence of absent RF energy.
Snapshot sample indices use elapsed wall time. Bank-rotation indices
come from the measured contiguous bank boundaries.

The end line has twelve decimal fields:

```
SPECEND status detail units pairs elapsed_us late_max work_max_cycles frames drops abandoned ffts stopped_by_host
```

Normal stops return status 0. A bank deadline/continuity failure stops capture
instead of silently presenting broken timing. Empty S3 frames are dropped.
A host that stops reading can lose a partial frame and the end record when
bounded output timeouts expire. The decoder resynchronizes on CRC-valid frames
or an end record; if it cannot establish the end boundary, it marks the
connection failed and requires reconnecting. It must not issue ordinary
commands into that uncertain stream. Reconnection reacquires the serial lease.

`SPS1` statistics are optional 40-byte records, emitted approximately every
250 ms. They have a 36-byte little-endian body followed by its CRC32:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u32 | ASCII `SPS1` |
| 4, 6 | u16 | Core 0 / core 1 processing load, per mille |
| 8 | u16 | Fraction of samples analyzed by FFT, per mille |
| 10 | u16 | Bit 0: dual core; bit 1: core 0 assists FFTs |
| 12, 16 | u32 | Free internal heap / largest block before capture, bytes |
| 20, 24 | u32 | Cumulative abandoned work / dropped frames |
| 28, 30 | u16 | Maximum bank lateness in samples / output queue fill per mille |
| 32 | u32 | FFTs per second |

Load measures time spent processing captures, not general FreeRTOS CPU usage.
Snapshot coverage includes gaps between captures. The heap figures are taken
before interrupts are masked; they are not live allocator measurements.

All targets advertise `DCT`: `DC?` queries the on-chip DC correction;
`DC 1` selects the default slow tracker (1/64 update per FFT), and `DC 0`
removes each FFT's instantaneous DC estimate. Both correct the Hann window's
adjacent bins. The tracker resets at each capture and can also suppress a
stationary signal exactly at the LO. The viewer's offset-LO setting moves the
frequency of interest away from this correction; “fill DC bins” only changes
the display. Raw I/Q captures retain their original behavior.

## Implementation constraints

S3 uses three SRAM banks and a bare second-core worker for SIMD unpacking,
FFT, power accumulation and encoding. Core 0 owns bank rotation and USB and
assists with FFTs when its deadline permits. Bank revocation waits for active
readers before giving SRAM back to the RF writer. DC state has one owner.
`DUAL 0` selects the single-core fallback, `DUAL 1` enables the worker with
assistance (default), and `DUAL 2` disables assistance. `DUAL?` returns the
active mode and whether the worker booted; re-query `SPECINFO?` after changing
modes. The fallback splits FFTs into radix-2 stages, with bounded unpacking,
accumulation and checksum slices. Both paths support 256/512/1024/2048 bins
at 16/40/80 MS/s. The capture scheduler and SIMD kernel
run from internal RAM to avoid flash-cache delays. C6 and C61 use two banks
with scalar FFT work split into short slices. These bank-rotation runs mask interrupts;
their firmware profiles disable interrupt/task watchdogs, as required by this
architecture. A stalled host ends the acquisition through the stream timeout.
C3 uses snapshots at every FFT size: CPU reads while RF owns its SRAM return
a repeating four-word bus pattern, producing false spectral peaks. It stops
the dump and restores CPU ownership before processing genuine samples. Its
watchdog settings remain enabled.
All scalar FFT input copies are independent of subsequent RF writes. C6/C61
warm the FFT, checksum and statistics paths before starting RF capture, then
budget each processing stage separately. Their 256-sample windows are copied
in one bounded operation before the bank can be reclaimed.

The portable snapshot backend computes power and peak accumulation in integer
arithmetic; mean accumulation retains floating-point precision. Both detectors
reuse the same workspace, and FFT bin ordering is updated incrementally without
an additional index table. The scalar butterflies use 32-bit modular sums with
the same output bits and rounding as their former 64-bit arithmetic.

S31 rotates two complete 128 KiB SRAM ownership groups without stopping the RF
writer. Core 0 validates exact bank boundaries and handles USB; core 1 runs
RISC-V SIMD unpacking, Hann windowing and rounded FFTs. The worker relinquishes
each bank before RF reuse, and skips work that cannot meet that deadline.
Its code, buffers and stack reside in internal SRAM. Interrupt/task watchdogs
are disabled for the continuous capture scheduler, with bounded capture and
host-stall timeouts. Snapshot and continuous modes share their FFT workspace.

ESP32, S2, C2, C3, C5 and H2 use snapshot FFTs because a safe continuous
processing path has not been established. Shared spectrum processing and
statistics work on all supported targets. Large transforms can skip
substantial processing work; the stream counters report it.

## Validation

All advertised rate/FFT/detector combinations were checked on ESP32, C3, C5,
C6, C61, S2, S3 and S31, using UART on ESP32 and native USB on the others.
Checks covered frame CRCs, command access after capture, host stops, recovery
from reader stalls, and switching modes in the browser. S3 single-core, worker-only and assisted profiles are checked separately.
Alternate UART wiring
on the native-USB boards was not tested. Continuous capture was checked using
hardware indices and bank boundaries, not calibrated RF phase coherence.

S31 SIMD unpacking matched scalar conversion across 32,768 samples and all
word alignments. Its rounded FFT matched the scalar reference within three
output counts for zero, tone and Hann-windowed random inputs at all four sizes.
These checks caught and corrected a vendor-derived final-stage loop that
skipped eight samples. All 48 S31 rate/size/detector combinations passed CRC
and sustained-output checks, followed by overload, long-averaging, stop,
reader-stall/reconnect and browser mode-switching checks.

To repeat the profile check on an attached board, install `pyserial` and run:

```sh
python tools/check_spectrum.py --port /dev/ttyACM2 --milliseconds 3000 --stats
```

The tool does not flash firmware and emits a JSON report.
