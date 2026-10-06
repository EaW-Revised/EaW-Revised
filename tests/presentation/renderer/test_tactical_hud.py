"""The tactical space HUD shell (P2-20a, #83; docs/ui/ui-layer.md 1.3).

`--eawr-hud tactical` draws the faction's shell over a map: the faceplate and
help droid, the radar's scan lines as the empty minimap frame, the options
button and the planet name. `--eawr-hud-probe` pushes left clicks through
Godot's real GUI dispatch and reports which reached the world (UI-I1/UI-I2).
`--eawr-ui-gallery --eawr-ui-hud <faction>` draws the same HUD at the window's
size, for 1080-line and ultrawide checks.

The structural half runs everywhere. The graphical half is opt-in: set
EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT.
It extracts the four faces from that install into a temporary cache, which is
deleted after the run; only the reports and captures are kept.
"""

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import source_text  # noqa: E402

SRC = ROOT / "apps/viewer/src"
UI = ROOT / "src/presentation/godot/ui"
TOOL = ROOT / "tools/fonts/extract_eaw_fonts.py"
CAMERA = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
CORUSCANT = "data/art/maps/_mp_space_coruscant.ted"

RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
FACEPLATES = {
    "empire": "i_galactic_dashboard_skirmish.tga",
    "rebel": "i_galactic_dashboard_skirmish_rebel.tga",
    "underworld": "i_galactic_dashboard_skirmish_under.tga",
}


