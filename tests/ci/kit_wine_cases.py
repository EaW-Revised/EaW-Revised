import importlib.util
import os
import pathlib
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('godot_wine', ROOT / 'tools/release/godot_wine.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class GodotWineTests(unittest.TestCase):
    def test_engine_and_project_paths_are_passed_without_shell(self):
        with patch.dict(os.environ, {'EAWR_WINE': 'wine with spaces'}), \
                patch.object(module.os, 'execvpe') as call:
            module.main(['engine.exe', '--headless', '--path', 'project with spaces', '--import'])
        self.assertEqual(call.call_args.args[0], 'wine with spaces')
        self.assertEqual(call.call_args.args[1], ['wine with spaces', module.windows_path('engine.exe'),
                         '--headless', '--path', module.windows_path('project with spaces'), '--import'])


if __name__ == '__main__':
    unittest.main()
