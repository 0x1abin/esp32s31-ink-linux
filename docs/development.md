# Development and troubleshooting

[Project overview](../README.md) · [Build guide](build.md) · [Hardware profile](hardware.md)

## Application behavior

`metalio-terminal` starts once from BusyBox `inittab`. It opens a UNIX98 PTY,
starts an interactive BusyBox `ash`, and parses output with libvterm 0.3.3. The
portrait terminal has 41 columns × 22 rows of 16 × 32 pixel characters with at
least a 10 pixel margin. Its six-row keyboard occupies the bottom 40% of the
screen, starting at portrait Y=728, and supports English letters,
numbers, symbols, Shift, Backspace, Enter, Tab, Esc, Ctrl-C, and arrows. The
cursor does not blink. The leftmost capacitive confirm key (`KEY_REFRESH`)
requests one full GC refresh per press, queued if busy. The two page keys
(`KEY_ENTER`) and the on-screen Enter key send Return; holding a physical key
does not repeat.

The keyboard has a permanent number row, QWERTY letters with a centered home
row, and 60 × 73 pixel standard touch targets separated by 6 pixel inactive
gaps. Shift and Backspace are 1.5 keys wide, Space is four keys wide, and Enter
is three keys wide. Single-character labels use 24 × 48 pixel glyphs;
function labels use 16 × 32 pixel glyphs and arrows are drawn as lines.

```text
[1] [2] [3] [4] [5] [6] [7] [8] [9] [0]
[q] [w] [e] [r] [t] [y] [u] [i] [o] [p]
  [a] [s] [d] [f] [g] [h] [j] [k] [l]
[Shift] [z] [x] [c] [v] [b] [n] [m] [Back]
[Sym] [Tab] [/] [      Space      ] [.] [-]
[Esc] [Ctrl-C] [←] [↓] [↑] [→] [  Enter  ]
```

