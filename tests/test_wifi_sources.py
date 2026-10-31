import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("wifi_sources", ROOT / "scripts/prepare-wifi-sources.py")
SOURCES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SOURCES)


class WifiSourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.previous_root = SOURCES.ROOT
        SOURCES.ROOT = self.root
        self.original = self.root / "upstream/demo"
        self.original.mkdir(parents=True)
        subprocess.run(["git", "init", "-q", str(self.original)], check=True)
        (self.original / "source.c").write_text("old\n")
        subprocess.run(["git", "-C", str(self.original), "add", "source.c"], check=True)
        subprocess.run(["git", "-C", str(self.original), "-c", "user.name=Selfcheck",
                        "-c", "user.email=selfcheck@example.invalid", "commit", "-qm", "fixture"], check=True)
        self.patch = self.root / "fix.patch"
        self.patch.write_text("--- a/source.c\n+++ b/source.c\n@@ -1 +1 @@\n-old\n+new\n")

    def tearDown(self):
        SOURCES.ROOT = self.previous_root
        self.temporary.cleanup()

    def test_patches_only_owned_copy_and_is_repeatable(self):
        SOURCES.prepare("demo", [self.patch])
        destination = self.root / "outputs/esp32s31-ink-linux-sources/demo"
        self.assertEqual((destination / "source.c").read_text(), "new\n")
        self.assertEqual((self.original / "source.c").read_text(), "old\n")
        before = (destination / "source.c").stat().st_mtime_ns
        SOURCES.prepare("demo", [self.patch])
        self.assertEqual((destination / "source.c").stat().st_mtime_ns, before)
        self.assertFalse((destination / ".git").exists())

    def test_unknown_destination_is_preserved(self):
        destination = self.root / "outputs/esp32s31-ink-linux-sources/demo"
        destination.mkdir(parents=True)
        (destination / "unique.txt").write_text("keep\n")
        with self.assertRaisesRegex(ValueError, "unowned"):
            SOURCES.prepare("demo", [self.patch])
        self.assertEqual((destination / "unique.txt").read_text(), "keep\n")

    def test_idf_version_preserves_pin_within_app_descriptor_limit(self):
        self.original.rename(self.root / 'upstream/esp-idf')
        SOURCES.prepare('esp-idf', [])
        prepared = self.root / 'outputs/esp32s31-ink-linux-sources/esp-idf'
        metadata = json.loads((prepared / '.esp32s31-ink-linux-source.json').read_text())
        version = (prepared / 'version.txt').read_text().strip()
        self.assertLessEqual(len(version), 31)
        self.assertTrue(version.endswith(metadata['revision'][:12]))
        self.assertEqual(version, metadata['version'])
