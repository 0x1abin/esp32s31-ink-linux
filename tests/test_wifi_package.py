import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("wifi_release", ROOT / "scripts/package-wifi-release.py")
PACKAGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGER)
IMAGES = PACKAGER.IMAGES


def firmware_elf(sram_size=0x18000, reservations=None):
    """A linked ELF32 with real PT_LOAD, BSS, and IDF reservation objects."""
    reserved = {"hosted_shm": (0x2F020000, 0x2F02D000),
                "linux_dma_pool": (0x2F030000, 0x2F070000),
                "rom_hart1_stack": (0x2F07E200, 0x2F07E600)}
    reserved.update(reservations or {})
    data = bytearray(0x100)
    data += bytes(range(16))
    reserve_offset = len(data)
    strings = bytearray(b"\0")
    symbols = bytearray(16)
    for index, (name, bounds) in enumerate(reserved.items()):
        data += struct.pack("<II", *bounds)
        symbols += struct.pack("<IIIBBH", len(strings), 0x40000020 + index * 8, 8, 1, 0, 3)
        strings += ("reserved_region_" + name).encode() + b"\0"
    strings_offset = len(data)
    data += strings
    symbols_offset = len(data)
    data += symbols
    section_names = b"\0.text\0.bss\0.reservations\0.strtab\0.symtab\0.shstrtab\0"
    names_offset = len(data)
    data += section_names
    sections_offset = len(data)
    sections = [(0,) * 10,
                (1, 1, 6, 0x2F000000, 0x100, 16, 0, 0, 4, 0),
                (7, 8, 3, 0x2F000010, 0, sram_size - 16, 0, 0, 4, 0),
                (12, 1, 2, 0x40000020, reserve_offset, len(reserved) * 8, 0, 0, 4, 0),
                (26, 3, 0, 0, strings_offset, len(strings), 0, 0, 1, 0),
                (34, 2, 0, 0, symbols_offset, len(symbols), 4, 1, 4, 16),
                (42, 3, 0, 0, names_offset, len(section_names), 0, 0, 1, 0)]
    for section in sections:
        data += struct.pack("<IIIIIIIIII", *section)
    struct.pack_into("<16sHHIIIIIHHHHHH", data, 0, b"\x7fELF\x01\x01\x01" + b"\0" * 9,
                     2, 243, 1, 0x2F000000, 52, sections_offset, 0, 52, 32, 2, 40, len(sections), 6)
    struct.pack_into("<IIIIIIII", data, 52, 1, 0x100, 0x2F000000, 0x2F000000, 16, sram_size, 7, 4)
    struct.pack_into("<IIIIIIII", data, 84, 1, reserve_offset, 0x40000020, 0x40000020,
                     len(reserved) * 8, len(reserved) * 8, 4, 4)
    return bytes(data)


class WifiPackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.root = self.directory / "source"
        self.build = self.directory / "buildroot"
        self.images = self.build / "images"
        self.output = self.directory / "releases/wifi-v1"
        self.archive = self.output.with_name(self.output.name + ".zip")
        for directory in PACKAGER.SOURCE_DIRS:
            shutil.copytree(ROOT / directory, self.root / directory,
                            symlinks=True, ignore=shutil.ignore_patterns("__pycache__"))
        for name in ("LICENSE", "README.md", "README.zh-CN.md", "Makefile", "Config.in",
                     "external.desc", "external.mk", "sources.lock.json"):
            self.write(self.root / name, (ROOT / name).read_bytes())
        self.layout_path = self.root / "board/metalio/eink4-plus/wifi/layout.json"
        self.layout = IMAGES.read_layout(self.layout_path)
        self.partitions = [
            ("nvs", 1, 2, 0x9000, 0x6000), ("phy_init", 1, 1, 0xF000, 0x1000),
            ("factory", 0, 0, 0x10000, 0x2F0000), ("opensbi", 1, 0x40, 0x300000, 0x100000),
            ("dtb", 1, 0x40, 0x400000, 0x100000), ("linux", 1, 0x40, 0x500000, 0x400000),
            ("rootfs", 1, 0x40, 0x900000, 0x600000), ("data", 1, 0x40, 0xF00000, 0x100000)]
        for number, slot in enumerate(self.layout["slots"]):
            self.write(self.images / slot["file"], bytes([number + 1]) * 64)
        self.write(self.images / "wifi-partition-table.bin", self.partition_table())
        lock = json.loads((self.root / "sources.lock.json").read_text())
        self.write(self.build / ".config", '\n'.join((
            "BR2_RISCV_32=y", "BR2_ESP32S31_INK_LINUX_PLATFORM_SUPPORT=y", "BR2_PACKAGE_ESP32S31_LINUX_COMPANION=y", "BR2_PACKAGE_METALIO_EPD=y",
            "BR2_PACKAGE_DROPBEAR_CLIENT_ONLY=y", "BR2_PACKAGE_WPA_SUPPLICANT_NL80211=y",
            "BR2_PACKAGE_WPA_SUPPLICANT_CLI=y", "BR2_PACKAGE_WPA_SUPPLICANT_PASSPHRASE=y", "BR2_PACKAGE_HOST_DTC=y",
            'BR2_LINUX_KERNEL_CUSTOM_REPO_VERSION="' + lock["sources"]["linux"]["commit"] + '"',
            'BR2_TARGET_OPENSBI_CUSTOM_REPO_VERSION="' + lock["sources"]["opensbi"]["commit"] + '"')))
        self.write(self.build / "build/linux-custom/.config", '\n'.join(
            "CONFIG_" + setting + "=y" for setting in (
                "ESP32S31_HOSTED", "PACKET", "CFG80211", "JFFS2_FS", "METALIO_EPD",
                "METALIO_EPD_FAST_REFRESH", "TOUCHSCREEN_METALIO_FT6336U")) +
                   "\n")
        self.write(self.build / "build/busybox-1.37.0/.config",
                   "CONFIG_FSYNC=y\nCONFIG_UDHCPC=y\nCONFIG_STAT=y\nCONFIG_FEATURE_STAT_FORMAT=y\n")
        self.write(self.build / "build/wpa_supplicant-2.11/wpa_supplicant/.config",
                   "CONFIG_NO_ROBUST_AV=y\n#CONFIG_BGSCAN_SIMPLE=y\n#CONFIG_WNM=y\n")
        self.write(self.images / "wifi-sdkconfig", '\n'.join((
            'CONFIG_IDF_TARGET="esp32s31"', "CONFIG_FREERTOS_UNICORE=y",
            "CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE=y", "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y",
            "CONFIG_SPI_FLASH_AUTO_SUSPEND=y", "CONFIG_SPI_FLASH_SUSPEND_TSUS_VAL_US=50",
            "CONFIG_SPI_FLASH_SUSPEND_TRS_VAL_US=100")))
        firmware = self.build / "build/esp32s31-linux-companion-1/build"
        self.write(firmware / "esp32s31-linux-companion.elf", firmware_elf())
        self.write(self.images / "metalio-eink4-plus-wifi.dtb", json.dumps(self.device_tree(False)))
        self.write(self.build / "host/bin/fdtget", """#!/usr/bin/env python3
import json, sys
args = sys.argv[1:]
default = None
if args[0] == '-t':
    del args[:2]
    if args[0] == '-d':
        default = args[1]
        del args[:2]
    mode = 'get'
else:
    mode = args.pop(0)
tree = json.load(open(args[0]))
node = args[1]
if node not in tree:
    sys.exit(2)
if mode == '-l':
    print('\\n'.join(path[len(node) + 1:] for path in tree
                    if path.startswith(node + '/') and '/' not in path[len(node) + 1:]))
elif mode == '-p':
    print('\\n'.join(tree[node]))
else:
    value = tree[node].get(args[2], default)
    if value is None:
        sys.exit(2)
    print(value)
""")
        (self.build / "host/bin/fdtget").chmod(0o755)
        self.write(firmware / "project_description.json", json.dumps({
            "target": "esp32s31", "project_name": "esp32s31-linux-companion", "project_version": "fixture", "idf_path": str(self.root / "outputs/esp32s31-ink-linux-sources/esp-idf"),
            "project_path": str(firmware.parent),
            "git_revision": "v6.1.0-ink-linux-" + lock["wifi_sources"]["esp-idf"]["commit"][:12]}))
        kernel = self.build / "build/linux-custom/arch/riscv/boot"
        for name, destination in (("wifi-bootloader.bin", firmware / "bootloader/bootloader.bin"),
                                  ("wifi-partition-table.bin", firmware / "partition_table/partition-table.bin"),
                                  ("esp32s31-linux-companion.bin", firmware / "esp32s31-linux-companion.bin"),
                                  ("wifi-sdkconfig", firmware / "sdkconfig"),
                                  ("xipImage", kernel / "xipImage"),
                                  ("metalio-eink4-plus-wifi.dtb", kernel / "dts/espressif/metalio-eink4-plus-wifi.dtb"),
                                  ("fw_jump.bin", self.build / "build/opensbi-custom/build/platform/esp32s31/firmware/fw_jump.bin")):
            self.write(destination, (self.images / name).read_bytes())
        for source, copied in PACKAGER.source_pairs(self.root, self.build):
            if source != copied:
                self.write(copied, source.read_bytes())
        for number, (directory, tree) in enumerate((
                ("platform/esp32s31/patches/linux", "linux-custom"),
                ("platform/esp32s31/patches/linux-companion", "linux-custom"),
                ("platform/esp32s31/patches/opensbi", "opensbi-custom"),
                ("patches/wpa_supplicant", "wpa_supplicant-2.11"))):
            for patch in (self.root / directory).glob("*.patch"):
                patch.unlink()
            name = f"patched-{number}.txt"
            self.write(self.root / directory / "0001-fixture.patch",
                       f"--- a/{name}\n+++ b/{name}\n@@ -1 +1 @@\n-old\n+new\n")
            self.write(self.build / "build" / tree / name, "new\n")
        for source, _ in PACKAGER.LICENSE_FILES:
            self.write(self.build / source, "fixture component license\n")
        for source, _ in PACKAGER.UPSTREAM_LICENSES:
            self.write(self.root / "upstream" / source, "fixture upstream license\n")
        for name in ("esp-idf", "esp-hosted-linux"):
            patch = self.root / "upstream" / name / "fixture.patch"
            self.write(patch, "fixture patch\n")
            marker = {"revision": lock["wifi_sources"][name]["commit"],
                      "patches": {str(patch.relative_to(self.root)): hashlib.sha256(patch.read_bytes()).hexdigest()}}
            self.write(self.root / "outputs/esp32s31-ink-linux-sources" / name / ".esp32s31-ink-linux-source.json", json.dumps(marker))
        self.write(self.build / "host/sbin/mkfs.jffs2", """#!/usr/bin/env python3
import os, pathlib, stat, sys
args = sys.argv[1:]
assert '--squash-uids' in args and '--little-endian' in args and '--faketime' in args
assert int(args[args.index('--eraseblock') + 1]) == 65536
tree = pathlib.Path(args[args.index('--root') + 1])
assert {p.name for p in tree.iterdir()} == {'wifi', '.metalio-data-v1'}
assert (tree / '.metalio-data-v1').read_text() == 'metalio-data-v1\\n'
assert stat.S_IMODE((tree / 'wifi').stat().st_mode) == 0o700
assert not list((tree / 'wifi').iterdir())
size = int(next(arg.split('=', 1)[1] for arg in args if arg.startswith('--pad=')))
data = b'fixture JFFS2 metalio-data-v1 private empty wifi directory'
pathlib.Path(args[args.index('--output') + 1]).write_bytes(data + b'\\xff' * (size - len(data)))
""")
        (self.build / "host/sbin/mkfs.jffs2").chmod(0o755)
        IMAGES.make_images(self.images, self.layout_path, self.build / "host")

    @staticmethod
    def write(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data.encode() if isinstance(data, str) else data)

    def partition_table(self):
        records = b"".join(struct.pack("<2sBBII16sI", b"\xaa\x50", kind, subtype, offset, size,
                                       name.encode(), 0)
                           for name, kind, subtype, offset, size in self.partitions)
        records += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(records).digest()
        return records + b"\xff" * (3072 - len(records))

    @staticmethod
    def device_tree(cpu0):
        tree = {"/reserved-memory": {},
                "/memory@50000000": {"reg": "50000000 1000000"},
                "/soc/flash@40800000": {"reg": "40800000 600000"},
                "/soc/serial@2038a000": {"status": "okay"},
                "/soc/syscon@20391000": {"status": "okay"},
                "/soc/wifi@2f020000": {"reg": "2f020000 d000 20586014 8"},
                "/soc/wifi@2f020000/flash@f00000": {"reg": "f00000 100000"}}
        regions = {"hosted-shm@2f020000": "2f020000 d000", "radio-heap@50e80000": "50e80000 40000",
                   "guard@50eff000": "50eff000 1000", "opensbi@50f00000": "50f00000 30000"}
        if cpu0:
            regions.update({"display@50e00000": "50e00000 80000",
                            "display-descriptors@2f040000": "2f040000 1000"})
        else:
            regions["dma-pool@2f030000"] = "2f030000 40000"
        for name, reg in regions.items():
            tree["/reserved-memory/" + name] = {"reg": reg, "no-map": ""}
        for node in ("/soc/epd@20396000", "/soc/i2c@20385000",
                     "/soc/gpio@20583000", "/soc/pinctrl@20582000"):
            tree[node] = {"compatible": "fixture-peripheral", "status": "disabled" if cpu0 else "okay"}
        if cpu0:
            tree["/reserved-memory/display@50e00000"]["phandle"] = "42"
            tree["/soc/wifi@2f020000"].update({"memory-region": "42", "metalio,cpu0-io": ""})
        else:
            tree["/reserved-memory/dma-pool@2f030000"].update({
                "phandle": "41", "compatible": "shared-dma-pool", "linux,dma-default": ""})
            tree["/soc/epd@20396000"]["memory-region"] = "41"
            tree["/soc/i2c@20385000/touchscreen@38"] = {"compatible": "metalio,ft6336u"}
        return tree

    def package(self):
        return PACKAGER.package(self.root, self.build, self.output)

    def assert_no_output(self):
        self.assertFalse(self.output.exists())
        self.assertFalse(self.archive.exists())


    def test_profile_image_filenames_cannot_overwrite_components(self):
        layout = json.loads(self.layout_path.read_text())
        for name in ("xipImage", "wifi-bootloader.bin", IMAGES.FULL_IMAGE):
            layout["image"] = name
            self.layout_path.write_text(json.dumps(layout))
            with self.assertRaises(ValueError):
                IMAGES.read_layout(self.layout_path)





    def test_native_device_tree_requires_full_sram_pool_and_no_cpu0_proxy(self):
        dtb = self.images / "display.dtb"
        for node, prop, bad, message in (
                ("/reserved-memory/dma-pool@2f030000", "reg", "2f030000 1000", "Incorrect reserved memory"),
                ("/reserved-memory/hosted-shm@2f020000", "reg", "2f050000 d000", "Incorrect reserved memory"),
                ("/soc/wifi@2f020000", "reg", "2f050000 d000 20586014 8", "Incorrect shared-memory"),
                ("/soc/epd@20396000", "memory-region", "42", "reference the SRAM DMA pool"),
                ("/soc/wifi@2f020000", "metalio,cpu0-io", "", "Unexpected CPU0 IO proxy"),
                ("/soc/wifi@2f020000/flash@f00000", "reg", "e00000 200000", "Incorrect writable data")):
            with self.subTest(node=node, prop=prop):
                tree = self.device_tree(False)
                tree[node][prop] = bad
                self.write(dtb, json.dumps(tree))
                with self.assertRaisesRegex(ValueError, message):
                    PACKAGER.check_memory_dtb(self.build, dtb)

    def test_factory_merge_and_data_have_separate_boundaries(self):
        image = (self.images / IMAGES.IMAGE).read_bytes()
        self.assertLessEqual(len(image), 0xF00000)
        self.assertEqual(len((self.images / IMAGES.DATA_IMAGE).read_bytes()), 0x100000)
        self.assertEqual(image[0x9000:0xF000], b"\xff" * 0x6000)
        for slot in self.layout["slots"]:
            content = (self.images / slot["file"]).read_bytes()
            self.assertEqual(image[slot["offset"]:slot["offset"] + len(content)], content)

    def test_full_factory_image_includes_all_firmware_and_initialized_data(self):
        image = (self.images / IMAGES.IMAGE).read_bytes()
        data = (self.images / IMAGES.DATA_IMAGE).read_bytes()
        full = (self.images / IMAGES.FULL_IMAGE).read_bytes()
        self.assertEqual(len(full), self.layout["flash_size"])
        self.assertEqual(full[:len(image)], image)
        self.assertEqual(full[len(image):0xF00000], b"\xff" * (0xF00000 - len(image)))
        self.assertEqual(full[0xF00000:], data)
        with self.assertRaisesRegex(ValueError, "overlaps"):
            IMAGES.full_image(b"x" * (0xF00000 + 1), data, self.layout)
        with self.assertRaisesRegex(ValueError, "unexpected size"):
            IMAGES.full_image(image, data[:-1], self.layout)
        self.package()
        manifest = json.loads((self.output / "manifest.json").read_text())
        self.assertEqual(manifest["project"], "esp32s31-ink-linux")
        self.assertEqual(manifest["board"], "Metalio E-Ink4 Plus")
        self.assertEqual(manifest["full_image_size"], len(full))
        self.assertEqual(manifest["full_image_sha256"], hashlib.sha256(full).hexdigest())
        script = (self.output / "flash-factory.sh").read_text()
        self.assertIn("0x0 " + IMAGES.FULL_IMAGE, script)
        self.assertNotIn("0xf00000", script)

    def test_oversized_component_does_not_replace_previous_images(self):
        previous = (self.images / IMAGES.IMAGE).read_bytes()
        self.write(self.images / "xipImage", b"x" * (0x400000 + 1))
        with self.assertRaisesRegex(ValueError, "exceeds flash slot"):
            IMAGES.make_images(self.images, self.layout_path, self.build / "host")
        self.assertEqual((self.images / IMAGES.IMAGE).read_bytes(), previous)

    def test_partition_bounds_and_checksum_are_verified(self):
        self.partitions[-1] = ("data", 1, 0x40, 0xE00000, 0x200000)
        with self.assertRaisesRegex(ValueError, "does not match"):
            IMAGES.validate_partitions(self.partition_table(), self.layout)
        data = bytearray(self.partition_table())
        data[20] ^= 1
        with self.assertRaisesRegex(ValueError, "checksum"):
            IMAGES.validate_partitions(bytes(data), self.layout)

    def test_package_contains_verified_components_sources_and_not_runtime_secrets(self):
        secret = os.urandom(24).hex()
        self.write(self.build / "run/wifi/wpa_supplicant.conf", secret)
        result = self.package()
        self.assertEqual(result["archive_sha256"], hashlib.sha256(self.archive.read_bytes()).hexdigest())
        manifest = json.loads((self.output / "manifest.json").read_text())
        self.assertEqual(manifest["validation"], "Build and packaging checks passed; "
                         "hardware acceptance is recorded separately and remains incomplete.")
        readme = (self.output / "README.txt").read_text()
        self.assertIn("Hardware acceptance remains incomplete", readme)
        self.assertIn("sources/esp32s31-ink-linux/docs/wifi.md", readme)
        self.assertNotIn("NOT been flashed", readme)
        self.assertEqual(manifest["layout"]["data_offset"], 0xF00000)
        self.assertEqual(manifest["display_dma"], "linux-coherent-sram")
        memory = json.loads((self.output / "configs/wifi-memory.json").read_text())
        self.assertEqual(memory, manifest["verified_memory"])
        self.assertEqual(memory["firmware"]["sram_static_end"], 0x2F018000)
        self.assertEqual(memory["firmware"]["heap_reservations"]["linux_dma_pool"],
                         {"address": 0x2F030000, "size": 0x40000})
        self.assertTrue((self.output / "LICENSES/third-party/esp-idf/esp_wifi/LICENSE").exists())
        self.assertEqual((self.output / "apps/LICENSES/Unifont.txt").read_bytes(),
                         (ROOT / "apps/LICENSES/Unifont.txt").read_bytes())
        self.assertTrue((self.output / "sources/patches/esp-idf/fixture.patch").exists())
        for name in ("platform/esp32s31/opensbi/platform.c", "patches/toolchain-external-custom/toolchain-external-custom.hash",
                     "board/metalio/eink4-plus/wifi/sdkconfig.defaults", "board/metalio/eink4-plus/wifi/partitions.csv"):
            self.assertTrue((self.output / "sources/esp32s31-ink-linux" / name).is_file())
        for name in ("scripts/build-wifi.sh", "board/metalio/eink4-plus/wifi/rootfs_overlay/usr/sbin/wifi-config"):
            self.assertTrue(os.access(self.output / "sources/esp32s31-ink-linux" / name, os.X_OK))
        with zipfile.ZipFile(self.archive) as archive:
            self.assertIsNone(archive.testzip())
            entry = archive.getinfo("wifi-v1/sources/esp32s31-ink-linux/scripts/build-wifi.sh")
            self.assertTrue((entry.external_attr >> 16) & 0o111)
            for name in archive.namelist():
                self.assertNotIn(secret.encode(), archive.read(name))
            for line in (self.output / "SHA256SUMS").read_text().splitlines():
                expected, name = line.split("  ", 1)
                self.assertEqual(expected, hashlib.sha256((self.output / name).read_bytes()).hexdigest())

    def test_stale_board_defaults_or_firmware_identity_is_rejected(self):
        path = self.build / "build/esp32s31-linux-companion-1/sdkconfig.defaults"
        original = path.read_bytes()
        path.write_text('stale board defaults')
        with self.assertRaisesRegex(ValueError, 'Stale build source'):
            self.package()
        self.assert_no_output()
        path.write_bytes(original)
        metadata = path.parent / 'build/project_description.json'
        value = json.loads(metadata.read_text())
        value['project_name'] = 'metalio-wifi'
        metadata.write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError, 'metadata does not match'):
            self.package()
        self.assert_no_output()

    def test_tampered_merged_or_factory_image_is_rejected(self):
        for name, message in ((IMAGES.IMAGE, "Merged image mismatch"),
                              (IMAGES.DATA_IMAGE, "credential-free"),
                              (IMAGES.FULL_IMAGE, "Full factory image mismatch")):
            with self.subTest(name=name):
                path = self.images / name
                original = path.read_bytes()
                changed = bytearray(original)
                changed[-1] ^= 1
                path.write_bytes(changed)
                with self.assertRaisesRegex(ValueError, message):
                    self.package()
                self.assert_no_output()
                path.write_bytes(original)

    def test_unsupported_supplicant_scan_ies_are_rejected(self):
        config = self.build / "build/wpa_supplicant-2.11/wpa_supplicant/.config"
        for setting in ("CONFIG_BGSCAN_SIMPLE=y", "CONFIG_BGSCAN_SIMPLE=n",
                        "CONFIG_WNM=y", "CONFIG_MBO=y",
                        "#CONFIG_NO_ROBUST_AV=y"):
            with self.subTest(setting=setting):
                config.write_text("CONFIG_NO_ROBUST_AV=y\n" + setting + "\n" if
                                  not setting.startswith("#") else setting + "\n")
                with self.assertRaisesRegex(ValueError, "scan IEs|CONFIG_NO_ROBUST_AV"):
                    self.package()
                self.assert_no_output()

    def test_tampered_component_or_stale_source_is_rejected(self):
        source = self.build / "build/esp32s31-linux-companion-1/main/main.c"
        source.write_text("stale source\n")
        with self.assertRaisesRegex(ValueError, "Stale build source"):
            self.package()
        self.assert_no_output()
        self.write(self.images / "esp32s31-linux-companion.bin", b"tampered component")
        with self.assertRaisesRegex(ValueError, "Merged image mismatch"):
            self.package()
        self.assert_no_output()

    def test_wrong_source_revision_and_build_configuration_are_rejected(self):
        marker = self.root / "outputs/esp32s31-ink-linux-sources/esp-idf/.esp32s31-ink-linux-source.json"
        original = marker.read_bytes()
        data = json.loads(original)
        data["revision"] = "0" * 40
        marker.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "Stale Wi-Fi source revision"):
            self.package()
        self.assert_no_output()
        marker.write_bytes(original)
        config = self.build / "build/linux-custom/.config"
        config.write_text(config.read_text() + "\nCONFIG_METALIO_EPD_PSRAM_DMA=y\n")
        with self.assertRaisesRegex(ValueError, "native SRAM profile"):
            self.package()
        self.assert_no_output()

    def test_missing_or_unsafe_flash_suspend_timings_are_rejected(self):
        config = self.images / "wifi-sdkconfig"
        original = config.read_text()
        for setting, unsafe in (("CONFIG_SPI_FLASH_SUSPEND_TSUS_VAL_US=50", "=29"),
                                ("CONFIG_SPI_FLASH_SUSPEND_TRS_VAL_US=100", "=50")):
            for replacement in ("", setting.split("=", 1)[0] + unsafe):
                with self.subTest(setting=setting, replacement=replacement):
                    config.write_text(original.replace(setting, replacement))
                    with self.assertRaisesRegex(ValueError, "expected " + setting):
                        self.package()
                    self.assert_no_output()
        config.write_text(original)

    def test_credentials_in_rootfs_are_rejected(self):
        self.write(self.build / "target/data/wifi/wpa_supplicant.conf", "secret")
        with self.assertRaisesRegex(ValueError, "credentials must not be installed"):
            self.package()
        self.assert_no_output()

    def test_firmware_built_with_another_idf_source_is_rejected(self):
        path = self.build / "build/esp32s31-linux-companion-1/build/project_description.json"
        description = json.loads(path.read_text())
        description["idf_path"] = str(self.root / "unrelated-idf")
        path.write_text(json.dumps(description))
        with self.assertRaisesRegex(ValueError, "Firmware build metadata does not match"):
            self.package()
        self.assert_no_output()

    def test_stale_firmware_memory_map_is_rejected_before_release_creation(self):
        path = self.build / "build/esp32s31-linux-companion-1/build/esp32s31-linux-companion.elf"
        self.write(path, firmware_elf(reservations={"linux_dma_pool": (0x2F040000, 0x2F041000)}))
        with self.assertRaisesRegex(ValueError, "Incorrect firmware heap reservation"):
            self.package()
        self.assert_no_output()

    def test_unapplied_kernel_or_opensbi_patch_is_rejected(self):
        for tree, name in (("linux-custom", "patched-0.txt"),
                           ("linux-custom", "patched-1.txt"),
                           ("opensbi-custom", "patched-2.txt"),
                           ("wpa_supplicant-2.11", "patched-3.txt")):
            with self.subTest(tree=tree, name=name):
                path = self.build / "build" / tree / name
                path.write_text("old\n")
                with self.assertRaisesRegex(ValueError, "Missing or modified build patch"):
                    self.package()
                self.assert_no_output()
                path.write_text("new\n")

    def test_upgrade_script_preserves_nvs_and_data(self):
        self.package()
        binary = self.directory / "bin"
        binary.mkdir()
        self.write(binary / "python", """#!/usr/bin/env python3
import os, pathlib, sys
arguments = sys.argv[sys.argv.index('write-flash') + 1:]
with open(os.environ['WIFI_TEST_FLASH'], 'r+b') as flash:
    for index in range(0, len(arguments), 2):
        offset = int(arguments[index], 0)
        data = pathlib.Path(arguments[index + 1]).read_bytes()
        assert not (offset < 0xF000 and offset + len(data) > 0x9000)
        assert offset + len(data) <= 0xF00000
        flash.seek(offset)
        flash.write(data)
""")
        (binary / "python").chmod(0o755)
        flash = self.directory / "flash.bin"
        flash.write_bytes(b"s" * 0x1000000)
        result = subprocess.run([str(self.output / "flash-upgrade.sh"), "/dev/test"],
                                env=dict(os.environ, PATH=str(binary) + os.pathsep + os.environ["PATH"],
                                         WIFI_TEST_FLASH=str(flash)), capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        content = flash.read_bytes()
        self.assertEqual(content[0x9000:0xF000], b"s" * 0x6000)
        self.assertEqual(content[0xF00000:], b"s" * 0x100000)

    def test_existing_output_is_preserved_and_zip_failure_cleans_partial_output(self):
        self.output.mkdir(parents=True)
        self.write(self.output / "keep", "keep")
        with self.assertRaisesRegex(ValueError, "Output already exists"):
            self.package()
        self.assertEqual((self.output / "keep").read_text(), "keep")
        shutil.rmtree(self.output)
        with mock.patch.object(PACKAGER.zipfile, "ZipFile", side_effect=OSError("ZIP failed")):
            with self.assertRaisesRegex(OSError, "ZIP failed"):
                self.package()
        self.assert_no_output()


class FirmwareMemoryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "firmware.elf"

    def check(self, content):
        self.path.write_bytes(content)
        return PACKAGER.MEMORY.check_firmware_memory(self.path)

    def test_actual_loads_bss_and_heap_reservations(self):
        result = self.check(firmware_elf(sram_size=0x20000))
        self.assertEqual(result["sram_static_end"], 0x2F020000)
        self.assertEqual(result["heap_reservations"]["hosted_shm"],
                         {"address": 0x2F020000, "size": 0xD000})
        self.assertEqual(result["heap_reservations"]["linux_dma_pool"]["size"], 0x40000)
        self.assertEqual(len(result["load_segments"]), 2)
        self.assertIn(".bss", [section["name"] for section in result["sram_sections"]])

    def test_pt_load_memory_cannot_touch_shm_dma_rom_or_linux_psram(self):
        for address, size in ((0x2F01FFF0, 0x20), (0x2F020000, 16), (0x2F030000, 16),
                              (0x2F07E200, 16), (0x50000000, 16), (0xC0E80000, 16),
                              (0x403FFFF0, 0x20)):
            with self.subTest(address=hex(address)):
                content = bytearray(firmware_elf())
                struct.pack_into("<IIII", content, 52 + 8, address, address, 16, size)
                with self.assertRaisesRegex(ValueError, "PT_LOAD.*overlaps"):
                    self.check(content)

    def test_allocated_sections_are_checked_even_outside_load_segments(self):
        for address in (0x2F020000, 0x2F030000, 0x2F07E200):
            with self.subTest(address=hex(address)):
                content = bytearray(firmware_elf())
                shoff = struct.unpack_from("<I", content, 32)[0]
                struct.pack_into("<I", content, shoff + 2 * 40 + 12, address)
                with self.assertRaisesRegex(ValueError, "section .bss.*overlaps"):
                    self.check(content)

    def test_every_idf_heap_reservation_must_match_the_actual_profile(self):
        for name, bounds in (("hosted_shm", (0x2F050000, 0x2F05D000)),
                             ("linux_dma_pool", (0x2F040000, 0x2F041000)),
                             ("rom_hart1_stack", (0x2F07E200, 0x2F07E5FF))):
            with self.subTest(reservation=name):
                with self.assertRaisesRegex(ValueError, "Incorrect firmware heap reservation.*" + name):
                    self.check(firmware_elf(reservations={name: bounds}))

    def test_missing_symbol_table_and_invalid_elf_mappings_are_rejected(self):
        content = bytearray(firmware_elf())
        shoff = struct.unpack_from("<I", content, 32)[0]
        struct.pack_into("<I", content, shoff + 5 * 40 + 4, 1)
        with self.assertRaisesRegex(ValueError, "Missing firmware heap reservation symbols"):
            self.check(content)
        for data in (b"not an ELF", firmware_elf()[:52], firmware_elf().replace(b"\x7fELF\x01", b"\x7fELF\x02", 1)):
            with self.subTest(size=len(data)):
                with self.assertRaises(ValueError):
                    self.check(data)
        content = bytearray(firmware_elf())
        struct.pack_into("<I", content, 52 + 12, 0x2F000004)
        with self.assertRaisesRegex(ValueError, "Invalid firmware PT_LOAD mapping"):
            self.check(content)


if __name__ == "__main__":
    unittest.main()
