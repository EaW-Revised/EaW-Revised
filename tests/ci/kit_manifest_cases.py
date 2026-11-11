import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/release'))
import kit_manifest  # noqa: E402


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f'tools/release/{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


finish_kit = load('finish_kit')


def run_git(root, *args):
    return subprocess.run(['git', '-C', str(root), *args], capture_output=True, check=True).stdout.decode().strip()


class KitRepo:
    """A throwaway repository holding every payload file the plan names, with a kit commit on top of a base."""

    def __init__(self, root):
        self.root = pathlib.Path(root)
        run_git(self.root, 'init', '-q')
        run_git(self.root, 'config', 'user.name', 'Test')
        run_git(self.root, 'config', 'user.email', 'test@example.invalid')
        for name in kit_manifest.PAYLOAD_REQUIRED:
            self.write(name, f'content of {name}\n')
        for name in ('finish_kit.py', 'kit_manifest.py'):  # finish refuses to run unless these are the kit commit's own bytes
            self.write('tools/release/' + name, (ROOT / 'tools/release' / name).read_text(encoding='utf-8'))
        self.write('tools/release/revised-layer-manifest.json', json.dumps(
            {'schema': 1, 'files': {'Data/XML/A.xml': {'size': 1, 'sha256': hashlib.sha256(b'a').hexdigest()}}}))
        self.write('tools/offload/windows_build.py', 'windows\n')
        self.write('docs/player-guide.md', 'guide\n')
        run_git(self.root, 'add', '-A')
        run_git(self.root, 'commit', '-qm', 'base')
        self.base = run_git(self.root, 'rev-parse', 'HEAD')
        self.write('tools/release/kit-label.txt', '0.3.3c\n')
        pin = json.dumps({'launcher_version': '1.2.3', 'artifact': {'file': 'eawr-launcher-1.2.3.zip', 'sha256': 'b' * 64}})
        self.write('config/launcher-pin.json', pin)
        run_git(self.root, 'add', '-A')
        run_git(self.root, 'commit', '-qm', 'kit')
        self.kit = run_git(self.root, 'rev-parse', 'HEAD')
        self.pin = json.loads(pin)

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode())

    def plan(self, **overrides):
        args = dict(base=self.base, kit=self.kit, label='0.3.3c', kit_version='0.3.3c-' + self.kit[:8],
                    overlay=['tools/release/kit-label.txt', 'config/launcher-pin.json'], launcher_pin=self.pin,
                    engine=('patched', {'Godot.exe': 'c' * 64}), changes_sha256='d' * 64,
                    route={'kind': 'linux-xbuild', 'hosts': ['x']}, operator=kit_manifest.orchestrator(self.root))
        args.update(overrides)
        return kit_manifest.build_plan(self.root, **args)


class KitManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        (self.root / 'repo').mkdir()
        self.repo = KitRepo(self.root / 'repo')
        self.player = self.root / 'eaw-revised-0.3.3c-aaaa-windows-x64-player.zip'
        with zipfile.ZipFile(self.player, 'w') as z:
            z.writestr('kit/EAWR.exe', 'exe')
        self.digest = hashlib.sha256(self.player.read_bytes()).hexdigest()

    def manifest(self, with_host=True):
        manifest = kit_manifest.new_manifest(self.repo.plan())
        if with_host:
            kit_manifest.add_host(manifest, {'host': 'h', 'received_from': 'log', 'outputs': [
                {'name': self.player.name, 'size': self.player.stat().st_size, 'sha256': self.digest}]})
        return manifest

    def problems(self, manifest, **kwargs):
        return kit_manifest.verify(manifest, repo=self.repo.root, **kwargs)

    def test_plan_records_the_commit_payload_overlay_and_inputs(self):
        plan = self.repo.plan()
        self.assertEqual(plan['source']['kit_sha'], self.repo.kit)
        self.assertEqual(plan['source']['kit_tree'], run_git(self.repo.root, 'rev-parse', self.repo.kit + '^{tree}'))
        paths = {e['path'] for e in plan['payload']}
        self.assertTrue(set(kit_manifest.PAYLOAD_REQUIRED) <= paths)
        self.assertIn('tools/release/finish_kit.py', paths)
        self.assertIn('tools/release/kit_manifest.py', paths)
        self.assertNotIn('tools/release/host_package.py', paths)
        self.assertEqual([e['path'] for e in plan['source']['overlay']], ['tools/release/kit-label.txt', 'config/launcher-pin.json'])
        self.assertEqual(plan['inputs']['launcher'], {'version': '1.2.3', 'file': 'eawr-launcher-1.2.3.zip', 'sha256': 'b' * 64})
        self.assertEqual(plan['inputs']['engine'], {'label': 'patched', 'files': {'Godot.exe': 'c' * 64}})
        self.assertIsNone(plan['version']['release_version'])
        for entry in plan['payload']:
            self.assertEqual(entry['git_blob'], run_git(self.repo.root, 'rev-parse', f"{self.repo.kit}:{entry['path']}"))

    def test_missing_required_payload_file_refuses_the_cut(self):
        run_git(self.repo.root, 'rm', '-q', 'tools/release/kit_engine.py')
        run_git(self.repo.root, 'commit', '-qm', 'drop')
        kit = run_git(self.repo.root, 'rev-parse', 'HEAD')
        with self.assertRaisesRegex(kit_manifest.ManifestError, 'kit_engine.py is not in kit commit'):
            self.repo.plan(kit=kit)

    def test_consistent_manifest_verifies_with_zip_and_repo(self):
        manifest = self.manifest()
        self.assertEqual(self.problems(manifest, zip_path=self.player, require_host=True), [])
        again = json.loads(json.dumps(manifest))
        self.assertEqual(kit_manifest.digest(again['plan']), again['plan_sha256'])

    def test_edited_plan_is_detected(self):
        manifest = self.manifest()
        manifest['plan']['inputs']['launcher']['sha256'] = 'e' * 64
        self.assertIn('plan_sha256 does not match the plan', self.problems(manifest))

    def test_swapped_player_archive_is_detected(self):
        manifest = self.manifest()
        self.player.write_bytes(b'someone else')
        found = self.problems(manifest, zip_path=self.player)
        self.assertTrue(any('player archive SHA-256 differs' in p for p in found), found)

    def test_host_section_written_against_a_different_plan_is_detected(self):
        manifest = self.manifest()
        other = copy.deepcopy(manifest)
        other['plan']['version']['label'] = '0.3.3d'
        other['plan_sha256'] = kit_manifest.digest(other['plan'])
        other['host'] = manifest['host']
        self.assertTrue(any('another plan' in p for p in self.problems(other)))

    def test_missing_host_is_reported_only_when_required(self):
        manifest = self.manifest(with_host=False)
        self.assertEqual(self.problems(manifest), [])
        self.assertIn('the manifest has no host section', self.problems(manifest, require_host=True))

    def test_payload_file_that_differs_in_the_repo_is_detected(self):
        manifest = self.manifest()
        manifest['plan']['payload'][0]['git_blob'] = 'f' * 40
        manifest['plan_sha256'] = kit_manifest.digest(manifest['plan'])
        self.assertTrue(any('differs from the kit commit' in p for p in self.problems(manifest)))

    def test_kit_commit_must_be_a_child_of_the_base_that_changes_only_the_overlay(self):
        manifest = self.manifest()
        self.assertEqual(self.problems(manifest), [])
        self.repo.write('tools/offload/windows_build.py', 'sneaked into the kit commit\n')
        run_git(self.repo.root, 'add', '-A')
        run_git(self.repo.root, 'commit', '-qm', 'beyond the overlay')
        later = run_git(self.repo.root, 'rev-parse', 'HEAD')
        plan = self.repo.plan(kit=later, kit_version='x-' + later[:8])
        found = self.problems(kit_manifest.new_manifest(plan))
        self.assertTrue(any('beyond the plan' in p for p in found), found)
        other = kit_manifest.new_manifest(self.repo.plan(base=self.repo.kit))
        self.assertTrue(any('not a child' in p for p in self.problems(other)))

    def test_overlay_file_equal_to_the_bases_is_fine(self):
        plan = self.repo.plan(overlay=['tools/release/kit-label.txt', 'config/launcher-pin.json',
                                       'tools/release/package_release.py'])
        self.assertEqual(self.problems(kit_manifest.new_manifest(plan)), [])

    def test_expected_base_is_checked_against_the_plan(self):
        manifest = self.manifest()
        self.assertEqual(self.problems(manifest, expect_base=self.repo.base), [])
        found = self.problems(manifest, expect_base='1' * 40)
        self.assertTrue(any('expected 111111111111' in p for p in found), found)

    def test_host_section_must_name_a_host(self):
        manifest = self.manifest(with_host=False)
        kit_manifest.add_host(manifest, {'host': None, 'received_from': 'log', 'outputs': [
            {'name': self.player.name, 'size': 1, 'sha256': self.digest}]})
        self.assertIn('the host section names no host', self.problems(manifest))

    def test_finish_section_binds_the_delivered_archive_and_sums(self):
        manifest = self.manifest()
        folder = self.root / 'kit'
        folder.mkdir()
        (folder / 'EAWR.exe').write_bytes(b'exe')
        sums = f"{hashlib.sha256(b'exe').hexdigest()}  EAWR.exe\n"
        (folder / 'SHA256SUMS').write_bytes(sums.encode())
        kit_manifest.add_finish(manifest, {'delivered': {
            'name': self.player.name, 'size': self.player.stat().st_size, 'sha256': self.digest,
            'sums_sha256': hashlib.sha256(sums.encode()).hexdigest()}})
        self.assertEqual(self.problems(manifest, zip_path=self.player, folder=folder, require_finish=True), [])
        (folder / 'EAWR.exe').write_bytes(b'patched')
        found = self.problems(manifest, zip_path=self.player, folder=folder, require_finish=True)
        self.assertIn('EAWR.exe: SHA-256 differs', found)
        (folder / 'EAWR.exe').write_bytes(b'exe')
        (folder / 'extra.txt').write_text('x')
        self.assertIn('extra.txt: not listed in SHA256SUMS', self.problems(manifest, folder=folder))
        (folder / 'extra.txt').unlink()
        (folder / 'SHA256SUMS').write_text(sums + '\n')
        self.assertIn('the kit\'s SHA256SUMS differs from the manifest', self.problems(manifest, folder=folder))

    def test_finish_section_edit_is_detected(self):
        manifest = self.manifest()
        kit_manifest.add_finish(manifest, {'delivered': {'name': 'n', 'size': 1, 'sha256': 'a' * 64, 'sums_sha256': 'b' * 64}})
        manifest['finish']['delivered']['sha256'] = 'c' * 64
        self.assertIn('finish_sha256 does not match the finish section', self.problems(manifest))

    def test_blob_id_of_file_applies_the_repositorys_line_ending_rules(self):
        run_git(self.repo.root, 'config', 'core.autocrlf', 'true')
        crlf = self.root / 'copy.py'
        crlf.write_bytes(b'one\r\ntwo\r\n')
        lf_blob = subprocess.run(['git', '-C', str(self.repo.root), 'hash-object', '--stdin'], input=b'one\ntwo\n',
                                 capture_output=True, check=True).stdout.decode().strip()
        self.assertEqual(kit_manifest.blob_id_of_file(self.repo.root, 'tools/release/x.py', crlf), lf_blob)

    def test_orchestrator_notes_a_dirty_script(self):
        self.assertTrue(kit_manifest.orchestrator(self.repo.root)['clean'])
        self.repo.write('tools/offload/windows_build.py', 'changed\n')
        state = kit_manifest.orchestrator(self.repo.root)
        self.assertFalse(state['clean'])
        names = {f['path'] for f in state['files']}
        self.assertIn('tools/offload/windows_build.py', names)
        self.assertEqual(state['tooling_commit'], run_git(self.repo.root, 'rev-parse', 'HEAD'))

    def test_extract_payload_reads_the_commit_not_the_working_tree(self):
        self.repo.write('tools/release/kit_engine.py', 'edited after the cut\n')
        out = kit_manifest.extract_payload(self.repo.root, self.repo.kit, self.root / 'payload')
        self.assertEqual((out / 'tools/release/kit_engine.py').read_text(), 'content of tools/release/kit_engine.py\n')
        self.assertTrue((out / 'docs/player-guide.md').is_file())
        self.assertFalse((out / 'tests').exists())
        again = kit_manifest.extract_payload(self.repo.root, self.repo.kit, self.root / 'payload')
        self.assertEqual(again, out)

    def test_cli_verifies_and_reports_failures(self):
        manifest = self.manifest()
        path = self.root / 'kit-manifest.json'
        kit_manifest.write(path, manifest)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(kit_manifest.main(['verify', str(path), '--zip', str(self.player), '--repo', str(self.repo.root)]), 0)
        self.assertIn('manifest ok', out.getvalue())
        self.player.write_bytes(b'x')
        with contextlib.redirect_stdout(out):
            self.assertEqual(kit_manifest.main(['verify', str(path), '--zip', str(self.player)]), 1)
        self.assertEqual(kit_manifest.main(['verify', str(self.root / 'missing.json')]), 2)