class TacticalHudStructure(unittest.TestCase):
    def test_the_hud_is_wired_into_the_map_mode_the_gallery_and_the_build(self):
        build = source_text("apps/viewer/CMakeLists.txt")
        for source in ("src/map_mode_hud.cpp", "src/presentation/godot/ui/tactical_hud.cpp",
                       "src/presentation/ui/hud_shell.cpp"):
            self.assertIn(source, build)
        self.assertIn("register_tactical_hud_classes()", (SRC / "register_types.cpp").read_text(encoding="utf-8"))
        map_mode = source_text("apps/viewer/src/map_mode.cpp")
        # Both the land path and the space hand-off build the HUD.
        self.assertEqual(map_mode.count("state.build_hud(host, map.context_name)"), 2)
        self.assertIn("state.hud->world_input(event)", map_mode)
        # On by default in a live session (#316), for its local player's faction.
        hud_arguments = (SRC / "map_mode_hud.cpp").read_text(encoding="utf-8")
        self.assertIn('if (mode.empty() && live_session) mode = "tactical";', hud_arguments)
        self.assertIn("live_session->local_faction()", hud_arguments)
        self.assertIn("build_hud_page(", source_text("apps/viewer/src/ui_gallery_mode.cpp"))

    def test_the_shell_comes_from_game_data(self):
        hud = source_text("src/presentation/godot/ui/tactical_hud.cpp")
        for call in ("load_command_bar(", "load_shell_anchors(", "load_mega_texture_atlas(",
                     "hud_view_model(", "load_language_text_database("):
            self.assertIn(call, hud)
        # src/presentation/godot never touches std::filesystem (asset_io_boundary): the
        # viewer mounts the UI-05 font cache and hands the HUD the loaded FontCache.
        self.assertNotIn("load_font_cache(", hud)
        self.assertIn("load_font_cache(", (SRC / "map_mode_hud.cpp").read_text(encoding="utf-8"))
        self.assertIn("EawrUiHitMask", hud)
        for path in (UI / "tactical_hud.cpp", UI / "tactical_hud.hpp", SRC / "map_mode_hud.cpp"):
            text = source_text(path.relative_to(ROOT).as_posix())
            self.assertNotIn(".tga\"", text, path)
            self.assertNotIn(".ttf\"", text, path)

    def test_the_options_button_takes_the_pointer_on_its_mesh_only(self):
        # FoC picks the shell mesh, not the drawn art (docs/ui/ui-layer.md 1.3):
        # the button spans its art quad, but only its component rect (the hit
        # mask's rect) is interactive.
        hud = source_text("src/presentation/godot/ui/tactical_hud.cpp")
        self.assertIn("bool _has_point(const Vector2& point) const override { return hit_rect_.has_point(point); }",
                      hud)
        self.assertIn("setup.options_hit = look.rect;", hud)
        self.assertIn("button->set_hit_rect(", hud)
        for probe in ("options_inside_left", "options_inside_right", "options_overhang_left", "options_overhang_right"):
            self.assertIn(f'"{probe}"', hud)
        # The hit mask's component rects are the same shell anchor rects.
        shell = (ROOT / "src/presentation/ui/hud_shell.cpp").read_text(encoding="utf-8")
        self.assertIn("button.rect = anchor.rect;", shell)

    def test_ability_buttons_follow_the_foc_command_bar(self):
        # #454 (docs/behaviour/foc-ability-buttons.md): the buttons come from the shell's
        # special_button_NN components, draw over the cards and hand their clicks to the battle input.
        build = source_text("apps/viewer/CMakeLists.txt")
        for source in ("src/presentation/ui/ability_buttons.cpp", "src/presentation/godot/ui/ability_buttons_view.cpp"):
            self.assertIn(source, build)
        hud = source_text("src/presentation/godot/ui/tactical_hud.cpp")
        self.assertIn("GDREGISTER_CLASS(EawrAbilityButtons);", hud)
        self.assertIn("abilities.buttons = state.shell.ability_buttons;", hud)
        self.assertIn('constexpr std::string_view ability_stem = "special_button_";',
                      (ROOT / "src/presentation/ui/hud_shell.cpp").read_text(encoding="utf-8"))
        wiring = (SRC / "map_mode_hud.cpp").read_text(encoding="utf-8")
        for call in ("abilities->set_click(", "input->ability_click(index, right, *live)", "battle->set_ability_point("):
            self.assertIn(call, wiring)
        # #530: the cards and the ability bar refresh together in State::sync_cards(), which the
        # tick and the input paths both call.
        self.assertIn("hud->set_ability_bar(battle->ability_bar());", wiring)
        self.assertEqual(source_text("apps/viewer/src/map_mode.cpp").count("state.sync_cards();"), 2)
        note = (ROOT / "docs/behaviour/foc-ability-buttons.md").read_text(encoding="utf-8")
        for rule in ("AB-01", "AB-05", "AB-06", "AB-08", "AB-09", "AB-10", "Interface to the simulation"):
            self.assertIn(rule, note)

    def test_unit_cards_follow_the_foc_command_bar(self):
        # #425: the selection's cards are drawn in the shell's own card slots and a card click goes
        # back to the battle's selection (docs/behaviour/foc-unit-cards.md).
        build = source_text("apps/viewer/CMakeLists.txt")
        for source in ("src/presentation/ui/unit_cards.cpp", "src/presentation/godot/ui/unit_cards_view.cpp"):
            self.assertIn(source, build)
        hud = source_text("src/presentation/godot/ui/tactical_hud.cpp")
        self.assertIn("GDREGISTER_CLASS(EawrUnitCards);", hud)
        self.assertIn("cards.slots = state.shell.card_slots;", hud)
        wiring = (SRC / "map_mode_hud.cpp").read_text(encoding="utf-8")
        for call in ("battle->set_card_slots(cards->slot_count());", "cards->set_click(", "input->card_click(slot, shift, *live)",
                     "battle->set_card_point(", "cards->set_clock("):
            self.assertIn(call, wiring)
        # #530: outside production mode (PU-60) State::sync_cards() shows the selection's cards.
        self.assertIn("hud->set_unit_cards(battle->card_layout(), battle->card_units());", wiring)
        view = (UI / "unit_cards_view.cpp").read_text(encoding="utf-8")
        # FoC's command bar runs a component's left action on the release over it, and on a double click.
        self.assertIn("button->is_double_click()", view)
        self.assertIn("spend_release_", view)
        # Only the slot meshes take the pointer.
        self.assertIn("bool EawrUnitCards::_has_point(const Vector2& point) const { return slot_at(point).has_value(); }",
                      view)
        note = (ROOT / "docs/behaviour/foc-unit-cards.md").read_text(encoding="utf-8")
        self.assertNotRegex(note, r"0x[0-9a-fA-F]{6,}")
        for heading in ("## Layout", "## Clicks", "## Unverified"):
            self.assertIn(heading, note)

    def test_the_minimap_is_drawn_in_the_radar_mesh(self):
        # #455 (docs/behaviour/foc-minimap.md): the minimap model, its Godot control and the live
        # battle's per-frame hand-off.
        build = source_text("apps/viewer/CMakeLists.txt")
        for source in ("src/presentation/ui/minimap.cpp", "src/presentation/godot/ui/minimap_view.cpp"):
            self.assertIn(source, build)
        hud = source_text("src/presentation/godot/ui/tactical_hud.cpp")
        self.assertIn("GDREGISTER_CLASS(EawrMinimap);", hud)
        self.assertIn("minimap.rect = *state.shell.minimap;", hud)
        self.assertIn("state.minimap_fog.advance(", hud)
        self.assertIn("state.sync_minimap();", source_text("apps/viewer/src/map_mode.cpp"))
        wiring = (SRC / "map_mode_hud.cpp").read_text(encoding="utf-8")
        for call in ("hud->set_minimap_handlers(", "battle_space->live_camera_focus(", "input->minimap_move(",
                     "battle->ground_corners(minimap_height.value_or(0.0))",
                     "hud->minimap_looks(name->second).visible", "live.visible_units()"):
            self.assertIn(call, wiring)
        note = (ROOT / "docs/behaviour/foc-minimap.md").read_text(encoding="utf-8")
        for rule in ("MM-01", "MM-04", "MM-09", "MM-10", "MM-11", "MM-13"):
            self.assertIn(rule, note)


