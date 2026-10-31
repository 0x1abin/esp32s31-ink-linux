# Hardware and display profile

[Project overview](../README.md) · [Build guide](build.md)

Both supported targets use Linux-native SRAM display and touch drivers.
The Wi-Fi target moves radio and writable Flash services to CPU0 while Linux
runs on CPU1. See [system architecture](../README.md#system-architecture) for
the current Wi-Fi mode.

## Board

| Item | Configuration |
| --- | --- |
| Board / SoC | Metalio E-Ink4 Plus / ESP32-S31 |
| Flash / Linux RAM layout | 16 MiB NOR / 16 MiB PSRAM |
| Panel | E0470A14-AF, 34-pin, 8-bit parallel, native 1216 × 684 |
| Application orientation | Portrait 684 × 1216 |
| Shared I²C | SDA GPIO0, SCL GPIO1, 100 kHz |
| I²C devices | TCA9555 `0x20`, TPS65185 `0x68`, FT6336U `0x38` |
| Touch interrupt / reset | GPIO5, both edges / TCA9555 P1.2, active low |
| Serial consoles | USB Serial/JTAG `/dev/ttyGS1`; UART0 `/dev/ttyS0`, GPIO58/59 TX/RX |

The [board device tree](../board/metalio/eink4-plus/dts/espressif/metalio-eink4-plus.dts)
is the pin and resource source of truth. Linux owns LCD-CAM, AXI-GDMA TX0, and
RMT channel 0 exclusively for display scanning. EMAC is disabled because its
default pins conflict with the display and touch interface. USB OTG, SD, camera,
modem, audio, frontlight control, and suspend power optimization are outside the
current application.

TCA9555 P1.2 is shared with the disabled modem/camera reset circuit. Enabling
those peripherals requires revisiting reset ownership.

## Scan timing and waveform

The current waveform and driver configuration have been validated on the
target Metalio display. These values are the application
baseline:

| Parameter | Value |
| --- | --- |
| PCLK | 20 MHz |
| Line length / period | 340 clocks / 17 µs |
| CKV high / low | 15.9 µs / 1.1 µs |
| STV startup waits | 17 µs, then 32 µs |
| Minimum phase start-to-start interval | 12 ms, including encoding and scanning |
| VCOM | −1.2 V; not programmed into PMIC EEPROM |

DU14 updates terminal content and FOLLOW8 updates Shift/symbol keyboard layers.
Ordinary key presses and releases do not request display refreshes. With
actions `0 = hold`, `1 = drive black`, `2 = drive white`, the effective binary
waveform is:

| Mode / phases | White → white | White → black | Black → white | Black → black |
| --- | --- | --- | --- | --- |
| DU14, 1–13 | 0 | 1 | 2 | 0 |
| DU14, 14 | 0 | 0 | 2 | 0 |
| FOLLOW8, 1–7 | 0 | 1 | 2 | 0 |
| FOLLOW8, 8 | 0 | 0 | 2 | 0 |

Adjacent phases with identical actions reuse the current encoded DMA data.
Each DU14 or FOLLOW8 refresh encodes twice while still scanning every phase and
checking LCD, DMA, and RMT completion. A single coherent DMA allocation holds
the complete phase and descriptors within the 256 KiB reserved SRAM pool.

GC uses 48 phases. On the first refresh or after an error invalidates display
history, the driver first performs 69 conditioning scans, then GC. Power and
VCOM sequencing are retained across all refresh modes. A scan error stops
output, powers down the panel, and invalidates history.

The terminal counts only completed successful refreshes, including FOLLOW8 and
the initial GC. Once the count reaches 120, 240, and subsequent multiples, the
next pending update uses GC. An idle display does not refresh merely to clean.
First-frame and error-recovery cleaning are independent of this interval.

`CONFIG_METALIO_EPD_FAST_REFRESH=y` selects DU14/FOLLOW8 and phase reuse. The
retained compatibility setting selects DU20 without FOLLOW8; both settings use
the same original scan timing and minimum phase interval. Provenance and
component notices are recorded in [the waveform attribution](../LICENSES/Metalio-waveform.md).
Working operation on the target unit does not qualify a temperature range,
long-term ghosting, or panel lifetime.

## Touch and bottom buttons

The board-specific FT6336U driver reports two Linux MT slots and a portrait
pointer range of X `0..683`, Y `0..1215`. It reads thirteen bytes from register
`0x02`, validates contact events and IDs, and captures the screen or bottom-key
region on first contact until release. Malformed reports and communication
errors release input and suppress new presses until all contacts are released.

The Metalio normalization accepts raw coordinates below 1216 on both axes,
except when both are at least 684. Raw X below 684 is used directly; otherwise
the portrait output is `(683 - raw Y, raw X)`. No extra framework rotation is
applied.

Three raw key regions are at Y `1570..1630`, X `100/200/300 ±30`. The leftmost
confirm region at X `100 ±30` reports `KEY_REFRESH`; each distinct press requests
one full GC refresh in the terminal, queuing while the display is busy. The two
page regions at X `200/300 ±30` retain one aggregated `KEY_ENTER` and send Return.
The two key states are independent, including simultaneous presses. Holding a
key does not repeat; another Enter requires both page contacts to be released.
The on-screen Enter key continues to send Return.

Reset is asserted low for at least 10 ms, followed by a 300 ms release wait.
Both-edge GPIO5 interrupts schedule a single sampling worker, with nominal
10 ms polling and at least 5 ms between reads. Timer waits round up to kernel
ticks. Protocol and behavior sources are documented in
[the touch attribution](../LICENSES/Metalio-touch.md).

## Flash layout

The ROM/SPL and XIP layout is defined by
[`esp32s31-layout.cfg`](../board/metalio/eink4-plus/esp32s31-layout.cfg).

| Component | Start | Next boundary |
| --- | --- | --- |
| Wrapped SPL | `0x002000` | `0x100000` |
| U-Boot FIT | `0x100000` | `0x300000` |
| Device tree | `0x300000` | `0x500000` |
| XIP Linux kernel | `0x500000` | `0xC00000` |
| Read-only cramfs root filesystem | `0xC00000` | `0x1000000` |

The root filesystem has a 4 MiB slot. `/tmp` and `/run` are RAM-backed and lose
their contents at reboot; no persistent writable filesystem is provided.
