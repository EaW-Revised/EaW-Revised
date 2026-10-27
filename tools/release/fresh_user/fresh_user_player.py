"""Fresh-user check of the Windows player (or debug) archive, run on a private GPU desktop of a rig host.

Rig: copy to tests/presentation/renderer/test_release_player_fresh_user.py (never committed), stage the archive
with -ExtraArtifact and run through Invoke-RigSuite. Environment:
  EAWR_JOB_RELEASE_PACKAGE  the zip (player or debug)
  EAWR_JOB_RELEASE_KIND     player (default) or debug: both run all nine cases; debug gets longer budgets
  EAWR_JOB_RELEASE_OUT      output folder; EAWR_GODOT_EXECUTABLE the rig's pinned console Godot (renderer helper
                            tests only); EAWR_EAW_GAME_ROOT the rig's installed game
Every launch uses the EXTRACTED folder's own EAWR.exe / bundled Godot with a restricted PATH, an empty APPDATA and a
space plus a non-ASCII character in the extraction path.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'tools' / 'common'))
import hidden_console  # noqa: E402

hidden_console.install()  # every child launch below (and the helpers' own) starts without a console window

GODOT_NAME = 'Godot_v4.7.2-stable_win64.exe'
EXTENSION = 'libeawr_viewer.windows.template_release.x86_64.dll'


class PlayerFreshUser(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = Path(os.environ.get('EAWR_JOB_RELEASE_SOURCE') or next(
            str(p) for p in Path(__file__).resolve().parents
            if (p / 'tests/presentation/renderer').is_dir() and (p / 'apps/viewer').is_dir()))
        cls.output = Path(os.environ.get('EAWR_JOB_RELEASE_OUT') or tempfile.mkdtemp(prefix='release-player-'))
        cls.output.mkdir(parents=True, exist_ok=True)
        cls.archive = Path(os.environ['EAWR_JOB_RELEASE_PACKAGE'])
        cls.kind = os.environ.get('EAWR_JOB_RELEASE_KIND', 'player')
        cls.extracted = cls.output / 'EAWR Tést dir'
        with zipfile.ZipFile(cls.archive) as archive:
            for name in archive.namelist():
                if not (cls.extracted / name).resolve().is_relative_to(cls.extracted.resolve()):
                    raise ValueError('unsafe archive member: ' + name)
            archive.extractall(cls.extracted)
        cls.package = next(cls.extracted.glob('*/EAWR.exe')).parent
        cls.godot = cls.package / 'godot' / GODOT_NAME
        cls.rig_godot = Path(os.environ['EAWR_GODOT_EXECUTABLE']).resolve()
        cls.game = os.environ['EAWR_EAW_GAME_ROOT']
        cls.appdata = cls.output / 'appdata'
        cls.appdata.mkdir(exist_ok=True)
        # The launcher finds its folder through the OS known-folder API, not APPDATA: pin it explicitly.
        cls.user_data = cls.appdata / 'Godot' / 'app_userdata' / 'EAWR Viewer'
        system = os.environ['SystemRoot']
        cls.env = dict(os.environ)
        cls.env['PATH'] = os.pathsep.join((str(Path(system) / 'System32'), system))
        cls.env['APPDATA'] = str(cls.appdata)
        for name in ('PYTHONPATH', 'EAWR_FONT_CACHE', 'EAWR_GODOT_EXECUTABLE', 'EAWR_EAW_GAME_ROOT'):
            cls.env.pop(name, None)
        cls.records = []
        sys.path.insert(0, str(cls.source / 'tests/presentation/renderer'))
        sys.path.insert(0, str(cls.source))

    @classmethod
    def tearDownClass(cls):
        extension = cls.package / 'project/bin' / EXTENSION
        (cls.output / 'fresh-user-receipt.json').write_text(json.dumps({
            'kind': cls.kind,
            'archive_sha256': hashlib.sha256(cls.archive.read_bytes()).hexdigest(),
            'extension_sha256': hashlib.sha256(extension.read_bytes()).hexdigest(),
            'launcher_sha256': hashlib.sha256((cls.package / 'EAWR.exe').read_bytes()).hexdigest(),
            'package': str(cls.archive), 'extracted': str(cls.package),
            'restricted_path': cls.env['PATH'], 'launches': cls.records,
        }, indent=2) + '\n', encoding='utf-8')

    def test_native_manager_entry(self):
        self.assertFalse(list(self.package.rglob("*.vbs")))
        if not (self.package / 'launcher').is_dir():
            return  # Manager is an optional package component.
        sys.path.insert(0, str(self.source / 'apps/launcher/tests'))
        from launcher_smoke import read_imports, ALLOWED_IMPORTS
        stub = self.package / 'EAWR Mods.exe'
        self.assertTrue(stub.is_file())
        imports, subsystem, machine = read_imports(stub)
        self.assertEqual(subsystem, 2)
        self.assertEqual(machine, 0x8664)
        self.assertLessEqual(imports, ALLOWED_IMPORTS)

    def run_process(self, name, command, timeout=900, cwd=None):
        if self.kind == 'debug':
            timeout = max(timeout, 2400)  # the Debug build is not performance-representative
        out = self.output / name
        out.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(command, cwd=cwd or self.package, env=self.env, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=timeout)
        (out / 'console.log').write_text(result.stdout + result.stderr, encoding='utf-8')
        self.records.append({'name': name, 'command': [str(c) for c in command], 'exit_code': result.returncode})
        return result, out

    def launcher(self, name, extra):
        result, out = self.run_process(name, [str(self.package / 'EAWR.exe'), '--no-dialogs', '--game-root', self.game,
                                              '--user-data', str(self.user_data), '--godot-quit-after', '600', *extra])
        return result, out

    def test_01_archive_integrity(self):
        sums = {}
        for line in (self.package / 'SHA256SUMS').read_text(encoding='utf-8').splitlines():
            digest, name = line.split('  ', 1)
            sums[name] = digest
        files = {p.relative_to(self.package).as_posix() for p in self.package.rglob('*')
                 if p.is_file() and p.name != 'SHA256SUMS'}
        self.assertEqual(set(sums), files, 'SHA256SUMS lists exactly the files')
        for name, digest in sums.items():
            self.assertEqual(hashlib.sha256((self.package / name).read_bytes()).hexdigest(), digest, name)
        self.assertFalse([n for n in files if n.lower().endswith(('.ttf', '.otf', '.meg', '.tga'))
                          or n.lower().endswith('starwarsg.exe')], 'no game files')
        runtime = json.loads((self.package / 'runtime.json').read_text(encoding='utf-8'))
        self.assertEqual(runtime.get('variant', 'player'), self.kind)
        self.assertTrue((self.package / 'godot' / GODOT_NAME).is_file())
        self.assertFalse(list((self.package / 'godot').glob('*console*')))
        if self.kind == 'debug':
            self.assertTrue(list((self.package / 'project/bin').glob('*.pdb')))
            self.assertTrue((self.package / 'tools/sim_headless.exe').is_file())
            self.assertTrue((self.package / 'DEBUG-BUILD.md').is_file())

    def test_02_launcher_first_start_reaches_front_end(self):
        result, out = self.launcher('first', [])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = self.user_data
        report = json.loads((data / 'launcher' / 'front-end-report.json').read_text(encoding='utf-8'))
        self.assertTrue(report['front_end_ready'], report)
        self.assertEqual(report['current_screen'], 'main_menu')
        self.assertEqual(len(list((data / 'fonts').glob('EmpireAtWar-*.ttf'))), 4)
        self.__class__.font_stamp = sorted((p.name, p.stat().st_mtime_ns) for p in (data / 'fonts').glob('*.ttf'))

    def test_03_second_start_reuses_choice_and_fonts(self):
        result, out = self.launcher('second', [])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = self.user_data
        self.assertEqual(sorted((p.name, p.stat().st_mtime_ns) for p in (data / 'fonts').glob('*.ttf')),
                         getattr(self, 'font_stamp', None))

    def test_04_package_folder_is_unchanged_after_runs(self):
        listing = sorted(p.relative_to(self.package).as_posix() for p in self.package.rglob('*') if p.is_file())
        sums = {l.split('  ', 1)[1] for l in (self.package / 'SHA256SUMS').read_text(encoding='utf-8').splitlines()}
        self.assertEqual(set(listing) - {'SHA256SUMS'}, sums, 'runs wrote nothing into the package folder')

    def viewer(self, name, args, ticks_frames=None):
        out = self.output / name
        out.mkdir(parents=True, exist_ok=True)
        report = out / 'report.json'
        command = [str(self.godot), '--resolution', '1280x720', '--path', str(self.package / 'project')]
        if ticks_frames:
            command += ['--quit-after', str(ticks_frames)]
        command += ['--', '--eawr-game-root', self.game, *args, '--eawr-report', str(report),
                    '--eawr-capture', str(out / 'capture.png'),
                    '--eawr-profile-file', str(out / 'profile.cfg'), '--eawr-settings-file', str(out / 'settings.cfg')]
        result, _ = self.run_process(name, command)
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr)[-4000:])
        self.assertTrue(report.is_file())
        return json.loads(report.read_text(encoding='utf-8'))

    def test_05_setup_and_skirmish_vs_ai(self):
        replay = self.output / 'setup.eawr-replay'
        data = self.viewer('setup', ['--eawr-skirmish-setup', '--eawr-map-effects', 'on', '--eawr-lighting', 'sh',
                                     '--eawr-environment', 'map', '--eawr-shadows', 'on', '--eawr-hud', 'tactical',
                                     '--eawr-setup-test', '--eawr-live-ai', 'on', '--eawr-live-ticks', '900',
                                     '--eawr-live-step', '3', '--eawr-live-replay-out', str(replay),
                                     '--eawr-audio', 'on'])
        live = data['live_session']
        self.assertTrue(live['headless_hashes_equal'])
        self.assertTrue(any(not row['human'] for row in live['start_slots']))
        self.assertTrue(live['ai']['players'])
        self.assertTrue(replay.is_file())

    def test_06_ai_fights_back(self):
        replay = self.output / 'combat.eawr-replay'
        data = self.viewer('combat', ['--eawr-live-session', 'skirmish', '--eawr-map', 'data/art/maps/_mp_space_kessel.ted',
                                      '--eawr-populate', '--eawr-skirmish-slot', '1:Rebel:0:human',
                                      '--eawr-skirmish-slot', '2:Empire:1:ai', '--eawr-live-ai', 'on',
                                      '--eawr-live-ticks', '9000', '--eawr-live-step', '10',
                                      '--eawr-live-replay-out', str(replay), '--eawr-lighting', 'sh',
                                      '--eawr-environment', 'map', '--eawr-shadows', 'on', '--eawr-audio', 'off'])
        live = data['live_session']
        self.assertTrue(live['headless_hashes_equal'])
        from test_skirmish_setup import SkirmishSetupGpu
        orders = SkirmishSetupGpu._command_players(replay)
        self.assertGreater(orders.get(2, 0), 0, 'AI issued no commands')
        self.assertGreater(sum(data['battle_effects']['hit_events'].values()), 0, 'no weapon hits observed')
        (self.output / 'combat-audit.json').write_text(json.dumps({'orders': orders,
            'hit_events': data['battle_effects']['hit_events']}, indent=2) + '\n', encoding='utf-8')

    def test_07_lan_online_selector_has_online_play(self):
        args = ['--eawr-lan-lobby', '--eawr-lan-name', 'Release smoke', '--eawr-lan-game-port', '0',
                '--eawr-lan-fake', 'connected']
        for action in ('snapshot', 'shot:lan', 'source:online', 'snapshot', 'shot:online', 'source:lan', 'snapshot',
                       'cancel'):
            args += ['--eawr-lan-do', action]
        data = self.viewer('lobby', args)
        self.assertEqual(data['status'], 'ok')
        self.assertEqual(data['failure'], '')
        self.assertTrue(all(row.endswith(' ok') for row in data['actions']), data['actions'])
        self.assertTrue(data['closed'])

    def helper_run(self, args, **kwargs):
        original = self._original_run
        if isinstance(args, (list, tuple)) and args and Path(str(args[0])).resolve() == self.rig_godot:
            args = list(args)
            args[args.index('--path') + 1] = str(self.package / 'project')
            kwargs['cwd'] = self.package
            kwargs['env'] = self.env
            kwargs.setdefault('timeout', 900)
            result = original(args, **kwargs)
            self.records.append({'name': 'renderer-helper', 'command': [str(a) for a in args],
                                 'exit_code': result.returncode})
            return result
        return original(args, **kwargs)

    def helper_popen(self, args, *positional, **kwargs):
        # The registered battle-input support starts the viewer with Popen; run it against the packaged project.
        original = self._original_popen
        if isinstance(args, (list, tuple)) and args and Path(str(args[0])).resolve() == self.rig_godot:
            args = list(args)
            args[0] = str(self.godot)
            args[args.index('--path') + 1] = str(self.package / 'project')
            kwargs['cwd'] = self.package
            requested = kwargs.get('env')
            overrides = {} if requested is None else {k: v for k, v in requested.items() if os.environ.get(k) != v}
            kwargs['env'] = {**self.env, **overrides}
            self.records.append({'name': 'renderer-helper-popen', 'command': [str(a) for a in args]})
        return original(args, *positional, **kwargs)

    def helper_case(self, case):
        self._original_run = subprocess.run
        self._original_popen = subprocess.Popen
        # The registered battle-input support bounds a case at 300 s; the Debug build is not performance-representative
        # and needs the same measured budget as the Debug subprocess limit. Assertions are unchanged.
        import battle_input_test_support as support
        budget = 2400 if self.kind == 'debug' else support.CASE_TIMEOUT_SECONDS
        with patch('subprocess.run', self.helper_run), patch('subprocess.Popen', self.helper_popen), \
                patch.object(support, 'CASE_TIMEOUT_SECONDS', budget):
            case.debug()

    def test_08_buy_and_place_reinforcement(self):
        from test_battle_input import BattleInputGraphical
        self.helper_case(BattleInputGraphical('test_station_buys_a_squadron_that_arrives'))

    def test_09_capture_build_and_mine_income(self):
        from test_pad_capture import MiningPadCaptureGraphical
        self.helper_case(MiningPadCaptureGraphical(
            'test_map_hazard_does_not_contest_capture_or_block_the_build_palette'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
