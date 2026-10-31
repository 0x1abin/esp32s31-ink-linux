#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate and package the supported esp32s31-ink-linux firmware."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import zipfile


IMAGE = "metalio-eink4-plus-full-flash.bin"
FLASH_SIZE = 0x1000000
SLOTS = (
    ("spl_app.bin", 0x2000, 0x100000),
    ("u-boot.itb", 0x100000, 0x300000),
    ("metalio-eink4-plus.dtb", 0x300000, 0x500000),
    ("xipImage", 0x500000, 0xC00000),
    ("rootfs.cramfs", 0xC00000, FLASH_SIZE),
)


PRIVACY_SPEC = importlib.util.spec_from_file_location("release_privacy", Path(__file__).with_name("release-privacy.py"))
PRIVACY = importlib.util.module_from_spec(PRIVACY_SPEC)
PRIVACY_SPEC.loader.exec_module(PRIVACY)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def config_has(path, *settings):
    lines = path.read_text().splitlines()
    for setting in settings:
        require(setting in lines, f"{path.name}: expected {setting}")


def check_sources(root, build):
    kernel = build / "build/linux-custom"
    pairs = [(path, kernel / "drivers/misc/metalio" / path.name)
             for path in (root / "kernel").iterdir()
             if path.suffix in (".c", ".h") or path.name in ("Kconfig", "Makefile")]
    pairs += [(path, build / "build/metalio-epd-1" / path.name)
              for path in (root / "apps").iterdir() if path.suffix in (".c", ".h")]
    for relative in ("include/uapi/linux/metalio_epd.h",
                     "arch/riscv/boot/dts/espressif/metalio-eink4-plus.dts"):
        source = (root / relative if relative.startswith("include/") else
                  root / "board/metalio/eink4-plus/dts/espressif/metalio-eink4-plus.dts")
        pairs.append((source, kernel / relative))
    pairs.append((build / "images/metalio-eink4-plus.dtb",
                  kernel / "arch/riscv/boot/dts/espressif/metalio-eink4-plus.dtb"))
    for source, copied in pairs:
        require(source.read_bytes() == copied.read_bytes(),
                f"Stale build source: {copied.relative_to(build)}; rebuild before packaging")


