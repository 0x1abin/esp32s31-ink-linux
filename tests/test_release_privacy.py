import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('privacy', Path(__file__).resolve().parent.parent / 'scripts/release-privacy.py')
PRIVACY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PRIVACY)


class ReleasePrivacyTests(unittest.TestCase):
    def test_configuration_preserves_options_and_replaces_host_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            outside = '/' + 'home' + '/build-user/private/toolchain'
            original = f'ROOT="{root}/board"\nTOOLS="{outside}"\nTARGET="/data/wifi"\nOPTION=y\n'.encode()
            public = PRIVACY.public_config(original, root)
            self.assertIn(b'ROOT="${PROJECT_ROOT}/board"', public)
            self.assertIn(b'TOOLS="${EXTERNAL_PATH}"', public)
            self.assertIn(b'TARGET="/data/wifi"\nOPTION=y', public)
            self.assertIn(str(root).encode(), original)
            PRIVACY.check_public_bytes(public, root)
            with self.assertRaisesRegex(ValueError, 'private build path'):
                PRIVACY.check_public_bytes(original, root)

    def test_firmware_description_exports_only_reproducible_identity(self):
        raw = dict(project_name='esp32s31-linux-companion', project_version='commit', target='esp32s31',
                   git_revision='pinned-idf', idf_path=str(Path.home() / 'private-idf'),
                   build_components=['private-component'], project_path='private-source')
        result = json.loads(PRIVACY.firmware_metadata(raw))
        self.assertEqual(set(result), {'project_name', 'project_version', 'target', 'git_revision'})
        self.assertEqual(result['project_version'], 'commit')

    def test_compressed_rootfs_inputs_are_checked_before_packaging(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / 'target'
            target.mkdir()
            binary = target / 'program'
            binary.write_bytes(b'\x7fELF\0' + str(root / 'source.c').encode() + b'\0')
            with self.assertRaisesRegex(ValueError, 'private build path'):
                PRIVACY.check_target(target, root)
            binary.write_bytes(b'\x7fELF\0./source.c\0')
            PRIVACY.check_target(target, root)
