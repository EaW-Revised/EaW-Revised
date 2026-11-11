"""Portable cross-package input validation."""
import importlib.util
import pathlib
import tempfile
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('package_cross', ROOT / 'tools/release/package_cross.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PackageCrossTests(unittest.TestCase):
    def test_missing_or_ambiguous_binary_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            with self.assertRaisesRegex(ValueError, 'found 0'):
                module.unique(root, 'EAWR.exe')
            for directory in ('one', 'two'):
                folder = root / 'apps' / directory
                folder.mkdir(parents=True)
                (folder / 'EAWR.exe').touch()
            with self.assertRaisesRegex(ValueError, 'found 2'):
                module.unique(root, 'EAWR.exe')

    def test_bad_source_or_missing_input_does_not_package(self):
        sha = 'a' * 40
        argv = ['--sha', sha, '--version', '0.3.3c', '--build-dir', 'missing',
                '--engine-dir', 'missing', '--launcher-artifact', 'missing',
                '--out', 'unused', '--symbol-store', 'unused-symbols']
        with patch.object(module.subprocess, 'check_output', return_value='b' * 40), \
             patch.object(module.subprocess, 'run') as run:
            self.assertEqual(module.main(argv), 1)
            run.assert_not_called()
        with patch.object(module.subprocess, 'check_output', return_value=sha), \
             patch.object(module.subprocess, 'run') as run:
            self.assertEqual(module.main(argv), 1)
            run.assert_not_called()

    def test_mismatched_launcher_fails_before_engine_or_package(self):
        sha = 'a' * 40
        with tempfile.TemporaryDirectory() as temporary:
            launcher = pathlib.Path(temporary) / 'launcher.zip'
            launcher.write_bytes(b'wrong')
            argv = ['--sha', sha, '--version', '0.3.3c', '--build-dir', temporary,
                    '--engine-dir', temporary, '--launcher-artifact', str(launcher),
                    '--out', 'unused', '--symbol-store', 'unused-symbols']
            with patch.object(module.subprocess, 'check_output', return_value=sha), \
                 patch.object(module.subprocess, 'run') as run:
                self.assertEqual(module.main(argv), 1)
                run.assert_not_called()
if __name__ == '__main__':
    unittest.main()
