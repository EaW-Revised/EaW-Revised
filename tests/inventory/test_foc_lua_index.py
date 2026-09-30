"""FoC registration differences and inventory routing for #293."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/inventory"))
from lua_inventory import DeclarationIndex, generate, index_for_profile  # noqa: E402


class FocLuaIndexTests(unittest.TestCase):
    def test_audited_visible_name_differences(self) -> None:
        glyphx = json.loads((ROOT / "plan/inventories/lua-declarations.json").read_text())
        foc = json.loads((ROOT / "plan/inventories/foc-lua-registrations.json").read_text())
        self.assertEqual(foc["schema_version"], 1)
        self.assertEqual(len(foc["entries"]), 472)
        self.assertTrue(all(set(row) == {"exposed_name", "call_kind", "receiver_type"}
                            for row in foc["entries"]))

        def names(rows: list[dict], kind: str) -> set[str]:
            return {row["exposed_name"] for row in rows if row["call_kind"] == kind}

        gg, gm = names(glyphx["entries"], "global"), names(glyphx["entries"], "method")
        fg, fm = names(foc["entries"], "global"), names(foc["entries"], "method")
        self.assertEqual((len(fg - gg), len(fm - gm), len(gm - fm)), (20, 61, 22))
        self.assertEqual(gg - fg, set())
        self.assertEqual(gm - fm, {row["exposed_name"] for row in glyphx["entries"]
                                    if row["receiver_type"] == "wide_string"})
        self.assertIn({"exposed_name": "Add_Force", "call_kind": "method",
                       "receiver_type": "taskforce"}, foc["entries"])

    def test_foc_profiles_drive_stub_and_missing_api_lookup(self) -> None:
        glyphx = DeclarationIndex(ROOT / "plan/inventories/lua-declarations.json")
        foc = DeclarationIndex(ROOT / "plan/inventories/foc-lua-registrations.json")
        self.assertIs(index_for_profile("eaw", glyphx, foc), glyphx)
        for profile in ("foc", "remake"):
            selected = index_for_profile(profile, glyphx, foc)
            self.assertIs(selected, foc)
            self.assertEqual(selected.match("GUI_Component_Enable", "global")[0], "present")
            self.assertEqual(selected.match("Add_Force", "method")[:2],
                             ("present", "taskforce"))
            self.assertEqual(selected.match("append", "method")[0], "absent")
        self.assertEqual(glyphx.match("GUI_Component_Enable", "global")[0], "absent")

    def test_foc_inventory_reports_registered_call_as_engine(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / "game"
            mod = root / "mod/Data"
            for data in (game / "GameData/Data", game / "corruption/Data", mod):
                data.mkdir(parents=True)
                (data / "MegaFiles.xml").write_text(
                    "<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
            script = game / "corruption/Data/Scripts/IndexProbe.lua"
            script.parent.mkdir()
            script.write_text("GUI_Component_Enable()\n", encoding="utf-8")
            out = root / "out"
            generate(game, mod, ROOT / "plan/inventories/lua-declarations.json", out)
            rows = json.loads((out / "lua-api.json").read_text())["rows"]
            row = next(row for row in rows if row["profile"] == "foc" and
                       row["symbol"] == "GUI_Component_Enable")
            self.assertEqual((row["classification"], row["reference_status"]),
                             ("engine", "present"))

    def test_shared_foc_methods_keep_all_registered_receivers(self) -> None:
        foc = DeclarationIndex(ROOT / "plan/inventories/foc-lua-registrations.json")
        for symbol, expected in (
            ("Move_To", ["GameObjectWrapper", "taskforce"]),
            ("Attack_Move", ["GameObjectWrapper", "land_taskforce", "space_taskforce"]),
        ):
            status, receiver, _refs, receivers = foc.match(symbol, "method")
            self.assertEqual((status, receiver, receivers), ("present", None, expected))

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / "game"
            mod = root / "mod/Data"
            for data in (game / "GameData/Data", game / "corruption/Data", mod):
                data.mkdir(parents=True)
                (data / "MegaFiles.xml").write_text(
                    "<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
            script = game / "corruption/Data/Scripts/IndexProbe.lua"
            script.parent.mkdir()
            script.write_text("unit:Move_To(target)\nunit:Attack_Move(target)\n", encoding="utf-8")
            out = root / "out"
            generate(game, mod, ROOT / "plan/inventories/lua-declarations.json", out)
            rows = json.loads((out / "lua-api.json").read_text())["rows"]
            for symbol, expected in (
                ("Move_To", ["GameObjectWrapper", "taskforce"]),
                ("Attack_Move", ["GameObjectWrapper", "land_taskforce", "space_taskforce"]),
            ):
                row = next(r for r in rows if r["profile"] == "foc" and r["symbol"] == symbol)
                self.assertEqual((row["classification"], row["reference_status"],
                                  row["receiver_type"], row["receiver_types"]),
                                 ("engine", "present", None, expected))

    def test_dotted_foc_call_matches_method_and_member_with_receiver_uncertainty(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / "game"
            mod = root / "mod/Data"
            for data in (game / "GameData/Data", game / "corruption/Data", mod):
                data.mkdir(parents=True)
                (data / "MegaFiles.xml").write_text(
                    "<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
            script = game / "corruption/Data/Scripts/IndexProbe.lua"
            script.parent.mkdir()
            script.write_text("MainForce.Produce_Force(x)\n", encoding="utf-8")
            out = root / "out"
            generate(game, mod, ROOT / "plan/inventories/lua-declarations.json", out)
            rows = json.loads((out / "lua-api.json").read_text())["rows"]
            row = next(r for r in rows if r["profile"] == "foc" and
                       r["symbol"] == "MainForce.Produce_Force")
            self.assertEqual((row["call_kind"], row["classification"], row["reference_status"],
                              row["receiver_type"], row["receiver_types"]),
                             ("dotted", "engine", "present", None, ["taskforce"]))
            glyphx = DeclarationIndex(ROOT / "plan/inventories/lua-declarations.json")
            self.assertEqual(glyphx.match("MainForce.Produce_Force", "dotted")[0], "absent")

            member_index = root / "member-index.json"
            member_index.write_text(json.dumps({"schema_version": 1, "source": "FoC fixture",
                                                "entries": [{"exposed_name": "Member_Call",
                                                             "call_kind": "member",
                                                             "receiver_type": "FixtureReceiver"}]}),
                                    encoding="utf-8")
            self.assertEqual(DeclarationIndex(member_index).match("obj.Member_Call", "dotted"),
                             ("present", None, [], ["FixtureReceiver"]))


if __name__ == "__main__":
    unittest.main()
