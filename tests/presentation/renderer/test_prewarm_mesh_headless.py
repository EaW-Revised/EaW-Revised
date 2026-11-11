"""Call the compiled production prewarm builder and Godot's CPU array validator."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from tools.common.engine_guard import require_patched


@unittest.skipUnless(os.environ.get('EAWR_PREWARM_VIEWER_ARTIFACT'),
                     'requires an exact-head compiled viewer artifact for the headless CPU seam')
class PrewarmMeshHeadlessContracts(unittest.TestCase):
    def test_compiled_production_builder_matches_engine_array_contract(self):
        artifact = Path(os.environ['EAWR_PREWARM_VIEWER_ARTIFACT'])
        manifest = json.loads(artifact.read_text(encoding='utf-8'))
        expected_head = os.environ['EAWR_PREWARM_HEADLESS_HEAD']
        self.assertEqual(manifest['commit'], expected_head)
        self.assertEqual(manifest['validationStatus'], 'passed')
        engine = Path(os.environ['EAWR_GODOT_EXECUTABLE'])
        identity = require_patched(engine, purpose='production prewarm headless CPU contract', environ={})
        output = Path(os.environ['EAWR_PREWARM_HEADLESS_OUT']).resolve()
        output.mkdir(parents=True, exist_ok=False)
        project = output / 'project'
        project.mkdir()
        archive = Path(manifest['localZip'])
        with zipfile.ZipFile(archive) as package:
            dlls = [item for item in package.infolist()
                    if item.filename == 'bin/libeawr_viewer.windows.template_release.x86_64.dll']
            self.assertEqual(len(dlls), 1)
            self.assertEqual(hashlib.sha256(package.read(dlls[0])).hexdigest(), manifest['sha256'])
            for item in package.infolist():
                if not item.filename.startswith('bin/') or item.is_dir():
                    continue
                target = (project / item.filename).resolve()
                self.assertTrue(target.is_relative_to(project))
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(package.read(item))
        shutil.copyfile(ROOT / 'apps/viewer/project/eawr_viewer.gdextension', project / 'eawr_viewer.gdextension')
        script = Path(__file__).with_name('prewarm_mesh_headless.gd')
        shutil.copyfile(script, project / 'probe.gd')
        (project / 'project.godot').write_text('config_version=5\n[application]\nconfig/name="Prewarm CPU contract"\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n')
        (project / '.godot').mkdir()
        (project / '.godot/extension_list.cfg').write_text('res://eawr_viewer.gdextension\n')
        command = [str(engine), '--headless', '--path', str(project), '--script', 'res://probe.gd',
                   '--log-file', str(output / 'engine.log')]
        timed_out = False
        try:
            completed = subprocess.run(command, capture_output=True, text=True, timeout=60,
                                      creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except subprocess.TimeoutExpired as error:
            timed_out = True
            def captured(value):
                return value.decode('utf-8', errors='replace') if isinstance(value, bytes) else value or ''
            completed = subprocess.CompletedProcess(command, -1, captured(error.stdout), captured(error.stderr))
        (output / 'stdout.log').write_text(completed.stdout)
        (output / 'stderr.log').write_text(completed.stderr)
        lines = [line.removeprefix('P9_PREWARM_BUILDER ') for line in completed.stdout.splitlines()
                 if line.startswith('P9_PREWARM_BUILDER ')]
        result = json.loads(lines[0]) if len(lines) == 1 else None
        receipt = {'head': expected_head, 'artifact_dll_sha256': manifest['sha256'],
                   'engine': identity, 'command': command, 'exit_code': completed.returncode,
                   'timed_out': timed_out,
                   'script_sha256': hashlib.sha256(script.read_bytes()).hexdigest(),
                   'gpu': False, 'result': result,
                   'limit': 'compiled production arrays and CPU conversion; no shader compile or GPU draw coverage'}
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertIsNotNone(result, completed.stdout + completed.stderr)
        self.assertTrue(result['headless'])
        self.assertTrue(result['production_builder'])
        self.assertTrue(result['passed'])
        self.assertEqual([case['skinned'] for case in result['cases']], [False, True])
        self.assertTrue(all(case['passed'] for case in result['cases']))
        self.assertNotIn('ERROR:', completed.stderr)
        self.assertNotIn('Using Device', completed.stdout)


if __name__ == '__main__':
    unittest.main()
