"""Rig-only first-open measurement; preserves process logs for A/B review."""
import os
import json
import subprocess
from pathlib import Path
import unittest
import test_front_end as front

DRIVER = r"""extends SceneTree
var scene
var phase = 0
var start = 0
var times = []
var pending = false
var elapsed = 0.0
func _initialize():
    scene = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(scene)
    current_scene = scene
func visible_named(name):
    for node in scene.find_children(name, "", true, false):
        if node is CanvasItem and node.is_visible_in_tree(): return node
    return null
func click(node):
    var point = node.get_global_rect().get_center()
    for down in [true, false]:
        var event = InputEventMouseButton.new()
        event.position = point
        event.global_position = point
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = down
        Input.parse_input_event(event)
func after_draw():
    await RenderingServer.frame_post_draw
    times.append((Time.get_ticks_usec() - start) / 1000.0)
    print("PF8312 draw phase ", phase, " ms ", times[-1])
    pending = false
    phase += 1
func _process(delta):
    elapsed += delta
    if elapsed > 90: print("PF8312 timeout phase ", phase, " pending ", pending); quit(2); return false
    if pending or elapsed < WAIT_SECONDS: return false
    if phase == 0 or phase == 4:
        var button = visible_named("IDC_OPTIONS")
        if button != null:
            start = Time.get_ticks_usec()
            click(button)
            print("PF8312 clicked phase ", phase)
            phase += 1
    elif phase == 1 or phase == 5:
        if visible_named("IDD_MAIN_OPTIONS_DIALOG") != null:
            pending = true
            after_draw()
    elif phase == 2:
        var back = visible_named("IDC_OK_MAIN")
        if back != null:
            click(back)
            print("PF8312 clicked back")
            phase = 3
    elif phase == 3:
        if visible_named("IDD_MAIN_OPTIONS_DIALOG") == null: phase = 4
    elif phase == 6:
        var file = FileAccess.open(TRACE, FileAccess.WRITE)
        file.store_string(JSON.stringify(times))
        quit()
    return false
"""

@unittest.skipUnless(os.environ.get('EAWR_GODOT_VIEWER_RUNTIME_TEST') and os.environ.get('EAWR_GODOT_EXECUTABLE')
                     and os.environ.get('EAWR_EAW_GAME_ROOT') and os.environ.get('EAWR_REMAKE_MOD_ROOT'),
                     'requires the GPU rig and Remake data')
