"""Front end shell: the main menu, its enable states and the way into and out of the setup screen.

Opt-in GPU case (docs/behaviour/front-end-shell.md): `--eawr-front-end-test` drives the real
controls with pointer events and logs what each screen shows. Run through a GPU lane with the
installed game; set EAWR_REMAKE_MOD_ROOT for the mod case.
"""

import json
import math
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

MENU_LINE = re.compile(r"EAWR front end menu (\w+):(.*)")
SCREEN_LINE = re.compile(r"EAWR front end screen: (\w+)")
SETUP_LINE = re.compile(r"EAWR front end setup: visible (\w+); lan (\w+)")
TEXT_LINE = re.compile(
    r"EAWR front end text scale: (\w+); viewport (\d+)x(\d+); scale ([\d.]+); (\S+) rect (-?\d+) (-?\d+) (\d+) (\d+)")
BEHIND_LINE = re.compile(
    r"EAWR front end behind (\w+): scene (\w+); sampled (\d+); differing (\d+); distinct (\d+)")
ATTRACT_LINE = re.compile(
    r"EAWR front end attract (\w+): running true; units (\d+); hud (\w+); input (\w+); camera_updates (\d+); "
    r"eye (\S+) (\S+) (\S+); first_eye (\S+) (\S+) (\S+)")


def run_front_end(output, name, extra=(), engine=(), timeout=600, with_capture=True, resolution="1280x720", scripted=True):
    capture = output / (name + ".png")
    capture_arguments = ["--eawr-capture", str(capture)] if with_capture else []
    try:
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", resolution, *engine,
            "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-front-end",
            *(["--eawr-front-end-test"] if scripted else []),
            "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"], *capture_arguments, *extra],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        # Keep what the run printed: a hang is read from its log.
        partial = error.stdout.decode("utf-8", "replace") if isinstance(error.stdout, bytes) else (error.stdout or "")
        (output / (name + ".log")).write_text(partial, encoding="utf-8")
        raise AssertionError("the run did not finish; log tail:\n" + partial[-4000:])
    (output / (name + ".log")).write_text(result.stdout, encoding="utf-8")
    return result, capture


def text_probes(log):
    """label -> (viewport width, height, scale, gadget id, (x, y, w, h)) of the text-sharpness probes."""
    found = {}
    for label, width, height, scale, gadget, x, y, w, h in TEXT_LINE.findall(log):
        found[label] = (int(width), int(height), float(scale), gadget, (int(x), int(y), int(w), int(h)))
    return found


def caption_edge_contrast(image, rect, scale=1.0):
    """The 99th percentile of the strongest adjacent-pixel luminance step in the middle of a button's
    rect (its caption, not the skin's frame), in 0..255. Glyph edges drawn at the pixels they land on
    keep their step; the same glyphs stretched from a smaller raster spread it over several pixels."""
    x, y, width, height = (value * scale for value in rect)
    centre_x, centre_y = x + width / 2.0, y + height / 2.0
    half_x, half_y = width * 0.3, height * 0.25
    crop = image.convert("L").crop((int(centre_x - half_x), int(centre_y - half_y),
                                    int(centre_x + half_x), int(centre_y + half_y)))
    columns, rows = crop.size
    pixels = crop.tobytes()
    steps = []
    for row in range(rows - 1):
        for column in range(columns - 1):
            here = pixels[row * columns + column]
            steps.append(max(abs(pixels[row * columns + column + 1] - here),
                             abs(pixels[(row + 1) * columns + column] - here)))
    steps.sort()
    return float(steps[int(0.99 * (len(steps) - 1))])


def menus(log):
    """label -> {gadget id: state} for every logged menu."""
    found = {}
    for label, rest in MENU_LINE.findall(log):
        found[label] = dict(item.split("=", 1) for item in rest.split())
    return found


