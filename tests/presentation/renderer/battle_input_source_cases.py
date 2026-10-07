"""Cases for test_battle_input; collected by its legacy facade."""

from battle_input_test_support import (
    ACCLAMATOR, ATTACKER_POINT, BattleInputRunner, CAMERA,
    CLEAR_OF_DOGFIGHT, CORUSCANT, CORVETTE, CORVETTE_START,
    DESTINATION, DOGFIGHT_ORDER_TICK, DOGFIGHT_TICK, END_TICK,
    HUD_OFF, LAUNCH_PROBE_TICK, MC80, MC80_ASIDE,
    MEETING, NEBULON, NEBULON_START, OPTIONS_HIT,
    OTHER_TIE_SQUADRON, ROOT, SPOTTER_POINT, SPREAD,
    STAR_BASE, STAR_BASE_POSITION, TARGET_POINT, TARTAN,
    TIE_CRAFT, TIE_SQUADRON, VIEWPORT, X_WING_3_CRAFT,
    X_WING_SQUADRON, X_WING_SQUADRON_3, Y_WINGS, Y_WING_ASIDE,
    Y_WING_SQUADRON, collections, contextlib, decode_png,
    hashlib, inside, json, math,
    os, pathlib, re, read,
    screen, shutil, source_text, strict_json,
    subprocess, sys, tempfile, unittest,
)