class FinishManifestTests(unittest.TestCase):
    def reviewed_layer(self, root, files, manifest):
        layer = root / 'layer'
        for name, data in files.items():
            (layer / name).parent.mkdir(parents=True, exist_ok=True)
            (layer / name).write_bytes(data)
        reviewed = root / 'revised-layer-manifest.json'
        reviewed.write_text(json.dumps({'schema': 1, 'files': {
            n: {'size': len(d), 'sha256': hashlib.sha256(d).hexdigest()} for n, d in manifest.items()}}))
        return layer, reviewed

    def test_layer_check_accepts_exactly_the_reviewed_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            layer, reviewed = self.reviewed_layer(root, {'Data/A.xml': b'a'}, {'Data/A.xml': b'a'})
            plan = {'inputs': {'revised_layer': {'manifest_sha256': 'x'}}}
            state = finish_kit.check_layer(layer, reviewed, plan, False)
            self.assertEqual((state['mode'], state['problems']), ('reviewed', []))

    def test_layer_check_refuses_changed_extra_and_missing_files(self):
        plan = {'inputs': {'revised_layer': {'manifest_sha256': 'x'}}}
        cases = {'changed': ({'Data/A.xml': b'changed'}, {'Data/A.xml': b'a'}),
                 'extra': ({'Data/A.xml': b'a', 'Data/B.xml': b'b'}, {'Data/A.xml': b'a'}),
                 'missing': ({}, {'Data/A.xml': b'a'})}
        for name, (files, manifest) in cases.items():
            with self.subTest(case=name), tempfile.TemporaryDirectory() as temporary:
                layer, reviewed = self.reviewed_layer(pathlib.Path(temporary), files, manifest)
                state = finish_kit.check_layer(layer, reviewed, plan, False)
                self.assertEqual(state['mode'], 'unreviewed')
                self.assertEqual(len(state['problems']), 1)
                self.assertIn('reviewed manifest', state['problems'][0])
                allowed = finish_kit.check_layer(layer, reviewed, plan, True)
                self.assertEqual((allowed['mode'], allowed['problems']), ('unreviewed', []))

    def make_tree(self, root, files):
        for name, data in files.items():
            path = pathlib.Path(root) / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

    def test_layout_reports_added_removed_and_hidden_names(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            ref = {'EAWR.exe': b'old', 'EAWR (Instanced hulls).lnk': b'link', 'README.md': b'old guide', 'godot/g.exe': b'g'}
            kit = {'EAWR.exe': b'new', 'README.md': b'new guide', 'godot/g.exe': b'g', 'extra.txt': b'x'}
            self.make_tree(root / 'ref', ref)
            self.make_tree(root / 'kit', kit)
            plain = finish_kit.compare_layout(root / 'kit', root / 'ref', finish_kit.BUILTIN_ALLOW)
            self.assertEqual((plain['added'], plain['removed']), (['extra.txt'], ['EAWR (Instanced hulls).lnk']))
            self.assertEqual(plain['changed_content'], ['EAWR.exe', 'README.md'])
            self.assertEqual(plain['changed_binaries'], ['EAWR.exe'])
            self.assertEqual(plain['pinned_binaries_changed'], [])
            allowed = finish_kit.compare_layout(root / 'kit', root / 'ref', finish_kit.BUILTIN_ALLOW + ['*.lnk', 'extra.txt'])
            self.assertEqual((allowed['added'], allowed['removed']), ([], []))
            self.assertEqual(allowed['allowed_differences'], ['EAWR (Instanced hulls).lnk', 'extra.txt'])

    def test_safe_render_thread_shortcut_is_a_built_in_allowance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            self.make_tree(root / 'ref', {'EAWR (Safe render thread).cmd': b'cmd', 'EAWR.exe': b'x'})
            self.make_tree(root / 'kit', {'EAWR.exe': b'x'})
            found = finish_kit.compare_layout(root / 'kit', root / 'ref', finish_kit.BUILTIN_ALLOW)
            self.assertEqual((found['added'], found['removed']), ([], []))

    def test_changed_engine_launcher_or_library_bytes_fail_unless_named(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            files = {'godot/Godot.exe': b'engine', 'launcher/project.godot': b'p', 'project/bin/viewer.dll': b'v',
                     'project/data.pck': b'pck'}
            self.make_tree(root / 'ref', files)
            self.make_tree(root / 'kit', {name: data + b'!' for name, data in files.items()})
            found = finish_kit.compare_layout(root / 'kit', root / 'ref', finish_kit.BUILTIN_ALLOW)
            self.assertEqual(found['pinned_binaries_changed'], ['godot/Godot.exe', 'launcher/project.godot', 'project/data.pck'])
            self.assertEqual(found['changed_binaries'], ['godot/Godot.exe', 'project/bin/viewer.dll'])
            named = finish_kit.compare_layout(root / 'kit', root / 'ref', finish_kit.BUILTIN_ALLOW, ['godot/Godot.exe'])
            self.assertNotIn('godot/Godot.exe', named['pinned_binaries_changed'])

    def test_changed_binaries_must_carry_the_kit_identity_or_be_named(self):
        changed = ['EAWR.exe', 'eawr_mods_service.exe', 'project/bin/viewer.dll', 'project/tools/other.exe',
                   'godot/Godot.exe']
        self.assertEqual(finish_kit.unexplained_binaries(changed, {'project/bin/viewer.dll'}), ['project/tools/other.exe'])
        self.assertEqual(finish_kit.unexplained_binaries(changed, {'project/bin/viewer.dll', 'project/tools/other.exe'}), [])

    def test_allow_diff_patterns_that_could_hide_binaries_are_refused(self):
        for pattern in ('*', '*.exe', '*.dll', 'godot/*', 'launcher/*', '*.pck', 'project/bin/*', '*/*.exe'):
            with self.subTest(pattern=pattern):
                self.assertEqual(finish_kit.refused_patterns([pattern]), [pattern])
        for pattern in ('*.lnk', 'README.md', 'EAWR (Instanced hulls).lnk', 'docs/*.md'):
            with self.subTest(pattern=pattern):
                self.assertEqual(finish_kit.refused_patterns([pattern]), [])

    def test_name_specific_patterns_cannot_hide_protected_files(self):
        names = {'EAWR.exe', 'EAWR Mods.exe', 'eawr_mods_service.exe', 'Revised/Data/Audio/a.wav', 'godot/g.exe',
                 'launcher/project.godot', 'project/bin/viewer.dll', 'project/data.pck', 'README.md',
                 'EAWR (Instanced hulls).lnk', 'LICENSE'}
        for pattern in ('EAWR.exe', 'eawr_*.exe', '*Mods.exe', 'Revised/*', 'EAWR*', 'project/bin/viewer.dll',
                        'launcher/project.godot', '*.pck'):
            with self.subTest(pattern=pattern):
                self.assertEqual(finish_kit.refused_by_name([pattern], names), [pattern])
        for pattern in ('*.lnk', 'README.md', 'LICENSE', 'EAWR (Instanced hulls).lnk'):
            with self.subTest(pattern=pattern):
                self.assertEqual(finish_kit.refused_by_name([pattern], names), [])

    def test_binary_allowance_that_matches_everything_is_refused(self):
        for pattern in ('*', '**', '*/*', '/'):
            with self.subTest(pattern=pattern):
                self.assertEqual(finish_kit.refused_binary_allowances([pattern]), [pattern])
        self.assertEqual(finish_kit.refused_binary_allowances(['godot/Godot_v4.7.2-stable_win64.exe', '*.pck']), [])

    def test_identityless_files_are_recorded_with_what_they_embed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / 'project/common').mkdir(parents=True)
            (root / 'EAWR.exe').write_bytes(b'MZ launcher without any identity')
            (root / 'EAWR Mods.exe').write_bytes(b'MZ 0.3.3c+' + b'a' * 40 + b' stub')
            (root / 'other.exe').write_bytes(b'not listed')
            found = finish_kit.describe_identityless(root)
            self.assertEqual(sorted(found), ['EAWR Mods.exe', 'EAWR.exe'])
            self.assertEqual(found['EAWR.exe']['embedded_identities'], [])
            self.assertEqual(found['EAWR Mods.exe']['embedded_identities'], ['a' * 40])
            self.assertEqual(found['EAWR.exe']['sha256'], hashlib.sha256(b'MZ launcher without any identity').hexdigest())

    def test_launcher_folder_must_equal_the_pinned_artifact(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            artifact = root / 'launcher.zip'
            with zipfile.ZipFile(artifact, 'w') as z:
                z.writestr('project.godot', 'p')
                z.writestr('scripts/a.gd', 'a')
            digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
            self.make_tree(root / 'launcher', {'project.godot': b'p', 'scripts/a.gd': b'a'})
            self.assertEqual(finish_kit.check_launcher(root / 'launcher', artifact, digest), [])
            (root / 'launcher/scripts/a.gd').write_bytes(b'changed')
            (root / 'launcher/new.dll').write_bytes(b'n')
            found = finish_kit.check_launcher(root / 'launcher', artifact, digest)
            self.assertEqual(sorted(found), ['launcher/new.dll: not in the pinned artifact',
                                             'launcher/scripts/a.gd: bytes differ from the pinned artifact'])
            self.assertEqual(finish_kit.check_launcher(root / 'launcher', artifact, '0' * 64),
                             ['launcher artifact differs from the plan\'s pinned SHA-256'])

    def run_finish(self, work, repo, root, *extra):
        out = io.StringIO()
        argv = ['finish_kit.py', '--work', str(work), '--out-root', str(root / 'out'), '--reference', str(root),
                '--layer', str(root), '--repo', str(repo), *extra]
        with patch.object(sys, 'argv', argv), contextlib.redirect_stdout(out):
            code = finish_kit.main()
        return code, out.getvalue()

    def test_finish_refuses_a_missing_or_unreadable_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            work = root / 'work'
            work.mkdir()
            (work / 'cut.json').write_text(json.dumps({'ok': True}))
            code, output = self.run_finish(work, root, root)
            self.assertEqual(code, 4)
            self.assertIn('cannot use kit-manifest.json', output)
            (work / kit_manifest.MANIFEST_NAME).write_text('not json')
            code, output = self.run_finish(work, root, root)
            self.assertEqual(code, 4)
            self.assertEqual([p.name for p in work.iterdir() if p.is_dir()], [])

    def test_finish_refuses_binary_hiding_allow_diff_patterns(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            work = root / 'work'
            work.mkdir()
            (work / 'cut.json').write_text(json.dumps({'ok': True}))
            code, output = self.run_finish(work, root, root, '--allow-diff', '*.exe')
            self.assertEqual(code, 4)
            self.assertIn("allow-diff: '*.exe' could hide", output)

    def test_finish_refuses_to_run_from_other_bytes_than_the_kit_commit(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / 'repo').mkdir()
            repo = KitRepo(root / 'repo')
            work = root / 'work'
            work.mkdir()
            player = work / 'kit-player.zip'
            player.write_bytes(b'original')
            manifest = kit_manifest.new_manifest(repo.plan())
            for entry in manifest['plan']['payload']:
                if entry['path'] == 'tools/release/finish_kit.py':
                    entry['git_blob'] = '1' * 40
            manifest['plan_sha256'] = kit_manifest.digest(manifest['plan'])
            kit_manifest.add_host(manifest, {'host': 'h', 'received_from': 'log', 'outputs': [
                {'name': player.name, 'size': 8, 'sha256': hashlib.sha256(b'original').hexdigest()}]})
            kit_manifest.write(work / kit_manifest.MANIFEST_NAME, manifest)
            (work / 'cut.json').write_text(json.dumps({'ok': True, 'kit_sha': repo.kit, 'player_zip': str(player),
                                                       'plan_sha256': manifest['plan_sha256'], 'changes': str(player)}))
            code, output = self.run_finish(work, repo.root, root)
            self.assertEqual(code, 4)
            self.assertIn("tool: finish_kit.py in this checkout is not the kit commit's copy", output)
            self.assertFalse((root / 'out').exists())

    def test_delivery_paths_also_protect_the_manifest_sidecar(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            sidecar = root / ('kit' + kit_manifest.SIDECAR_SUFFIX)
            sidecar.write_bytes(b'keep')
            with self.assertRaisesRegex(SystemExit, 'refusing to overwrite'):
                finish_kit.delivery_paths(root, 'kit')
            sidecar.unlink()
            self.assertEqual(len(finish_kit.delivery_paths(root, 'kit')), 4)

    def test_finish_stops_before_extracting_when_the_player_zip_changed_after_the_cut(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / 'repo').mkdir()
            repo = KitRepo(root / 'repo')
            work = root / 'work'
            work.mkdir()
            player = work / 'kit-player.zip'
            player.write_bytes(b'original')
            changes = work / 'changes.md'
            changes.write_text('- change')
            manifest = kit_manifest.new_manifest(repo.plan(changes_sha256=hashlib.sha256(b'- change').hexdigest()))
            kit_manifest.add_host(manifest, {'host': 'h', 'received_from': 'log', 'outputs': [
                {'name': player.name, 'size': 8, 'sha256': hashlib.sha256(b'original').hexdigest()}]})
            kit_manifest.write(work / kit_manifest.MANIFEST_NAME, manifest)
            (work / 'cut.json').write_text(json.dumps({
                'ok': True, 'kit_sha': repo.kit, 'plan_sha256': manifest['plan_sha256'], 'player_zip': str(player),
                'changes': str(changes)}))
            player.write_bytes(b'swapped!')
            out = io.StringIO()
            argv = ['finish_kit.py', '--work', str(work), '--out-root', str(root / 'out'), '--reference', str(root),
                    '--layer', str(root), '--repo', str(repo.root)]
            with patch.object(sys, 'argv', argv), contextlib.redirect_stdout(out):
                self.assertEqual(finish_kit.main(), 4)
            self.assertIn('player archive SHA-256 differs', out.getvalue())
            self.assertEqual([p.name for p in work.iterdir() if p.is_dir()], [])
            self.assertFalse((root / 'out').exists())


if __name__ == '__main__':
    unittest.main()
