import hashlib
import json
import os
from pathlib import Path
import pty
import re
import select
import shutil
import signal
import stat
import subprocess
import tempfile
import termios
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parent.parent
OVERLAY = ROOT / "board/metalio/eink4-plus/wifi/rootfs_overlay"


class WifiTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.data = self.directory / "data"
        self.wifi = self.data / "wifi"
        self.wifi.mkdir(parents=True, mode=0o700)
        (self.data / ".metalio-data-v1").write_text("metalio-data-v1\n")
        self.config = self.wifi / "wpa_supplicant.conf"
        self.config.write_text("previous configuration\n")
        self.config.chmod(0o600)
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        self.mounts = self.directory / "mounts"
        self.mounts.write_text(f"mtd:data {self.data} jffs2 rw,nosuid,nodev,noexec 0 0\n")
        self.log = self.directory / "commands.jsonl"
        self.environment = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                                WIFI_TEST_LOG=str(self.log), WIFI_TEST_MOUNTS=str(self.mounts),
                                WIFI_TEST_DATA=str(self.data))
        for name in ("wifi-start", "wifi-stop", "wifi-config"):
            source = (OVERLAY / "usr/sbin" / name).read_text()
            source = source.replace("/data", str(self.data)).replace("/proc/mounts", str(self.mounts))
            self.write_command(name, source)
        self.write_command("id", "#!/bin/sh\necho 0\n")
        self.write_command("stat", """#!/usr/bin/env python3
import os, sys
mode = format(os.stat(sys.argv[-1]).st_mode & 0o7777, 'o')
print({'%u': '0', '%u:%a': '0:' + mode}[sys.argv[2]])
""")
        common = """#!/usr/bin/env python3
import hashlib, json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
with open(os.environ['WIFI_TEST_LOG'], 'a') as log:
    log.write(json.dumps([name] + sys.argv[1:]) + '\\n')
"""
        self.write_command("fsync", common + """
if os.environ.get('WIFI_TEST_FSYNC_FAIL') == ('directory' if pathlib.Path(sys.argv[1]).is_dir() else 'file'):
    sys.exit(1)
fd = os.open(sys.argv[1], os.O_RDONLY)
os.fsync(fd)
os.close(fd)
""")
        self.write_command("mv", common + "os.replace(sys.argv[-2], sys.argv[-1])\n")
        self.write_command("wpa_passphrase", common + """
assert len(sys.argv) == 2, 'password must not appear in argv'
password = sys.stdin.buffer.readline().rstrip(b'\\n')
if os.environ.get('WIFI_TEST_DERIVE_FAIL'):
    print(password.decode(), file=sys.stderr)
    print('unexpected output')
    sys.exit(1)
print('network={')
print('    #psk="' + password.decode() + '"')
print('    psk=' + hashlib.pbkdf2_hmac('sha1', password, sys.argv[1].encode(), 4096, 32).hex())
print('}')
""")
        self.write_command("ifup", common + """
if os.environ.get('WIFI_TEST_IFUP_FAIL'):
    print('sensitive parser error', file=sys.stderr)
    sys.exit(1)
""")
        self.write_command("ifdown", common + "if os.environ.get('WIFI_TEST_IFDOWN_FAIL'): sys.exit(1)\n")
        self.write_command("wpa_cli", common + """
if os.environ.get('WIFI_TEST_PING_FAIL'):
    print('sensitive parser error', file=sys.stderr)
    sys.exit(1)
print('PONG')
""")
        self.write_command("mountpoint", """#!/usr/bin/env python3
import os, pathlib, sys
text = pathlib.Path(os.environ['WIFI_TEST_MOUNTS']).read_text()
sys.exit(0 if ' ' + sys.argv[-1] + ' ' in text else 1)
""")
        self.write_command("mount", common + """
if os.environ.get('WIFI_TEST_MOUNT_FAIL'):
    sys.exit(1)
flags = 'rw' if 'remount' in sys.argv[2] else 'ro'
pathlib.Path(os.environ['WIFI_TEST_MOUNTS']).write_text(
    'mtd:data ' + os.environ['WIFI_TEST_DATA'] + ' jffs2 ' + flags + ',nosuid,nodev,noexec 0 0\\n')
""")

    def write_command(self, name, source):
        path = self.bin / name
        path.write_text(source)
        path.chmod(0o755)

    def commands(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def invoke(self, name, **environment):
        return subprocess.run([str(self.bin / name)], env=dict(self.environment, **environment),
                              stdin=subprocess.DEVNULL, capture_output=True, text=True, start_new_session=True)

    def configure(self, ssid, password, shell=None, **environment):
        child, terminal = pty.fork()
        if child == 0:
            executable = shell or str(self.bin / "wifi-config")
            arguments = [shell, "sh", str(self.bin / "wifi-config")] if shell else ["wifi-config"]
            os.execve(executable, arguments, dict(self.environment, **environment))
        output = bytearray()
        completed = False
        deadline = time.monotonic() + 10
        inputs = [(b"SSID: ", ssid.encode() + b"\n"),
                  (b"WPA2 password (8-63 bytes, or 64 hex digits): ",
                   password.encode() + b"\n" if password is not None else None)]
        try:
            while time.monotonic() < deadline:
                if select.select([terminal], [], [], 0.1)[0]:
                    try:
                        chunk = os.read(terminal, 4096)
                    except OSError:
                        break
                    if not chunk:
                        break
                    output.extend(chunk)
                    if inputs and inputs[0][0] in output:
                        _, response = inputs.pop(0)
                        if response is None:
                            os.kill(child, signal.SIGTERM)
                        else:
                            os.write(terminal, response)
                waited, status = os.waitpid(child, os.WNOHANG)
                if waited:
                    completed = True
                    break
            # PTY EOF can arrive just before waitpid observes the child's exit.
            while not completed:
                waited, status = os.waitpid(child, os.WNOHANG)
                if waited:
                    completed = True
                    break
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                time.sleep(min(0.01, remaining))
            if not completed:
                os.kill(child, signal.SIGKILL)
                os.waitpid(child, 0)
                self.fail("wifi-config did not finish; output: " + output.decode(errors="replace"))
            self.assertTrue(termios.tcgetattr(terminal)[3] & termios.ECHO, "terminal echo was not restored")
            return os.waitstatus_to_exitcode(status), output.decode(errors="replace")
        finally:
            os.close(terminal)

    def test_pty_eof_waits_for_child_exit_within_original_deadline(self):
        with mock.patch.object(pty, "fork", return_value=(12345, 456)), \
             mock.patch.object(select, "select", return_value=([456], [], [])), \
             mock.patch.object(os, "read", side_effect=OSError("PTY closed")), \
             mock.patch.object(os, "close"), \
             mock.patch.object(os, "waitpid", side_effect=[(0, 0), (0, 0), (12345, 0)]) as wait, \
             mock.patch.object(os, "kill") as kill, \
             mock.patch.object(termios, "tcgetattr", return_value=[0, 0, 0, termios.ECHO]):
            status, output = self.configure("test", "private-password")
        self.assertEqual((status, output), (0, ""))
        self.assertEqual(wait.call_count, 3)
        kill.assert_not_called()

    def test_interactive_password_is_hidden_and_configuration_is_atomic(self):
        ssid = 'Office "quoted" \\ WiFi'
        password = "test-private-password"
        status, output = self.configure(ssid, password)
        self.assertEqual(status, 0, output)
        self.assertNotIn(password, output)
        data = self.config.read_text()
        self.assertIn("ssid=" + ssid.encode().hex(), data)
        expected = hashlib.pbkdf2_hmac("sha1", password.encode(), ssid.encode(), 4096, 32).hex()
        self.assertIn("psk=" + expected, data)
        self.assertNotIn(password, data)
        self.assertNotIn("#psk", data)
        self.assertEqual(stat.S_IMODE(self.config.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(self.wifi.stat().st_mode), 0o700)
        names = [command[0] for command in self.commands()]
        self.assertEqual(names, ["wpa_passphrase", "fsync", "mv", "fsync", "ifdown", "mount", "ifup", "wpa_cli"])
        self.assertFalse(list(self.wifi.glob(".wpa_supplicant.conf.*")))

    def test_raw_psk_and_maximum_ssid(self):
        status, output = self.configure("s" * 32, "aB" * 32)
        self.assertEqual(status, 0, output)
        self.assertIn("psk=" + "aB" * 32, self.config.read_text())
        self.assertNotIn("wpa_passphrase", [command[0] for command in self.commands()])

    @unittest.skipUnless(shutil.which("wpa_passphrase"), "host wpa_passphrase is unavailable")
    def test_real_passphrase_tool_derives_the_expected_key(self):
        (self.bin / "wpa_passphrase").unlink()
        (self.bin / "wpa_passphrase").symlink_to(shutil.which("wpa_passphrase"))
        status, output = self.configure('Office "quote"', "private-password")
        self.assertEqual(status, 0, output)
        expected = hashlib.pbkdf2_hmac("sha1", b"private-password", b'Office "quote"', 4096, 32).hex()
        self.assertIn("psk=" + expected, self.config.read_text())
        self.assertNotIn("private-password", output + self.config.read_text())

    def test_interrupted_password_input_restores_echo_and_keeps_old_file(self):
        status, output = self.configure("test", None)
        self.assertNotEqual(status, 0)
        self.assertEqual(self.config.read_text(), "previous configuration\n")

    @unittest.skipUnless(shutil.which("busybox"), "native BusyBox is unavailable")
    def test_busybox_shell_runs_the_interactive_configuration(self):
        # Some host BusyBoxes prefer built-in applets over PATH test doubles.
        script = self.bin / "wifi-config"
        source = script.read_text()
        for command in ("id", "stat", "fsync", "mv"):
            source = re.sub(r"\b" + command + r"(?= )", str(self.bin / command), source)
        script.write_text(source)
        status, output = self.configure("BusyBox", "private-password", shell=shutil.which("busybox"))
        self.assertEqual(status, 0, output)
        self.assertNotIn("private-password", output)

    def test_oversized_ssid_keeps_previous_file(self):
        status, output = self.configure("s" * 33, "private-password")
        self.assertNotEqual(status, 0)
        self.assertIn("SSID must contain 1 to 32 bytes", output)
        self.assertEqual(self.config.read_text(), "previous configuration\n")

    def test_invalid_password_and_failed_derivation_keep_previous_file(self):
        for password, environment in (("short", {}), ("z" * 64, {}),
                                      ("private-password", {"WIFI_TEST_DERIVE_FAIL": "1"})):
            with self.subTest(password_length=len(password), environment=environment):
                status, output = self.configure("test", password, **environment)
                self.assertNotEqual(status, 0)
                self.assertNotIn(password, output)
                self.assertEqual(self.config.read_text(), "previous configuration\n")
                self.assertFalse(list(self.wifi.glob(".wpa_supplicant.conf.*")))

    def test_file_fsync_failure_preserves_old_file(self):
        status, output = self.configure("test", "private-password", WIFI_TEST_FSYNC_FAIL="file")
        self.assertNotEqual(status, 0)
        self.assertEqual(self.config.read_text(), "previous configuration\n")
        self.assertFalse(list(self.wifi.glob(".wpa_supplicant.conf.*")))
        self.assertNotIn("ifup", [command[0] for command in self.commands()])

    def test_directory_fsync_failure_leaves_a_complete_new_file(self):
        status, output = self.configure("test", "private-password", WIFI_TEST_FSYNC_FAIL="directory")
        self.assertNotEqual(status, 0)
        self.assertIn("persistence could not be confirmed", output)
        self.assertTrue(self.config.read_text().endswith("\n}\n"))
        self.assertNotIn("ifup", [command[0] for command in self.commands()])

    def test_configuration_requires_tty_and_initialized_rw_data(self):
        result = self.invoke("wifi-config")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("interactive local terminal", result.stderr)
        self.mounts.write_text(f"mtd:data {self.data} jffs2 ro 0 0\n")
        result = self.invoke("wifi-config")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("writable data partition unavailable", result.stderr)

    def test_start_mounts_readonly_before_validating_and_enabling_writes(self):
        self.mounts.write_text("")
        result = self.invoke("wifi-start")
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = self.commands()
        self.assertEqual([command[0] for command in commands], ["mount", "mount", "ifup", "wpa_cli"])
        self.assertIn("ro,nosuid,nodev,noexec", commands[0])
        self.assertIn("remount,rw,nosuid,nodev,noexec", commands[1])

    def test_uninitialized_or_unavailable_data_never_starts_or_formats(self):
        self.mounts.write_text("")
        (self.data / ".metalio-data-v1").unlink()
        result = self.invoke("wifi-start")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual([command[0] for command in self.commands()], ["mount"])
        self.assertIn(" jffs2 ro,", self.mounts.read_text())
        self.mounts.write_text("")
        result = self.invoke("wifi-start", WIFI_TEST_MOUNT_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("ifup", [command[0] for command in self.commands()])

    def test_other_filesystem_and_symlinked_configuration_are_rejected(self):
        self.mounts.write_text(f"none {self.data} tmpfs rw 0 0\n")
        result = self.invoke("wifi-start")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.commands(), [])
        self.mounts.write_text(f"mtd:data {self.data} jffs2 rw 0 0\n")
        self.config.unlink()
        other = self.data / "other.conf"
        other.write_text("private\n")
        other.chmod(0o600)
        self.config.symlink_to(other)
        result = self.invoke("wifi-start")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("ifup", [command[0] for command in self.commands()])

    def test_missing_config_skips_join_and_insecure_config_is_rejected(self):
        self.config.unlink()
        result = self.invoke("wifi-start")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("ifup", [command[0] for command in self.commands()])
        self.config.write_text("private\n")
        self.config.chmod(0o644)
        result = self.invoke("wifi-start")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("ifup", [command[0] for command in self.commands()])

    def test_supplicant_errors_do_not_reach_consoles(self):
        result = self.invoke("wifi-start", WIFI_TEST_IFUP_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("sensitive parser error", result.stdout + result.stderr)
        self.assertIn("network start failed", result.stderr)

    def test_stop_is_safe_after_a_failed_start(self):
        result = self.invoke("wifi-stop", WIFI_TEST_IFDOWN_FAIL="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.commands(), [["ifdown", "-f", "wlan0"]])

    def test_supplicant_failure_hidden_by_ifup_is_detected_and_cleaned_up(self):
        result = self.invoke("wifi-start", WIFI_TEST_PING_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("supplicant start failed", result.stderr)
        self.assertNotIn("sensitive parser error", result.stdout + result.stderr)
        self.assertEqual(self.commands()[-1], ["ifdown", "-f", "wlan0"])

    def test_overlay_never_joins_default_open_network_or_blocks_terminal(self):
        fragment = (ROOT / "board/metalio/eink4-plus/wifi/busybox.config").read_text().splitlines()
        for setting in ("CONFIG_FSYNC=y", "CONFIG_STAT=y", "CONFIG_FEATURE_STAT_FORMAT=y"):
            self.assertIn(setting, fragment)
        default = (OVERLAY / "etc/wpa_supplicant.conf").read_text()
        self.assertNotIn("network={", default)
        self.assertNotIn("key_mgmt=NONE", default)
        interfaces = (OVERLAY / "etc/network/interfaces").read_text()
        self.assertNotIn("auto wlan0", interfaces)
        inittab = (OVERLAY / "etc/inittab").read_text()
        self.assertIn("::once:/usr/sbin/wifi-start", inittab)
        self.assertIn("::once:/usr/bin/metalio-terminal", inittab)
        self.assertLess(inittab.index("::shutdown:/usr/sbin/wifi-stop"),
                        inittab.index("::shutdown:/bin/umount -a -r"))


class TargetPassphraseTests(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("ESP32S31_INK_LINUX_TEST_TARGET_ROOT"), "optional target rootfs smoke check")
    def test_target_password_stdin_pipe_uses_known_wpa2_vector(self):
        target = Path(os.environ["ESP32S31_INK_LINUX_TEST_TARGET_ROOT"]).resolve()
        emulator = os.environ.get("ESP32S31_INK_LINUX_TEST_QEMU", shutil.which("qemu-riscv32"))
        self.assertTrue(emulator, "set ESP32S31_INK_LINUX_TEST_QEMU to the RISC-V 32-bit emulator")
        result = subprocess.run([emulator, "-L", str(target), str(target / "usr/sbin/wpa_passphrase"), "IEEE"],
                                input="password\n", capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("psk=f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", result.stdout)


if __name__ == "__main__":
    unittest.main()