class BattleInputSourceCases:
    def test_orders_leave_only_through_order_input(self):
        source = read("apps/viewer/src/battle_input.cpp")
        # UI-07: no direct session access; every order is an OrderInput call.
        for forbidden in ("submit(", "platform::", "tactical/session.hpp", "->take("):
            self.assertNotIn(forbidden, source)
        self.assertIn("input->world_command(pick, ui::CommandOrigin::world_click, ", source)
        self.assertIn("command_click(ui::OrderMode::none, true, live, ui::CommandOrigin::hotkey)", source)
        self.assertIn("input->stop(origin)", source)


    def test_world_layer_goes_before_the_camera(self):
        mode = (ROOT / "apps/viewer/src/map_mode_live.cpp").read_text(encoding="utf-8")
        body = mode[mode.index("void MapMode::input("):]
        self.assertLess(body.index("state.battle->input("), body.index("state.space->camera_event("))


    def test_live_camera_leaves_foc_order_keys_free(self):
        config = read("apps/viewer/project/config/coruscant-live-session-camera.xml")
        self.assertIn('bindings path="space-live-camera-bindings.json"', config)
        bindings = json.loads(read("apps/viewer/project/config/space-live-camera-bindings.json"))
        keys = {binding["control"] for binding in bindings["bindings"] if binding["device"] == "keyboard"}
        # FoC's order keys: A attack, S stop, M move, T attack-move, G guard (#452).
        self.assertFalse(keys & {"A", "S", "M", "T", "G", "W", "D", "Insert"}, keys)
        self.assertTrue({"Left", "Right", "Up", "Down"} <= keys)


    def test_live_camera_follows_the_space_map_camera(self):
        # The live battle camera is FoC's same tactical camera: the space map table without WASD,
        # middle-button law included (#328: plain middle drag pans, Ctrl rotates, a click resets).
        live = json.loads(read("apps/viewer/project/config/space-live-camera-bindings.json"))
        space = json.loads(read("apps/viewer/project/config/space-map-camera-bindings.json"))
        for key in ("edge_scroll", "click_reset", "screen_mouse_units"):
            self.assertEqual(live.get(key), space.get(key), key)
        self.assertNotIn("pan_speed_scale", live)
        wasd = {"W", "A", "S", "D"}
        expected = [row for row in space["bindings"] if not (row["device"] == "keyboard" and row["control"] in wasd)]
        self.assertEqual(live["bindings"], expected)


    def test_live_camera_starts_at_the_space_map_default_distance(self):
        # Project deviations (owner): both space cameras open at distance 1200 (#337/#348), a bit
        # further out than FoC's Distance_Default 1000, of the close-zoom range 100..1900 (#390,
        # FoC's Space_Mode is 200..1900), so zoom 0.611111.
        def zoom(path):
            return float(re.search(r'<initial [^>]*zoom="([0-9.]+)"', read(path)).group(1))

        def overrides(path):
            return re.findall(r'<override tag="([A-Za-z_]+)" value="(-?[0-9.]+)"/>', read(path))
        live = zoom("apps/viewer/project/config/coruscant-live-session-camera.xml")
        self.assertEqual(live, zoom("apps/viewer/project/config/coruscant-space-map-camera.xml"))
        self.assertAlmostEqual(100.0 + live * 1800.0, 1200.0, delta=0.01)
        live_overrides = overrides("apps/viewer/project/config/coruscant-live-session-camera.xml")
        self.assertEqual(live_overrides, overrides("apps/viewer/project/config/coruscant-space-map-camera.xml"))
        self.assertEqual(dict(live_overrides),
                         {"Distance_Min": "100", "Tactical_Min_Scroll_Speed": "823.529412",
                          "Pitch_Min": "-60", "Tactical_Overview_Clicks": "5"})


    def test_world_ui_follows_the_behaviour_note(self):
        note = read("docs/behaviour/foc-battle-world-ui.md")
        self.assertNotRegex(note, r"0x[0-9a-fA-F]{6,}")
        for heading in ("## Selection circle", "## Shield and health bars", "## Squadron icon",
                        "## Dogfight grid", "## Hardpoint reticles", "## Unverified"):
            self.assertIn(heading, note)
        source = read("apps/viewer/src/world_ui_view.cpp")
        # The rules live in the engine-free model; the view resolves art and draws.
        for rule in ("ui::bar_visibility(", "ui::bar_scale(", "ui::bar_level(", "ui::health_bar_colour(",
                     "ui::hardpoint_reticle_tint(", "ui::selection_circle_side(", "ui::bar_anchor_lift(",
                     "ui::bar_candidate(", "icon_grid_.position(", "ui::combat_cell_point(",
                     "ui::hardpoint_reticle_rect(", "ui::hardpoint_reticle_anchor("):
            self.assertIn(rule, source)
        # UI-07: the world UI never reaches the session.
        for forbidden in ("submit(", "platform::", "tactical/session.hpp", "order_input("):
            self.assertNotIn(forbidden, source)


    def test_pad_build_action_shares_the_menu_availability_gate(self):
        # WBP-08/36: only the explicit palette bypasses ordinary SELECTABLE selection.
        source = read("apps/viewer/src/battle_input.cpp")
        commands = read("apps/viewer/src/battle_input_commands.cpp")
        self.assertLess(commands.index("live.pad_action_allowed(*picked)"),
                        commands.index("selection_.click(picked, modifiers"))
        self.assertEqual(source.count("pad_palette_.open("), 1)
        self.assertNotIn("pad_menu_available", source)
        self.assertIn("point != nullptr && point->build_pad) continue", source)
        self.assertIn("ui::update_pad_buttons(", source)
        helper = (ROOT / "apps/viewer/src/live_session_economy.cpp").read_text(encoding="utf-8").split("bool LiveSessionView::pad_action_allowed", 1)[1]
        helper = helper.split("sim::EntityId LiveSessionView::pad_selection_target", 1)[0]
        self.assertIn("tactical::pad_construction_allowed(", helper)
        # RG-04: the menu's shared gate also disables the completed type behind its UC entry.
        self.assertIn("roster_disabled_reason(built->constructed_type)", read("src/skirmish/economy.cpp"))
        self.assertIn("button.enabled = option.available && affordable", read("src/presentation/ui/production.cpp"))


    def test_behaviour_note_marks_what_is_unverified(self):
        note = read("docs/behaviour/foc-battle-selection.md")
        self.assertNotRegex(note, r"0x[0-9a-fA-F]{6,}")
        for heading in ("## Picking", "## Control groups", "## Tactical overview", "## Unverified"):
            self.assertIn(heading, note)
