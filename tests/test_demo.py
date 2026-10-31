import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


WIDTH, HEIGHT, STRIDE = 1216, 684, 152
HEADER = b"P4\n1216 684\n"


class DemoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        configured = os.environ.get("ESP32S31_INK_LINUX_TEST_DEMO")
        if not configured or not Path(configured).is_absolute():
            raise RuntimeError("Run make test, or set ESP32S31_INK_LINUX_TEST_DEMO to the absolute demo executable")
        cls.demo = configured
        cls.mock_directory = tempfile.TemporaryDirectory(prefix="epd-demo-ioctl-")
        cls.addClassCleanup(cls.mock_directory.cleanup)
        source = Path(cls.mock_directory.name) / "ioctl.c"
        cls.mock = source.with_suffix(".so")
        source.write_text(r'''
#include <errno.h>
#include <linux/metalio_epd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/ioctl.h>

int ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    void *data;
    (void)fd;
    va_start(args, request);
    data = va_arg(args, void *);
    va_end(args);
    if (request == METALIO_EPD_GET_INFO) {
        *(struct metalio_epd_info *)data = (struct metalio_epd_info){
            .abi_version = METALIO_EPD_ABI_VERSION,
            .width = METALIO_EPD_WIDTH, .height = METALIO_EPD_HEIGHT,
            .stride = METALIO_EPD_STRIDE, .frame_bytes = METALIO_EPD_FRAME_BYTES,
            .supported_modes = strtoul(getenv("ESP32S31_INK_LINUX_TEST_MODES"), NULL, 0),
        };
        return 0;
    }
    if (request == METALIO_EPD_GET_STATUS) {
        *(struct metalio_epd_status *)data = (struct metalio_epd_status){
            .sequence = 1, .elapsed_ms = 123,
        };
        return 0;
    }
    if (request == METALIO_EPD_REFRESH &&
        ((struct metalio_epd_refresh *)data)->mode ==
        strtoul(getenv("ESP32S31_INK_LINUX_TEST_REQUEST"), NULL, 0))
        return 0;
    errno = EINVAL;
    return -1;
}
''')
        command = shlex.split(os.environ.get("CC", "cc"))
        command += ["-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                    "-I" + str(Path(__file__).resolve().parents[1] / "include/uapi"),
                    str(source), "-o", str(cls.mock)]
        subprocess.run(command, check=True, capture_output=True, timeout=30)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="epd-demo-")
        self.directory = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)

    def invoke(self, *arguments):
        return subprocess.run([self.demo, *arguments], capture_output=True, timeout=5)

    def image(self, pattern, *arguments):
        destination = self.directory / "preview.pbm"
        result = self.invoke(pattern, "--output", str(destination), *arguments)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        image = destination.read_bytes()
        self.assertEqual(image[:len(HEADER)], HEADER)
        data = image[len(HEADER):]
        self.assertEqual(len(data), STRIDE * HEIGHT)
        return data

    def test_white_and_black_all_pixels(self):
        self.assertEqual(self.image("white"), bytes(STRIDE * HEIGHT))
        self.assertEqual(self.image("black"), b"\xff" * (STRIDE * HEIGHT))

    def test_checker_all_pixels(self):
        expected = bytes(255 if (byte // 4 + y // 32) % 2 else 0
                         for y in range(HEIGHT) for byte in range(STRIDE))
        self.assertEqual(self.image("checker"), expected)

    def test_default_text_is_nonempty_and_has_white_background(self):
        image = self.image("text")
        self.assertTrue(any(image))
        self.assertLess(sum(value.bit_count() for value in image), WIDTH * HEIGHT)
        self.assertEqual(image[:48 * STRIDE], bytes(48 * STRIDE))

    def test_text_lowercase_and_unsupported_punctuation(self):
        self.assertEqual(self.image("text", "--text", "Linux 123"),
                         self.image("text", "--text", "LINUX 123"))
        self.assertEqual(self.image("text", "--text", "#"),
                         self.image("text", "--text", "?"))

    def test_stdout_pbm(self):
        result = self.invoke("white", "--output", "-")
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.assertEqual(result.stdout, HEADER + bytes(STRIDE * HEIGHT))

    def test_mode_arguments_preserve_preview(self):
        for mode in ("gc", "du", "feedback"):
            with self.subTest(mode=mode):
                self.assertEqual(self.image("white", "--mode", mode),
                                 bytes(STRIDE * HEIGHT))

    def test_modes_and_old_kernel_capabilities(self):
        for supported, mode, request, succeeds in (
                (7, "du", 0, True), (7, "gc", 1, True),
                (7, "feedback", 2, True),
                (0, "du", 0, True), (0, "gc", 1, True),
                (0, "feedback", 2, False), (3, "feedback", 2, False)):
            with self.subTest(supported=supported, mode=mode):
                device = self.directory / "fake-device"
                device.write_bytes(b"")
                environment = dict(os.environ, LD_PRELOAD=str(self.mock),
                                   ESP32S31_INK_LINUX_TEST_MODES=str(supported),
                                   ESP32S31_INK_LINUX_TEST_REQUEST=str(request))
                result = subprocess.run([self.demo, "checker", "--mode", mode,
                                         "--device", str(device)], env=environment,
                                        capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 0 if succeeds else 1,
                                 result.stderr.decode(errors="replace"))
                if succeeds:
                    self.assertEqual(device.stat().st_size, STRIDE * HEIGHT)
                    self.assertIn(b"Refresh completed", result.stdout)
                else:
                    self.assertEqual(device.stat().st_size, 0)
                    self.assertIn(b"not supported by this kernel", result.stderr)
                    self.assertNotIn(b"Refresh completed", result.stdout)

    def test_invalid_arguments_are_rejected(self):
        destination = self.directory / "invalid.pbm"
        cases = [(), ("unknown",), ("white", "--mode", "invalid"),
                 ("white", "--output"), ("white", "--unknown", "value"),
                 ("white", "--text", "text"),
                 ("white", "--device", str(self.directory / "missing-device"),
                  "--output", str(destination))]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                result = self.invoke(*arguments)
                self.assertEqual(result.returncode, 2)
                self.assertFalse(destination.exists())

    def test_non_ascii_control_and_overflow_text_are_rejected(self):
        destination = self.directory / "invalid.pbm"
        for text in ("\x01", "中文", "A" * 6000):
            with self.subTest(text_length=len(text)):
                result = self.invoke("text", "--text", text, "--output", str(destination))
                self.assertEqual(result.returncode, 2)
                self.assertFalse(destination.exists())

    def test_missing_device_fails_without_hardware_success(self):
        device = str(self.directory / "missing-device")
        result = self.invoke("checker", "--device", device)
        self.assertEqual(result.returncode, 1)
        self.assertIn(device.encode(), result.stderr)
        self.assertNotIn(b"Refresh completed", result.stdout)


if __name__ == "__main__":
    unittest.main()