class MenuMusicContract(unittest.TestCase):
    def test_menu_updates_preserve_music_and_battle_exit_starts_it(self):
        source = (ROOT / "apps/viewer/src/front_end_mode.cpp").read_text(encoding="utf-8")
        process = source.split("std::optional<int> FrontEndMode::process() {", 1)[1]
        music = process.split("state.audio->set_music(", 1)[1].split(");", 1)[0]
        for screen in ("load_game", "options", "campaign_setup"):
            self.assertIn("current == model::FrontEndScreen::" + screen, music)
        self.assertIn("is_menu(current)", music)
        self.assertIn("state.setup->visible()", music)
        self.assertNotIn("restart_music", source)
        set_music = source.split("void set_music(const bool on) {", 1)[1].split("void play(", 1)[0]
        self.assertLess(set_music.index("if (on == music_on_) return;"), set_music.index("start_music();"))
        self.assertIn("music_player_->stop();", set_music)

    def test_menu_open_starts_an_ended_non_looping_event_once(self):
        # A mod can turn Loop off. The player may stop while music_on_ is still true:
        # event requests must check the player, while frame updates retain their on/off guard.
        source = (ROOT / "apps/viewer/src/front_end_mode.cpp").read_text(encoding="utf-8")
        request = source.split("void start_music() {", 1)[1].split("void play(", 1)[0]
        self.assertLess(request.index("if (music_player_->is_playing()) return;"),
                        request.index("music_player_->play();"))
        self.assertNotIn("music_on_", request)
        self.assertNotIn("music_player_->stop()", request)
        self.assertIn("stream->set_loop(music_.loop);", request)
        show = source.split("void FrontEndMode::State::show_screen() {", 1)[1].split(
            "void FrontEndMode::State::", 1)[0]
        self.assertLess(show.index("if (shown && *shown == screen) return;"), show.index("audio->start_music();"))
        self.assertIn("is_menu(screen) || screen == model::FrontEndScreen::campaign_setup", show)


def music_phases(log):
    return [(screen, playing.lower() == "true", float(position)) for screen, playing, position in re.findall(
        r"EAWR front end music phase: (\w+); playing (\w+); position (\S+)", log)]


class CrispTextContract(unittest.TestCase):
    def test_scaled_roots_rasterise_text_at_the_drawn_size(self):
        # #3176: the menus are laid out on a 1280x720 canvas and scaled by a node transform. Without
        # oversampling the glyphs are rasterised at the canvas size and stretched.
        enabled = "set_oversampling_with_scale(CanvasItem::OVERSAMPLING_WITH_SCALE_ENABLED)"
        for path, root in (("apps/viewer/src/front_end_mode.cpp", "root"),
                           ("apps/viewer/src/skirmish_setup_mode.cpp", "root"),
                           ("apps/viewer/src/map_mode_hud.cpp", "lan_overlay")):
            source = (ROOT / path).read_text(encoding="utf-8")
            self.assertIn(root + "->" + enabled, source, path)
            self.assertIn("fit_letterbox_canvas(*", source, path)
        helper = (ROOT / "src/presentation/godot/ui/kit.cpp").read_text(encoding="utf-8")
        self.assertIn("canvas.set_scale(Vector2(scale, scale));", helper)
        self.assertIn("refresh_text_oversampling(canvas);", helper)
        for path in ("kit_buttons.cpp", "kit_controls.cpp", "kit_text.cpp"):
            source = (ROOT / "src/presentation/godot/ui" / path).read_text(encoding="utf-8")
            self.assertIn("track_oversampling(*this);", source, path)
        # Every kit control that draws text keeps its level when it is shown after a resize.
        controls = sum((ROOT / "src/presentation/godot/ui" / path).read_text(encoding="utf-8")
                       .count("track_oversampling(*this);")
                       for path in ("kit_buttons.cpp", "kit_controls.cpp", "kit_text.cpp"))
        self.assertEqual(controls, 6)


