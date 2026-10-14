"""Checkpoint inventory gate rejects future omissions, including the cloak regression."""
from pathlib import Path
import unittest
import re

import generate_fields


class FieldCompletenessTests(unittest.TestCase):
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
            ("src/campaign/game_session.cpp", "withdrawing_player", "GameSession::Impl"),
        ):
            with self.subTest(path=path):
                source = re.sub(r"\bv\." + field + r",\s*", "", (root / path).read_text(encoding="utf-8"))
                errors, _ = generate_fields.check({path: source})
                self.assertTrue(any(owner + ": missing=['" + field + "']" in error for error in errors), errors)

    def test_new_owner_member_requires_serialization_or_exclusion(self):
        root = Path(__file__).resolve().parents[3]
        for path, declaration in (
            ("src/sim/galactic/session_impl.hpp", "class GalacticSession::Impl {"),
            ("src/campaign/game_session.cpp", "class GameSession::Impl {"),
        ):
            with self.subTest(path=path):
                source = (root / path).read_text(encoding="utf-8").replace(
                    declaration, declaration + "\n    std::uint64_t future_cursor{};")
                errors, _ = generate_fields.check({path: source})
                self.assertTrue(any("missing=['future_cursor']" in error for error in errors), errors)

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