def extract_fonts(directory):
    extracted = subprocess.run([sys.executable, str(TOOL), "--game-root", GAME_ROOT, "--out", str(directory),
                                "--allow-unknown-build", "--json"],
                               capture_output=True, text=True, timeout=120, check=False)
    if extracted.returncode != 0:
        raise RuntimeError(extracted.stderr)


def run(test, arguments, report, resolution=None):
    executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
    test.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
    engine = ["--resolution", resolution] if resolution else []
    completed = subprocess.run(
        [executable, *engine, "--path", str(ROOT / "apps/viewer/project"), "--",
         "--eawr-game-root", GAME_ROOT, *arguments, "--eawr-report", str(report)],
        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=600)
    test.assertTrue(report.is_file(), completed.stdout)
    result = json.loads(report.read_text(encoding="utf-8"))
    test.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
    return result, completed.stdout


@unittest.skipUnless(RUNTIME and GAME_ROOT,
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT")
class TacticalHudGraphical(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-tactical-hud-"))
        # A temporary cache, removed after the class, so no font leaves the host.
        cls.cache = pathlib.Path(tempfile.mkdtemp(prefix="eawr-font-cache-"))
        extract_fonts(cls.cache)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.cache, ignore_errors=True)

    def assert_hud_diagnostics(self, hud):
        # #1632: the authored Resume rollover has no texture. Keep every other
        # diagnostic fatal so this accepted warning cannot mask a HUD failure.
        known_resume_warning = "EAWR-UI-0322 [warning] : Resume rollover texture  cannot be drawn"
        self.assertEqual([row for row in hud["diagnostics"] if row != known_resume_warning], [])

    def assert_shell(self, hud, faction):
        self.assert_hud_diagnostics(hud)
        self.assertEqual((hud["faction"], hud["shell_model"]), (faction, "i_tactical_controls.alo"))
        self.assertGreater(hud["faceplate_masks"], 0)
        self.assertEqual(len(hud["meshes"]), 3, hud["meshes"])
        for mesh in hud["meshes"]:
            self.assertTrue(mesh.endswith("(loaded)"), mesh)
        self.assertIn(FACEPLATES[faction], hud["meshes"][1])
        self.assertTrue(hud["meshes"][2].startswith("radar: "))
        self.assertEqual([entry.split(" (")[1] for entry in hud["options_textures"]], ["atlas)"] * 4)
        planet = hud["planet_name"]
        self.assertEqual((planet["text"], planet["source"], planet["text_id"]),
                         ("Coruscant", "text", "TEXT_OBJECT_STAR_SYSTEM_CORUSCANT"))
        self.assertEqual((planet["face"], planet["face_source"]), ("EmpireAtWar-Bold", "cache"))

    def test_the_map_hud_draws_and_routes_clicks(self):
        report = self.directory / "hud-map-rebel.json"
        capture = self.directory / "hud-map-rebel.png"
        result, log = run(self, ["--eawr-map", CORUSCANT, "--eawr-populate", "--eawr-map-camera-config", str(CAMERA),
                                 "--eawr-font-cache", str(self.cache), "--eawr-hud", "tactical",
                                 "--eawr-hud-faction", "rebel", "--eawr-hud-probe", "--eawr-capture", str(capture)],
                          report)
        self.assertTrue(capture.is_file())
        hud = result["hud"]
        self.assert_shell(hud, "rebel")
        # UI-L1/UI-L2 at 1280x720: the shell's origin in the lower-left corner at 720/768.
        self.assertEqual(hud["viewport"], [1280, 720])
        self.assertEqual(hud["placement"], {"left": 0, "bottom": 720, "scale": 0.9375})
        # #349: button art keeps its texture's size around the component's bone:
        # b_option_t's 36 x 25 MT_CommandBar art centred on (217, 19), not its 24 x 24 mesh.
        for value, expected in zip(hud["options_rect"], [186.5625, 690.469, 33.75, 23.4375]):
            self.assertAlmostEqual(value, expected, places=2)
        # Only the 24 x 24 mesh (x 205..229) takes the pointer, as in FoC.
        hit_x, _, hit_width, hit_height = hud["options_hit_rect"]
        for value, expected in zip((hit_x, hit_width, hit_height), [192.1875, 22.5, 22.5]):
            self.assertAlmostEqual(value, expected, places=2)
        # The faceplate's time panel has divider lines in its texture that FoC
        # hides under four opaque buttons; their art covers the whole panel.
        panel = hud["panel_buttons"]
        self.assertEqual([(entry["name"], entry["origin"]) for entry in panel],
                         [("b_droid_help_tactical", "atlas"), ("b_story_arc_t", "atlas"),
                          ("b_play_pause_t", "atlas"), ("b_fast_forward_t", "atlas")])
        for value, expected in zip(panel[2]["rect"], [0.9375, 492.656, 33.75, 34.6875]):
            self.assertAlmostEqual(value, expected, places=2)
        rects = [entry["rect"] for entry in panel]
        for py in range(470, 527):
            for px in range(2, 56):
                self.assertTrue(any(x <= px <= x + w and y <= py <= y + h for x, y, w, h in rects), (px, py))
        self.assertEqual(hud["minimap_rect"], [13.5938, 546.094, 164.062, 164.062])
        # #455: the minimap control fills the radar mesh; without a battle it draws its backdrop grid
        # only, and the probe's minimap click is its one camera look.
        minimap = hud["minimap"]
        for value, expected in zip(minimap["rect"], hud["minimap_rect"]):
            self.assertAlmostEqual(value, expected, places=2)
        self.assertTrue(minimap["backdrop_drawn"], minimap)
        self.assertEqual((minimap["blips"], minimap["looks"], minimap["moves"]), (0, 1, 0), minimap)
        # Section 1.4: the rig measured the planet name's centre 495.5 px from the top.
        x, y, width, height = hud["planet_rect"]
        self.assertAlmostEqual(y + height / 2, 495.94, places=1)
        self.assertEqual(hud["planet_pixels"], 15)
        # UI-I1/UI-I2 through Godot's dispatch: the button and opaque art stop a
        # click, the sky and the transparent gap in the shell pass it on.
        probe = hud["probe"]
        self.assertTrue(probe["ran"])
        clicks = {click["name"]: click for click in probe["clicks"]}
        self.assertEqual(set(clicks), {"options", "sky", "minimap", "faceplate_art", "faceplate_gap",
                                       "options_inside_left", "options_inside_right",
                                       "options_overhang_left", "options_overhang_right"})
        expected = {"options": (True, 0, 1), "sky": (False, 1, 0), "minimap": (True, 0, 0),
                    "faceplate_art": (True, 0, 0), "faceplate_gap": (False, 1, 0),
                    "options_inside_left": (True, 0, 1), "options_inside_right": (True, 0, 1)}
        for name, (hit, world, button) in expected.items():
            click = clicks[name]
            self.assertEqual((click["hud_hit"], click["world"], click["button"]), (hit, world, button), click)
        # The art's overhang is not the button: the click goes where the hit mask
        # sends it (opaque faceplate stops it, a transparent pixel passes it on).
        for name in ("options_overhang_left", "options_overhang_right"):
            click = clicks[name]
            self.assertEqual(click["button"], 0, click)
            self.assertEqual(click["world"], 0 if click["hud_hit"] else 1, click)
        self.assertEqual(hud["options_presses"], 3)
        # #459: pause and fast forward are live toggle buttons with their four atlas textures; on a
        # map without a battle they are idle. The mesh (not the art) takes the pointer.
        time_panel = hud["time_panel"]
        self.assertEqual(len(time_panel["textures"]), 6, time_panel["textures"])
        for entry in time_panel["textures"]:
            self.assertTrue(entry.endswith("(atlas)"), entry)
        for name in ("pause", "fast_forward"):
            button = time_panel[name]
            self.assertEqual((button["pressed"], button["disabled"], button["presses"]), (False, False, 0))
        for value, expected in zip(time_panel["pause"]["rect"], panel[2]["rect"]):
            self.assertAlmostEqual(value, expected, places=2)
        # #453: no battle, no message.
        self.assertEqual(hud["battle_overlay"]["message"], None)
        self.assertEqual(hud["battle_overlay"]["end_panel"], None)
        self.assertNotIn("ERROR:", log)

    def test_every_faction_has_its_faceplate(self):
        for faction in ("empire", "underworld"):
            with self.subTest(faction=faction):
                result, _ = run(self, ["--eawr-ui-gallery", "--eawr-font-cache", str(self.cache),
                                       "--eawr-ui-hud", faction, "--eawr-capture",
                                       str(self.directory / f"hud-{faction}.png")],
                                self.directory / f"hud-{faction}.json", resolution="1280x720")
                self.assertEqual((result["status"], result["page"]), ("captured", "hud"))
                self.assert_shell(result["hud"], faction)

    def test_many_units_stack_on_the_cards(self):
        # #425: 30 made-up units overflow the 24 card slots, so the largest stack (14 X-wing
        # squadrons) collapses into one "x14" card; groups follow UnitAbilityType order and an
        # odd-sized group leaves the rest of its column blank.
        spec = ("Rebel_X-Wing_Squadron*14:SPOILER_LOCK@0.8,Y-Wing_Squadron*9:ION_CANNON_SHOT@0.45,"
                "Corellian_Corvette*4:TURBO@0.6~0.3,Nebulon_B_Frigate*3:DEFEND@0.9~1")
        capture = self.directory / "hud-cards-many.png"
        result, _ = run(self, ["--eawr-ui-gallery", "--eawr-font-cache", str(self.cache), "--eawr-ui-hud", "rebel",
                               "--eawr-ui-hud-cards", spec, "--eawr-capture", str(capture)],
                        self.directory / "hud-cards-many.json", resolution="1280x720")
        self.assertTrue(capture.is_file())
        hud = result["hud"]
        self.assert_hud_diagnostics(hud)
        cards = hud["unit_cards"]
        self.assertEqual((cards["slots"], cards["borders"]), (24, 12))
        drawn = {card["slot"]: card for card in cards["drawn"]}
        self.assertEqual(sorted(drawn), [0, 1, 2, 4, 5, 6, 7, *range(8, 17), 18])
        self.assertEqual([drawn[slot]["type"] for slot in (0, 4, 8, 18)],
                         ["Nebulon_B_Frigate", "Corellian_Corvette", "Y-Wing_Squadron", "Rebel_X-Wing_Squadron"])
        self.assertEqual((drawn[18]["stacked"], drawn[18]["count"]), (True, 14))
        self.assertFalse(any(card["stacked"] for slot, card in drawn.items() if slot != 18))
        self.assertEqual([drawn[slot]["health_level"] for slot in (0, 4, 8)], [9, 6, 5])
        for card in drawn.values():
            self.assertTrue(card["icon_drawn"], card)
        self.assertEqual(drawn[0]["icon"], "i_button_escort_frigate.tga")
        # A column is two slots: slot 1 sits under slot 0, slot 2 one column right.
        rects = cards["slot_rects"]
        self.assertAlmostEqual(rects[0][0], rects[1][0], places=2)
        self.assertGreater(rects[1][1], rects[0][1])
        self.assertGreater(rects[2][0], rects[0][0] + rects[0][2] * 0.9)

    def test_mixed_ship_types_keep_separate_stacks(self):
        # Retail FoC: Corvette and Gunboat share TURBO, but Add_Ship_To_Stack keys on type;
        # Y-wing and B-wing squadrons are both bombers with different abilities.
        spec = ("Corellian_Corvette*30:TURBO,Corellian_Gunboat*30:TURBO,"
                "Y-Wing_Squadron:ION_CANNON_SHOT,B-Wing_Squadron:SPOILER_LOCK")
        capture = self.directory / "hud-cards-mixed-types.png"
        result, _ = run(self, ["--eawr-ui-gallery", "--eawr-font-cache", str(self.cache), "--eawr-ui-hud", "rebel",
                               "--eawr-ui-hud-cards", spec, "--eawr-capture", str(capture)],
                        self.directory / "hud-cards-mixed-types.json", resolution="1280x720")
        self.assertTrue(capture.is_file())
        cards = result["hud"]["unit_cards"]
        drawn = {card["slot"]: card for card in cards["drawn"]}
        self.assertEqual(sorted(drawn), [0, 1, 2, 4])
        self.assertEqual([(drawn[slot]["type"], drawn[slot]["count"], drawn[slot]["stacked"])
                          for slot in (0, 1, 2, 4)],
                         [("Corellian_Corvette", 30, True), ("Corellian_Gunboat", 30, True),
                          ("Y-Wing_Squadron", 1, False), ("B-Wing_Squadron", 1, False)])
        self.assertEqual(cards["borders"], 12)  # the shell's border slots, including empty ones
        self.assertNotEqual(drawn[0]["icon"], drawn[1]["icon"])
        self.assertTrue(all(card["icon_drawn"] for card in drawn.values()), drawn)

    def test_card_bar_pixel_heights_match_in_both_rows(self):
        from PIL import Image

        # WSU-60: the atlas bars retain the same pixel height in every row,
        # including the owner's fractional HUD scale at 2574x1399.
        spec = "Nebulon_B_Frigate*2:NONE@1~1,Calamari_Cruiser*2:NONE@1~1"
        for resolution in ("1280x720", "1920x1080", "2574x1399"):
            with self.subTest(resolution=resolution):
                capture = self.directory / f"hud-card-bar-heights-{resolution}.png"
                result, _ = run(self, ["--eawr-ui-gallery", "--eawr-font-cache", str(self.cache),
                                       "--eawr-ui-hud", "rebel", "--eawr-ui-hud-cards", spec,
                                       "--eawr-capture", str(capture)],
                                capture.with_suffix(".json"), resolution=resolution)
                cards = result["hud"]["unit_cards"]["drawn"]
                self.assertEqual([card["slot"] for card in cards], [0, 1, 2, 3])
                with Image.open(capture) as image:
                    pixels = image.convert("RGB")
                    heights = {"health_rect": [], "shield_rect": []}
                    for card in cards:
                        for key in heights:
                            rect = card[key]
                            self.assertTrue(all(value == int(value) for value in rect), rect)
                            x, y, width, height = map(int, rect)
                            # Sample the rendered fill through its centre. Counting
                            # actual coloured rows catches row-dependent rasterisation.
                            column = [pixels.getpixel((x + width // 2, row))
                                      for row in range(y - 2, y + height + 2)]
                            coloured = [g > r * 1.1 and g > b * 1.5 if key == "health_rect"
                                        else b > r * 1.5 and b > g * 1.2
                                        for r, g, b in column]
                            self.assertGreater(sum(coloured), 0, (key, rect, column))
                            heights[key].append(sum(coloured))
                    for key, values in heights.items():
                        self.assertEqual(len(set(values)), 1, (resolution, key, values))
                    self.assertEqual(heights["health_rect"], heights["shield_rect"])

    def test_the_hud_scales_to_1080_lines(self):
        result, _ = run(self, ["--eawr-ui-gallery", "--eawr-font-cache", str(self.cache), "--eawr-ui-hud", "rebel",
                               "--eawr-capture", str(self.directory / "hud-1080.png")],
                        self.directory / "hud-1080.json", resolution="1920x1080")
        hud = result["hud"]
        self.assert_shell(hud, "rebel")
        # A window as high as the desktop gets a clamped client (1920x1061 on the
        # rig, docs/ui/ui-layer.md 1.4); the HUD lays out for the viewport it has.
        width, lines = hud["viewport"]
        self.assertEqual(width, 1920)
        self.assertTrue(1040 <= lines <= 1080, lines)
        # D4: a client wider than 16:9 (1920x1061) centres a 16:9 safe area, 17 px in.
        self.assertAlmostEqual(hud["placement"]["left"], max(0.0, (width - lines * 16 / 9) / 2), places=3)
        self.assertAlmostEqual(hud["placement"]["scale"], lines / 768, places=4)
        # UI-F1: EaW-Bold 10 pt is 23 px at 1080 and at 1061 lines.
        self.assertEqual(hud["planet_pixels"], (96 * lines // 600) * 10 // 72)
        x, y, width, height = hud["planet_rect"]
        self.assertAlmostEqual(y + height / 2, lines - 239.0 * lines / 768, places=1)


if __name__ == "__main__":
    unittest.main()