class SettingsFirstOpenMeasurement(unittest.TestCase):
    def test_main_and_tactical(self):
        os.environ['EAWR_OPTIONS_TIMING'] = '1'
        out = front.ROOT.parent / 'out' / 'tmp' / 'pf8312-measure'
        out.mkdir(parents=True, exist_ok=True)
        gestures = ('f5:click:hud=begin', 'f40:click:hud=options', 'f60:key:ESCAPE',
                    'f80:key:ESCAPE', 'f100:click:hud=menu_options', 'f120:click:hud=menu_back',
                    'f140:click:hud=menu_quit', 'f160:click:hud=menu_no',
                    'f180:click:hud=menu_quit', 'f200:click:hud=menu_yes')
        for profile in ('stock', 'remake'):
            mod = [] if profile == 'stock' else ['--eawr-profile', 'remake', '--eawr-mod-root', os.environ['EAWR_REMAKE_MOD_ROOT']]
            for run in range(3):
                for kind in ('main', 'tactical'):
                    name = f'{profile}-{kind}-{run}'
                    path = out / name
                    path.mkdir(exist_ok=True)
                    common = [*mod, '--eawr-menu-timeline', '--eawr-attract', 'off', '--eawr-audio', 'off']
                    if kind == 'main':
                        driver = path / 'driver.gd'
                        trace = path / 'present.json'
                        driver.write_text(DRIVER.replace('WAIT_SECONDS', '3.0').replace('TRACE', json.dumps(str(trace).replace('\\', '/'))), encoding='utf-8')
                        result = subprocess.run([os.environ['EAWR_GODOT_EXECUTABLE'], '--resolution', '1280x720',
                            '--render-thread', 'separate', '--path', str(front.ROOT / 'apps/viewer/project'),
                            '--script', str(driver), '--', '--eawr-front-end', '--eawr-game-root', os.environ['EAWR_EAW_GAME_ROOT'],
                            *common, '--eawr-settings-file', str(path / 'settings.cfg')],
                            cwd=front.ROOT, text=True, capture_output=True, timeout=600)
                        (path / 'viewer.log').write_text(result.stdout + result.stderr, encoding='utf-8')
                        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr)[-6000:])
                        self.assertEqual(len(json.loads(trace.read_text(encoding='utf-8'))), 2)
                        self.assertGreaterEqual(result.stdout.count('EAWR options timing show:'), 2, result.stdout[-6000:])
                        continue
                    else:
                        args = [*common, '--eawr-front-end-test-battle', '--eawr-report', str(path / 'battle.json'),
                                '--eawr-live-step', '1', '--eawr-live-ticks', '6000', '--eawr-live-ai', 'off',
                                '--eawr-perf-trace', str(path / 'perf.csv')]
                        for gesture in gestures:
                            args += ['--eawr-live-input', gesture]
                    result, _ = front.run_front_end(path, name, args, engine=('--render-thread', 'separate', '--quit-after', '6000'), timeout=600, with_capture=False)
                    self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                    self.assertGreaterEqual(result.stdout.count('EAWR options timing show:'), 2, result.stdout[-6000:])
                    self.assertIn('front_end_ready', result.stdout)

    def test_open_before_prewarm_finishes(self):
        os.environ['EAWR_OPTIONS_TIMING'] = '1'
        path = front.ROOT.parent / 'out' / 'tmp' / 'pf8312-early-open'
        path.mkdir(parents=True, exist_ok=True)
        driver = path / 'driver.gd'
        trace = path / 'present.json'
        driver.write_text(DRIVER.replace('WAIT_SECONDS', '0.05').replace('TRACE', json.dumps(str(trace).replace('\\', '/'))), encoding='utf-8')
        result = subprocess.run([os.environ['EAWR_GODOT_EXECUTABLE'], '--resolution', '1280x720', '--render-thread', 'separate',
            '--path', str(front.ROOT / 'apps/viewer/project'), '--script', str(driver), '--', '--eawr-front-end',
            '--eawr-game-root', os.environ['EAWR_EAW_GAME_ROOT'], '--eawr-attract', 'off', '--eawr-audio', 'off',
            '--eawr-settings-file', str(path / 'settings.cfg')], cwd=front.ROOT, text=True, capture_output=True, timeout=600)
        (path / 'viewer.log').write_text(result.stdout + result.stderr, encoding='utf-8')
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr)[-6000:])
        self.assertEqual(len(json.loads(trace.read_text(encoding='utf-8'))), 2)
        if os.environ.get('EAWR_JOB_EXPECT_FALLBACK') == '1':
            self.assertIn('EAWR options timing fallback', result.stdout)

    def test_settings_changed_after_prewarm_are_read_on_open(self):
        path = front.ROOT.parent / 'out' / 'tmp' / 'pf8312-settings-refresh'
        path.mkdir(parents=True, exist_ok=True)
        settings = path / 'settings.cfg'
        trace = path / 'value.json'
        driver = path / 'driver.gd'
        settings.write_text('eawr-settings 1\naudio.music=10\n', encoding='utf-8')
        script = DRIVER[:DRIVER.index('func _process(delta):')] + r"""func _process(delta):
    elapsed += delta
    if elapsed > 90: quit(2); return false
    if phase == 0 and elapsed > 2.0:
        var file = FileAccess.open(SETTINGS, FileAccess.WRITE)
        file.store_string("eawr-settings 1\naudio.music=90\n")
        file.close()
        phase = 1
    elif phase == 1 and elapsed > 3.0:
        var button = visible_named("IDC_OPTIONS")
        if button != null: click(button); phase = 2
    elif phase == 2:
        var button = visible_named("IDC_MAIN_OPTIONS_AUDIO")
        if button != null: click(button); phase = 3
    elif phase == 3:
        var slider = visible_named("IDC_SLIDER_MUSIC_VOLUME")
        if slider != null:
            var file = FileAccess.open(TRACE, FileAccess.WRITE)
            file.store_string(JSON.stringify(slider.value))
            quit()
    return false
"""
        script = script.replace('SETTINGS', json.dumps(str(settings).replace('\\', '/'))).replace('TRACE', json.dumps(str(trace).replace('\\', '/')))
        driver.write_text(script, encoding='utf-8')
        result = subprocess.run([os.environ['EAWR_GODOT_EXECUTABLE'], '--resolution', '1280x720', '--render-thread', 'separate',
            '--path', str(front.ROOT / 'apps/viewer/project'), '--script', str(driver), '--', '--eawr-front-end',
            '--eawr-game-root', os.environ['EAWR_EAW_GAME_ROOT'], '--eawr-attract', 'off', '--eawr-audio', 'off',
            '--eawr-settings-file', str(settings)], cwd=front.ROOT, text=True, capture_output=True, timeout=600)
        (path / 'viewer.log').write_text(result.stdout + result.stderr, encoding='utf-8')
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr)[-6000:])
        self.assertEqual(json.loads(trace.read_text(encoding='utf-8')), 90)
