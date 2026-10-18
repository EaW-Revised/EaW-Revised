"""`tag_registry.py relocate`: code anchors follow a move, the gate stays green (docs/tag-coverage.md, "Moving code")."""
from __future__ import annotations

import contextlib
import io
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT))

import test_tag_registry as base  # noqa: E402
from tools.inventory import census_review  # noqa: E402

tr = base.tr
OLD = base.CODE
NEW = "src/sim/tactical/motion_apply.cpp"


class RelocateTests(base.Fixture):
    def setUp(self) -> None:
        super().setUp()
        directory = self.root / "docs" / "tag-coverage"
        directory.mkdir(parents=True)
        self.statuses = directory / "statuses.json"
        self.child = directory / "statuses-child.json"
        self.inventory = self.root / "tags.json"
        self.inventory.write_text(json.dumps(base.INVENTORY), encoding="utf-8")
        rows = sorted(base.full_rows(), key=tr.row_order)
        root_rows = [row for row in rows if row["tag"] != "@Name"]
        child_rows = [row for row in rows if row["tag"] == "@Name"]
        self.write(self.statuses, {"schema_version": 2, "shards": ["statuses-child.json"],
                                   "evidence": base.registry()["evidence"], "rows": root_rows})
        self.write(self.child, {"schema_version": 2, "evidence": {}, "rows": child_rows})
        self.census = self.root / "mechanic-review.json"
        region = {"path": OLD, "start": "void apply()", "end": "}", "sha256_lf": "0" * 64}
        self.write_census({"schema_version": 2, "purpose": "test",
                           "regions": {f"{OLD}#apply": region,
                                       "src/other.cpp#other": dict(region, path="src/other.cpp")}})
        self.moves = self.root / "moves.json"
        for target in (NEW, "src/sim/layer.cpp", "src/x.cpp", "src/y.cpp"):
            (self.root / target).parent.mkdir(parents=True, exist_ok=True)
            (self.root / target).write_text("int max_speed; int layer_z; void apply();\nint other;\n", encoding="utf-8")

    @staticmethod
    def write(path: Path, value: dict) -> None:
        path.write_bytes(tr.dumps_registry(value).encode("utf-8"))

    def write_census(self, value: dict) -> None:
        self.census.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8", newline="\n")

    def move(self, *pairs: tuple[str, str], extra: list[str] | None = None) -> int:
        self.moves.write_text(json.dumps([{"old": old, "new": new} for old, new in pairs]), encoding="utf-8")
        args = ["relocate", "--map", str(self.moves), "--statuses", str(self.statuses),
                "--census", str(self.census), "--repo", str(self.root)] + (extra or [])
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return tr.main(args)

    def check(self) -> int:
        args = ["check", "--statuses", str(self.statuses), "--inventory", str(self.inventory), "--repo", str(self.root)]
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return tr.main(args)

    def move_source(self) -> None:
        (self.root / OLD).unlink()

    def anchors(self) -> list[str]:
        registry = tr.load_registry(self.statuses)
        return sorted(t["code"] for row in registry["rows"] for t in row.get("applied", ()))

    def test_a_symbol_move_keeps_the_gate_green(self) -> None:
        self.assertEqual(self.check(), 0)
        self.move_source()
        self.assertEqual(self.check(), 1)  # the anchors now point at a file that is gone
        self.assertEqual(self.move((f"{OLD}#max_speed", f"{NEW}#max_speed"), (f"{OLD}#layer_z", f"{NEW}#layer_z")), 0)
        self.assertEqual(self.check(), 0)
        self.assertEqual(self.anchors(), [f"{NEW}#layer_z", f"{NEW}#max_speed", f"{NEW}#max_speed"])

    def test_a_file_move_renames_the_path_of_every_anchor_in_every_shard(self) -> None:
        self.move_source()
        self.assertEqual(self.move((OLD, NEW)), 0)
        self.assertEqual(self.check(), 0)
        self.assertTrue(all(code.startswith(NEW + "#") for code in self.anchors()))
        self.assertIn(NEW + "#apply", json.loads(self.census.read_text(encoding="utf-8"))["regions"])

    def test_relocation_routes_root_rows_to_area_and_keeps_domain_rows(self) -> None:
        root = tr.load_shard(self.statuses)
        movement = self.statuses.with_name("statuses-movement.json")
        for area in tr.AREAS:
            name = f"statuses-{area}.json"
            root["shards"].append(name)
            self.write(self.statuses.with_name(name), {"schema_version": 2, "evidence": {}, "rows": []})
        self.write(self.statuses, root)
        before = tr.load_registry(self.statuses)
        self.assertEqual(self.move((OLD, NEW), extra=["--dry-run"]), 0)
        self.assertEqual(tr.load_shard(movement)["rows"], [])
        self.assertEqual(self.move((OLD, NEW)), 0)
        self.assertEqual({row["tag"] for row in tr.load_shard(movement)["rows"]}, {"Max_Speed", "Layer_Z_Adjust"})
        self.assertFalse(any(row["area"] == "movement" for row in tr.load_shard(self.statuses)["rows"]))
        self.assertEqual([row["tag"] for row in tr.load_shard(self.child)["rows"]], ["@Name"])
        for row in before["rows"]:
            for target in row.get("applied", []):
                target["code"] = target["code"].replace(OLD, NEW)
        self.assertEqual(tr.load_registry(self.statuses), before)
        self.assertEqual(self.check(), 0)

    def test_a_symbol_move_wins_over_a_file_move(self) -> None:
        self.assertEqual(self.move((OLD, NEW), (f"{OLD}#layer_z", "src/sim/layer.cpp#layer_z")), 0)
        self.assertEqual(self.anchors(), ["src/sim/layer.cpp#layer_z", f"{NEW}#max_speed", f"{NEW}#max_speed"])

    def test_only_the_shards_with_a_moved_anchor_are_rewritten(self) -> None:
        child_before = self.child.read_bytes()
        self.assertEqual(self.move((f"{OLD}#layer_z", f"{NEW}#layer_z")), 0)
        self.assertEqual(self.child.read_bytes(), child_before)
        self.assertIn(f"{NEW}#layer_z".encode(), self.statuses.read_bytes())

    def test_dry_run_writes_nothing(self) -> None:
        before = (self.statuses.read_bytes(), self.child.read_bytes(), self.census.read_bytes())
        self.assertEqual(self.move((OLD, NEW), extra=["--dry-run"]), 0)
        self.assertEqual(before, (self.statuses.read_bytes(), self.child.read_bytes(), self.census.read_bytes()))

    def test_census_region_keeps_its_fingerprint_fields(self) -> None:
        self.assertEqual(self.move((f"{OLD}#apply", f"{NEW}#apply")), 0)
        regions = json.loads(self.census.read_text(encoding="utf-8"))["regions"]
        self.assertEqual(regions[f"{NEW}#apply"], {"path": NEW, "start": "void apply()", "end": "}",
                                                   "sha256_lf": "0" * 64})
        self.assertIn("src/other.cpp#other", regions)

    def snapshot(self) -> tuple[bytes, bytes, bytes]:
        return self.statuses.read_bytes(), self.child.read_bytes(), self.census.read_bytes()

    def test_a_move_that_matches_nothing_fails_and_writes_nothing(self) -> None:
        before = self.snapshot()
        pairs = ((f"{OLD}#layer_z", f"{NEW}#layer_z"), ("src/none.cpp#gone", "src/x.cpp#other"))
        self.assertEqual(self.move(*pairs), 2)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.move(*pairs, extra=["--dry-run"]), 2)
        self.assertEqual(self.move(*pairs, extra=["--allow-unmatched"]), 0)
        self.assertIn(f"{NEW}#layer_z".encode(), self.statuses.read_bytes())

    def test_a_file_move_into_a_file_lacking_the_symbols_fails(self) -> None:
        (self.root / "src/y.cpp").write_text("int unrelated;", encoding="utf-8")
        before = self.snapshot()
        self.assertEqual(self.move((OLD, "src/y.cpp")), 2)
        self.assertEqual(self.snapshot(), before)

    def test_only_the_final_location_of_a_chain_is_checked(self) -> None:
        self.assertEqual(self.move((OLD, "src/absent.cpp"), ("src/absent.cpp", NEW)), 0)
        self.assertTrue(all(code.startswith(NEW + "#") for code in self.anchors()))

    def test_census_keys_are_not_symbol_checked(self) -> None:
        (self.root / "src/y.cpp").write_text("int unrelated;", encoding="utf-8")
        self.assertEqual(self.move(("src/other.cpp#other", "src/y.cpp#logical_name")), 0)
        self.assertIn("src/y.cpp#logical_name", json.loads(self.census.read_text(encoding="utf-8"))["regions"])

    def test_a_new_that_is_not_in_the_tree_fails(self) -> None:
        before = self.snapshot()
        self.assertEqual(self.move((f"{OLD}#layer_z", "src/missing.cpp#layer_z")), 2)
        self.assertEqual(self.move((f"{OLD}#layer_z", f"{NEW}#not_there")), 2)
        self.assertEqual(self.snapshot(), before)

    def test_chains_end_at_the_last_move_and_cycles_fail(self) -> None:
        self.assertEqual(self.move((OLD, "src/x.cpp"), ("src/x.cpp", NEW)), 0)
        self.assertEqual(self.anchors(), [f"{NEW}#layer_z", f"{NEW}#max_speed", f"{NEW}#max_speed"])
        self.assertIn(f"{NEW}#apply", json.loads(self.census.read_text(encoding="utf-8"))["regions"])
        before = self.snapshot()
        self.assertEqual(self.move(("src/a.cpp", "src/b.cpp"), ("src/b.cpp", "src/a.cpp")), 2)
        self.assertEqual(self.snapshot(), before)

    def test_census_region_order_is_kept(self) -> None:
        region = {"path": "p", "start": "s", "end": "e", "sha256_lf": "0" * 64}
        self.write_census({"schema_version": 2, "purpose": "test", "regions": {
            "src/z.cpp#z": dict(region, path="src/z.cpp"), f"{OLD}#apply": dict(region, path=OLD),
            "src/a.cpp#a": dict(region, path="src/a.cpp")}})
        self.assertEqual(self.move((f"{OLD}#apply", f"{NEW}#apply")), 0)
        regions = json.loads(self.census.read_text(encoding="utf-8"))["regions"]
        self.assertEqual(list(regions), ["src/z.cpp#z", f"{NEW}#apply", "src/a.cpp#a"])

    def test_a_census_only_run_moves_the_regions(self) -> None:
        self.assertEqual(self.move((f"{OLD}#apply", f"{NEW}#apply")), 0)
        self.assertIn(NEW + "#apply", json.loads(self.census.read_text(encoding="utf-8"))["regions"])

    def test_a_write_failure_on_the_second_file_leaves_every_file_unchanged(self) -> None:
        before = self.snapshot()
        real, calls = tr.os.replace, []

        def flaky(source, target):
            calls.append(target)
            if len(calls) == 2:
                raise OSError("simulated write failure")
            return real(source, target)

        tr.os.replace = flaky
        try:
            self.assertEqual(self.move((OLD, NEW)), 2)
        finally:
            tr.os.replace = real
        self.assertEqual(len(calls), 2)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual([p.name for p in self.root.rglob("*.relocate-tmp")], [])

    def test_a_move_that_merges_two_entries_of_a_row_fails(self) -> None:
        shard = tr.load_registry(self.statuses)
        row = next(r for r in shard["rows"] if any(t["code"] == f"{OLD}#max_speed" for t in r.get("applied", ())))
        row["applied"].append({"code": f"{OLD}#layer_z", "rules": ["MV-11"]})
        self.write(self.statuses, shard)
        before = self.snapshot()
        self.assertEqual(self.move((f"{OLD}#layer_z", f"{OLD}#max_speed")), 2)
        self.assertEqual(self.snapshot(), before)

    def test_the_line_ending_of_a_file_is_kept(self) -> None:
        self.census.write_bytes(self.census.read_bytes().replace(b"\n", b"\r\n"))
        self.assertEqual(self.move((f"{OLD}#apply", f"{NEW}#apply")), 0)
        data = self.census.read_bytes()
        self.assertIn(b"\r\n", data)
        self.assertNotIn(b"\r\r", data)
        self.assertEqual(data.count(b"\n"), data.count(b"\r\n"))

    def scoped_move(self, entries: list[dict], extra: list[str] | None = None) -> int:
        self.moves.write_text(json.dumps(entries), encoding="utf-8")
        args = ["relocate", "--map", str(self.moves), "--statuses", str(self.statuses),
                "--census", str(self.census), "--repo", str(self.root)] + (extra or [])
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return tr.main(args)

    def test_distinct_tags_split_a_shared_anchor_without_moving_its_census_region(self) -> None:
        # The handler was shared by two fields and by a reviewed region before extraction.
        for path in (self.statuses, self.child):
            shard = tr.load_shard(path)
            for row in shard["rows"]:
                for target in row.get("applied", ()):
                    if target["code"] == f"{OLD}#max_speed":
                        target["code"] = f"{OLD}#apply"
            self.write(path, shard)
        census_before = self.census.read_bytes()
        entries = [
            {"old": f"{OLD}#apply", "new": f"{NEW}#max_speed", "tag": "Max_Speed"},
            {"old": f"{OLD}#apply", "new": "src/x.cpp#layer_z", "tag": "@Name"},
        ]
        self.assertEqual(self.scoped_move(entries, ["--dry-run"]), 0)
        self.assertEqual(self.scoped_move(entries), 0)
        self.assertEqual(self.census.read_bytes(), census_before)
        self.assertEqual(self.anchors(), sorted([f"{NEW}#max_speed", "src/x.cpp#layer_z", f"{OLD}#layer_z"]))
        self.assertEqual(self.check(), 0)

    def test_an_unmatched_tag_rolls_back_matching_unscoped_moves(self) -> None:
        before = self.snapshot()
        entries = [
            {"old": f"{OLD}#layer_z", "new": f"{NEW}#layer_z"},
            {"old": f"{OLD}#max_speed", "new": f"{NEW}#max_speed", "tag": "Not_Authored"},
        ]
        self.assertEqual(self.scoped_move(entries), 2)
        self.assertEqual(self.snapshot(), before)

    def test_a_duplicate_old_in_the_same_tag_is_rejected(self) -> None:
        before = self.snapshot()
        entries = [
            {"old": f"{OLD}#max_speed", "new": f"{NEW}#max_speed", "tag": "Max_Speed"},
            {"old": f"{OLD}#max_speed", "new": "src/x.cpp#other", "tag": "Max_Speed"},
        ]
        self.assertEqual(self.scoped_move(entries), 2)
        self.assertEqual(self.snapshot(), before)

    def test_scoped_targets_are_checked_before_writing(self) -> None:
        before = self.snapshot()
        self.assertEqual(self.scoped_move([
            {"old": f"{OLD}#max_speed", "new": f"{NEW}#missing", "tag": "Max_Speed"},
        ]), 2)
        self.assertEqual(self.snapshot(), before)

    def test_scoped_override_wins_and_global_move_still_applies_to_other_tags(self) -> None:
        self.assertEqual(self.scoped_move([
            {"old": OLD, "new": NEW},
            {"old": f"{OLD}#max_speed", "new": "src/x.cpp#other", "tag": "@Name"},
        ]), 0)
        self.assertEqual(self.anchors(), sorted([f"{NEW}#max_speed", f"{NEW}#layer_z", "src/x.cpp#other"]))
        # The ordinary file move still relocates the census region, without the tag override.
        self.assertIn(f"{NEW}#apply", json.loads(self.census.read_text())["regions"])

    def test_cycles_crossing_scoped_and_unscoped_moves_are_rejected(self) -> None:
        before = self.snapshot()
        self.assertEqual(self.scoped_move([
            {"old": f"{OLD}#max_speed", "new": f"{NEW}#max_speed", "tag": "Max_Speed"},
            {"old": f"{NEW}#max_speed", "new": f"{OLD}#max_speed"},
        ]), 2)
        self.assertEqual(self.snapshot(), before)

    def test_a_scoped_move_that_matches_only_a_census_region_is_unmatched(self) -> None:
        before = self.snapshot()
        self.assertEqual(self.scoped_move([
            {"old": f"{OLD}#apply", "new": f"{NEW}#apply", "tag": "Max_Speed"},
        ]), 2)
        self.assertEqual(self.snapshot(), before)


if __name__ == "__main__":
    import unittest

    unittest.main()
