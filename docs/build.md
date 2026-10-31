# Building and flashing

[Project overview](../README.md)

Run all commands from the repository root. The reference host is **Ubuntu 24.04
x86_64**, with **Python 3.10 or newer**. The pinned external toolchain is an
x86_64 Linux download; other host architectures are not covered by this guide.
Use a regular user for fetching and building.

This guide builds the Linux-only baseline. The optional [Wi-Fi v1 guide](wifi.md)
covers its pinned ESP-IDF tool setup, separate build and manual flashing layout.
See that guide for hardware results and remaining acceptance limits.

## Dependency versions

| Dependency | Required version |
| --- | --- |
| ESP-IDF | Not required for this default Linux-only build; the Wi-Fi candidate uses its separately pinned IDF source |
| Linux cross-toolchain | `riscv64-esp-linux-musl` 14.1.1, release `20260922`; downloaded and checksum-verified by Buildroot |
| esptool | `5.5.0`, installed in a separate Python virtual environment |
| Buildroot, Linux, OpenSBI, U-Boot and ESP Linux BSP | Exact source commits in [`sources.lock.json`](../sources.lock.json) |

The build uses Buildroot and the Linux boot chain directly. It does not call
`idf.py` or require `IDF_PATH` or ESP-IDF's `export.sh`. Use the standalone
esptool environment below; its installation follows the
[official esptool installation guide](https://docs.espressif.com/projects/esptool/en/latest/esp32/installation.html),
with this project's version pinned to 5.5.0.

For separate ESP-IDF firmware development, refer to Espressif's official
[ESP32-S31 getting-started guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/get-started/index.html)
and [ESP-IDF setup on Linux](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/get-started/linux-setup.html).
These optional links describe ESP-IDF development, not prerequisites for this
Linux image. Follow the ESP-IDF application's own version requirements; the
linked `latest` documentation tracks ESP-IDF's development branch.

## 1. Install host dependencies

```sh
sudo apt update
sudo apt install build-essential git make rsync patch bc bison flex cpio \
  unzip zip wget curl file xz-utils bzip2 gzip perl gawk \
  python3 python3-dev python3-venv python3-pip python3-setuptools \
  python3-pyelftools libssl-dev libncurses-dev libelf-dev \
  device-tree-compiler swig
python3 --version
```

Use a writable Linux filesystem with enough space for the upstream checkouts,
toolchain, and Buildroot output. Do not build as root or in a checkout path
containing spaces.

## 2. Fetch the pinned sources and install esptool

```sh
python3 -m venv upstream/esptool-env
upstream/esptool-env/bin/pip install esptool==5.5.0
export ESP_ESPTOOL="$PWD/upstream/esptool-env/bin/esptool"
export BR2_DL_DIR="$PWD/upstream/downloads/buildroot"
python3 scripts/fetch-sources.py
```

[`sources.lock.json`](../sources.lock.json) pins Buildroot, Espressif Linux,
OpenSBI, U-Boot, the image packager, and the toolchain checksum. The fetch script
refuses to replace locally modified upstream checkouts. It does not require the
original Metalio or muxOS reference repositories.

`ESP_ESPTOOL` must point to the installed executable, including during image
packaging. Keep these exports in each new build shell. `BR2_DL_DIR` is the shared
package download cache; using the location above also lets the host tests find
libvterm without installing it on the host.

## 3. Build the complete image

```sh
scripts/build.sh -j8
```

The script verifies the pinned checkouts, initializes the default configuration
on the first run, and builds the boot chain, kernel, applications, and image.
Adjust `-j8` to suit the host. The first build needs network access for package
and toolchain downloads.

| Output | Purpose |
| --- | --- |
| `outputs/buildroot-eink4-plus/images/metalio-eink4-plus-full-flash.bin` | Complete image to flash at `0x0` |
| `outputs/buildroot-eink4-plus/.config` | Actual Buildroot configuration |
| `outputs/buildroot-eink4-plus/build/linux-custom/.config` | Actual kernel configuration |

For a separate build directory, set an **absolute** path before the first build
and retain the export for subsequent commands:

```sh
export ESP32S31_INK_LINUX_BUILD_DIR="$PWD/outputs/my-build"
scripts/build.sh -j8
```

Images then appear under `$ESP32S31_INK_LINUX_BUILD_DIR/images/`. Do not move an initialized
Buildroot output directory between source trees.

The default configuration is `metalio_eink4_plus_defconfig`. This repository's
Buildroot external name is `ESP32S31_INK_LINUX`.

For incremental development:

```sh
# After editing apps/:
scripts/build.sh metalio-epd-rebuild -j8
scripts/build.sh -j8

# After editing kernel/, the board device tree, or the kernel fragment:
scripts/build.sh linux-reconfigure -j8
scripts/build.sh -j8
```

The final build command regenerates the root filesystem and complete image.
For changes to the main defconfig or package selection, use a fresh build
directory to reproduce the published defaults.

## 4. Run host checks

```sh
make test
```

The tests compile the shared phase and touch logic, check the terminal renderer
and PTY behavior, and validate build integration. They cover both fast and
compatibility waveform configurations. The libvterm source archive must already
be downloaded; `scripts/build.sh libvterm` is sufficient if a complete image is
not needed yet. Tests use temporary directories and retain a log in
`outputs/logs/host-tests.log`.

Generate a hardware-independent display pattern with:

```sh
make demo
outputs/host/metalio-epd-demo checker --output outputs/checker.pbm
```

Host checks do not measure electrical timing, touch calibration, or panel wear.

## 5. Flash and open the console

Use the USB data connection to the board's **USB Serial/JTAG** interface. Enter
ROM download mode by holding BOOT while pulsing RESET, then release BOOT. If the
board is unpowered, hold BOOT while connecting power. Select the port shown on
your host; `/dev/ttyACM0` below is an example.

Flashing the complete image replaces the previous firmware and resources.
Close any serial monitor before running esptool.

```sh
PORT=/dev/ttyACM0
"$ESP_ESPTOOL" --chip esp32s31 --port "$PORT" --baud 1152000 \
  write-flash 0x0 outputs/buildroot-eink4-plus/images/metalio-eink4-plus-full-flash.bin
```

Use `$ESP32S31_INK_LINUX_BUILD_DIR/images/metalio-eink4-plus-full-flash.bin` instead if a custom
build directory was selected. esptool **5.5.0** is the pinned version for this
board. After writing, release BOOT and reset the board for normal boot.

```sh
upstream/esptool-env/bin/python -m serial.tools.miniterm "$PORT" 115200
```

Exit miniterm with `Ctrl-]`. The host port may change after reconnecting; its
number is unrelated to the Linux console name `/dev/ttyGS1`. UART0 is also
available as `/dev/ttyS0`, with TX on GPIO58 and RX on GPIO59, at 115200 8N1.
Both consoles start a local root shell without a password.

Useful commands on the device:

```sh
uname -a
cat /proc/meminfo
dmesg | grep -E 'metalio|ft6336|i2c|ttyGS'
```

See [development and troubleshooting](development.md) for display demos and
common build or connection errors.

## 6. Package a release

After a successful complete build, collect the redistribution notices before
creating the release:

```sh
scripts/build.sh legal-info
python3 scripts/package-release.py \
  --build-dir outputs/buildroot-eink4-plus \
  --output-dir outputs/releases/esp32s31-ink-linux-metalio-eink4-plus-linux-only-v1
```

For a custom build, pass that directory with `--build-dir`. The output directory
contains the complete image, actual configurations, source and partition
manifest, license notices, flash instructions, and `SHA256SUMS`. The sibling
archive is `outputs/releases/esp32s31-ink-linux-metalio-eink4-plus-linux-only-v1.zip`. The script verifies slot
boundaries and embedded image contents, and refuses to overwrite an existing
release directory or archive; choose a new output name for each release.
Exported configurations replace host paths with `${PROJECT_ROOT}` or
`${EXTERNAL_PATH}` after checking the actual build inputs. Rebuild with the
repository defconfig rather than importing those audit snapshots verbatim.

After extracting the ZIP, run `sha256sum -c SHA256SUMS` inside its release
directory and follow its `README.txt` to flash the included image at `0x0`.

Review `outputs/buildroot-eink4-plus/legal-info/` (or the custom build directory) before
redistribution. Its license files and manifests are included in the ZIP when
present. This supplements the project notices; it does not collect every source
automatically. In particular, retain the pinned local-override Linux and U-Boot
sources together with this project's drivers, patches, and build instructions.
See the [component license overview](../LICENSES/README.md).
