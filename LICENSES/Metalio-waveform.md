# Metalio E0470A14-AF waveform provenance

## Fast DU14 and FOLLOW8 profile

With `CONFIG_METALIO_EPD_FAST_REFRESH=y` (the default), ordinary DU uses
14 phases and explicit binary feedback uses FOLLOW8. The Linux implementation
independently implements the binary pulse rules: ordinary DU
drives toward black 13 times and white 14 times; feedback drives toward black
7 times and white 8 times; unchanged pixels always hold. It selects between
two basic drive actions according to the phase number and pulse budget.
This project-owned implementation retains its MIT license; it does not include
the Apache-licensed LUT, an extracted per-phase table, the waveform generator
or renderer implementation from muxOS.

The following sources at `muxOS-project` commit
`c6a3e5f436e5b44234be4a5e2c3985abad480e8e` were inspected to compare the
functional rules, rather than imported:

| File relative to `firmware/boards/packages/` | SHA-256 |
| --- | --- |
| `e0470_epaper_waveform/waveforms/du14.h` | `f04da9e8d21355592e84092040ac2b7c4e3cdfe8b9ed1e065754ab496e781afc` |
| `e0470_epaper_waveform/e0470_epaper_waveform.c` | `65976d74d87d94a391ec16b5e215d9eb3516446ca077d4fd860d71eab2e9116d` |
| `e0470_epaper_waveform/include/e0470_epaper_waveform.h` | `d0cfbcd03fc0b1af9f973b3e6d84b28b893e131a904b006768d1be45b972208f` |
| `epdiy/src/output_common/lut.c` | `ece6b407748188d1aa84ef124c0d4e3a9e9c412c0da0f32b8dd6452c4c0efe46` |
| `mux_display_metalio/mux_display_metalio.c` | `b9062b5f2ad8e16ab28a1faace585381c19ac6c19d9ef56e73b180ceddc497cd` |

The E0470 waveform component is Copyright 2026 mindreset, Apache-2.0.
Its original reference notice is retained as `MuxOS-E0470-Apache-2.0.txt`;
this records the inspected reference's license and does not relicense its
code or identify it as code compiled into the GPLv2-only Linux kernel.

The original DU14 LUT contains 896 bytes, SHA-256
`7e63cbde8a52b3141abc48d2559842e61cb33fe32a7d6fcccd0170d0344c425d`.
In that reference, the raw black-to-black drive in phases 11–14 (one-based)
is suppressed by its DU renderer. The independently implemented functional
rules produce effective actions ordered white-to-white, white-to-black,
black-to-white, black-to-black: `{0,1,2,0}` for ordinary DU phases 1–13
and `{0,0,2,0}` for phase 14; feedback uses the first action for phases
1–7 and the second for phase 8. These observations do not grant a new
license to any reference implementation.

The current Linux profile uses the original scan timing for both option
settings: 20 MHz, 340 clocks / 17 us per line, CKV high 15.9 us and low
1.1 us, with STV startup delays of 17 us and 32 us. All modes, including
DU14 and FOLLOW8, use a 12,000 us minimum phase start-to-start interval;
each complete DMA/RMT scan still finishes before the next begins. The
interval includes scanning and generation rather than adding a 12 ms dwell
after scanning.

With the option enabled, adjacent identical effective phase actions reuse
the encoded DMA buffer, reducing encoding to two passes per binary refresh
without skipping any scan. Disabling it selects legacy DU20 actions and
per-phase encoding; feedback is then unavailable in the reported mode bitmap.
The option no longer changes scan timing or minimum phase pacing. GC48,
startup conditioning, VCOM and power sequencing remain unchanged.

The default terminal requests GC after every 120 successful refreshes,
including FOLLOW8, when the next display update is needed. This GC120 policy
is terminal scheduling, not an additional waveform or an autonomous driver
timer. The first frame and recovery from unknown display history use GC.

