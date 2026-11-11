import importlib.util
import pathlib
import tempfile
import unittest
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('finish_kit', ROOT / 'tools/release/finish_kit.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class FinishKitTests(unittest.TestCase):
    def test_existing_delivery_files_are_not_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            for name in ('kit', 'kit.zip', 'kit.zip.sha256'):
                path = root / name
                path.write_bytes(b'keep')
                with self.assertRaisesRegex(SystemExit, 'refusing to overwrite'):
                    module.delivery_paths(root, 'kit')
                self.assertEqual(path.read_bytes(), b'keep')
                path.unlink()

    def test_rejects_unsafe_archive_without_extracting_anything(self):
        for bad in ('../outside', '/absolute', 'C:/outside', 'a\\..\\outside'):
            with self.subTest(member=bad), tempfile.TemporaryDirectory() as tmp:
                root = pathlib.Path(tmp)
                archive = root / 'player.zip'
                with zipfile.ZipFile(archive, 'w') as z:
                    z.writestr('good/file', 'good')
                    z.writestr(bad, 'bad')
                target = root / 'target'
                with self.assertRaises(ValueError):
                    module.extract_player(archive, target)
                self.assertFalse(target.exists())


if __name__ == '__main__':
    unittest.main()
