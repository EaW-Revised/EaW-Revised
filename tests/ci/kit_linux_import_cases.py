"""The Linux pre-import: the pinned official Linux editor writes the project metadata, the shipped Windows exe is never run."""
import hashlib
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/release'))
import godot_linux_import  # noqa: E402
import package_release  # noqa: E402


class LinuxImportTests(unittest.TestCase):
    def editor(self, root, data=b'linux editor'):
        path = root / godot_linux_import.LINUX_GODOT_NAME
        path.write_bytes(data)
        return path

    def test_only_the_pinned_editor_is_accepted(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            editor = self.editor(root)
            with self.assertRaisesRegex(ValueError, 'not the pinned official Linux Godot'):
                godot_linux_import.check_pinned(editor)
            with self.assertRaisesRegex(ValueError, 'missing'):
                godot_linux_import.check_pinned(root / 'absent')
            with patch.object(godot_linux_import, 'LINUX_GODOT_SHA256', hashlib.sha256(b'linux editor').hexdigest()):
                self.assertEqual(godot_linux_import.check_pinned(editor), hashlib.sha256(b'linux editor').hexdigest())

    def test_import_runs_the_editor_headless_in_an_isolated_home_and_never_the_shipped_exe(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            editor = self.editor(root)
            project = root / 'project'
            project.mkdir()
            shipped = root / 'Godot_v4.7.2-stable_win64.exe'
            shipped.write_bytes(b'must not run')
            done = subprocess.CompletedProcess([], 1, stdout='out', stderr='warning')
            with patch.object(godot_linux_import, 'LINUX_GODOT_SHA256', hashlib.sha256(b'linux editor').hexdigest()), \
                    patch.object(godot_linux_import.subprocess, 'run', return_value=done) as run:
                result = godot_linux_import.importer(editor)(shipped, project)
            command = run.call_args.args[0]
            self.assertEqual(command, [str(editor), '--headless', '--path', str(project), '--import'])
            self.assertNotIn(str(shipped), command)
            environment = run.call_args.kwargs['env']
            self.assertNotEqual(environment['HOME'], str(pathlib.Path.home()))
            self.assertTrue(environment['XDG_CONFIG_HOME'].startswith(environment['HOME']))
            self.assertIn('exit 1', result)

    def test_a_changed_editor_stops_before_it_runs(self):
        with tempfile.TemporaryDirectory() as temporary:
            editor = self.editor(pathlib.Path(temporary))
            with patch.object(godot_linux_import.subprocess, 'run') as run:
                with self.assertRaises(ValueError):
                    godot_linux_import.importer(editor)
                with self.assertRaises(ValueError):
                    godot_linux_import.import_project(editor, pathlib.Path(temporary))
                run.assert_not_called()

    def test_the_official_pin_matches_the_published_line(self):
        self.assertEqual(len(godot_linux_import.LINUX_GODOT_ZIP_SHA512), 128)
        self.assertEqual(len(godot_linux_import.LINUX_GODOT_SHA256), 64)


class RuntimeIdentityTests(unittest.TestCase):
    def test_identity_comes_from_the_embedded_fork_commit_without_running_the_exe(self):
        commit = package_release.export_package.PINNED_GODOT_FORK_COMMIT
        with tempfile.TemporaryDirectory() as temporary:
            exe = pathlib.Path(temporary) / 'Godot_v4.7.2-stable_win64.exe'
            exe.write_bytes(b'MZ' + (commit + 'a' * (40 - len(commit))).encode() + b'\0rest')
            with patch.object(package_release.subprocess, 'run') as run:
                identity, version = package_release.pinned_runtime_identity(exe)
                run.assert_not_called()
            self.assertEqual((identity, version), (package_release.export_package.PINNED_GODOT_IDENTITY,
                                                   package_release.export_package.PINNED_GODOT_VERSION))
            exe.write_bytes(b'MZ without the fork commit')
            with self.assertRaisesRegex(ValueError, 'does not embed the pinned fork commit'):
                package_release.pinned_runtime_identity(exe)


if __name__ == '__main__':
    unittest.main()