Sym replaces the letter rows with `! @ # $ % ^ & * ( )`,
`[ ] { } < > = + |`, and `` ` \ ; : ' " , `` respectively. Numbers,
function keys, and `/ . -` retain their positions. Shift toggles until tapped
again, including across symbol-layer changes; together the layers provide all
printable ASCII characters. Touching a gap sends nothing, and holding or sliding
a finger does not repeat input.

PTY and input events continue to be processed during a display refresh. Terminal
output is coalesced after 120 ms of idle time or about one second of continuous
output. Ordinary keys send input without highlight or release refreshes, including
when password input produces no echo. Shift/symbol layer changes use FOLLOW8
when no terminal output is pending, preserving the completed terminal image.
If output is pending, one DU14 frame includes both the output and keyboard layer.
Refresh failures invalidate
display history so the next update rebuilds it through cleaning.

The terminal currently opens `/dev/input/event0` and expects the board's
portrait touch device. Adding another input device requires revisiting this
assumption. An evdev overflow triggers state synchronization and a release
barrier to avoid sending unintended input.

The local display and both serial consoles run as root. There is no login
authentication on these local consoles and no SSH server in the image.

## Display demo

Only one process may open the display device. Stop the terminal before running
a pattern, then restart it afterward:

```sh
killall metalio-terminal
sleep 1
metalio-epd-demo white
metalio-epd-demo black
metalio-epd-demo checker
metalio-epd-demo text --text 'METALIO LINUX'
metalio-epd-demo checker --mode du
metalio-epd-demo text --mode feedback --text 'FOLLOW8'
metalio-terminal &
```

The default mode is GC. `--mode du` selects the kernel's ordinary binary mode;
`--mode feedback` requires FOLLOW8 capability and fails if unsupported. An
explicit feedback request is not silently changed to another waveform. First
refresh and fault recovery are forced through conditioning and GC, so their
elapsed times do not represent ordinary DU14 or FOLLOW8 latency.

## Display userspace interface

[`metalio_epd.h`](../include/uapi/linux/metalio_epd.h) defines ABI version 1 for
`/dev/metalio-epd`, an exclusive-open device with mode `0600`.

1. Query `METALIO_EPD_GET_INFO` for geometry, ABI version, and `supported_modes`.
   Bits 0, 1, and 2 represent DU, GC, and feedback. A zero bitmap from an older
   kernel means only DU and GC are assumed available.
2. Write one complete 103968-byte frame: native landscape 1216 × 684, row-major
   1 bpp, MSB first, `1 = black`, `0 = white`.
3. Submit `METALIO_EPD_REFRESH` with DU `0`, GC `1`, or feedback `2`.
4. Wait with `poll()`, then read `METALIO_EPD_GET_STATUS` to acknowledge the
   completion and inspect sequence, duration, busy state, and negative errno.

Write and refresh return `EBUSY` while a frame is in flight. The capability
bitmap uses an original reserved field, preserving the structure size, ioctl
numbers, and ABI version. Closing during a scan cancels further output and
powers down safely.

## SSH client and storage

The image contains Dropbear 2026.94 `dbclient` and its `ssh` alias, built without
a server. The default Linux-only target has no external network interface;
the optional [Wi-Fi target](wifi.md) provides `wlan0` and persistent configuration.
After configuring an interface, typical use is:

```sh
ssh user@192.0.2.1
ssh -p 2222 user@192.0.2.1
ssh -i /tmp/id_dropbear user@192.0.2.1
```

Check the server fingerprint before accepting a first connection. `~/.ssh`
points to `/run/ssh`, created with mode `0700`. Known-host entries and private
keys stored there are lost on reboot. Do not bake private keys or passwords
into firmware.

IPv4/TCP and UNIX sockets are enabled and loopback is initialized. In the default
Linux-only target, EMAC conflicts with board pins and USB Serial/JTAG provides
a serial console. Remote SSH requires a working interface, addressing, routing
and DNS as appropriate; the Wi-Fi target's hardware network acceptance remains
incomplete.

## Checks and changes

Run `make test` after changing shared display encoding, touch parsing, terminal
behavior, or packaging. The host suite tests both waveform configurations,
binary transitions and frame boundaries, touch contact capture and recovery,
GC120 scheduling, input without highlight refreshes, layer updates and real PTY input. It uses the checksum-verified
libvterm archive from the build download cache. See the
[build guide](build.md#4-run-host-checks) for setup and incremental commands.

Keep upstream checkout revisions synchronized with `sources.lock.json`. Kernel
integration copies project-owned drivers into the build tree and applies board
patches there; do not use edits inside `upstream/` as a release source.

When changing timing, waveforms, power sequencing, or coordinate handling,
validate on the target hardware as well. Report the source revision, actual
configurations, pattern or command that reproduces the issue, and relevant
kernel messages. Exclude credentials and private keys from logs.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Source verification fails | Run `python3 scripts/fetch-sources.py`; preserve local upstream edits before restoring pinned revisions. |
| Image packaging cannot find esptool | Export the absolute `ESP_ESPTOOL` path to the 5.5.0 environment in the current shell. |
| Host tests cannot find libvterm | Keep `BR2_DL_DIR` consistent with the build guide and run `scripts/build.sh libvterm`. |
| Serial permission denied | Check the selected port and host serial-group permissions; reconnect after group changes. |
| Serial write timeout | Close other monitors, check the USB data connection, re-enter ROM download mode, and verify esptool 5.5.0. |
| Root filesystem exceeds its slot | Reduce image contents or intentionally update the complete flash layout; the current rootfs budget is 4 MiB. |
| Display open fails with `EBUSY` | Stop the running terminal or other demo; the display accepts one opener. |
| Touch device missing or rejected | Inspect `dmesg` and `/proc/bus/input/devices`; verify FT6336U identity, reset, shared I²C, and the portrait coordinate range. |
| Screen corruption or scan errors | Stop display testing, retain `dmesg`, and inspect timing, power, and hardware connections against the board profile. |
| SSH cannot reach another host | Linux-only has no external network interface; for the Wi-Fi target check authentication, DHCP, route and DNS in the [Wi-Fi guide](wifi.md). |

The confirmed application profile establishes operation on the tested unit.
It does not establish a cold-boot reliability campaign, full-temperature
qualification, or long-term panel lifetime.