def collect_inputs(root, build):
    """Read and check every input before creating a release directory."""
    kernel_config = build / "build/linux-custom/.config"
    config_has(kernel_config, "CONFIG_METALIO_EPD=y", "CONFIG_METALIO_EPD_FAST_REFRESH=y",
               "CONFIG_TOUCHSCREEN_METALIO_FT6336U=y",
               "# CONFIG_TOUCHSCREEN_EDT_FT5X06 is not set")
    config_has(build / ".config", "BR2_RISCV_32=y", "BR2_ESP32S31_INK_LINUX_PLATFORM_SUPPORT=y",
               "BR2_PACKAGE_METALIO_EPD=y",
               "BR2_PACKAGE_DROPBEAR_CLIENT_ONLY=y", "BR2_PACKAGE_LIBVTERM=y")
    lock = json.loads((root / "sources.lock.json").read_text())
    for name, setting in (("linux", "BR2_LINUX_KERNEL_CUSTOM_REPO_VERSION"),
                          ("u-boot", "BR2_TARGET_UBOOT_CUSTOM_REPO_VERSION"),
                          ("opensbi", "BR2_TARGET_OPENSBI_CUSTOM_REPO_VERSION")):
        config_has(build / ".config", f'{setting}="{lock["sources"][name]["commit"]}"')
    check_sources(root, build)
    PRIVACY.check_target(build / "target", root)
    timing_source = (root / "kernel/metalio-epd-regs.h").read_text()
    for name, expected in (("PCLK_MHZ", 20), ("LINE_US", 17), ("LINE_CLOCKS", 340),
                           ("CKV_HIGH_TICKS", 159), ("CKV_LOW_TICKS", 11), ("CKV_LAUNCH_US", 15)):
        require(re.search(rf"^#define EPD_{name} {expected}U$", timing_source, re.M),
                f"Unsupported scan timing: EPD_{name}")
    phase = (root / "kernel/metalio-epd-phase.h").read_text()
    require("return 14U;" in phase and "return 8U;" in phase and "? 12000U : 0U" in phase,
            "Unsupported phase counts or pacing")
    require("metalio_epd_gc_actions[48][4]" in (root / "kernel/metalio-epd-waveform.h").read_text(),
            "Unsupported GC phase count")
    require("term->updates % 120" in (root / "apps/terminal.c").read_text(),
            "Unsupported periodic GC interval")
    image = (build / "images" / IMAGE).read_bytes()
    require(0xC00000 < len(image) <= FLASH_SIZE, "Invalid full flash image size")
    slots = []
    for name, offset, limit in SLOTS:
        data = (build / "images" / name).read_bytes()
        require(data and offset + len(data) <= limit, f"Image exceeds flash slot: {name}")
        require(image[offset:offset + len(data)] == data, f"Embedded image mismatch: {name}")
        slots.append({"file": name, "offset": hex(offset), "limit": hex(limit),
                      "size": len(data), "sha256": digest(data)})
    files = {IMAGE: image}
    for source, name in ((build / ".config", "buildroot.config"),
                         (kernel_config, "linux.config"),
                         (build / "build/uboot-custom/.config", "uboot.config"),
                         (root / "sources.lock.json", "sources.lock.json"),
                         (root / "board/metalio/eink4-plus/linux.config", "board-linux.config"),
                         (root / "board/metalio/eink4-plus/uboot.config", "board-uboot.config"),
                         (root / "board/metalio/eink4-plus/esp32s31-layout.cfg", "esp32s31-layout.cfg")):
        files["configs/" + name] = PRIVACY.public_config(source.read_bytes(), root)
    files["LICENSE"] = (root / "LICENSE").read_bytes()
    files["sources.lock.json"] = (root / "sources.lock.json").read_bytes()
    for directory, prefix in ((root / "LICENSES", "LICENSES"),
                              (root / "apps/LICENSES", "apps/LICENSES")):
        for source in sorted(directory.rglob("*")):
            if source.is_file():
                files[str(Path(prefix) / source.relative_to(directory))] = source.read_bytes()
    files["LICENSES/metalio_epd.h"] = (root / "include/uapi/linux/metalio_epd.h").read_bytes()
    files["apps/terminal-font.h"] = (root / "apps/terminal-font.h").read_bytes()
    third_party = (
        ("libvterm-0.3.3/LICENSE", "libvterm/LICENSE"),
        ("dropbear-client-only-2026.94/LICENSE", "dropbear/LICENSE"),
        ("dropbear-client-only-2026.94/libtommath/LICENSE", "dropbear/libtommath/LICENSE"),
        ("dropbear-client-only-2026.94/libtomcrypt/LICENSE", "dropbear/libtomcrypt/LICENSE"),
        ("opensbi-custom/COPYING.BSD", "opensbi/COPYING.BSD"),
        ("opensbi-custom/ThirdPartyNotices.md", "opensbi/ThirdPartyNotices.md"),
        ("busybox-1.37.0/LICENSE", "busybox/LICENSE"),
        ("linux-custom/COPYING", "linux/COPYING"),
        ("uboot-custom/Licenses/README", "u-boot/README"),
        ("linux-custom/LICENSES/exceptions/Linux-syscall-note", "linux/Linux-syscall-note"),
    )
    for source, name in third_party:
        files["LICENSES/third-party/" + name] = (build / "build" / source).read_bytes()
    for source in sorted((build / "build/uboot-custom/Licenses").rglob("*")):
        if source.is_file():
            name = source.relative_to(build / "build/uboot-custom/Licenses")
            files[str(Path("LICENSES/third-party/u-boot") / name)] = source.read_bytes()
    toolchain_licenses = build / "host/opt/ext-toolchain/share/licenses"
    for source in sorted(toolchain_licenses.rglob("*")):
        if source.is_file():
            files[str(Path("LICENSES/third-party/toolchain") / source.relative_to(toolchain_licenses))] = source.read_bytes()
    for source, name in ((root / "upstream/buildroot/COPYING", "buildroot/COPYING"),
                         (root / "upstream/esp-linux-bsp/LICENSE", "esp-linux-bsp/LICENSE")):
        if source.is_file():
            files["LICENSES/third-party/" + name] = source.read_bytes()
    legal_dir = build / "legal-info"
    legal_files = sorted(legal_dir.glob("*.csv"))
    if (legal_dir / "README").is_file():
        legal_files.append(legal_dir / "README")
    for directory in ("licenses", "host-licenses"):
        legal_files += [source for source in sorted((legal_dir / directory).rglob("*"))
                        if source.is_file()]
    for source in legal_files:
        files[str(Path("LICENSES/buildroot-legal-info") / source.relative_to(legal_dir))] = (
            PRIVACY.public_config(source.read_bytes(), root)
            if source.suffix == ".csv" or source.name == "README" else source.read_bytes())
    source_hashes = {}
    for directory in ("apps", "board", "configs", "docs", "include", "kernel", "LICENSES", "package", "patches", "platform", "scripts", "tests"):
        for source in sorted((root / directory).rglob("*")):
            if source.is_file() and not source.is_symlink() and "__pycache__" not in source.parts:
                source_hashes[str(source.relative_to(root))] = digest(source.read_bytes())
    for name in ("LICENSE", "README.md", "README.zh-CN.md", "Makefile", ".gitignore",
                 "Config.in", "external.desc", "external.mk", "sources.lock.json"):
        source_hashes[name] = digest((root / name).read_bytes())
    manifest = {
        "project": "esp32s31-ink-linux", "board": "Metalio E-Ink4 Plus",
        "chip": "esp32s31", "panel": "E0470A14-AF", "flash_size": FLASH_SIZE,
        "flash_offset": "0x0", "image_size": len(image), "image_sha256": digest(image),
        "validation": {
            "baseline": "The current waveform, display and driver configuration has been confirmed on Metalio E-Ink4 Plus hardware.",
            "this_build": "Image layout, embedded components, source copies and configuration checked by this packager; this build has not been flashed again.",
            "limits": "No claim of temperature-range, lifetime or external networking validation.",
        },
        "refresh": {"pclk_mhz": 20, "line_us": 17, "line_clocks": 340,
                    "ckv_high_us": 15.9, "ckv_low_us": 1.1,
                    "stv_first_delay_us": 17, "stv_second_delay_us": 32,
                    "minimum_phase_start_interval_us": 12000,
                    "du_phases": 14, "feedback_phases": 8, "gc_phases": 48,
                    "supported_modes": 7, "periodic_gc_completed_refreshes": 120},
        "touch": {"driver": "metalio-ft6336u", "compatible": "metalio,ft6336u",
                  "i2c_address": "0x38", "shared_i2c_hz": 100000, "irq_gpio": 5,
                  "irq_edges": "both", "reset_tca9555_pin": 10,
                  "portrait_size": [684, 1216], "bottom_keys": "left KEY_REFRESH; other two KEY_ENTER; no repeat"},
        "legal_info": {
            "collected": bool(legal_files), "source_archives_included": False,
            "note": "Generated license files and manifests are included when available; run Buildroot legal-info and retain corresponding sources before redistribution.",
        },
        "sources": lock, "slots": slots, "source_sha256": source_hashes,
        "configurations": {name: digest(data) for name, data in files.items() if name.startswith("configs/")},
    }
    files["manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    files["README.txt"] = f"""esp32s31-ink-linux

Target: ESP32-S31, 16 MiB flash, E0470A14-AF panel.
DU14 + FOLLOW8; 20 MHz, 340 clocks / 17 us; CKV 15.9/1.1 us;
STV 17/32 us; phase starts at least 12 ms apart; GC after every 120
completed refreshes, including FOLLOW8. First use and error recovery clear
immediately. Metalio FT6336U touch; left bottom key requests a full refresh,
and the other two act as Enter.
The current baseline has been confirmed on Metalio E-Ink4 Plus hardware.
This newly packaged build has not been flashed again. Temperature range,
lifetime and external networking have not been validated.
SSH client is included; an external network interface is not enabled.

Enter ROM download mode (hold BOOT, press/release RESET, release BOOT).
Install esptool 5.5.0 in a Python virtual environment, then run from here:

sha256sum -c SHA256SUMS
PORT=/dev/ttyACM0  # replace with your device port
python -m esptool --chip esp32s31 --port "$PORT" --baud 1152000 write-flash 0x0 {IMAGE}

Writing from 0x0 replaces the existing firmware and resources.
Full image SHA-256: {manifest['image_sha256']}
The manifest lists embedded slot components; only the full image is needed
for flashing. configs/ contains validated build configurations with host paths
replaced by privacy markers; rebuild using the repository defconfig.
LICENSES/ contains project and component notices. Generated legal-info notices
and manifests are included under LICENSES/buildroot-legal-info/ when available;
source archives are distributed separately. Run scripts/build.sh legal-info
before redistributing firmware. Firmware licensing also requires preserving
corresponding upstream source and modifications; see
the project license documentation and sources.lock.json in configs/.
""".encode()
    for content in files.values():
        PRIVACY.check_public_bytes(content, root)
    return files, manifest


def package(root, build, output):
    archive = output.parent / (output.name + ".zip")
    require(not output.exists() and not output.is_symlink(), f"Output already exists: {output}")
    require(not archive.exists() and not archive.is_symlink(), f"Output already exists: {archive}")
    files, manifest = collect_inputs(root, build)
    files["SHA256SUMS"] = "".join(f"{digest(data)}  {name}\n" for name, data in sorted(files.items())).encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.mkdir()
    archive_created = False
    try:
        for name, data in files.items():
            destination = output / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        with archive.open("xb") as stream:
            archive_created = True
            with zipfile.ZipFile(stream, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
                for name in sorted(files):
                    bundle.write(output / name, str(Path(output.name) / name))
        with zipfile.ZipFile(archive) as bundle:
            require(bundle.testzip() is None, "ZIP integrity check failed")
            for name, data in files.items():
                require(bundle.read(str(Path(output.name) / name)) == data, f"ZIP content mismatch: {name}")
        return {"archive": str(archive), "archive_size": archive.stat().st_size,
                "archive_sha256": digest(archive.read_bytes()), "image_sha256": manifest["image_sha256"]}
    except BaseException:
        if archive_created:
            archive.unlink()
        shutil.rmtree(output)
        raise


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=root / "outputs/buildroot-eink4-plus")
    parser.add_argument("--output-dir", type=Path, default=root / "outputs/releases/esp32s31-ink-linux-metalio-eink4-plus-linux-only-v1")
    args = parser.parse_args()
    try:
        result = package(root, args.build_dir.resolve(), args.output_dir.absolute())
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Packaging failed: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
