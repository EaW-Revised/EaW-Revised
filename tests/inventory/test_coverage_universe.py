"""Missing-pair regressions: authored context and engine recognition are independent."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from tools.inventory import coverage_universe as cu, tag_registry as tr
from tools.inventory.xml_inventory import SchemaLookup


def authored(cls="SpaceUnit", tag="Health", profile="stock", owner="GameObjectType"):
    return {"class": cls, "tag": tag, "canonical_types": [owner], "area": "combat", "owning_lane": "SK",
            "sources": [{"profile": profile, "occurrences": 1, "recovered": False}]}


def todo(row):
    return {"classes": [row["class"]], "tag": row["tag"], "status": "todo", "area": "combat", "ticket": 6215}


class CoverageFixtures(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.directory = self.root / "docs/tag-coverage"
        self.directory.mkdir(parents=True)
        # Use every real declared profile: no special opt-in registry rows.
        self.declarations = cu.read(cu.PROFILES)
        self.owners = {"schema_version": 1, "owners": {"object-definition": "GameObjectType"}}
        self.catalog = {"schema_version": 1, "evidence": {"ET-1": "debug build"}, "rows": [
            {"tag": "Health", "table_kind": "object-definition", "value_type": "float", "evidence": "ET-1"}]}
        self.rows = [authored()]
        self.profiles = []
        for p in self.declarations["profiles"]:
            self.profiles.append({"id": p["id"], "layers_leaf_first": p["layers_leaf_first"],
                                  "xml_files": 1, "content_sha256": "a" * 64,
                                  "manifest_sha256": "b" * 64, "pairs": int(p["id"] == "stock")})

    def manifest(self):
        obligations, placeholders = cu.project_engine(self.rows, self.catalog, self.owners)
        profiles = copy.deepcopy(self.profiles)
        for p in profiles:
            p["pairs"] = sum(any(s["profile"] == p["id"] for s in r["sources"]) for r in self.rows)
        return {"schema_version": 1, "profiles_sha256": cu.fingerprint(self.declarations),
                "owners_sha256": cu.fingerprint(self.owners), "profiles": profiles,
                "pairs": self.rows + placeholders, "engine_obligations": obligations}

    def files(self, manifest=None):
        for name, value in (("coverage-profiles.json", self.declarations),
                            ("coverage-parser-owners.json", self.owners),
                            ("coverage-universe.json", manifest or self.manifest()), ("engine-tags.json", self.catalog)):
            cu.write(self.directory / name, value)

    def load(self):
        return cu.load(*(self.directory / name for name in ("coverage-universe.json", "coverage-profiles.json",
                                                           "coverage-parser-owners.json", "engine-tags.json")))

    def problems(self, rows=None):
        manifest, _ = self.load()
        universe = tr.load_universe({"rows": []}, coverage=manifest)
        registry = {"schema_version": 2, "evidence": {}, "rows": sorted(rows or [todo(self.rows[0])], key=tr.row_order)}
        return "\n".join(tr.registry_problems(registry, universe, None))

    def test_new_catalog_tag_owner_and_value_type_fail_independently(self):
        self.files()
        for change in ({"tag": "UnrowedEngineTag"}, {"table_kind": "new-owner"}, {"value_type": "integer"}):
            with self.subTest(change=change):
                catalog = copy.deepcopy(self.catalog)
                catalog["rows"].append({**catalog["rows"][0], **change})
                cu.write(self.directory / "engine-tags.json", catalog)
                with self.assertRaisesRegex(cu.CoverageError, "engine (obligation|owner mapping)"):
                    self.load()

    def test_catalog_only_pair_after_refresh_requires_own_row(self):
        self.catalog["rows"].append({**self.catalog["rows"][0], "tag": "UnauthoredDefault"})
        self.files()
        self.assertIn("GameObjectType/UnauthoredDefault has no registry row", self.problems())

    def test_each_target_profile_requires_unclaimed_mod_pair(self):
        for profile in self.declarations["profiles"]:
            if profile["id"] == "stock":
                continue
            with self.subTest(profile=profile["id"]):
                self.rows = [authored(), authored(tag="ModOnly", profile=profile["id"])]
                self.files()
                self.assertIn("SpaceUnit/ModOnly has no registry row", self.problems())

    def test_known_name_new_class_and_nested_context_fail(self):
        for row in (authored(cls="Container"), authored(tag="Abilities/New_Ability/Health")):
            with self.subTest(row=row):
                self.rows = [authored(), row]
                self.files()
                self.assertIn(f"{row['class']}/{row['tag']} has no registry row", self.problems())

    def test_all_carrier_starting_and_reserve_tiers_need_exact_rows(self):
        for kind in ("Starting", "Reserve"):
            for tier in range(6):
                row = authored(cls="NewCarrierType", tag=f"{kind}_Spawned_Units_Tech_{tier}", profile="bfc")
                with self.subTest(kind=kind, tier=tier):
                    self.rows = [authored(), row]
                    self.files()
                    self.assertIn(row["tag"] + " has no registry row", self.problems())

    def test_sourced_no_target_and_todo_pass_without_claiming_application(self):
        self.files()
        row = todo(self.rows[0])
        self.assertEqual(self.problems([row]), "")
        value = {"rows": [row]}
        self.assertEqual(tr.counts(value)[0], {"todo": 1})
        self.assertIn("Todo by ticket", tr.render(value))
        row.update(status="foc-ignores", evidence="debug build: the field is not read")
        row.pop("ticket")
        self.assertEqual(self.problems([row]), "")
        row.update(status="applied")
        self.assertIn("an applied row lists the code", self.problems([row]))

    def test_concrete_rows_cannot_borrow_generic_or_other_class_acknowledgment(self):
        self.rows.append(authored(cls="Container"))
        self.files()
        generic = {**todo(self.rows[0]), "classes": ["GameObjectType"]}
        problems = self.problems([todo(self.rows[0]), generic])
        self.assertIn("Container/Health has no registry row", problems)
        self.assertIn("gameobjecttype/health is not in the loaded XML", problems)

    def test_engine_and_mod_members_accepted_but_unrelated_stale_row_rejected(self):
        self.rows.append(authored(tag="ModOnly", profile="bfc"))
        self.catalog["rows"].append({**self.catalog["rows"][0], "tag": "DefaultOnly"})
        self.files()
        manifest, _ = self.load()
        rows = [todo(r) for r in manifest["pairs"]]
        self.assertEqual(self.problems(rows), "")
        rows.append(todo(authored(tag="UnrelatedStale")))
        self.assertIn("spaceunit/unrelatedstale is not in the loaded XML", self.problems(rows))

    def test_removed_profile_missing_manifest_chain_and_fingerprint_metadata_fail(self):
        self.files()
        original = self.manifest()
        for change in ("removed", "chain", "fingerprint", "empty"):
            with self.subTest(change=change):
                value = copy.deepcopy(original)
                if change == "removed":
                    value["profiles"].pop()
                elif change == "chain":
                    next(p for p in value["profiles"] if p["id"] == "remake-chain")["layers_leaf_first"].reverse()
                elif change == "fingerprint":
                    value["profiles"][0]["content_sha256"] = ""
                else:
                    value["profiles"][0]["xml_files"] = 0
                cu.write(self.directory / "coverage-universe.json", value)
                with self.assertRaises(cu.CoverageError):
                    self.load()
        (self.directory / "coverage-universe.json").unlink()
        with self.assertRaises(FileNotFoundError):
            self.load()

    def test_profiles_declared_independently_cannot_be_silently_dropped(self):
        self.files()
        declarations = copy.deepcopy(self.declarations)
        declarations["profiles"].pop()
        cu.write(self.directory / "coverage-profiles.json", declarations)
        with self.assertRaisesRegex(cu.CoverageError, "profile declarations"):
            self.load()

    def test_named_concrete_definitions_wrappers_and_recovery_are_distinct(self):
        schema = SchemaLookup({"revision": "test", "types": [{"name": "GameObjectType"}], "tags": [], "metafiles": []})
        data = b'<ModEnvelope><SpaceUnit Name="X"><Health>1</Health><Value><SpaceUnit>text</SpaceUnit></Value></SpaceUnit></ModEnvelope>'
        events, recovered = cu.scan_xml(data, "data/xml/test.xml", schema, {}, {"spaceunit": "GameObjectType"})
        self.assertFalse(recovered)
        self.assertIn(("SpaceUnit/Health", "GameObjectType"), events)
        self.assertIn(("SpaceUnit/Value/SpaceUnit", "GameObjectType"), events)
        recovered_events, recovered = cu.scan_xml(data[:-len(b"</ModEnvelope>")], "data/xml/test.xml", schema, {}, {"spaceunit": "GameObjectType"})
        self.assertTrue(recovered)
        self.assertIn(("SpaceUnit/Health", "GameObjectType"), recovered_events)

    def test_seed_is_deterministic_and_never_changes_existing_status(self):
        self.rows.append(authored(tag="ModOnly", profile="bfc"))
        path = self.root / "statuses.json"
        tr.write_text(path, tr.dumps_registry({"schema_version": 2, "evidence": {}, "rows": [todo(self.rows[0])]}))
        self.assertEqual(cu.seed_rows(self.manifest(), path)["generated_pairs"], 1)
        before = path.read_bytes()
        self.assertEqual(cu.seed_rows(self.manifest(), path)["generated_pairs"], 0)
        self.assertEqual(path.read_bytes(), before)
        self.assertTrue(all(r["status"] == "todo" for r in tr.load_registry(path)["rows"]))

    def test_drive_root_and_unavailable_roots_never_scan(self):
        with mock.patch.object(Path, "rglob", side_effect=AssertionError("must not scan")):
            with self.assertRaisesRegex(cu.CoverageError, "root-level"):
                cu.safe_root(Path.cwd().anchor, "mod")
            with self.assertRaisesRegex(cu.CoverageError, "unavailable"):
                cu.safe_root(str(self.root / "missing"), "mod")

    def test_offline_refresh_checks_real_content_precedence_and_recovery(self):
        declarations = {"schema_version": 1, "profiles": [
            {"id": "stock", "layers_leaf_first": ["stock"]},
            {"id": "mod", "layers_leaf_first": ["leaf", "parent"]}]}
        roots = {}
        for identity in ("stock", "leaf", "parent"):
            root = self.root / identity
            data = root / ("data" if identity == "stock" else "Data") / "xml"
            data.mkdir(parents=True)
            (data / "units.xml").write_text('<SpaceUnit Name="X"><Health>1</Health></SpaceUnit>', encoding="utf-8")
            roots[identity] = str(root)
        schema = {"revision": "fixture", "types": [{"name": "GameObjectType"}], "tags": [], "metafiles": []}
        inventory = {"rows": [{"profile": "foc", "tag_path": "SpaceUnit/Health", "node_kind": "element",
                               "usage_count": 1, "file_count": 1, "object_type": "GameObjectType"}]}

        def generate():
            return cu.refresh(declarations, self.owners, roots, schema, inventory, self.catalog)

        with mock.patch("tools.inventory.xml_inventory.registry_type_hints", return_value={}):
            first = generate()
            cu.check_refresh(first, generate())
            (self.root / "leaf/Data/xml/units.xml").write_text('<SpaceUnit Name="X"><Health>2</Health></SpaceUnit>', encoding="utf-8")
            with self.assertRaisesRegex(cu.CoverageError, "fingerprint differs"):
                cu.check_refresh(first, generate())
            # Same names and pair count; a value-only update still fails freshness.
            self.assertEqual(first["pairs"], generate()["pairs"])
            declarations["profiles"][1]["layers_leaf_first"].reverse()
            with self.assertRaises(cu.CoverageError):
                cu.check_refresh(first, generate())
            (self.root / "parent/Data/xml/broken.xml").write_text('<SpaceUnit Name="Y"><NewTag>1', encoding="utf-8")
            recovered = generate()
            candidate = next(r for r in recovered["pairs"] if r["tag"] == "NewTag")
            self.assertTrue(candidate["sources"][0]["recovered"])
            roots.pop("leaf")
            with self.assertRaisesRegex(cu.CoverageError, "root unavailable"):
                generate()


if __name__ == "__main__":
    unittest.main()