class EdgeContrastMetric(unittest.TestCase):
    def test_a_stretched_raster_loses_contrast_and_a_redrawn_one_keeps_it(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("requires Pillow")
        # One-pixel light strokes on dark, as a small caption's stems are.
        image = Image.new("L", (200, 40), 30)
        for x in range(60, 140, 3):
            for y in range(12, 28):
                image.putpixel((x, y), 230)
        rect = (40, 4, 120, 32)
        base = caption_edge_contrast(image, rect)
        self.assertGreaterEqual(base, 190.0)
        for scale in (1.5, 2.0):
            size = (round(200 * scale), round(40 * scale))
            stretched = caption_edge_contrast(image.resize(size, Image.BILINEAR), rect, scale)
            self.assertLess(stretched / base, 0.7, scale)
        # Strokes drawn again at twice the size, two pixels wide: the step is the same.
        doubled = Image.new("L", (400, 80), 30)
        for x in range(120, 280, 6):
            for y in range(24, 56):
                for offset in (0, 1):
                    doubled.putpixel((x + offset, y), 230)
        self.assertGreaterEqual(caption_edge_contrast(doubled, rect, 2.0) / base, 0.95)


class MenuVolumeContract(unittest.TestCase):
    """#3165: the sliders reach the front end's sound through the run's shared audio buses."""

    def test_apply_audio_without_a_battle_opens_the_shared_buses(self):
        store = (ROOT / "apps/viewer/src/settings_store.cpp").read_text(encoding="utf-8")
        apply = store.split("void apply_audio(", 1)[1].split("void apply_video(", 1)[0]
        self.assertNotIn("no battle buses", apply)
        self.assertIn("audio_buses::ensure(mix_levels(audio));", apply)
        self.assertIn('report.applied.emplace_back("audio: "', apply)

    def test_battle_and_front_end_share_one_layout(self):
        buses = (ROOT / "apps/viewer/src/audio_buses.cpp").read_text(encoding="utf-8")
        battle = (ROOT / "apps/viewer/src/battle_audio.cpp").read_text(encoding="utf-8")
        self.assertIn("audio_buses::open(&captures)", battle)
        self.assertNotIn("add_bus()", battle.split("for (MusicTrack& track", 1)[0].split("EAWR_Pan_", 1)[0])
        self.assertEqual(buses.count("add_bus()"), 1)
        ready = (ROOT / "apps/viewer/src/viewer_host_ready.cpp").read_text(encoding="utf-8")
        self.assertIn("audio_buses::ensure(", ready)

    def test_menu_music_and_clicks_play_on_the_slider_buses(self):
        source = (ROOT / "apps/viewer/src/front_end_mode.cpp").read_text(encoding="utf-8")
        menu = source.split("class MenuAudio final {", 1)[1].split("void click(Vector2 position)", 1)[0]
        self.assertIn("music_player_->set_bus(audio_buses::names[audio_buses::music]);", menu)
        self.assertIn("player->set_bus(audio_buses::names[audio_buses::sfx]);", menu)
        self.assertIn("audio_buses::voice : audio_buses::sfx", menu)
        # The sliders are the buses' alone: no second copy of the master and music gain on the player.
        self.assertNotIn("levels.master", menu)
        self.assertNotIn("category_gain", menu)


class SetupEscapeContract(unittest.TestCase):
    def test_return_arms_escape_until_release(self):
        # The live-input harness emits key press and release in the same call, so it cannot
        # hold Escape across show(). Check the return seam and its edge detector directly.
        source = (ROOT / "apps/viewer/src/skirmish_setup_mode.cpp").read_text(encoding="utf-8")
        show = source.split("void SkirmishSetupMode::show(std::string message) {", 1)[1].split(
            "SkirmishSetupMode::UiResources", 1)[0]
        reset = show.index("state_->escape_was_down = true;")
        self.assertLess(reset, show.index("state_->visible = true;"))
        self.assertNotRegex(show[reset:], r"escape_was_down\s*=\s*false")
        poll = source.split("void SkirmishSetupMode::State::process_back", 1)[1].split(
            "SkirmishSetupMode::SkirmishSetupMode", 1)[0]
        edge = re.search(r"if \(cancel_routed && down && !escape_was_down.*\) cancel_requested = true;", poll)
        self.assertIsNotNone(edge)
        self.assertLess(edge.end(), poll.index("escape_was_down = down;"))


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the viewer GPU lane and installed game data")
class FrontEndGpu(unittest.TestCase):
    def test_front_end_writes_its_own_report_on_both_quit_routes(self):
        # The engine-driven preview and the menu's Exit both report to the same UTF-8 path flag.
        with tempfile.TemporaryDirectory(prefix="front-end-report-") as temporary:
            output = Path(temporary).resolve()
            for scripted, reason in ((True, "clean_quit"), (False, "quit_after")):
                with self.subTest(exit_reason=reason):
                    report = output / "report-\U0001f30c-\u03b1" / (reason + ".json")
                    result, _ = run_front_end(output, reason, ("--eawr-report", str(report)),
                                              engine=() if scripted else ("--quit-after", "90"),
                                              with_capture=False, scripted=scripted)
                    self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                    data = json.loads(report.read_text(encoding="utf-8"))
                    self.assertEqual(data["schema_version"], 1)
                    self.assertEqual(data["status"], "ok")
                    self.assertEqual(data["failure"], "")
                    self.assertIs(data["front_end_ready"], True)
                    self.assertEqual(data["current_screen"], "main_menu")
                    self.assertIs(data["attract_started"], True)
                    self.assertIs(data["attract_failed"], False)
                    self.assertEqual(data["attract_failure"], "")
                    self.assertIs(data["music_playing"], True)
                    self.assertGreater(data["frames"], 0)
                    self.assertIsInstance(data["problems"], list)
                    self.assertTrue(all(isinstance(problem, str) for problem in data["problems"]))
                    self.assertEqual(data["exit_reason"], "clean_quit" if scripted else "engine_shutdown")

    def test_online_service_menu_routes_open_network_options_and_online_lobby(self):
        with tempfile.TemporaryDirectory(prefix="front-end-online-service-") as temporary:
            output = Path(temporary).resolve()
            result, _ = run_front_end(output, "online-service-routes",
                                      ("--eawr-front-end-test-online", "--eawr-settings-file", str(output / "settings.cfg"),
                                       "--eawr-attract", "off"))
            self.assertEqual(result.returncode, 0, result.stdout[-6000:])
            self.assertIn("EAWR online service lobby source: online", result.stdout)
            self.assertIn("EAWR online service network page: network", result.stdout)
            self.assertIn("EAWR online service options page: network", result.stdout)
            self.assertNotIn("ERROR:", result.stdout)

    def test_front_end_unwritable_report_exits_two_on_both_quit_routes(self):
        with tempfile.TemporaryDirectory(prefix="front-end-report-failure-") as temporary:
            output = Path(temporary).resolve()
            blocked = output / "parent-is-a-file"
            blocked.write_text("blocks directory creation", encoding="utf-8")
            for scripted in (True, False):
                with self.subTest(scripted=scripted):
                    result, _ = run_front_end(output, "unwritable-" + str(scripted),
                                              ("--eawr-report", str(blocked / "report.json"),
                                               "--eawr-attract", "off", "--eawr-audio", "off"),
                                              engine=() if scripted else ("--quit-after", "30"),
                                              with_capture=False, scripted=scripted)
                    self.assertEqual(result.returncode, 2, result.stdout[-6000:])
                    self.assertIn("EAWR front end report output could not be", result.stdout)

    def check_flow(self, result, capture, stock=True):
        """`stock`: every FoC entry must be there. A mod's dialogs may drop entries (the Remake has
        no campaign button and its main menu is rejected, so the fallback column stands in): the
        entries that are there still follow the rules and the core flow still works."""
        log = result.stdout
        self.assertEqual(result.returncode, 0, log[-6000:])
        self.assertIn("EAWR front end ready", log)
        screens = SCREEN_LINE.findall(log)
        self.assertEqual(screens, [
            "main_menu", "single_player", "skirmish_setup", "single_player", "main_menu",
            "multiplayer", "lan_setup", "multiplayer", "main_menu", "options", "main_menu",
            "load_game", "main_menu", "single_player",
            *(("campaign_setup", "single_player") if stock else ()), "main_menu"], screens)
        states = menus(log)

        def expect(menu, entry, wanted, required):
            if entry in states[menu] or required:
                self.assertEqual(states[menu].get(entry), wanted, menu + " " + entry)

        main, single, multi = "main_menu", "single_player", "multiplayer"
        for entry in ("IDC_MAIN_MENU_SINGLE_PLAYER_GAMES", "IDC_MAIN_MENU_MULTIPLAYER", "IDC_OPTIONS", "IDC_MAIN_MENU_EXIT2", "IDC_LOAD_GAME_MAIN"):
            expect(main, entry, "enabled", True)
        for entry in ("IDC_MAIN_MENU_TUTORIAL", "IDC_MENU_CREDITS"):
            expect(main, entry, "disabled", stock)
        expect(main, "IDC_MAIN_MENU_PLAY_DEMO", "hidden", stock)
        expect(main, "IDC_MENU_PETRO_LOGO", "hidden", stock)
        expect(single, "IDC_SKIRMISH_GAMES_BUTTON", "enabled", True)
        expect(single, "IDC_BACK_BUTTON_MAIN", "enabled", True)
        expect(single, "IDC_NEW_CAMPAIGN_BUTTON", "disabled", stock)
        # The data lists galactic campaigns, so the custom campaign entry opens its staging screen (FES-13).
        expect(single, "IDC_CUSTOM_CAMPAIGN_BUTTON", "enabled", stock)
        expect(single, "IDC_DEBUG_LOAD_SKIRMISH_MAP_BUTTON", "hidden", stock)
        expect(multi, "IDC_MULTIPLAYER_LAN_BUTTON", "enabled", True)
        expect(multi, "IDC_MULTIPLAYER_INTERNET_BUTTON", "enabled", stock)
        setups = SETUP_LINE.findall(log)
        self.assertEqual([(visible.lower(), lan.lower()) for visible, lan in setups],
                         [("true", "false"), ("true", "true")], setups)
        self.assertIn("EAWR front end quit", log)
        for label in ("main_menu", "single_player", "skirmish_setup", "lan_setup", "options"):
            image = capture.parent / (capture.stem + "." + label + ".png")
            self.assertGreater(image.stat().st_size, 10000, label)

    def test_main_menu_to_setup_and_back(self):
        with tempfile.TemporaryDirectory(prefix="front-end-") as temporary:
            output = Path(os.environ.get("EAWR_FRONT_END_EYECHECK", temporary)).resolve() / "front-end"
            output.mkdir(parents=True, exist_ok=True)
            result, capture = run_front_end(output, "foc", ("--eawr-lighting", "sh"))
            self.check_flow(result, capture)
            # The main-menu music event plays while the menus show.
            self.assertRegex(result.stdout, r"EAWR front end music: playing \S+")
            self.check_attract_behind_the_menus(result.stdout)
            phases = music_phases(result.stdout)
            self.assertTrue(phases, result.stdout[-6000:])
            self.assertTrue(all(playing for _, playing, _ in phases), phases)
            for screen in ("main_menu", "single_player", "multiplayer", "skirmish_setup", "lan_setup",
                           "options", "load_game", "campaign_setup"):
                self.assertIn(screen, [screen for screen, _, _ in phases])
            positions = [position for _, _, position in phases]
            for before, after in zip(positions, positions[1:]):
                self.assertGreaterEqual(after, before, phases)
            self.assertGreater(positions[-1], positions[0], phases)

    def test_text_is_as_sharp_at_a_large_window_as_at_1280x720(self):
        # #3176: the same screens at 1280x720 and at 2560x1440. Text rasterised at the canvas size and
        # stretched by the root transform loses its edge contrast (a 2x stretch halves the step between
        # glyph and background); text rasterised at the drawn size keeps it. The probes are the first
        # enabled main-menu button, the staging's Start and the LAN Host button, which is hidden
        # until the LAN connection is picked (after the scale was set).
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("requires Pillow")
        with tempfile.TemporaryDirectory(prefix="front-end-sharp-") as temporary:
            output = Path(os.environ.get("EAWR_FRONT_END_EYECHECK", temporary)).resolve() / "front-end-sharp"
            output.mkdir(parents=True, exist_ok=True)
            runs = {}
            for resolution in ("1280x720", "2560x1440"):
                result, capture = run_front_end(output, "sharp-" + resolution,
                                                ("--eawr-attract", "off", "--eawr-audio", "off"),
                                                resolution=resolution)
                self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                runs[resolution] = (text_probes(result.stdout), capture)
            small_probes, small_capture = runs["1280x720"]
            large_probes, large_capture = runs["2560x1440"]
            for label in ("main_menu", "skirmish_setup", "lan_setup"):
                with self.subTest(screen=label):
                    self.assertIn(label, small_probes)
                    self.assertIn(label, large_probes)
                    _, _, small_scale, gadget, small_rect = small_probes[label]
                    width, height, scale, large_gadget, large_rect = large_probes[label]
                    self.assertEqual(gadget, large_gadget)
                    self.assertEqual(small_scale, 1.0)
                    # A host that clamps the window below this cannot show the difference.
                    self.assertGreaterEqual(scale, 1.4, (width, height))
                    with Image.open(small_capture.parent / (small_capture.stem + "." + label + ".png")) as small, \
                            Image.open(large_capture.parent / (large_capture.stem + "." + label + ".png")) as large:
                        self.assertEqual(large.size, (width, height))
                        base = caption_edge_contrast(small, small_rect)
                        self.assertGreater(base, 40.0, "the caption has edges to measure")
                        drawn = caption_edge_contrast(large, large_rect)
                        # What the stretched 1x raster would give at this scale.
                        stretched = small.convert("RGB").resize(
                            (round(small.width * scale), round(small.height * scale)), Image.BILINEAR)
                        blurred = caption_edge_contrast(stretched, small_rect, scale)
                    # A fractional scale (the rig's window is 1924x1061: 1.47) lands some glyphs between
                    # two pixels; the stretched raster is at 0.6 of the contrast, the redrawn one above 0.8.
                    self.assertGreaterEqual(drawn / base, 0.7, (label, base, drawn, blurred))
                    self.assertGreaterEqual(drawn, blurred * 1.2, (label, base, drawn, blurred))

    def check_attract_behind_the_menus(self, log):
        """MAT-01, MAT-14: the demo battle runs behind the main menu and stays up behind the
        single-player menu and the settings screen; no HUD, no world input, a camera that moves."""
        self.assertIn("EAWR front end attract started", log)
        seen = {label: rest for label, *rest in ATTRACT_LINE.findall(log)}
        for label in ("main_menu", "single_player", "options"):
            self.assertIn(label, seen, "the attract runs behind " + label)
            units, hud, battle_input, updates = seen[label][:4]
            # The first sample may still wait for the session's first snapshot at the main menu.
            if label != "main_menu":
                self.assertGreater(int(units), 10, label)
            self.assertEqual((hud.lower(), battle_input.lower()), ("false", "false"), label)
        self.assertRegex(log, r"EAWR front end attract skirmish_setup: running true")
        # #3168: the single-player staging leaves the battle visible behind its controls; the LAN
        # connection (a multiplayer menu) hides it. Read from the rendered frame with the dialog's own
        # controls hidden: the battle's frame differs from the flat backing nearly everywhere, the
        # opaque backing does not.
        behind = {label: (scene.lower(), int(sampled), int(differing), int(distinct))
                  for label, scene, sampled, differing, distinct in BEHIND_LINE.findall(log)}
        self.assertIn("skirmish_setup", behind, log[-4000:])
        scene, sampled, differing, distinct = behind["skirmish_setup"]
        self.assertEqual(scene, "true")
        self.assertGreater(sampled, 1000)
        self.assertGreater(differing / sampled, 0.5, "the battle is visible behind the staging screen")
        self.assertGreater(distinct, 8)
        scene, sampled, differing, _ = behind["lan_setup"]
        self.assertEqual(scene, "false")
        self.assertLess(differing / sampled, 0.02, "the multiplayer staging hides the battle")
        # The camera is alive: by the settings screen its eye left the first update's.
        eye = [float(value) for value in seen["options"][4:7]]
        first = [float(value) for value in seen["options"][7:10]]
        self.assertGreater(int(seen["options"][3]), int(seen["main_menu"][3]))
        self.assertGreater(sum((a - b) ** 2 for a, b in zip(eye, first)) ** 0.5, 0.0, "the camera moved")
        # Exit stops it before the viewer ends, with no leak or crash at shutdown.
        self.assertIn("EAWR front end attract stopped: exit", log)

    def test_saved_volumes_apply_to_the_music_bus_at_start(self):
        # #3165: a player's front end (no capture) takes the saved record: the music plays on the
        # music bus at the saved music level times nothing else, the mix bus at the master level.
        with tempfile.TemporaryDirectory(prefix="front-end-volume-") as temporary:
            output = Path(temporary)
            settings = output / "settings.cfg"
            settings.write_text("\n".join(["eawr-settings 1", "audio.master=40", "audio.music=60", "audio.sfx=20", ""]),
                                encoding="utf-8")
            result, _ = run_front_end(output, "volume", ("--eawr-settings-file", str(settings)), with_capture=False)
            log = result.stdout
            self.assertEqual(result.returncode, 0, log[-6000:])
            line = re.search(r"EAWR front end buses: music_bus=(\S+) sfx_db=(\S+) voice_db=(\S+) music_db=(\S+) mix_db=(\S+)", log)
            self.assertIsNotNone(line, log[-4000:])
            self.assertEqual(line.group(1), "EAWR_Music")
            db = lambda gain: 20.0 * math.log10(gain)
            self.assertAlmostEqual(float(line.group(2)), db(0.20), delta=0.1)
            self.assertAlmostEqual(float(line.group(4)), db(0.60), delta=0.1)
            self.assertAlmostEqual(float(line.group(5)), db(0.40), delta=0.1)
            self.assertRegex(log, r"EAWR front end music: playing \S+")

    def test_start_stops_the_attract_before_the_battle(self):
        with tempfile.TemporaryDirectory(prefix="front-end-start-") as temporary:
            output = Path(temporary).resolve()
            result, _ = run_front_end(output, "start", ("--eawr-front-end-test-start", "--eawr-lighting", "sh"))
            log = result.stdout
            self.assertEqual(result.returncode, 0, log[-6000:])
            self.assertIn("EAWR front end attract started", log, log[-6000:])
            self.assertIn("EAWR front end start pressed: attract running false", log, log[-6000:])
            self.assertIn("EAWR front end attract stopped: game start", log, log[-6000:])

    def test_attract_off_is_the_plain_backdrop(self):
        with tempfile.TemporaryDirectory(prefix="front-end-off-") as temporary:
            output = Path(temporary).resolve()
            result, _ = run_front_end(output, "off", ("--eawr-attract", "off", "--eawr-audio", "off"))
            log = result.stdout
            self.assertEqual(result.returncode, 0, log[-6000:])
            self.assertNotIn("EAWR front end attract started", log)
            self.assertIn("EAWR front end attract main_menu: running false", log)

    def test_quit_with_settings_screen_open(self):
        # The settings screen is a child of the host; quitting while it is open must not free it twice.
        with tempfile.TemporaryDirectory(prefix="front-end-quit-") as temporary:
            output = Path(temporary).resolve()
            result, _ = run_front_end(output, "quit-open", ("--eawr-front-end-test-quit-open", "--eawr-audio", "off"))
            log = result.stdout
            self.assertIn("EAWR front end quit with the settings screen open", log, log[-6000:])
            self.assertEqual(result.returncode, 0, log[-6000:])
            # The attract's battle was running behind the open settings screen.
            self.assertIn("EAWR front end attract started", log, log[-6000:])

    def test_battle_end_continue_returns_to_the_setup_screen(self):
        self.check_battle_end_return("ENTER")

    def test_battle_end_escape_returns_to_the_setup_screen(self):
        self.check_battle_end_return("ESCAPE")

    def check_battle_end_return(self, key):
        # MNU-45: Continue on the battle-end dialog leaves the battle for the skirmish setup screen
        # (retail's staging dialog), with the shell's navigation and music intact, and the viewer
        # still quits cleanly from the main menu afterwards. The Escape case has no button-click
        # fallback, so only its keyboard route can bring the setup screen back.
        with tempfile.TemporaryDirectory(prefix="front-end-battle-") as temporary:
            output = Path(temporary).resolve()
            report = output / "battle.json"
            result, capture = run_front_end(output, "battle", (
                "--eawr-front-end-test-battle", "--eawr-report", str(report),
                "--eawr-live-order", "15:damage:1@1000000", "--eawr-live-order", "15:damage:2@1000000",
                "--eawr-live-step", "3", "--eawr-live-ticks", "300",
                "--eawr-live-input", "f5:click:hud=begin",
                "--eawr-live-input", "225:key:" + key,
                *(("--eawr-live-input", "225:click:hud=quit") if key == "ENTER" else ())),
                engine=("--quit-after", "6000"), timeout=300)
            log = result.stdout
            self.assertEqual(result.returncode, 0, log[-6000:])
            self.assertIn("EAWR front end battle: starting from skirmish_setup", log)
            self.assertRegex(log, r"EAWR front end battle returned: skirmish_setup; setup visible true")
            before_back = log.split("EAWR front end battle music:", 1)[0]
            self.assertNotIn("EAWR front end back:", before_back)
            self.assertEqual(SCREEN_LINE.findall(before_back), ["main_menu", "single_player", "skirmish_setup"])
            # The menu music resumes with the setup screen, and Back / quit still work from there.
            self.assertRegex(log, r"EAWR front end battle music: playing \S+")
            phases = music_phases(log)
            silent = [index for index, (_, playing, _) in enumerate(phases) if not playing]
            self.assertTrue(silent, "music stops during the battle")
            resumed = phases[silent[-1] + 1:]
            self.assertGreaterEqual(len(resumed), 2, phases)
            self.assertTrue(all(playing for _, playing, _ in resumed), resumed)
            self.assertLess(resumed[0][2], phases[silent[0] - 1][2], "fresh playback starts near the beginning")
            self.assertGreater(resumed[-1][2], resumed[0][2], resumed)
            self.assertIn("EAWR front end back: single_player", log)
            self.assertIn("EAWR front end quit", log)
            self.assertNotIn("EAWR front end test:", log)
            self.assertGreater((capture.parent / (capture.stem + ".after_battle.png")).stat().st_size, 10000)

    @unittest.skipUnless(os.environ.get("EAWR_REMAKE_MOD_ROOT"), "requires the Remake mod")
    def test_mod_menu_keeps_working(self):
        with tempfile.TemporaryDirectory(prefix="front-end-mod-") as temporary:
            output = Path(os.environ.get("EAWR_FRONT_END_EYECHECK", temporary)).resolve() / "front-end-mod"
            output.mkdir(parents=True, exist_ok=True)
            result, capture = run_front_end(output, "remake", ("--eawr-mod-root", os.environ["EAWR_REMAKE_MOD_ROOT"],
                                                               "--eawr-audio", "off"))
            self.check_flow(result, capture, stock=False)


if __name__ == "__main__":
    unittest.main()
