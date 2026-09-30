"""Contracts for the P1-08 viewer effect mode and particle render adapters.

The structural half runs everywhere: it reads committed sources, the committed
synthetic fixtures and the particle render-family inventory, and needs neither
Godot nor an installed corpus.

The graphical half is opt-in. Set EAWR_GODOT_VIEWER_RUNTIME_TEST and
EAWR_GODOT_EXECUTABLE to run the synthetic effect twice through the pinned
Godot binary and check its decoded-PNG evidence and capture identity. Set
EAWR_EAW_GAME_ROOT as well (and EAWR_REMAKE_MOD_ROOT for Remake-only samples)
to add one read-only run per implemented family over the installed corpus
effects named in the inventory; only numbers and hashes are asserted and no
capture is written. Set EAWR_PARTICLE_CORPUS_SCAN with the same roots to
recount the renderer-family and blend-selector breakdown the inventory records.
"""

import json
import os
import pathlib
import struct
import sys
import unittest
from collections import Counter

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from effect_mode_fixtures import (build_effect, build_glow, build_parent_effect,
    build_proxy_animation, build_proxy_effect, build_proxy_host, build_visibility_animation, _chunk,
    EFFECT_FIXTURE, EFFECT_TITLE, _emitter, FIXTURES, FORBIDDEN, from_hex, GLOW_TITLE, _group,
    INVENTORY, LEDGER, ROOT, _text, TEXTURE_FIXTURE, to_hex, VISIBILITY_CLIP_FRAMES,
    VISIBILITY_HIDDEN_FRAMES)  # noqa: E402
from viewer_mode_sources import mode_source  # noqa: E402

# --- Structure -----------------------------------------------------------------

