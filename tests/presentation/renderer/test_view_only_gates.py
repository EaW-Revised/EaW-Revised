"""#8182: a watched (view-only) battle view issues no order, selects no unit and shows no order control.

The viewer cannot run headless here, so each gate is pinned at the function that would issue the order: the body of every
order entry point must test the view-only flag before it does anything. Dropping one gate fails the test of that entry point.
"""
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))


def source_text(path: str) -> str:
    """The one physical file (positional lookups need a single member, not a move bundle)."""
    return (ROOT / path).read_text(encoding="utf-8")


ECONOMY = "apps/viewer/src/live_session_economy.cpp"
VIEW_HEADER = "apps/viewer/src/live_session_view.hpp"
INPUT_COMMANDS = "apps/viewer/src/battle_input_commands.cpp"
INPUT_PRODUCTION = "apps/viewer/src/battle_input_production.cpp"
INPUT_HEADER = "apps/viewer/src/battle_input.hpp"
INPUT = "apps/viewer/src/battle_input.cpp"
HUD = "src/presentation/godot/ui/tactical_hud.cpp"
MAP_HUD = "apps/viewer/src/map_mode_hud.cpp"
HANDOFF = "apps/viewer/src/galactic/galactic_handoff.cpp"
MAP_READY = "apps/viewer/src/map_mode_ready.cpp"


def body(path: str, signature: str, length: int = 900) -> str:
    """The first `length` characters from a function's signature: its opening statements."""
    text = source_text(path)
    start = text.index(signature)
    return text[start:start + length]


class ViewOnlyGates(unittest.TestCase):
    def assert_gated(self, path: str, signature: str, window: int = 420):
        opening = body(path, signature, window)
        self.assertIn("view_only", opening, f"{signature} must test the view-only flag first")

    def test_every_live_session_order_entry_point_is_gated(self):
        for signature in (
            "bool LiveSessionView::can_control(const tactical::TacticalInstance& instance) const noexcept {",
            "bool LiveSessionView::sell_pad_structure(",
            "bool LiveSessionView::repair_hardpoint(",
            "bool LiveSessionView::buy(",
            "bool LiveSessionView::cancel_build(",
            "bool LiveSessionView::rotate_reinforcement(",
            "bool LiveSessionView::reinforce(",
            "bool LiveSessionView::select_campaign_reinforcement(",
            "bool LiveSessionView::special_fire(",
            "bool LiveSessionView::superweapon_request(",
        ):
            with self.subTest(signature=signature):
                self.assert_gated(ECONOMY, signature, 260)
        with self.subTest(signature="Abilities::request"):
            self.assert_gated(ECONOMY, "void LiveSessionView::Abilities::request(", 520)

    def test_beacon_is_gated_at_the_view(self):
        self.assert_gated(VIEW_HEADER, "bool place_beacon(", 220)

    def test_battle_input_order_entry_points_are_gated(self):
        for path, signature in (
            (INPUT_COMMANDS, "bool BattleInput::input("),
            (INPUT_COMMANDS, "bool BattleInput::key("),
            (INPUT_COMMANDS, "bool BattleInput::minimap_move("),
            (INPUT_PRODUCTION, "void BattleInput::begin_placement("),
            (INPUT_HEADER, "void placement_drop("),
        ):
            with self.subTest(signature=signature):
                self.assert_gated(path, signature, 520)
        # No unit of a watched view is ever ours: not selectable, not a build pad.
        self.assertIn("!live.view_only()", source_text(INPUT))

    def test_hud_hides_every_order_control_including_the_gift_control(self):
        gift = body(HUD, "void TacticalHud::set_gift(", 1500)
        self.assertIn("state.view_only ||", gift, "set_gift must not re-show the gift control in a watched view")
        hide = body(HUD, "void TacticalHud::set_view_only(", 900)
        for control in ("order_buttons", "special_button", "retreat_button", "beacon_button", "gift_button", "cards",
                        "heroes", "abilities", "production"):
            self.assertIn(control, hide)
        text = source_text(MAP_HUD)
        for signature in ("void MapMode::State::sync_cards() {", "void MapMode::State::sync_production() {",
                          "void MapMode::State::sync_battle_hud() {"):
            self.assertIn("ViewOnlyApply<State>", body(MAP_HUD, signature, 200))
        self.assertIn("hud->set_view_only(true)", text)

    def test_only_a_spectating_seat_gets_a_view_only_battle(self):
        self.assertIn("map.borrowed_view_only = spectating;", source_text(HANDOFF))
        self.assertIn("state.live_options.view_only = state.options.borrowed_view_only;", source_text(MAP_READY))


if __name__ == "__main__":
    unittest.main()
