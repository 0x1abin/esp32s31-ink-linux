import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class BuildTests(unittest.TestCase):
    def test_local_nor_configuration_preserves_cramfs_and_usb_serial(self):
        config = (ROOT / "board/metalio/eink4-plus/linux.config").read_text()
        for disabled in ("BLOCK", "USB", "USB_GADGET", "PHY_ESP32S31_USB", "MMC"):
            self.assertIn("# CONFIG_" + disabled + " is not set", config)
        for enabled in ("MTD", "MTD_ROM", "MTD_PHYSMAP", "MTD_PHYSMAP_OF", "CRAMFS", "CRAMFS_MTD",
                        "SERIAL_ESP32_ACM", "INPUT_EVDEV", "UNIX98_PTYS", "METALIO_EPD",
                        "NET", "INET", "UNIX"):
            self.assertIn("CONFIG_" + enabled + "=y", config)

    def test_ssh_host_keys_have_private_writable_storage(self):
        overlay = ROOT / "board/metalio/eink4-plus/rootfs_overlay"
        self.assertEqual(os.readlink(overlay / "root/.ssh"), "/run/ssh")
        self.assertEqual(os.readlink(overlay / ".ssh"), "/run/ssh")
        inittab = (overlay / "etc/inittab").read_text().splitlines()
        mount = inittab.index("::sysinit:/bin/mount -a")
        private = inittab.index("::sysinit:/bin/mkdir -m 0700 -p /run/ssh")
        shell = inittab.index("::once:/usr/bin/metalio-terminal")
        self.assertLess(mount, private)
        self.assertLess(private, shell)

    def test_build_does_not_forward_host_linker_flags(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "scripts").mkdir()
            (directory / "bin").mkdir()
            script = directory / "scripts/build.sh"
            shutil.copy2(ROOT / "scripts/build.sh", script)
            (directory / "scripts/fetch-sources.py").write_text(
                "import sys\nassert sys.argv[1:] == ['--verify']\n")
            make = directory / "bin/make"
            make.write_text('''#!/usr/bin/env python3
import os, pathlib, sys
assert 'LDFLAGS' not in os.environ
build = pathlib.Path(next(a[2:] for a in sys.argv if a.startswith('O=')))
if sys.argv[-1].endswith('_defconfig'):
    (build / '.config').write_text('BR2_ESP32S31_INK_LINUX_PLATFORM_SUPPORT=y\\n')
with open(os.environ['CHECK_LOG'], 'a') as stream:
    stream.write('make\\n')
''')
            make.chmod(0o755)
            log = directory / "make.log"
            env = dict(os.environ, LDFLAGS="-fuse-ld=mold", CHECK_LOG=str(log),
                       PATH=str(directory / "bin") + os.pathsep + os.environ["PATH"],
                       ESP32S31_INK_LINUX_BUILD_DIR=str(directory / "output"))
            result = subprocess.run([str(script)], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(log.read_text().splitlines(), ["make", "make"])

    def test_source_pins_match_build_configuration(self):
        lock = json.loads((ROOT / "sources.lock.json").read_text())
        config = (ROOT / "configs/metalio_eink4_plus_defconfig").read_text()
        bsp = (ROOT / "package/esp-linux-bsp/esp-linux-bsp.mk").read_text()
        for name, source in lock["sources"].items():
            self.assertRegex(source["commit"], r"^[0-9a-f]{40}$")
            if name in ("linux", "u-boot", "opensbi"):
                self.assertIn('"' + source["commit"] + '"', config)
            elif name == "esp-linux-bsp":
                self.assertIn("ESP_LINUX_BSP_VERSION = " + source["commit"], bsp)
        tool = lock["tools"]["riscv-toolchain-x86_64"]
        hashes = (ROOT / "patches/toolchain-external-custom/toolchain-external-custom.hash").read_text()
        self.assertRegex(tool["sha256"], r"^[0-9a-f]{64}$")
        self.assertIn(tool["sha256"] + "  " + tool["url"].rsplit("/", 1)[1], hashes)

    def test_source_fetch_preserves_local_changes_and_requires_pins(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary)
            (fixture / "scripts").mkdir()
            script = fixture / "scripts/fetch-sources.py"
            shutil.copy2(ROOT / "scripts/fetch-sources.py", script)
            remote = fixture / "remote"
            subprocess.run(["git", "init", "-q", str(remote)], check=True)
            (remote / "README").write_text("fixture\n")
            subprocess.run(["git", "-C", str(remote), "add", "README"], check=True)
            subprocess.run(["git", "-C", str(remote), "-c", "user.name=Selfcheck",
                            "-c", "user.email=selfcheck@example.invalid", "commit", "-qm", "fixture"], check=True)
            commit = subprocess.check_output(["git", "-C", str(remote), "rev-parse", "HEAD"], text=True).strip()
            lock = {"sources": {"demo": {"url": remote.as_uri(), "commit": commit}}}
            lock_path = fixture / "sources.lock.json"
            lock_path.write_text(json.dumps(lock))

            def invoke(*args):
                return subprocess.run(["python3", str(script), *args], capture_output=True, text=True)

            self.assertIn("Missing source", invoke("--verify").stderr)
            result = invoke()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(invoke("--verify").returncode, 0)
            checkout = fixture / "upstream/demo"
            (checkout / "README").write_text("local modification\n")
            self.assertIn("local changes", invoke().stderr)
            self.assertEqual((checkout / "README").read_text(), "local modification\n")
            subprocess.run(["git", "-C", str(checkout), "restore", "README"], check=True)
            lock["sources"]["demo"]["commit"] = "0" * 40
            lock_path.write_text(json.dumps(lock))
            self.assertIn("expected", invoke("--verify").stderr)
            lock["sources"]["demo"]["commit"] = "main"
            lock_path.write_text(json.dumps(lock))
            self.assertIn("not pinned", invoke("--verify").stderr)

    def test_rootfs_flash_slot_boundary_and_missing_image(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary)
            (fixture / "host/bin").mkdir(parents=True)
            (fixture / "images").mkdir()
            (fixture / "target").mkdir()
            mkcramfs = fixture / "host/bin/mkcramfs"
            mkcramfs.write_text('#!/bin/sh\nfor output do :; done\ntruncate -s "$TEST_SIZE" "$output"\n')
            mkcramfs.chmod(0o755)
            env = dict(os.environ, HOST_DIR=str(fixture / "host"), BINARIES_DIR=str(fixture / "images"))
            for size, expected in ((4194304, 0), (4194305, 1)):
                result = subprocess.run([str(ROOT / "board/metalio/eink4-plus/rootfs-cramfs.sh"), str(fixture / "target")],
                                        env=dict(env, TEST_SIZE=str(size)), capture_output=True, text=True)
                self.assertEqual(result.returncode, expected, result.stderr)
            (fixture / "images/rootfs.cramfs").unlink()
            result = subprocess.run([str(ROOT / "board/metalio/eink4-plus/post-image.sh"), str(fixture / "images")],
                                    env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Missing rootfs", result.stderr)

    def test_usb_console_alias_and_getty_agree(self):
        dts = (ROOT / "board/metalio/eink4-plus/dts/espressif/metalio-eink4-plus.dts").read_text()
        alias = re.search(r"serial(\d+)\s*=\s*&usb_jtag", dts)
        self.assertIsNotNone(alias)
        device = "ttyGS" + alias.group(1)
        self.assertIn("console=" + device, dts)
        inittab = (ROOT / "board/metalio/eink4-plus/rootfs_overlay/etc/inittab").read_text()
        self.assertIn("115200 " + device + " vt100", inittab)
        kernel_config = (ROOT / "board/metalio/eink4-plus/linux.config").read_text()
        self.assertIn("CONFIG_SERIAL_ESP32_ACM=y", kernel_config)

    def test_io_expander_has_supported_pca9555_fallback(self):
        dts = (ROOT / "board/metalio/eink4-plus/dts/espressif/metalio-eink4-plus.dts").read_text()
        node = re.search(r"ioexp:\s*gpio@20\s*\{\s*compatible\s*=\s*([^;]+);", dts)
        self.assertIsNotNone(node)
        compatibles = re.findall(r'"([^\"]+)"', node.group(1))
        self.assertIn("nxp,pca9555", compatibles,
                      "TCA9555 needs the fallback supported by the pinned GPIO_PCA953X driver")

    def test_metalio_touch_binding_uses_board_protocol_and_shared_i2c_rate(self):
        dts = (ROOT / "board/metalio/eink4-plus/dts/espressif/metalio-eink4-plus.dts").read_text()
        node = re.search(r"touchscreen@38\s*\{([^}]+)\}", dts, re.S)
        self.assertIsNotNone(node)
        self.assertIn('compatible = "metalio,ft6336u";', node.group(1))
        self.assertIn("reg = <0x38>;", node.group(1))
        self.assertIn("interrupts = <5 IRQ_TYPE_EDGE_BOTH>;", node.group(1))
        self.assertIn("reset-gpios = <&ioexp 10 GPIO_ACTIVE_LOW>;", node.group(1))
        bus = re.search(r"&i2c0\s*\{(.*?)touchscreen@38", dts, re.S)
        self.assertIsNotNone(bus)
        self.assertIn("clock-frequency = <100000>;", bus.group(1))
        config = (ROOT / "board/metalio/eink4-plus/linux.config").read_text()
        self.assertIn("CONFIG_TOUCHSCREEN_METALIO_FT6336U=y", config)
        self.assertNotIn("CONFIG_TOUCHSCREEN_EDT_FT5X06=y", config)

    def test_local_kernel_patching_is_repeatable_and_rejects_drift(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "patches/linux").mkdir(parents=True)
            kernel = directory / "kernel"
            kernel.mkdir()
            source = kernel / "target"
            source.write_text("old\n")
            script = directory / "patch-local-kernel.sh"
            shutil.copy2(ROOT / "platform/esp32s31/patch-local-kernel.sh", script)
            (directory / "patches/linux/0001-test.patch").write_text(
                "--- a/target\n+++ b/target\n@@ -1 +1 @@\n-old\n+new\n")
            for _ in range(2):
                result = subprocess.run([str(script), str(kernel)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(source.read_text(), "new\n")
            source.write_text("unrelated change\n")
            result = subprocess.run([str(script), str(kernel)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(source.read_text(), "unrelated change\n")

    def test_usb_console_full_fifo_times_out_without_jiffies(self):
        patch = (ROOT / "platform/esp32s31/patches/linux/0002-serial-esp32-acm-bound-console-poll.patch").read_text()
        replacement = "\n".join(line[1:] for line in patch.splitlines()
                                if line.startswith((" ", "+")) and not line.startswith("+++"))
        function = re.search(r"static void esp32s3_acm_put_char_sync\([^\n]+\)\n\{.*?\n\}", replacement, re.S)
        self.assertIsNotNone(function)
        # The fake helper supplies the kernel atomic poll's bounded success/timeout contract.
        # No jiffies or IRQ tick exists in this fixture.
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
struct uart_port { unsigned int polls, ready_after, writes, flushes; u8 value; };
static u32 esp32s3_acm_tx_fifo_free(struct uart_port *port)
{ return ++port->polls >= port->ready_after; }
static void esp32s3_acm_put_char(struct uart_port *port, u8 value)
{ ++port->writes; port->value = value; }
static void esp32s3_acm_push(struct uart_port *port) { ++port->flushes; }
#define read_poll_timeout_atomic(op, val, cond, delay, timeout, before, port) \
({ unsigned int budget = (timeout) / (delay); int result = -1; \
   assert((delay) > 0 && (timeout) > 0 && !(before)); \
   while (budget--) { val = op(port); if (cond) { result = 0; break; } } result; })
''' + function.group(0) + r'''
int main(void)
{
    struct uart_port ready = { .ready_after = 3 };
    struct uart_port stuck = { .ready_after = UINT32_MAX };
    esp32s3_acm_put_char_sync(&ready, 'X');
    assert(ready.polls == 3 && ready.writes == 1 && ready.flushes == 1 && ready.value == 'X');
    esp32s3_acm_put_char_sync(&stuck, 'Y');
    assert(stuck.polls > 0 && stuck.polls <= 1000 && !stuck.writes && !stuck.flushes);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            fixture = directory / "console.c"
            fixture.write_text(source)
            executable = directory / "console"
            compiler = shlex.split(os.environ.get("CC", "cc"))
            subprocess.run(compiler + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", str(fixture),
                                       "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=2)


if __name__ == "__main__":
    unittest.main()
