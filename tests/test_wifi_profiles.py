import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class WifiProfileTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "scripts").mkdir()
        self.tools = self.root / "tools"
        self.tools.mkdir()
        self.env = dict(os.environ, PATH=str(self.tools) + os.pathsep + os.environ["PATH"],
                        ESP32S31_INK_LINUX_IDF_TOOLS_PATH=str(self.root / "idf-tools"))
        self.env.pop("ESP32S31_INK_LINUX_WIFI_BUILD_DIR", None)

    def executable(self, path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(data)
        path.chmod(0o755)

    def run_script(self, name, *args, **env):
        return subprocess.run([str(self.root / "scripts" / name), *map(str, args)],
                              env=dict(self.env, **env), stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True)

    def test_wifi_build_passes_arguments_and_rejects_old_experiment(self):
        shutil.copy2(ROOT / "scripts/build-wifi.sh", self.root / "scripts/build-wifi.sh")
        for name in ("fetch-sources.py", "prepare-wifi-sources.py"):
            (self.root / "scripts" / name).write_text("pass\n")
        (self.root / "upstream/buildroot").mkdir(parents=True)
        self.executable(self.tools / "make", """#!/usr/bin/env python3
import json, pathlib, sys, os
build = pathlib.Path(next(a[2:] for a in sys.argv if a.startswith('O=')))
if sys.argv[-1].endswith('_defconfig'):
    assert sys.argv[-1] == 'metalio_eink4_plus_wifi_defconfig'
    (build / '.config').write_text('BR2_ESP32S31_INK_LINUX_PLATFORM_SUPPORT=y\\nBR2_PACKAGE_ESP32S31_LINUX_COMPANION=y\\n')
else:
    (build / 'make-args.json').write_text(json.dumps(sys.argv[1:]))
    assert os.environ['KBUILD_BUILD_USER'] == 'esp32s31-ink-linux'
    assert os.environ['KBUILD_BUILD_HOST'] == 'builder'
""")
        result = self.run_script("build-wifi.sh", "-j7", "linux-rebuild")
        self.assertEqual(result.returncode, 0, result.stderr)
        build = self.root / "outputs/buildroot-eink4-plus-wifi"
        self.assertEqual(json.loads((build / "make-args.json").read_text())[-2:],
                         ["-j7", "linux-rebuild"])
        (build / '.config').write_text('BR2_ESP32S31_INK_LINUX_PLATFORM_SUPPORT=y\nBR2_PACKAGE_ESP32S31_LINUX_COMPANION=y\nBR2_PACKAGE_METALIO_CPU0_IO=y\n')
        result = self.run_script("build-wifi.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Stale experimental", result.stderr)
        (build / '.config').write_text('BR2_PACKAGE_METALIO_WIFI=y\n')
        result = self.run_script("build-wifi.sh")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Stale build configuration", result.stderr)

    def test_firmware_uses_only_native_defaults_and_rejects_stale_config(self):
        shutil.copy2(ROOT / "scripts/build-wifi-firmware.sh", self.root / "scripts/build-wifi-firmware.sh")
        project, build, board = self.root / "project", self.root / "build", self.root / "board-config"
        project.mkdir(); build.mkdir(); board.mkdir()
        (board / "sdkconfig.defaults").write_text("base")
        (board / "partitions.csv").write_text("board partitions")
        python = self.tools / "idf-python"
        self.executable(python, """#!/usr/bin/env python3
import json, pathlib, sys
pathlib.Path(__file__).with_name('idf-args.json').write_text(json.dumps(sys.argv[1:]))
""")
        self.executable(self.tools / "riscv32-esp-elf-gcc", "#!/bin/sh\nexit 0\n")
        env = dict(ESP32S31_INK_LINUX_IDF_PYTHON=str(python), ESP32S31_INK_LINUX_IDF_TOOLCHAIN_BIN=str(self.tools))
        result = self.run_script("build-wifi-firmware.sh", project, build, board, **env)
        self.assertEqual(result.returncode, 0, result.stderr)
        args = json.loads((self.tools / "idf-args.json").read_text())
        self.assertIn("SDKCONFIG_DEFAULTS=" + str(project / "sdkconfig.defaults"), args)
        self.assertIn("SDKCONFIG=" + str(build / "sdkconfig"), args)
        self.assertEqual((project / "partitions.csv").read_text(), "board partitions")
        self.assertEqual((project / "sdkconfig.defaults").read_text(), "base")
        (build / "sdkconfig").write_text("CONFIG_METALIO_CPU0_IO=y\n")
        result = self.run_script("build-wifi-firmware.sh", project, build, board, **env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Stale experimental", result.stderr)

    def test_native_build_rejects_old_and_wifi_configurations(self):
        shutil.copy2(ROOT / "scripts/build.sh", self.root / "scripts/build.sh")
        (self.root / "scripts/fetch-sources.py").write_text("pass\n")
        build = self.root / "build"
        build.mkdir()
        for config, message in (
            ('BR2_RISCV_32=y\n', 'Stale build configuration'),
            ('BR2_ESP32S31_INK_LINUX_PLATFORM_SUPPORT=y\nBR2_PACKAGE_ESP32S31_LINUX_COMPANION=y\n',
             'Use scripts/build-wifi.sh'),
        ):
            (build / '.config').write_text(config)
            result = self.run_script('build.sh', ESP32S31_INK_LINUX_BUILD_DIR=str(build))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(message, result.stderr)

    def test_post_image_finds_project_when_called_elsewhere(self):
        self.executable(self.tools / 'python3', '#!/bin/sh\nprintf "%s\\n" "$@" > "$CHECK_ARGS"\n')
        log = self.root / 'post-image-args'
        result = subprocess.run(
            [str(ROOT / 'board/metalio/eink4-plus/wifi/post-image.sh'), '/images'],
            cwd=self.root, env=dict(self.env, HOST_DIR='/host', CHECK_ARGS=str(log)),
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(log.read_text().splitlines(), [
            str(ROOT / 'scripts/make-wifi-image.py'), '--images-dir', '/images',
            '--host-dir', '/host', '--layout', str(ROOT / 'board/metalio/eink4-plus/wifi/layout.json')])

    def test_platform_hooks_are_scoped_to_the_selected_target(self):
        for platform, companion in ((False, False), (False, True), (True, False), (True, True)):
            makefile = f'''BR2_EXTERNAL_ESP32S31_INK_LINUX_PATH := {ROOT}
BR2_ESP32S31_INK_LINUX_PLATFORM_SUPPORT := {'y' if platform else ''}
BR2_PACKAGE_ESP32S31_LINUX_COMPANION := {'y' if companion else ''}
include {ROOT}/external.mk
check:
\t@printf '%s\\n' '$(LINUX_POST_RSYNC_HOOKS)' '$(OPENSBI_POST_RSYNC_HOOKS)' '$(WPA_SUPPLICANT_CONFIG_ENABLE)'
'''
            result = subprocess.run(['make', '--no-print-directory', '-s', '-f', '-', 'check'],
                                    input=makefile, capture_output=True, text=True, check=True)
            linux, opensbi, wpa = result.stdout.splitlines()
            self.assertEqual('ESP32S31_PATCH_LOCAL_KERNEL' in linux, platform)
            self.assertEqual('ESP32S31_INSTALL_HOSTED_KERNEL' in linux, platform and companion)
            self.assertEqual(bool(opensbi), platform and companion)
            self.assertEqual('CONFIG_NO_ROBUST_AV' in wpa, platform and companion)
