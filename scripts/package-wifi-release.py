#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate and package an esp32s31-ink-linux Wi-Fi candidate for manual installation."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile


PRIVACY_SPEC = importlib.util.spec_from_file_location("release_privacy", Path(__file__).with_name("release-privacy.py"))
PRIVACY = importlib.util.module_from_spec(PRIVACY_SPEC)
PRIVACY_SPEC.loader.exec_module(PRIVACY)


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("wifi_images", ROOT / "scripts/make-wifi-image.py")
IMAGES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(IMAGES)
SPEC = importlib.util.spec_from_file_location("wifi_memory", ROOT / "scripts/wifi-memory.py")
MEMORY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MEMORY)
require = IMAGES.require
SOURCE_DIRS = ("apps", "board", "configs", "docs", "firmware", "include", "kernel", "LICENSES",
               "package", "patches", "platform", "scripts", "shared", "tests")
LICENSE_FILES = (
    ("build/libvterm-0.3.3/LICENSE", "libvterm/LICENSE"),
    ("build/dropbear-client-only-2026.94/LICENSE", "dropbear/LICENSE"),
    ("build/dropbear-client-only-2026.94/libtommath/LICENSE", "dropbear/libtommath/LICENSE"),
    ("build/dropbear-client-only-2026.94/libtomcrypt/LICENSE", "dropbear/libtomcrypt/LICENSE"),
    ("build/opensbi-custom/COPYING.BSD", "opensbi/COPYING.BSD"),
    ("build/opensbi-custom/ThirdPartyNotices.md", "opensbi/ThirdPartyNotices.md"),
    ("build/busybox-1.37.0/LICENSE", "busybox/LICENSE"),
    ("build/linux-custom/COPYING", "linux/COPYING"),
    ("build/linux-custom/LICENSES/preferred/GPL-2.0", "linux/GPL-2.0"),
    ("build/linux-custom/LICENSES/exceptions/Linux-syscall-note", "linux/Linux-syscall-note"),
    ("build/wpa_supplicant-2.11/README", "wpa_supplicant/README"),
    ("build/libnl-3.11.0/COPYING", "libnl/COPYING"),
)
UPSTREAM_LICENSES = (
    ("buildroot/COPYING", "buildroot/COPYING"),
    ("wifi-reference/LICENSE", "wifi-reference/LICENSE"),
    ("esp-hosted-linux/LICENSE", "esp-hosted-linux/LICENSE"),
    ("esp-hosted-linux/host/LICENSE", "esp-hosted-linux/host/LICENSE"),
    ("esp-hosted-linux/esp/esp_driver/LICENSE", "esp-hosted-linux/esp_driver/LICENSE"),
    ("esp-hosted-linux/esp/esp_driver/network_adapter/LICENSE", "esp-hosted-linux/network_adapter/LICENSE"),
    ("esp-idf/LICENSE", "esp-idf/LICENSE"),
    ("esp-idf/components/esp_wifi/lib/LICENSE", "esp-idf/esp_wifi/LICENSE"),
    ("esp-idf/components/esp_phy/lib/LICENSE", "esp-idf/esp_phy/LICENSE"),
    ("esp-idf/components/esp_coex/lib/LICENSE", "esp-idf/esp_coex/LICENSE"),
    ("esp-idf/components/freertos/FreeRTOS-Kernel/LICENSE.md", "esp-idf/FreeRTOS/LICENSE.md"),
    ("esp-idf/components/wpa_supplicant/COPYING", "esp-idf/wpa_supplicant/COPYING"),
    ("esp-idf/components/protobuf-c/protobuf-c/LICENSE", "esp-idf/protobuf-c/LICENSE"),
    ("esp-idf/components/esp_libc/COPYING.picolibc", "esp-idf/picolibc/COPYING"),
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def config_has(path, *settings):
    lines = path.read_text().splitlines()
    for setting in settings:
        require(setting in lines, f"{path.name}: expected {setting}")


def check_patches(root, build):
    checked = {}
    for directory, tree in (("platform/esp32s31/patches/linux", "linux-custom"),
                            ("platform/esp32s31/patches/linux-companion", "linux-custom"),
                            ("platform/esp32s31/patches/opensbi", "opensbi-custom"),
                            ("patches/wpa_supplicant", "wpa_supplicant-2.11")):
        patches = sorted((root / directory).glob("*.patch"))
        require(patches, "Missing board patch series: " + directory)
        for patch in patches:
            with patch.open("rb") as source:
                result = subprocess.run(["patch", "--dry-run", "--reverse", "--force", "--batch", "--fuzz=0",
                                         "-p1", "-d", str(build / "build" / tree)],
                                        stdin=source, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            require(result.returncode == 0,
                    "Missing or modified build patch: " + str(patch.relative_to(root)))
            checked[str(patch.relative_to(root))] = digest(patch.read_bytes())
    return checked


def check_memory_dtb(build, dtb):
    tool = build / "host/bin/fdtget"
    require(tool.is_file(), "Missing host fdtget; enable BR2_PACKAGE_HOST_DTC")

    def get(node, prop, kind="s", default=None):
        args = [str(tool), "-t", kind]
        if default is not None:
            args += ["-d", default]
        result = subprocess.run(args + [str(dtb), node, prop],
                                check=True, stdout=subprocess.PIPE, text=True)
        return result.stdout.strip()

    regions = {"hosted-shm@2f020000": "2f020000 d000", "radio-heap@50e80000": "50e80000 40000",
               "guard@50eff000": "50eff000 1000", "opensbi@50f00000": "50f00000 30000"}
    regions["dma-pool@2f030000"] = "2f030000 40000"
    children = subprocess.run([str(tool), "-l", str(dtb), "/reserved-memory"],
                              check=True, stdout=subprocess.PIPE, text=True).stdout.split()
    require(set(children) == set(regions), "Unexpected device tree reserved-memory regions")
    for name, expected in regions.items():
        node = "/reserved-memory/" + name
        require(get(node, "reg", "x") == expected, "Incorrect reserved memory: " + node)
        require(get(node, "no-map") == "", "Reserved memory must be excluded from the Linux linear map")
    wifi = "/soc/wifi@2f020000"
    require(get(wifi, "reg", "x") == "2f020000 d000 20586014 8", "Incorrect shared-memory device mapping")
    for node in ("/soc/epd@20396000", "/soc/i2c@20385000",
                 "/soc/gpio@20583000", "/soc/pinctrl@20582000"):
        require(get(node, "compatible"), "Missing peripheral: " + node)
        require(get(node, "status", default="okay") == "okay",
                "Incorrect Linux peripheral ownership: " + node)
    pool = "/reserved-memory/dma-pool@2f030000"
    require(get(pool, "compatible") == "shared-dma-pool" and get(pool, "linux,dma-default") == "",
            "Missing Linux default SRAM DMA pool")
    require(get("/soc/epd@20396000", "memory-region", "x") == get(pool, "phandle", "x"),
            "Linux display must reference the SRAM DMA pool")
    properties = subprocess.run([str(tool), "-p", str(dtb), wifi], check=True,
                                stdout=subprocess.PIPE, text=True).stdout.split()
    require(not {"metalio,cpu0-io", "memory-region"} & set(properties), "Unexpected CPU0 IO proxy")
    require(get("/soc/i2c@20385000/touchscreen@38", "compatible") == "metalio,ft6336u",
            "Missing Linux touch driver")
    require(get("/memory@50000000", "reg", "x") == "50000000 1000000", "Incorrect Linux PSRAM mapping")
    require(get("/soc/flash@40800000", "reg", "x") == "40800000 600000", "Incorrect rootfs XIP mapping")
    require(get(wifi + "/flash@f00000", "reg", "x") == "f00000 100000", "Incorrect writable data partition")
    require(get("/soc/serial@2038a000", "status") == "okay" and
            get("/soc/syscon@20391000", "status") == "okay", "Linux serial consoles must remain enabled")
    return {"dtb_sha256": digest(dtb.read_bytes()), "reserved_memory": regions}


def source_pairs(root, build):
    kernel = build / "build/linux-custom"
    dtb_name = "metalio-eink4-plus-wifi"
    for directory, destination in ((root / "kernel", kernel / "drivers/misc/metalio"),
                                   (root / "kernel/esp32s31-hosted", kernel / "drivers/net/wireless/esp32s31_hosted"),
                                   (root / "apps", build / "build/metalio-epd-1")):
        for source in sorted(directory.iterdir()):
            if source.suffix in (".c", ".h") or source.name in ("Kconfig", "Makefile"):
                yield source, destination / source.name
    yield (root / "shared/esp32s31-hosted-shm.h",
           kernel / "drivers/net/wireless/esp32s31_hosted/esp32s31-hosted-shm.h")
    for source in sorted((root / "include/uapi/linux").glob("metalio_epd*.h")):
        yield source, kernel / "include/uapi/linux" / source.name
    for source in (root / "board/metalio/eink4-plus/dts/espressif/metalio-eink4-plus.dts",
                   root / "board/metalio/eink4-plus/wifi/dts/espressif/metalio-eink4-plus-wifi.dts"):
        yield source, kernel / "arch/riscv/boot/dts/espressif" / source.name
    for source in sorted((root / "firmware/esp32s31-linux-companion").rglob("*")):
        if source.is_file():
            yield source, build / "build/esp32s31-linux-companion-1" / source.relative_to(root / "firmware/esp32s31-linux-companion")
    for name in ("sdkconfig.defaults", "partitions.csv"):
        yield (root / "board/metalio/eink4-plus/wifi" / name,
               build / "build/esp32s31-linux-companion-1" / name)
    for source in sorted((root / "platform/esp32s31/opensbi").rglob("*")):
        if source.is_file():
            yield source, build / "build/opensbi-custom/platform/esp32s31" / source.relative_to(root / "platform/esp32s31/opensbi")
    firmware = build / "build/esp32s31-linux-companion-1/build"
    for source, image in ((firmware / "bootloader/bootloader.bin", "wifi-bootloader.bin"),
                          (firmware / "partition_table/partition-table.bin", "wifi-partition-table.bin"),
                          (firmware / "esp32s31-linux-companion.bin", "esp32s31-linux-companion.bin"),
                          (firmware / "sdkconfig", "wifi-sdkconfig"),
                          (kernel / "arch/riscv/boot/xipImage", "xipImage"),
                          (kernel / "arch/riscv/boot/dts/espressif" / (dtb_name + ".dtb"),
                           dtb_name + ".dtb"),
                          (build / "build/opensbi-custom/build/platform/esp32s31/firmware/fw_jump.bin",
                           "fw_jump.bin")):
        yield source, build / "images" / image
    for source in sorted((root / "board/metalio/eink4-plus/wifi/rootfs_overlay").rglob("*")):
        if source.is_file():
            yield source, build / "target" / source.relative_to(root / "board/metalio/eink4-plus/wifi/rootfs_overlay")


def flash_script(layout, factory):
    if factory:
        arguments = "0x0 " + IMAGES.image_names(layout)[1]
        notice = "First installation/reset: this erases saved NVS and Wi-Fi credentials."
    else:
        arguments = " ".join(f"{hex(slot['offset'])} {slot['file']}" for slot in layout["slots"])
        notice = "Upgrade: selected components preserve NVS and the data partition."
    return f'''#!/bin/sh
set -eu
[ "$#" -eq 1 ] || {{ echo "Usage: $0 SERIAL_PORT" >&2; exit 1; }}
cd -- "$(dirname -- "$0")"
sha256sum -c SHA256SUMS
echo '{notice}'
exec python -m esptool --chip esp32s31 --port "$1" --baud 1152000 write-flash \\
    {arguments}
'''.encode()


def collect_inputs(root, build):
    profile = "wifi"
    layout_path = root / "board/metalio/eink4-plus/wifi" / "layout.json"
    layout = IMAGES.read_layout(layout_path)
    image_name, full_name = IMAGES.image_names(layout)
    components = IMAGES.collect_components(build / "images", layout)
    image = (build / "images" / image_name).read_bytes()
    require(image == IMAGES.merged_image(components, layout), "Merged image mismatch; rebuild before packaging")
    data = (build / "images" / IMAGES.DATA_IMAGE).read_bytes()
    require(data == IMAGES.empty_data_image(build / "host", layout),
            "Factory data image is not the credential-free initialization image")
    full = (build / "images" / full_name).read_bytes()
    require(full == IMAGES.full_image(image, data, layout),
            "Full factory image mismatch; rebuild before packaging")
    config_has(build / ".config", "BR2_RISCV_32=y", "BR2_ESP32S31_INK_LINUX_PLATFORM_SUPPORT=y",
               "BR2_PACKAGE_ESP32S31_LINUX_COMPANION=y",
               "BR2_PACKAGE_METALIO_EPD=y", "BR2_PACKAGE_DROPBEAR_CLIENT_ONLY=y",
               "BR2_PACKAGE_WPA_SUPPLICANT_NL80211=y", "BR2_PACKAGE_WPA_SUPPLICANT_CLI=y",
               "BR2_PACKAGE_WPA_SUPPLICANT_PASSPHRASE=y", "BR2_PACKAGE_HOST_DTC=y")
    linux_config = build / "build/linux-custom/.config"
    config_has(linux_config, "CONFIG_ESP32S31_HOSTED=y", "CONFIG_PACKET=y",
               "CONFIG_CFG80211=y", "CONFIG_JFFS2_FS=y")
    config_has(linux_config, "CONFIG_METALIO_EPD=y", "CONFIG_METALIO_EPD_FAST_REFRESH=y",
               "CONFIG_TOUCHSCREEN_METALIO_FT6336U=y")
    require(not {"CONFIG_METALIO_CPU0_IO=y", "CONFIG_METALIO_EPD_PSRAM_DMA=y"} &
            set(linux_config.read_text().splitlines()), "Linux native SRAM profile differs from Buildroot")
    tree_memory = check_memory_dtb(build, build / "images/metalio-eink4-plus-wifi.dtb")
    config_has(build / "build/busybox-1.37.0/.config", "CONFIG_FSYNC=y", "CONFIG_UDHCPC=y",
               "CONFIG_STAT=y", "CONFIG_FEATURE_STAT_FORMAT=y")
    supplicant_config = build / "build/wpa_supplicant-2.11/wpa_supplicant/.config"
    config_has(supplicant_config, "CONFIG_NO_ROBUST_AV=y")
    require(not re.search(r"^CONFIG_(BGSCAN_SIMPLE|WNM|MBO)=.+$",
                          supplicant_config.read_text(), re.M),
            "wpa_supplicant scan IEs exceed the Hosted firmware capability")
    config_has(build / "images/wifi-sdkconfig", 'CONFIG_IDF_TARGET="esp32s31"',
               "CONFIG_FREERTOS_UNICORE=y", "CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE=y",
               "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y", "CONFIG_SPI_FLASH_AUTO_SUSPEND=y",
               "CONFIG_SPI_FLASH_SUSPEND_TSUS_VAL_US=50", "CONFIG_SPI_FLASH_SUSPEND_TRS_VAL_US=100")
    require("CONFIG_METALIO_CPU0_IO=y" not in (build / "images/wifi-sdkconfig").read_text().splitlines(),
            "Stale experimental firmware configuration; use a clean build directory")
    lock = json.loads((root / "sources.lock.json").read_text())
    firmware_build = build / "build/esp32s31-linux-companion-1/build"
    description_path = firmware_build / "project_description.json"
    description = json.loads(description_path.read_text())
    require(description["target"] == "esp32s31" and
            description["project_name"] == "esp32s31-linux-companion" and
            Path(description["idf_path"]).resolve() == (root / "outputs/esp32s31-ink-linux-sources/esp-idf").resolve() and
            Path(description["project_path"]).resolve() == firmware_build.parent.resolve() and
            description["git_revision"] == "v6.1.0-ink-linux-" + lock["wifi_sources"]["esp-idf"]["commit"][:12],
            "Firmware build metadata does not match the pinned project and ESP-IDF sources")
    memory = {"firmware": MEMORY.check_firmware_memory(firmware_build / "esp32s31-linux-companion.elf"),
              "device_tree": tree_memory}
    for name, setting in (("linux", "BR2_LINUX_KERNEL_CUSTOM_REPO_VERSION"),
                          ("opensbi", "BR2_TARGET_OPENSBI_CUSTOM_REPO_VERSION")):
        config_has(build / ".config", f'{setting}="{lock["sources"][name]["commit"]}"')
    for source, copied in source_pairs(root, build):
        require(source.read_bytes() == copied.read_bytes(), "Stale build source: " + str(copied.relative_to(build)))
    PRIVACY.check_target(build / "target", root)
    patches = check_patches(root, build)
    require(not (build / "target/data/wifi/wpa_supplicant.conf").exists(),
            "Persistent Wi-Fi credentials must not be installed in the root filesystem")
    require(not re.search(r"^\s*network\s*=", (build / "target/etc/wpa_supplicant.conf").read_text(), re.M),
            "Default root filesystem supplicant configuration must not contain a network")
    files = {image_name: image, IMAGES.DATA_IMAGE: data, full_name: full, **components}
    files["configs/wifi-build.json"] = PRIVACY.firmware_metadata(description)
    files["configs/wifi-memory.json"] = (json.dumps(memory, indent=2) + "\n").encode()
    files["flash-factory.sh"] = flash_script(layout, True)
    files["flash-upgrade.sh"] = flash_script(layout, False)
    for name, source in (("buildroot.config", build / ".config"),
                         ("linux.config", build / "build/linux-custom/.config"),
                         ("busybox.config", build / "build/busybox-1.37.0/.config"),
                         ("wpa_supplicant.config", supplicant_config),
                         ("wifi-sdkconfig", build / "images/wifi-sdkconfig"),
                         ("layout.json", layout_path), ("sources.lock.json", root / "sources.lock.json")):
        files["configs/" + name] = PRIVACY.public_config(source.read_bytes(), root)
    for name in ("esp-idf", "esp-hosted-linux"):
        marker_path = root / "outputs/esp32s31-ink-linux-sources" / name / ".esp32s31-ink-linux-source.json"
        marker = json.loads(marker_path.read_text())
        require(marker["revision"] == lock["wifi_sources"][name]["commit"], "Stale Wi-Fi source revision: " + name)
        files["configs/" + name + "-source.json"] = marker_path.read_bytes()
        for relative, expected in marker["patches"].items():
            patch = root / relative
            require(patch.resolve().is_relative_to(root.resolve()), "Patch path escapes project")
            content = patch.read_bytes()
            require(digest(content) == expected, "Changed source patch: " + relative)
            files["sources/patches/" + name + "/" + patch.name] = content
    for source, name in LICENSE_FILES:
        files["LICENSES/third-party/" + name] = (build / source).read_bytes()
    for source, name in UPSTREAM_LICENSES:
        files["LICENSES/third-party/" + name] = (root / "upstream" / source).read_bytes()
    legal_files = []
    legal = build / "legal-info"
    for pattern in ("*.csv", "README", "licenses/**/*", "host-licenses/**/*"):
        legal_files += [path for path in legal.glob(pattern) if path.is_file()]
    for source in sorted(set(legal_files)):
        files["LICENSES/buildroot-legal-info/" + str(source.relative_to(legal))] = (
            PRIVACY.public_config(source.read_bytes(), root)
            if source.suffix == ".csv" or source.name == "README" else source.read_bytes())
    toolchain = build / "host/opt/ext-toolchain/share/licenses"
    for source in sorted(toolchain.rglob("*")):
        if source.is_file():
            files["LICENSES/third-party/toolchain/" + str(source.relative_to(toolchain))] = source.read_bytes()
    hashes = {}
    symlinks = {}
    for directory in SOURCE_DIRS:
        for source in sorted((root / directory).rglob("*")):
            if source.is_symlink():
                symlinks[str(source.relative_to(root))] = os.readlink(source)
            if source.is_file() and not source.is_symlink() and "__pycache__" not in source.parts:
                relative = source.relative_to(root)
                if "rootfs_overlay/data/" in str(relative) and (source.name != ".keep" or source.stat().st_size):
                    continue
                content = source.read_bytes()
                hashes[str(relative)] = digest(content)
                files["sources/esp32s31-ink-linux/" + str(relative)] = content
                if directory == "LICENSES":
                    files["LICENSES/" + str(source.relative_to(root / "LICENSES"))] = content
                elif str(relative).startswith("apps/LICENSES/"):
                    files[str(relative)] = content
    for name in ("LICENSE", "README.md", "README.zh-CN.md", "Makefile", "Config.in", "external.desc",
                 "external.mk", "sources.lock.json"):
        content = (root / name).read_bytes()
        files["sources/esp32s31-ink-linux/" + name] = content
        hashes[name] = digest(content)
    files["LICENSE"] = (root / "LICENSE").read_bytes()
    slots = [dict(slot, size=len(components[slot["file"]]), sha256=digest(components[slot["file"]]))
             for slot in layout["slots"]]
    manifest = {"project": "esp32s31-ink-linux", "board": "Metalio E-Ink4 Plus", "chip": "esp32s31",
                "profile": profile, "shm_protocol": 2,
                "display_dma": "linux-coherent-sram",
                "verified_memory": memory,
                "validation": "Build and packaging checks passed; hardware acceptance is recorded separately and remains incomplete.",
                "layout": layout, "slots": slots, "image_size": len(image), "image_sha256": digest(image),
                "factory_data_sha256": digest(data), "normal_upgrade": "flash-upgrade.sh preserves NVS and data",
                "image": image_name, "full_image": full_name, "full_image_size": len(full),
                "full_image_sha256": digest(full),
                "sources": lock, "source_sha256": hashes, "source_symlinks": symlinks,
                "verified_build_patches": patches,
                "legal_info_collected": bool(legal_files)}
    files["manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    ownership = "CPU0: ESP-IDF/ESP-Hosted and Flash. CPU1: Linux display, touch, terminal and applications."
    guide = "wifi.md"
    files["README.txt"] = f'''esp32s31-ink-linux {profile} v1 candidate

{ownership}
Hardware acceptance remains incomplete. Validation results and remaining checks
are recorded in sources/esp32s31-ink-linux/docs/{guide}.
Wi-Fi, writable flash/XIP coordination, and display/network concurrency
must be tested before relying on this candidate. No SSH server is included.

Manual installation only. Close serial monitors and enter ROM download mode:
hold BOOT, press/release RESET, release BOOT. Install esptool 5.5.0 in a venv.
Verify checksums: sha256sum -c SHA256SUMS

FIRST INSTALL / INTENTIONAL DATA RESET (erases NVS and saved Wi-Fi credentials):
    ./flash-factory.sh /dev/ttyACM0
NORMAL UPGRADE (preserves NVS and the last 1 MiB data partition):
    ./flash-upgrade.sh /dev/ttyACM0

The 16 MiB {full_name} includes all firmware and the empty data
filesystem. Flash this single file at 0x0 for first installation:
    python -m esptool --chip esp32s31 --port /dev/ttyACM0 --baud 1152000 write-flash 0x0 {full_name}
It erases NVS and saved Wi-Fi credentials. Never use it to preserve saved state.
The merged {image_name} ends before data and requires the separate data
initializer on first installation; its padding also replaces NVS.
Normal upgrades flash the separate components and omit NVS, phy_init and data.
{IMAGES.DATA_IMAGE} is a separate 1 MiB, credential-free JFFS2 initializer.
Do not use whole-chip erase for a state-preserving upgrade.

After boot, run wifi-config without arguments on the local/serial terminal.
SSID and a hidden WPA2 password are prompted interactively; the private
configuration lives in /data/wifi/wpa_supplicant.conf and survives upgrades.
Use wpa_cli -i wlan0 status; ip addr show dev wlan0; ip route.
See sources/esp32s31-ink-linux/docs/{guide} for behavior and hardware acceptance checks.

Sources and project patches are included under sources/. Recreate source
symlinks listed in manifest.json source_symlinks before rebuilding the snapshot.
Pinned third-party
sources remain identified by sources.lock.json; retain corresponding upstream
sources before redistribution. LICENSES/ includes project/component notices
and generated Buildroot legal-info when available. Run scripts/build-wifi.sh legal-info
before redistributing. Exported configurations replace host paths with privacy
markers; rebuild using the repository defconfig.
'''.encode()
    for content in files.values():
        PRIVACY.check_public_bytes(content, root)
    return files, manifest


def package(root, build, output):
    archive = output.with_name(output.name + ".zip")
    require(not output.exists() and not output.is_symlink(), "Output already exists: " + str(output))
    require(not archive.exists() and not archive.is_symlink(), "Output already exists: " + str(archive))
    files, manifest = collect_inputs(root, build)
    files["SHA256SUMS"] = "".join(f"{digest(data)}  {name}\n" for name, data in sorted(files.items())).encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.mkdir()
    created = False
    try:
        for name, data in files.items():
            target = output / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            if name in ("flash-factory.sh", "flash-upgrade.sh"):
                target.chmod(0o755)
            elif name.startswith("sources/esp32s31-ink-linux/"):
                source = root / name.removeprefix("sources/esp32s31-ink-linux/")
                target.chmod(0o755 if source.stat().st_mode & 0o111 else 0o644)
        with archive.open("xb") as stream:
            created = True
            with zipfile.ZipFile(stream, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
                for name in sorted(files):
                    bundle.write(output / name, str(Path(output.name) / name))
        with zipfile.ZipFile(archive) as bundle:
            require(bundle.testzip() is None, "ZIP integrity check failed")
            for name, data in files.items():
                require(bundle.read(str(Path(output.name) / name)) == data, "ZIP content mismatch: " + name)
        return {"archive": str(archive), "archive_size": archive.stat().st_size,
                "archive_sha256": digest(archive.read_bytes()), "image_sha256": manifest["image_sha256"]}
    except BaseException:
        if created:
            archive.unlink(missing_ok=True)
        shutil.rmtree(output)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    build = args.build_dir or ROOT / "outputs/buildroot-eink4-plus-wifi"
    output = args.output_dir or ROOT / "outputs/releases/esp32s31-ink-linux-metalio-eink4-plus-wifi-v1"
    try:
        print(json.dumps(package(ROOT, build.resolve(), output.absolute()), indent=2))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
