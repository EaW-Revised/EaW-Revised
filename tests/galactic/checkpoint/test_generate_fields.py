"""Checkpoint inventory gate rejects future omissions, including the cloak regression."""
from pathlib import Path
import unittest
import re

import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "presentation/renderer"))
import generate_fields
from viewer_mode_sources import source_text  # noqa: E402


class FieldCompletenessTests(unittest.TestCase):
    def test_generic_team_runtime_fields_require_serialization(self):
        path = "src/sim/tactical/land_checkpoint_fields_internal.hpp"
        wire = source_text(path)
        for owner, field in (("LandMovingMember", "next_node"), ("LandMovingMember", "temporary_next"), ("LandMovingMember", "detour_path"),
                             ("LandMovingMember", "detour_next"),
                             ("LandMovingSquad", "generic_team")):
            with self.subTest(owner=owner, field=field):
                block = re.search(r"template <> struct Fields<" + owner + r"> \{.*?\n\};", wire, re.S).group()
                changed = block.replace("v." + field, "")
                self.assertNotEqual(block, changed)
                errors, _ = generate_fields.check({path: wire.replace(block, changed)})
                self.assertTrue(any(owner + ": missing=['" + field + "']" in error for error in errors), errors)

    def test_inherited_store_nested_member_requires_serialization(self):
        path = "src/sim/tactical/session_store_projectiles.hpp"
        source = source_text(path).replace(
            "struct PendingBlastDamage {", "struct PendingBlastDamage {\n        std::uint64_t future_deadline{};")
        errors, _ = generate_fields.check({path: source})
        self.assertTrue(any("PendingBlastDamage: missing=['future_deadline']" in error for error in errors), errors)

    def test_inherited_store_nested_field_omission_fails(self):
        path = "src/sim/tactical/checkpoint_fields_internal.hpp"
        source = source_text(path).replace("a(v.due, v.owner, v.damage_type,", "a(v.owner, v.damage_type,")
        self.assertNotEqual(source, source_text(path))
        errors, _ = generate_fields.check({path: source})
        self.assertTrue(any("PendingBlastDamage: missing=['due']" in error for error in errors), errors)

    def test_current_fields_are_complete(self):
        errors, count = generate_fields.check()
        self.assertEqual(errors, [])
        self.assertGreater(count, 80)

    def test_gc_fields_and_owners_reject_omissions(self):
        root = Path(__file__).resolve().parents[3]
        for path, field, owner in (
            ("src/sim/galactic/checkpoint_fields_internal.hpp", "next_request", "GalacticAbilityState"),
            ("src/campaign/checkpoint_fields_internal.hpp", "withdraw_admitted", "AutoResolveProgress"),
            ("src/campaign/checkpoint_owner_internal.hpp", "sequences", "ChildOwner"),
            ("src/sim/galactic/session.cpp", "native_sequence", "GalacticSession::Impl"),
            ("src/campaign/game_session_lifecycle.cpp", "withdrawing_player", "GameSession::Impl"),
        ):
            with self.subTest(path=path):
                source = re.sub(r"\bv\." + field + r",\s*", "", source_text(path))
                errors, _ = generate_fields.check({path: source})
                self.assertTrue(any(owner + ": missing=['" + field + "']" in error for error in errors), errors)

    def test_new_owner_member_requires_serialization_or_exclusion(self):
        root = Path(__file__).resolve().parents[3]
        for path, declaration in (
            ("src/sim/galactic/session_impl.hpp", "class GalacticSession::Impl {"),
            ("src/campaign/game_session_impl.hpp", "class GameSession::Impl {"),
        ):
            with self.subTest(path=path):
                source = source_text(path).replace(
                    declaration, declaration + "\n    std::uint64_t future_cursor{};")
                errors, _ = generate_fields.check({path: source})
                self.assertTrue(any("missing=['future_cursor']" in error for error in errors), errors)

    def test_plain_duplicate_fails_but_indexed_slots_do_not(self):
        path = "src/sim/tactical/checkpoint_fields_internal.hpp"
        wire = source_text(path)
        self.assertIn("a(v.upgrade_bonuses[6]);", wire)
        doubled = wire.replace("a(v.state, v.durability,", "a(v.state, v.state, v.durability,", 1)
        self.assertNotEqual(doubled, wire)
        errors, _ = generate_fields.check({path: doubled})
        self.assertTrue(any("LiveUnit:" in error and "duplicate=True" in error for error in errors), errors)
        repeated = wire.replace("a(v.upgrade_bonuses[6]);", "a(v.upgrade_bonuses[6]); a(v.upgrade_bonuses[6]);", 1)
        errors, _ = generate_fields.check({path: repeated})
        self.assertTrue(any("LiveUnit:" in error and "duplicate=True" in error for error in errors), errors)

    def test_omitted_cloak_deadline_fails(self):
        root = Path(__file__).resolve().parents[3]
        path = "src/sim/tactical/checkpoint_fields_internal.hpp"
        source = (root / path).read_text(encoding="utf-8").replace("v.stealth_end, ", "")
        errors, _ = generate_fields.check({path: source})
        self.assertTrue(any("AbilityState: missing=['stealth_end']" in error for error in errors), errors)

    def test_new_member_fails_without_changing_field_list(self):
        root = Path(__file__).resolve().parents[3]
        path = "include/eawr/sim/tactical/abilities.hpp"
        source = (root / path).read_text(encoding="utf-8").replace(
            "struct AbilityState {", "struct AbilityState {\n    std::uint64_t future_deadline{};")
        errors, _ = generate_fields.check({path: source})
        self.assertTrue(any("AbilityState: missing=['future_deadline']" in error for error in errors), errors)

    def test_nested_type_in_new_sim_header_requires_fields(self):
        root = Path(__file__).resolve().parents[3]
        path = "include/eawr/sim/tactical/abilities.hpp"
        source = (root / path).read_text(encoding="utf-8").replace(
            "struct AbilityState {", "struct AbilityState {\n    FutureState future;")
        fields = "src/sim/tactical/checkpoint_fields_internal.hpp"
        wire = (root / fields).read_text(encoding="utf-8").replace("a(v.slots,", "a(v.future, v.slots,")
        errors, _ = generate_fields.check({path: source, fields: wire,
            "src/sim/future_state.hpp": "struct FutureState { std::uint64_t deadline{}; };"})
        self.assertTrue(any("FutureState has no Fields list" in error for error in errors), errors)

    def test_nested_new_struct_requires_fields(self):
        root = Path(__file__).resolve().parents[3]
        path = "include/eawr/sim/tactical/abilities.hpp"
        source = (root / path).read_text(encoding="utf-8").replace(
            "struct AbilityState {", "struct FutureState { std::uint64_t deadline{}; };\n"
            "struct AbilityState {\n    FutureState future;")
        fields = "src/sim/tactical/checkpoint_fields_internal.hpp"
        wire = (root / fields).read_text(encoding="utf-8").replace("a(v.slots,", "a(v.future, v.slots,")
        errors, _ = generate_fields.check({path: source, fields: wire})
        self.assertTrue(any("FutureState has no Fields list" in error for error in errors), errors)

    def test_same_named_structs_in_other_namespaces_do_not_collide(self):
        errors, _ = generate_fields.check({"src/sim/future_state.hpp": """
            namespace eawr::sim::galactic {
                struct AbilityState { int unrelated; };
                namespace detail { struct BlastRecipient { int unrelated; }; }
            }
        """})
        self.assertEqual(errors, [])

    def test_anonymous_namespace_helper_does_not_collide_with_public_state(self):
        errors, _ = generate_fields.check({"src/sim/future_state.hpp": """
            namespace eawr::sim::tactical {
                namespace { struct AbilityState { int unrelated; }; }
            }
        """})
        self.assertEqual(errors, [])

    def test_qualified_nested_member_checks_the_correct_namespace(self):
        root = Path(__file__).resolve().parents[3]
        path = "include/eawr/sim/tactical/abilities.hpp"
        source = (root / path).read_text(encoding="utf-8").replace(
            "struct AbilityState {", "struct AbilityState {\n    future::State future;")
        fields = "src/sim/tactical/checkpoint_fields_internal.hpp"
        wire = (root / fields).read_text(encoding="utf-8").replace("a(v.slots,", "a(v.future, v.slots,")
        wire += """
template <> struct Fields<eawr::sim::tactical::future::State> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.deadline); }
};
"""
        overrides = {path: source, fields: wire, "src/sim/future_state.hpp": """
            namespace eawr::sim::tactical::future { struct State { int deadline; }; }
            namespace eawr::sim::galactic::future { struct State { int unrelated; }; }
        """}
        self.assertEqual(generate_fields.check(overrides)[0], [])
        overrides[fields] = wire.replace("a(v.deadline);", "a();")
        errors, _ = generate_fields.check(overrides)
        self.assertTrue(any("tactical::future::State: missing=['deadline']" in error
                            for error in errors), errors)

    def test_unqualified_nested_member_uses_its_declaring_scope(self):
        root = Path(__file__).resolve().parents[3]
        path = "include/eawr/sim/tactical/abilities.hpp"
        source = (root / path).read_text(encoding="utf-8").replace(
            "struct AbilityState {", "struct FutureState { int deadline; };\n"
            "struct AbilityState {\n    FutureState future;")
        fields = "src/sim/tactical/checkpoint_fields_internal.hpp"
        wire = (root / fields).read_text(encoding="utf-8").replace("a(v.slots,", "a(v.future, v.slots,")
        errors, _ = generate_fields.check({path: source, fields: wire,
            "src/sim/future_state.hpp": "namespace other { struct FutureState { int unrelated; }; }"})
        self.assertTrue(any("tactical::FutureState has no Fields list" in error for error in errors), errors)
        self.assertFalse(any("other::FutureState" in error for error in errors), errors)


if __name__ == "__main__":
    unittest.main()
