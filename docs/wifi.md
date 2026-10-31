# Wi-Fi firmware and persistent configuration

[Overview](../README.md) · [System architecture](../README.md#system-architecture)

The Wi-Fi target runs ESP-IDF/ESP-Hosted radio and Flash services on CPU0.
CPU1 Linux directly owns display, touch, terminal and applications, using SRAM
for phase data and DMA descriptors. The default Linux-only image has no `wlan0`.
Supported networking is WPA2-Personal STA, IPv4 DHCP, DNS and an SSH client.
There is no SSH server, AP, WPA3, enterprise authentication or graphical setup.

## Build

Install the dependencies in the [build guide](build.md), then run from the
repository root as a regular user:

```sh
sudo apt install cmake ninja-build libffi-dev libusb-1.0-0
python3 scripts/fetch-sources.py --wifi
export IDF_TOOLS_PATH="$PWD/upstream/idf-tools"
python3 upstream/esp-idf/tools/idf_tools.py install --targets esp32s31
python3 upstream/esp-idf/tools/idf_tools.py install-python-env
export BR2_DL_DIR="$PWD/upstream/downloads/buildroot"
scripts/build-wifi.sh -j8
scripts/build-wifi.sh legal-info
python3 scripts/package-wifi-release.py
```

Upstream revisions are pinned in `sources.lock.json`. The default output is
`outputs/buildroot-eink4-plus-wifi`; override `ESP32S31_INK_LINUX_WIFI_BUILD_DIR` for an independent
build. The firmware wrapper selects the IDF Python environment matching the host
Python version. `ESP32S31_INK_LINUX_IDF_PYTHON` and `ESP32S31_INK_LINUX_IDF_TOOLCHAIN_BIN` can select an
explicit installation. Use a separate build directory for each target.

The configuration is `metalio_eink4_plus_wifi_defconfig`. Board-specific IDF
defaults and the partition table live in `board/metalio/eink4-plus/wifi/`, selected
by `BR2_PACKAGE_ESP32S31_LINUX_COMPANION_BOARD_DIR` and copied into the firmware
build tree. CPU0 sources are under `firmware/esp32s31-linux-companion/`; S31 boot
integration and patches are under `platform/esp32s31/`. Prepared IDF/Hosted copies
use `outputs/esp32s31-ink-linux-sources/`, leaving upstream checkouts unchanged.
Buildroot enables `BR2_PACKAGE_ESP32S31_LINUX_COMPANION`; Linux enables
`CONFIG_ESP32S31_HOSTED`.

The candidate is `outputs/releases/esp32s31-ink-linux-metalio-eink4-plus-wifi-v1`, with separate
components, factory images, an independent data initializer, source, configuration,
licenses and SHA256 checksums. `${PROJECT_ROOT}` and `${EXTERNAL_PATH}` in exported
configuration are privacy markers. Rebuild with the repository defconfig rather
than importing these audit snapshots verbatim.

## Install or upgrade

Close serial monitors. Hold BOOT, press and release RESET, then release BOOT to
enter ROM download mode. From the extracted package directory:

```sh
PORT=/dev/ttyACM0  # replace with the actual device
python -m esptool --chip esp32s31 --port "$PORT" read-mac
sha256sum -c SHA256SUMS
```

Identify the intended device before writing. First installation, including a
switch from the Linux-only Flash layout, uses:

```sh
./flash-factory.sh "$PORT"
```

This installs the full layout and initializes NVS, PHY and `/data`, replacing
existing state. Back up useful state privately first; data/NVS backups can contain
Wi-Fi keys. Serial and esptool setup are described in the build guide.

For an existing matching Wi-Fi layout, upgrade all components from one package:

```sh
./flash-upgrade.sh "$PORT"
```

This preserves NVS, PHY and the final 1 MiB data partition. To keep credentials,
use component upgrades; do not write a full image, initialize data, or erase the
chip. Keep firmware, OpenSBI, DTB and kernel together. Reset to normal boot after
installation. The Linux-only and Wi-Fi Flash layouts are different.

## Configure Wi-Fi

Run on the board's serial shell or screen terminal, without arguments:

```sh
wifi-config
```

Enter the SSID and WPA2 password at the prompts. Password input is hidden and
never passed as a command argument. SSIDs accept 1–32 bytes; passwords accept
8–63 bytes or a 64-digit hexadecimal PSK. Use a serial terminal to paste characters
not available on the on-screen English keyboard.

Saving atomically replaces `/data/wifi/wpa_supplicant.conf` and restarts networking.
The directory is `0700` and the file `0600`. Valid saved configuration connects
automatically after boot. Missing configuration, authentication failure or DHCP
failure leaves the local terminal usable. Run `wifi-config` again to change the
network; partition initialization is unnecessary.

## Check and reconnect

```sh
wpa_cli -i wlan0 status
ip addr show dev wlan0
ip route
nslookup example.com
wget -O /tmp/example.html http://example.com/
ssh user@your-server
```

`wpa_state=COMPLETED` confirms authentication; also check the IPv4 address,
default route and DNS. SSH connects to your server; the board runs a client.
Restart networking with:

```sh
wifi-stop
wifi-start
```

| Symptom | Check |
| --- | --- |
| Missing `wlan0` | Confirm the Wi-Fi image and matching components; inspect protocol/binding errors in `dmesg` |
| Authentication failure | Run `wifi-config` again and confirm WPA2-Personal on the AP |
| Authenticated without an IP | Check AP DHCP, restart networking, then inspect address and route |
| IP works but DNS fails | Check `/etc/resolv.conf`, default route and AP DNS |
| Unwritable `/data` | Run `wifi-start`, inspect mount/format-marker errors, and retain data; never auto-format |
| No automatic reconnection | Check data mounting, configuration permissions and startup logs |

Do not include credentials, PSKs, configuration files or data/NVS backups in bug
reports. Avoid supplicant key-debug logging.

## Validation and known limitations

GC clearing, DU erasure, moving text, keyboard margins, touch keys and basic
WPA2/DHCP/DNS/HTTP/SSH operation have been exercised. One concurrent run completed
100 mixed refreshes, 30 actual scans and 6 MiB of hash-verified downloads, but
counted **413 CPU0 heap allocation failures**. Full wireless-load qualification
therefore remains incomplete; source publication does not promote that candidate.

```sh
cat /sys/bus/platform/devices/*wifi*/firmware_heap
```

The snapshot includes internal heap and internal DMA free/minimum/largest-block
values and `allocation_failures` since transport initialization. Failures cover
all allocation capabilities, not only DMA. `minimum_dma` sums each heap's historic
low-water mark; these minima need not occur simultaneously. Network service
activity refreshes the snapshot. Keep allocation/error reporting and power guards.
Flash garbage-collection concurrency, physical power interruption and long-duration
operation remain unqualified.