FOLLOW8 is intended only for known binary display history. Success commits
the submitted frame; an error invalidates history and the next refresh
clears the panel and uses GC. The DU14/FOLLOW8 profile with this scan timing
has been verified on the target Metalio device. This does not establish
full-temperature calibration, long-term panel life or a measured latency
improvement across operating conditions.

## Legacy DU20 and retained GC48

The legacy DU20 and retained GC48 endpoint tables in
`kernel/metalio-epd-waveform.h` are derived from
[CloudZao/Metalio-E-INK4-Plus](https://github.com/CloudZao/Metalio-E-INK4-Plus),
commit `2f55cb1234baae4968982feb23e7a9001dac16f0`.
The original MIT copyright and permission notice is retained in
`Metalio-reference-MIT.txt` in this directory.

Reference paths are relative to `main/boards/metalio-e-ink4-plus/display/`:

| File | SHA-256 |
| --- | --- |
| `waveforms/my_waveform_du.h` | `62ff83aeb2aae3048078f8d1c1d5bdcf24231b4b142d1893d566e64f496ee7db` |
| `waveforms/my_waveform_gc.h` | `f8c7f1a79981f45025867461d2d7074859e343797fc5257fc4fe4740b07f497d` |
| `waveforms/my_waveform.c` | `65150122028d3432f3ea28bbd498a1b1065441dc4fafa217d4322431168525bd` |
| `output_common/lut.c` | `5c477ffa6fbd14ce0a6c2a807ea3ce88e76141ff14c62c21464739a24c0a0bfa` |
| `output_common/white_border.h` | `ad99e229dfc09fe0fe783b2e9f9c9e5c689942b424fe0db761265fa274cb3f1b` |
| `output_lcd/lcd_driver.c` | `00039432f74461e8b2dca5c53d1a0ce3076a82769676c1958a34481b1099f155` |
| `output_lcd/render_lcd.c` | `d08742051a5bab2380b8b5089fd6e11842e369d14bddfa579c1eac5c97e3bdec` |
| `render.c` | `2723457ea4683e95108d8212464791f4f93cc87a28187b591da98923c30a2f9e` |

Extraction uses the original LUT indexing `(lut[phase][to][from / 4] >>
(6 - 2 * (from % 4))) & 3`, for `from` and `to` equal to 0 (black) or
15 (white). DU includes `complete_du_build()`'s additional black-to-black
black drive in phases 16–19. GC is the full 48-phase GC16 table, including its
implicitly zero-initialized 48th row. The legacy DU table has 20 phases.
No ED047TC2, trimmed or gray-eight table is used for the retained GC profile.
The temperature interval in the reference is 0–50 °C and does not imply
factory calibration across that range. These are the reference project's
experimental tables. They remain the source for the legacy DU20 and GC48
profiles; target validation does not extend their calibration range.

The new encoder follows the ESP32-S31 branch of `lookup_pixels_in_VE_LUT()`:
x0 uses bits 7:6, x1 bits 5:4, x2 bits 3:2, x3 bits 1:0. It adds one zero
byte before each 304-byte source line, pads 684 image rows to 688 DMA rows,
and leaves those final four rows undriven. It also leaves a requested border
undriven, matching `epd_filter_white_border()` with `whitening=0`. The board's
current border setting is zero pixels. The LCD driver must disable hardware
byte/bit reversal to preserve this byte stream.

`render_lcd.c` uses a 12,000 µs minimum phase start-to-start interval; LUT
`phase_times` are null. The encoder reports this interval. It is not an extra
dwell to add after scanning, and Linux scheduling or slower generation may
extend it. Revalidation is required when changing DMA strategy or clocking.

The uniform encoder supports the source's startup conditioning sequence in
`render.c`: three repetitions of ten black scans, ten white scans and three
undriven scans. Each uniform scan has the same dummy byte and four undriven
tail rows. Unlike the binary encoder's optional border, uniform conditioning
uses the current board's zero-pixel border. Panel state can only be considered
known after every conditioning scan has completed successfully.
