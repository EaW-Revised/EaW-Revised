"""Recovery HUD eye check using the production status overlay and UI kit."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

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
        header = (ROOT / "apps/viewer/src/live_session_view.hpp").read_text(encoding="utf-8")
        economy = (ROOT / "apps/viewer/src/live_session_economy.cpp").read_text(encoding="utf-8")
        frame = (ROOT / "apps/viewer/src/live_session_frame.cpp").read_text(encoding="utf-8")
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
        source = (ROOT / "apps/viewer/src/lan_match.cpp").read_text(encoding="utf-8")
        activate = function_body(source, "core::Result<void> activate_adapter(")
        self.assertIn("core_session().active_adapter() != &adapter", activate)
        self.assertIn("adapter.staging->activate()", activate)
        self.assertIn("adapter.bind_live_context(*current_context_", activate)
        self.assertIn("adapter_ = &adapter", activate)
        drive = function_body(source, "DriverResult drive(")
        self.assertIn("session.active_adapter()", drive)
        self.assertIn("activate_adapter(*published)", drive)
        frame = (ROOT / "apps/viewer/src/live_session_frame.cpp").read_text(encoding="utf-8")
        self.assertIn("session_->frame()", frame)
        platform = (ROOT / "src/platform/live_session.cpp").read_text(encoding="utf-8")
        self.assertIn("frame_ = {snapshot, snapshot, Clock::now()}", platform)
        self.assertIn("event_log_ = LiveEventLog", platform)

    def test_drop_feedback_and_speed_admission(self):
        source = (ROOT / "apps/viewer/src/lan_match.cpp").read_text(encoding="utf-8")
        self.assertIn("lan_gameplay_enabled(view_)", function_body(source, "bool request_speed("))
        drop = function_body(source, "void discard_command(")
        self.assertIn("observer(command.key.sequence)", drop)
        self.assertIn("view_.input_feedback", drop)
        gate = (ROOT / "apps/viewer/src/lan_match.hpp").read_text(encoding="utf-8")
        gate_issue = function_body(gate, "core::Result<void> issue(")
        self.assertLess(gate_issue.index("acknowledge_input_feedback()"), gate_issue.index("sink_.issue(intent)"))
        frame = (ROOT / "apps/viewer/src/live_session_frame.cpp").read_text(encoding="utf-8")
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
                ("recovery_failed", "Synchronization could not be restored. This match cannot continue." + details)):
                with self.subTest(state=state):
                    report = output / (state + ".json")
                    capture = output / (state + ".png")
                    result = subprocess.run([
                        os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                        "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-live-session", "m2",
                        "--eawr-map", "data/art/maps/_mp_space_coruscant.ted", "--eawr-populate",
                        "--eawr-live-lan-test", state, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                        "--eawr-report", str(report), "--eawr-capture", str(capture),
                        "--eawr-live-ticks", "1", "--eawr-live-ai", "off", "--eawr-audio", "off",
                        "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
                        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
                    (output / (state + ".log")).write_text(result.stdout, encoding="utf-8")
                    self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                    data = json.loads(report.read_text(encoding="utf-8"))
                    self.assertTrue(data["live_session"]["multiplayer"])
                    # The report writer flattens the overlay's line break to a space.
                    self.assertEqual(data["live_session"]["lan_message"], message)
                    self.assertTrue(capture.exists())
                    self.assertGreater(capture.stat().st_size, 1000)

if __name__ == "__main__":
    unittest.main()
