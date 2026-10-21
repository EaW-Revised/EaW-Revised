"""Recovery HUD eye check using the production status overlay and UI kit."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from viewer_mode_sources import source_text  # noqa: E402

def function_body(text, signature):
    begin = text.index("{", text.index(signature))
    depth = 1
    end = begin + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[begin + 1:end - 1]


class LanRecoveryWiring(unittest.TestCase):
    """Headless architecture contracts for routes that require Godot to construct."""
    def test_actual_economy_beacon_quit_routes_use_the_live_gate(self):
        # These inline methods remain in the physical class header.
        header = (ROOT / "apps/viewer/src/live_session_view.hpp").read_text(encoding="utf-8")
        economy = (ROOT / "apps/viewer/src/live_session_economy.cpp").read_text(encoding="utf-8")
        frame = source_text("apps/viewer/src/live_session_frame.cpp")
        prepare = (ROOT / "apps/viewer/src/live_session_prepare.cpp").read_text(encoding="utf-8")
        self.assertIn("input_sink_ ? input_sink_.get() : scheduler_.get()", function_body(header, "command_sink()"))
        for method in ("sell_pad_structure", "special_fire", "repair_hardpoint", "buy", "cancel_build", "reinforce"):
            with self.subTest(method=method):
                body = function_body(economy, "LiveSessionView::" + method + "(")
                self.assertIn("issue_economy(command_sink(), intent)", body)
                self.assertNotIn("scheduler_->issue", body)
        self.assertIn("command_sink()->issue(intent)", function_body(header, "bool place_beacon("))
        self.assertIn("command_sink()->issue(intent)", function_body(frame, "LiveSessionView::quit()"))
        self.assertIn("make_unique<LanCommandSink>", prepare)
        self.assertIn("OrderInput>(input_sink_ ? *input_sink_ : *scheduler_)", prepare)
        self.assertIn("producer->present_result(sequence)", function_body(prepare, "set_discard_observer("))

    def test_only_outcome_and_summary_audio_bypass_recovery(self):
        source = (ROOT / "apps/viewer/src/battle_audio_events.cpp").read_text(encoding="utf-8")
        body = function_body(source, "BattleAudio::frame(")
        phase_call = "present_outcome_cues(live, battle, recovery_quiet);"
        self.assertEqual(body.count(phase_call), 1)
        body = body.replace(phase_call, function_body(source, "BattleAudio::present_outcome_cues("))
        quiet = body.index("if (recovery_quiet) return;")
        self.assertLess(body.index("if (!outcome_shown_"), quiet)
        self.assertLess(body.index("if (!summary_shown_"), quiet)
        self.assertLess(quiet, body.index("for (const auto& cue : reinforcement_feedback)"))
        self.assertIn("beacon_cues(battle.latest)", function_body(body, "if (!recovery_quiet) {"))
        declaration = next(line for line in body.splitlines() if "const bool recovery_quiet" in line)
        self.assertNotIn("battle_end", declaration)

    def test_hud_uses_presented_game_time_and_keeps_save_reachable(self):
        source = (ROOT / "apps/viewer/src/map_mode_hud.cpp").read_text(encoding="utf-8")
        body = function_body(source, "MapMode::State::sync_battle_hud()")
        self.assertIn("lan_recovered_notice.advance(view, live_session->presented_tick(), time.paused())", body)
        self.assertIn("lan_incident_message(view)", body)
        self.assertIn('get_node<Node>("LanIncidentSave")', body)
        self.assertIn("retry->set_visible(view.diagnostic_available)", body)
        self.assertIn("retry->is_pressed()", body)
        self.assertIn("save_diagnostic_details()", body)

    def test_controllers_and_hud_read_the_published_adapter(self):
        source = (ROOT / "src/platform/net/lan_match_recovery.cpp").read_text(encoding="utf-8")
        activate = function_body(source, "core::Result<void> TcpLanMatch::activate_adapter(")
        self.assertIn("core_session().active_adapter() != &adapter", activate)
        self.assertIn("adapter.staging->activate()", activate)
        self.assertIn("adapter.bind_live_context(*current_context_", activate)
        self.assertIn("adapter_ = &adapter", activate)
        drive = function_body((ROOT / "src/platform/net/lan_match_drive_session.cpp").read_text(encoding="utf-8"),
                              "DriverResult TcpLanMatch::drive_session(")
        self.assertIn("session.active_adapter()", drive)
        self.assertIn("activate_adapter(*published)", drive)
        frame = source_text("apps/viewer/src/live_session_frame.cpp")
        self.assertIn("session_->frame()", frame)
        platform = (ROOT / "src/platform/live_session.cpp").read_text(encoding="utf-8")
        self.assertIn("frame_ = {snapshot, snapshot, Clock::now()}", platform)
        self.assertIn("event_log_ = LiveEventLog", platform)

    def test_drop_feedback_and_speed_admission(self):
        source = (ROOT / "src/platform/net/lan_match_lifecycle.cpp").read_text(encoding="utf-8")
        self.assertIn("lan_gameplay_enabled(view_)", function_body(source, "bool TcpLanMatch::request_speed("))
        drop = function_body((ROOT / "src/platform/net/lan_match_diagnostics.cpp").read_text(encoding="utf-8"),
                             "void TcpLanMatch::discard_command(")
        self.assertIn("observer(command.key.sequence)", drop)
        self.assertIn("view_.input_feedback", drop)
        gate = source_text("apps/viewer/src/lan_match.hpp")
        gate_issue = function_body(gate, "core::Result<void> issue(")
        self.assertLess(gate_issue.index("acknowledge_input_feedback()"), gate_issue.index("sink_.issue(intent)"))
        frame = source_text("apps/viewer/src/live_session_frame.cpp")
        self.assertIn("if (lan_gameplay_enabled(view)) recovery_input_feedback_.clear()", frame)


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the viewer GPU lane and installed game data")
class LanRecoveryGpu(unittest.TestCase):
    def test_recovery_status_overlays(self):
        with tempfile.TemporaryDirectory(prefix="lan-recovery-") as temporary:
            output = Path(os.environ.get("EAWR_LAN_EYECHECK", temporary)).resolve() / "recovery"
            output.mkdir(parents=True, exist_ok=True)
            details = " Incident at tick 120 Local diagnostic details: out/desync/match-120"
            policy = " Grace 17 s | Progress timeout 23 s | Incident cap 91 s"
            for state, message in (
                ("held", "Connection lost. Reconnecting... Initial grace remaining: 12 s" + policy),
                ("catching_up", "Rejoining game... Verified completed tick: 87" + policy),
                ("desync_paused", "Players' game states differ. Play is paused." + details),
                ("resyncing", "Restoring synchronized game state... Recovery timeout: 19 s"),
                ("recovered", "Game synchronized." + details),
                ("recovery_failed", "Synchronization could not be restored. This match cannot continue." + details),
                ("peer_left", "Opponent left."),
                ("result", "Victory - opponent left."),
                ("loss", "Defeat"),
                ("single_player", "")):
                with self.subTest(state=state):
                    report = output / (state + ".json")
                    capture = output / (state + ".png")
                    result = subprocess.run([
                        os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                        "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-live-session", "m2",
                        "--eawr-map", "data/art/maps/_mp_space_coruscant.ted", "--eawr-populate",
                        *([] if state == "single_player" else ["--eawr-live-lan-test", state]),
                        "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                        "--eawr-report", str(report), "--eawr-capture", str(capture),
                        "--eawr-live-ticks", "1", "--eawr-live-ai", "off", "--eawr-audio", "off",
                        "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
                        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
                    (output / (state + ".log")).write_text(result.stdout, encoding="utf-8")
                    self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                    data = json.loads(report.read_text(encoding="utf-8"))
                    self.assertEqual(data["live_session"]["multiplayer"], state != "single_player")
                    # The report writer flattens the overlay's line break to a space.
                    self.assertEqual(data["live_session"]["lan_message"], message)
                    self.assertTrue(capture.exists())
                    self.assertGreater(capture.stat().st_size, 1000)

    def test_recovery_button_states(self):
        # Frame-indexed input also works while recovery holds the simulation tick.
        # Keep the same scene and compare only the Leave button's rectangle.
        from PIL import Image, ImageChops
        with tempfile.TemporaryDirectory(prefix="lan-button-") as temporary:
            output = Path(os.environ.get("EAWR_LAN_EYECHECK", temporary)).resolve() / "buttons"
            output.mkdir(parents=True, exist_ok=True)
            buttons = []
            for state, gesture in (("normal", "hover:screen=20,120"),
                                   ("hover", "hover:screen=610,466"),
                                   ("pressed", "press:screen=610,466")):
                report = output / (state + ".json")
                capture = output / (state + ".png")
                result = subprocess.run([
                    os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                    "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-live-session", "m2",
                    "--eawr-map", "data/art/maps/_mp_space_coruscant.ted", "--eawr-populate",
                    "--eawr-live-lan-test", "held", "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                    "--eawr-report", str(report), "--eawr-capture", str(capture),
                    "--eawr-live-capture-frames", "30", "--eawr-live-input", "f10:" + gesture,
                    "--eawr-live-ticks", "1", "--eawr-live-ai", "off", "--eawr-audio", "off",
                    "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
                    cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
                (output / (state + ".log")).write_text(result.stdout, encoding="utf-8")
                self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                with Image.open(output / (state + "_f0030.png")) as image:
                    buttons.append(image.convert("RGB").crop((510, 448, 710, 484)))
            self.assertIsNotNone(ImageChops.difference(buttons[0], buttons[1]).getbbox())
            self.assertIsNotNone(ImageChops.difference(buttons[1], buttons[2]).getbbox())

if __name__ == "__main__":
    unittest.main()
