import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock
import zipfile


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("package_release", ROOT / "scripts/package-release.py")
PACKAGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGER)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.root = self.directory / "source"
        self.build = self.directory / "buildroot"
        self.output = self.directory / "releases/esp32s31-ink-linux-metalio-eink4-plus-linux-only-v1"
        self.archive = self.output.with_name(self.output.name + ".zip")

        def write(path, data):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data.encode() if isinstance(data, str) else data)

        self.write = write
        for name in ("LICENSE", "README.md", "README.zh-CN.md", "Makefile", ".gitignore",
                     "Config.in", "external.desc", "external.mk",
                     "LICENSES/README.md", "apps/LICENSES/MIT.txt", "apps/LICENSES/Unifont.txt", "apps/terminal-font.h",
                     "include/uapi/linux/metalio_epd.h", "kernel/Kconfig", "kernel/Makefile",
                     "board/metalio/eink4-plus/linux.config", "board/metalio/eink4-plus/uboot.config",
                     "board/metalio/eink4-plus/esp32s31-layout.cfg",
                     "board/metalio/eink4-plus/dts/espressif/metalio-eink4-plus.dts"):
            write(self.root / name, "fixture\n")
        write(self.root / "kernel/metalio-epd-regs.h", "".join(
            f"#define EPD_{name} {value}U\n" for name, value in (
                ("PCLK_MHZ", 20), ("LINE_US", 17), ("LINE_CLOCKS", 340),
                ("CKV_HIGH_TICKS", 159), ("CKV_LOW_TICKS", 11), ("CKV_LAUNCH_US", 15))))
        write(self.root / "kernel/metalio-epd-phase.h", "return 14U; return 8U; ? 12000U : 0U\n")
        write(self.root / "kernel/metalio-epd-waveform.h", "metalio_epd_gc_actions[48][4]\n")
        write(self.root / "apps/terminal.c", "term->updates % 120\n")
        for source in (self.root / "kernel").iterdir():
            write(self.build / "build/linux-custom/drivers/misc/metalio" / source.name, source.read_bytes())
        for source in (self.root / "apps").glob("*.*"):
            write(self.build / "build/metalio-epd-1" / source.name, source.read_bytes())
        for source, target in (
                ("include/uapi/linux/metalio_epd.h", "include/uapi/linux/metalio_epd.h"),
                ("board/metalio/eink4-plus/dts/espressif/metalio-eink4-plus.dts",
                 "arch/riscv/boot/dts/espressif/metalio-eink4-plus.dts")):
            write(self.build / "build/linux-custom" / target, (self.root / source).read_bytes())
        lock = {"sources": {name: {"url": "https://example.invalid/source", "commit": "a" * 40}
                            for name in ("linux", "u-boot", "opensbi")}}
        write(self.root / "sources.lock.json", json.dumps(lock))
        write(self.build / ".config", '\n'.join((
            "BR2_RISCV_32=y", "BR2_ESP32S31_INK_LINUX_PLATFORM_SUPPORT=y", "BR2_PACKAGE_METALIO_EPD=y", "BR2_PACKAGE_DROPBEAR_CLIENT_ONLY=y",
            "BR2_PACKAGE_LIBVTERM=y", 'BR2_LINUX_KERNEL_CUSTOM_REPO_VERSION="' + "a" * 40 + '"',
            'BR2_TARGET_UBOOT_CUSTOM_REPO_VERSION="' + "a" * 40 + '"',
            'BR2_TARGET_OPENSBI_CUSTOM_REPO_VERSION="' + "a" * 40 + '"')))
        write(self.build / "build/linux-custom/.config", '\n'.join((
            "CONFIG_METALIO_EPD=y", "CONFIG_METALIO_EPD_FAST_REFRESH=y",
            "CONFIG_TOUCHSCREEN_METALIO_FT6336U=y", "# CONFIG_TOUCHSCREEN_EDT_FT5X06 is not set")))
        write(self.build / "build/uboot-custom/.config", "fixture\n")
        for name in ("libvterm-0.3.3/LICENSE", "dropbear-client-only-2026.94/LICENSE",
                     "dropbear-client-only-2026.94/libtommath/LICENSE",
                     "dropbear-client-only-2026.94/libtomcrypt/LICENSE",
                     "opensbi-custom/COPYING.BSD", "opensbi-custom/ThirdPartyNotices.md",
                     "busybox-1.37.0/LICENSE", "linux-custom/COPYING", "uboot-custom/Licenses/README",
                     "linux-custom/LICENSES/exceptions/Linux-syscall-note", "uboot-custom/Licenses/gpl-2.0.txt"):
            write(self.build / "build" / name, "fixture license\n")
        image = bytearray(0xC00000 + 64)
        for number, (name, offset, _) in enumerate(PACKAGER.SLOTS):
            data = bytes([number + 1]) * 64
            write(self.build / "images" / name, data)
            image[offset:offset + len(data)] = data
        write(self.build / "images" / PACKAGER.IMAGE, image)
        write(self.build / "build/linux-custom/arch/riscv/boot/dts/espressif/metalio-eink4-plus.dtb",
              (self.build / "images/metalio-eink4-plus.dtb").read_bytes())

    def package(self):
        return PACKAGER.package(self.root, self.build, self.output)

    def assert_no_output(self):
        self.assertFalse(self.output.exists())
        self.assertFalse(self.archive.exists())

    def test_package_hashes_manifest_and_zip(self):
        result = self.package()
        manifest = json.loads((self.output / "manifest.json").read_text())
        self.assertEqual(manifest["refresh"]["periodic_gc_completed_refreshes"], 120)
        self.assertEqual(manifest["refresh"]["supported_modes"], 7)
        self.assertFalse(manifest["legal_info"]["collected"])
        self.assertIn("confirmed", manifest["validation"]["baseline"])
        self.assertIn("not been flashed", manifest["validation"]["this_build"])
        self.assertNotIn(str(self.directory), json.dumps(manifest))
        self.assertEqual(result["archive_sha256"], hashlib.sha256(self.archive.read_bytes()).hexdigest())
        with zipfile.ZipFile(self.archive) as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual(archive.read(self.output.name + "/" + PACKAGER.IMAGE),
                             (self.build / "images" / PACKAGER.IMAGE).read_bytes())
            for line in (self.output / "SHA256SUMS").read_text().splitlines():
                expected, name = line.split("  ", 1)
                data = (self.output / name).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(), expected, name)
                self.assertEqual(archive.read(self.output.name + "/" + name), data)
        self.assertIn('PORT=/dev/ttyACM0', (self.output / "README.txt").read_text())
        for name in ("LICENSES/metalio_epd.h", "LICENSE", "sources.lock.json",
                     "apps/terminal-font.h", "apps/LICENSES/MIT.txt", "apps/LICENSES/Unifont.txt"):
            self.assertTrue((self.output / name).is_file(), name)

    def test_optional_legal_info_includes_notices_without_source_archives(self):
        for name in ("README", "manifest.csv", "host-manifest.csv", "licenses/musl/COPYRIGHT",
                     "host-licenses/compiler/COPYING", "sources/musl.tar.gz"):
            self.write(self.build / "legal-info" / name, "fixture legal information")
        self.write(self.build / "host/opt/ext-toolchain/share/licenses/musl/COPYRIGHT", "fixture musl notice")
        self.package()
        self.assertEqual((self.output / "LICENSES/third-party/toolchain/musl/COPYRIGHT").read_text(),
                         "fixture musl notice")
        manifest = json.loads((self.output / "manifest.json").read_text())
        self.assertTrue(manifest["legal_info"]["collected"])
        self.assertFalse(manifest["legal_info"]["source_archives_included"])
        legal = self.output / "LICENSES/buildroot-legal-info"
        for name in ("README", "manifest.csv", "host-manifest.csv", "licenses/musl/COPYRIGHT",
                     "host-licenses/compiler/COPYING"):
            self.assertTrue((legal / name).is_file(), name)
        self.assertFalse((legal / "sources").exists())

    def test_missing_image_creates_no_output(self):
        (self.build / "images" / PACKAGER.IMAGE).unlink()
        with self.assertRaises(FileNotFoundError):
            self.package()
        self.assert_no_output()

    def test_oversized_slot_creates_no_output(self):
        (self.build / "images/rootfs.cramfs").write_bytes(b"\0" * (0x400000 + 1))
        with self.assertRaisesRegex(ValueError, "exceeds flash slot"):
            self.package()
        self.assert_no_output()

    def test_mismatched_embedded_bytes_create_no_output(self):
        (self.build / "images/xipImage").write_bytes(b"wrong")
        with self.assertRaisesRegex(ValueError, "Embedded image mismatch: xipImage"):
            self.package()
        self.assert_no_output()

    def test_existing_directory_or_zip_is_preserved(self):
        self.output.mkdir(parents=True)
        sentinel = self.output / "keep"
        sentinel.write_text("keep")
        with self.assertRaisesRegex(ValueError, "Output already exists"):
            self.package()
        self.assertEqual(sentinel.read_text(), "keep")
        shutil.rmtree(self.output)
        self.archive.write_bytes(b"keep archive")
        with self.assertRaisesRegex(ValueError, "Output already exists"):
            self.package()
        self.assertEqual(self.archive.read_bytes(), b"keep archive")
        self.assertFalse(self.output.exists())

    def test_stale_application_and_driver_are_rejected(self):
        for relative in ("build/metalio-epd-1/terminal.c",
                         "build/linux-custom/drivers/misc/metalio/metalio-epd-regs.h"):
            with self.subTest(source=relative):
                path = self.build / relative
                original = path.read_bytes()
                path.write_text("stale")
                with self.assertRaisesRegex(ValueError, "Stale build source"):
                    self.package()
                self.assert_no_output()
                path.write_bytes(original)

    def test_incompatible_configuration_is_rejected(self):
        path = self.build / "build/linux-custom/.config"
        path.write_text(path.read_text().replace("CONFIG_METALIO_EPD_FAST_REFRESH=y", ""))
        with self.assertRaisesRegex(ValueError, "expected CONFIG_METALIO_EPD_FAST_REFRESH=y"):
            self.package()
        self.assert_no_output()

    def test_zip_creation_failure_removes_half_package(self):
        with mock.patch.object(PACKAGER.zipfile, "ZipFile", side_effect=OSError("ZIP creation failed")):
            with self.assertRaisesRegex(OSError, "ZIP creation failed"):
                self.package()
        self.assert_no_output()

    def test_zip_validation_failure_removes_half_package(self):
        with mock.patch.object(PACKAGER.zipfile.ZipFile, "testzip", return_value="damaged"):
            with self.assertRaisesRegex(ValueError, "ZIP integrity"):
                self.package()
        self.assert_no_output()


if __name__ == "__main__":
    unittest.main()