class EffectModeStructure(unittest.TestCase):
    def test_fixtures_are_the_generated_original_bytes(self):
        self.assertEqual(EFFECT_FIXTURE.read_text(encoding="utf-8"), to_hex(build_effect(), EFFECT_TITLE))
        self.assertEqual(TEXTURE_FIXTURE.read_text(encoding="utf-8"), to_hex(build_glow(), GLOW_TITLE))
        self.assertTrue(build_parent_effect().startswith(struct.pack("<I", 0x900)))
        self.assertTrue(build_proxy_effect().startswith(struct.pack("<I", 0x900)))
        self.assertTrue(build_proxy_host().startswith(struct.pack("<I", 0x200)))
        self.assertTrue(build_proxy_animation().startswith(struct.pack("<I", 0x1000)))
        self.assertTrue(build_visibility_animation().startswith(struct.pack("<I", 0x1000)))

    def test_visibility_clip_hides_reappears_and_hides(self):
        clip = build_visibility_animation()
        # The track ends with the 0x1007 visibility bitset: one bit per stored frame.
        bits = clip[-4:]
        self.assertEqual(clip[-12:-4], struct.pack("<II", 0x1007, 4))
        visible = [(bits[index // 8] >> (index % 8)) & 1 == 1 for index in range(VISIBILITY_CLIP_FRAMES)]
        self.assertEqual(visible[:6], [True] * 6)
        self.assertEqual(visible[6:14], [False] * 8)
        self.assertEqual(visible[14:24], [True] * 10)
        self.assertEqual(visible[24:], [False] * 7)

    def test_visibility_lifecycle_is_owned_by_the_mode(self):
        source = mode_source("effect_mode")
        host = mode_source("viewer_host")
        helper = (ROOT / "include/eawr/presentation/particles/attachment_lifecycle.hpp").read_text(encoding="utf-8")
        for token in ("--eawr-effect-visibility", "particles::AttachmentLifecycle", "visibility_step(",
                      "pose.value().bones[*visibility_bone].visible", "replay_visibility",
                      "\"stay-detached\"", "\"respawn\""):
            self.assertIn(token, source, token)
        self.assertNotIn("visibility", host)
        # The trailing flag is scanned like the detach frame and rejected.
        start = source.index("visibility_argument() {")
        scan = source[start:source.index("\n}\n", start)]
        self.assertIn("index < arguments.size(); ++index", scan)
        self.assertIn("index + 1 < arguments.size() ? utf8(arguments[index + 1]) : std::string{}", scan)
        guard = source[source.index("if (const auto requested_visibility = visibility_argument())"):]
        guard = guard[:guard.index("// Layer selection")]
        self.assertIn("return give_up(\"--eawr-effect-visibility expects respawn or stay-detached\")", guard)
        self.assertIn("cannot be combined with --eawr-effect-detach-frame", guard)
        # The helper composes the public registry and cites its reference.
        for token in ("registry_->detach(", "registry_->release(", "RenderObject.cpp",
                      "9bb0053919cc5df8377610d4f91b11d956d6c2f4", "ReappearancePolicy::respawn"):
            self.assertIn(token, helper, token)

    def test_detach_fixtures_carry_both_leave_particles_values(self):
        # Root chunk 2 is the system's one-byte leave-particles flag, last in the root.
        self.assertTrue(build_parent_effect().endswith(struct.pack("<II", 2, 1) + b"\x01"))
        self.assertTrue(build_parent_effect(leave_particles=False).endswith(
            struct.pack("<II", 2, 1) + b"\x00"))
        self.assertEqual(build_parent_effect()[:-1], build_parent_effect(leave_particles=False)[:-1])

    def test_detach_schedule_is_owned_by_the_mode(self):
        source = mode_source("effect_mode")
        host = mode_source("viewer_host")
        for token in ("--eawr-effect-detach-frame", "detach(effect)", "empty_after_detach",
                      "\"completion\"", "capture_plan"):
            self.assertIn(token, source, token)
        self.assertNotIn("detach", host)

    def test_trailing_detach_flag_without_value_is_an_option_error(self):
        # A trailing --eawr-effect-detach-frame must not be skipped by the scan
        # (which would silently run without a detach); it yields an empty value
        # that the frame-index parser rejects through give_up.
        source = mode_source("effect_mode")
        start = source.index("detach_frame_argument() {")
        scan = source[start:source.index("\n}\n", start)]
        self.assertIn("index < arguments.size(); ++index", scan)
        self.assertNotIn("index + 1 < arguments.size(); ++index", scan)
        self.assertIn("index + 1 < arguments.size() ? utf8(arguments[index + 1]) : std::string{}", scan)
        self.assertIn("if (text.empty() || text.size() > 10) return std::nullopt;", source)
        guard = source[source.index("if (const auto requested_detach = detach_frame_argument())"):]
        guard = guard[:guard.index("state.detach_frame = *parsed;")]
        self.assertIn("if (!parsed || *parsed >= options.frames)", guard)
        self.assertIn("return give_up(\"--eawr-effect-detach-frame needs", guard)

    def test_parent_creators_are_catalogued_as_v1_cpu_behavior(self):
        ledger = json.loads(LEDGER.read_text(encoding="utf-8"))
        creators = {entry["id"]: entry for entry in ledger["plugins"]}
        for plugin_id in (39, 40):
            self.assertEqual(creators[plugin_id]["status"], "cpu-supported")
        self.assertEqual(ledger["plugin_occurrences_by_status"]["cpu-supported"], 54450)

    def test_mode_is_self_contained(self):
        source = mode_source("effect_mode")
        host = mode_source("viewer_host")
        for token in ("EffectRegistry", "GodotParticleBackend", "verify_capture", "set_camera",
                      "attachment(", "live_rids_after_release"):
            self.assertIn(token, source, token)
        self.assertIn("EffectMode::requested()", host)
        self.assertIn("effect_mode_->ready(*this)", host)
        self.assertIn("effect_mode_->process()", host)
        for token in ("particles::", "--eawr-effect-attach", "GodotParticleBackend"):
            self.assertNotIn(token, host, f"{token} leaked into the shared viewer host")

    def test_particle_module_is_engine_and_simulation_free(self):
        paths = [ROOT / "include/eawr/presentation/particles/particles.hpp",
                 ROOT / "include/eawr/presentation/particles/render.hpp",
                 ROOT / "include/eawr/presentation/particles/proxy_binding.hpp",
                 ROOT / "include/eawr/presentation/particles/attachment_lifecycle.hpp",
                 ROOT / "src/presentation/particles/alo_particles.cpp",
                 ROOT / "src/presentation/particles/cpu_system.cpp",
                 ROOT / "src/presentation/particles/proxy_binding.cpp",
                 ROOT / "src/presentation/particles/render.cpp"]
        for path in paths:
            text = path.read_text(encoding="utf-8")
            for token in ("godot_cpp", "#include <godot", "eawr/sim/", "fstream", "<filesystem>"):
                self.assertNotIn(token, text, f"{token} in {path.name}")
        # No particle state reaches simulation, snapshots or replay.
        for path in (ROOT / "src/sim").rglob("*"):
            if path.suffix in {".cpp", ".hpp"}:
                self.assertNotIn("particles", path.read_text(encoding="utf-8").lower(), path.name)
        for path in (ROOT / "include/eawr/sim").rglob("*.hpp"):
            self.assertNotIn("particles", path.read_text(encoding="utf-8").lower(), path.name)

    def test_adapter_uses_versioned_material_gate_and_one_conversion(self):
        adapter = (ROOT / "src/presentation/godot/particle_adapter.cpp").read_text(encoding="utf-8")
        self.assertIn("validate_material(material)", adapter)
        self.assertIn("MaterialRoute::modern_spatial", adapter)
        self.assertIn("eawr_compile_probe", adapter)
        self.assertIn("return {value.x, value.z, -value.y};", adapter)
        for mode in ("blend_add", "blend_mix", "blend_mul", "hint_screen_texture",
                     "depth_draw_never", "depth_test_disabled"):
            self.assertIn(mode, adapter, mode)

    def test_render_family_inventory_is_clean_and_closed(self):
        text = INVENTORY.read_text(encoding="utf-8")
        found = FORBIDDEN.search(text)
        self.assertIsNone(found, f"inventory leaks an installation path: {found}")
        document = json.loads(text)
        self.assertEqual(document["schema_version"], 1)
        ledger = json.loads(LEDGER.read_text(encoding="utf-8"))
        observed = {entry["id"]: entry for entry in ledger["plugins"]
                    if entry["family"] == "renderer" and entry["occurrences"] > 0}
        families = {entry["renderer_id"]: entry for entry in document["renderer_families"]}
        # Every renderer family observed in the accepted CPU ledger is present,
        # with the ledger's count, and is either implemented or listed with cause.
        for identifier, entry in observed.items():
            self.assertIn(identifier, families, entry["name"])
            self.assertEqual(families[identifier]["corpus_emitters"], entry["occurrences"], entry["name"])
        for entry in document["renderer_families"]:
            self.assertIn(entry["disposition"], {"implemented", "listed"})
            if entry["disposition"] == "listed":
                self.assertTrue(entry["cause"], entry["name"])
        self.assertEqual(sum(entry["corpus_emitters"] for entry in document["renderer_families"]),
                         ledger["counts"]["particle_v1_emitters"])
        for selector in document["legacy_blend_selectors"]:
            self.assertIn(selector["disposition"], {"implemented", "listed"})
            if selector["disposition"] == "listed":
                self.assertTrue(selector["cause"], selector["program"])
        for entry in document["behaviour_families"]:
            self.assertIn(entry["disposition"], {"implemented", "listed"})
            if entry["disposition"] == "listed":
                self.assertTrue(entry["cause"], entry["name"])

# --- Corpus selector scan (opt-in, read-only) --------------------------------------

def scan_corpus(game_root: pathlib.Path, mod_root: pathlib.Path | None) -> dict:
    """Counts V1 emitters by derived renderer family and legacy blend selector."""
    sys.path.insert(0, str(ROOT / "tools"))
    import particle_inventory as inventory  # noqa: E402  (tool module, read-only helpers)

    families = Counter()
    selectors = Counter()
    pairs = Counter()

    def inspect(data: bytes):
        roots = list(inventory.chunks(data))
        if len(roots) != 1 or roots[0][0] != 0x900 or not roots[0][1]:
            return
        for kind, grouped, begin, end in inventory.chunks(data, roots[0][2], roots[0][3]):
            if kind != 0x800 or not grouped:
                continue
            for emitter in inventory.chunks(data, begin, end):
                if emitter[0] != 0x700 or not emitter[1]:
                    continue
                props = {}
                for child_kind, child_group, child_begin, child_end in inventory.chunks(data, emitter[2], emitter[3]):
                    if child_kind == 2 and not child_group:
                        props = {item[0]: item for item in inventory.minis(data, child_begin, child_end)}
                value = lambda key, default=0: inventory.scalar(data, props.get(key, (0, 0, 0)), default)
                family = (38 if value(0x3B) else 28 if value(0x2E) else 52 if value(0x41) else 22)
                blend = value(0x04, 1)
                families[family] += 1
                selectors[blend] += 1
                pairs[(family, blend)] += 1

    layers = [("base", game_root / "GameData" / "Data")]
    if (game_root / "corruption" / "Data").is_dir():
        layers.append(("expansion", game_root / "corruption" / "Data"))
    if mod_root:
        layers.append(("mod", mod_root))
    for _, root in layers:
        for archive in sorted(root.rglob("*.meg"), key=lambda item: str(item).lower()):
            for _, data in inventory.meg_members(archive):
                inspect(data)
        for alo in sorted(root.rglob("*.alo"), key=lambda item: str(item).lower()):
            inspect(alo.read_bytes())
    return {"families": families, "selectors": selectors, "pairs": pairs}

class CorpusSelectorScan(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("EAWR_PARTICLE_CORPUS_SCAN") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_PARTICLE_CORPUS_SCAN and EAWR_EAW_GAME_ROOT for the selector recount")
    def test_selector_breakdown_matches_inventory(self):
        mod = os.environ.get("EAWR_REMAKE_MOD_ROOT")
        counts = scan_corpus(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                             pathlib.Path(mod) if mod else None)
        document = json.loads(INVENTORY.read_text(encoding="utf-8"))
        for entry in document["renderer_families"]:
            self.assertEqual(counts["families"][entry["renderer_id"]], entry["corpus_emitters"], entry["name"])
        for entry in document["legacy_blend_selectors"]:
            self.assertEqual(counts["selectors"][entry["value"]], entry["corpus_emitters"], entry["program"])
        recorded = {(entry["renderer_id"], entry["blend_selector"]): entry["corpus_emitters"]
                    for entry in document["family_selector_pairs"]}
        self.assertEqual(recorded, {key: value for key, value in counts["pairs"].items()})

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--write-fixtures":
        FIXTURES.mkdir(parents=True, exist_ok=True)
        EFFECT_FIXTURE.write_text(to_hex(build_effect(), EFFECT_TITLE), encoding="utf-8", newline="\n")
        TEXTURE_FIXTURE.write_text(to_hex(build_glow(), GLOW_TITLE), encoding="utf-8", newline="\n")
        sys.exit(0)
    unittest.main(argv=[sys.argv[0]])
