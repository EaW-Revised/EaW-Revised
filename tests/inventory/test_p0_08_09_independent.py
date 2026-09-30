"""Independent P0-08/P0-09 contract fixtures.

These cases are authored here rather than copied from the checked-in generated
inventories.  They exercise the parser/analyzers and XML provenance directly.
"""
from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "inventory"))

from corpus import Corpus, CorpusError, Source  # noqa: E402
from lua_calls import LuaError, PGLUA_HEADER, analyze_pglua, parse_source_calls  # noqa: E402
from lua_inventory import generate as generate_lua  # noqa: E402
from xml_inventory import SchemaLookup, XmlInventoryError, parse_xml  # noqa: E402


def instruction(op: int, a: int = 0, b: int = 0, c: int = 0, bx: int | None = None) -> int:
    return op | (a << 24) | ((bx << 6) if bx is not None else (b << 15) | (c << 6))


def pglua_string(value: bytes | None) -> bytes:
    if value is None:
        return struct.pack("<I", 0)
    return struct.pack("<I", len(value) + 1) + value + b"\0"


def prototype(pid: int, constants: list[bytes], code: list[int], *, debug: bool = True) -> bytes:
    value = pglua_string(b"@independent") + struct.pack("<iiBBBB", 1, pid, 0, 0, 0, 4)
    lines = [1] * len(code) if debug else []
    value += struct.pack("<i", len(lines)) + b"".join(struct.pack("<i", n) for n in lines)
    locals_ = [(b"local_value", 0, len(code))] if debug else []
    value += struct.pack("<i", len(locals_))
    for name, start, end in locals_:
        value += pglua_string(name) + struct.pack("<ii", start, end)
    upnames = [b"captured"] if debug else []
    value += struct.pack("<i", len(upnames)) + b"".join(pglua_string(name) for name in upnames)
    value += struct.pack("<i", len(constants))
    for value_ in constants:
        value += b"\4" + pglua_string(value_)
    value += struct.pack("<i", 0)
    value += struct.pack("<i", len(code)) + b"".join(struct.pack("<I", word) for word in code)
    return value


class SourceContractTests(unittest.TestCase):
    def test_original_scope_shadow_alias_branch_loop_receiver_and_noncode_fixture(self) -> None:
        source = br'''-- Comment_Call("not code")
local text = "String_Call()"
local alias = Engine_Do
function helper(x)
  if x then Branch_Call() else Else_Call() end
  while x do Loop_Call(); break end
end
local function local_helper() Local_Call() end
local_helper()
alias(
  7
)
object:Receiver_Call()
unknown()[key]()
'''
        calls, helpers = parse_source_calls(source)
        observed = {(c.symbol, c.call_kind, c.reason) for c in calls}
        self.assertEqual(helpers, {"helper"})
        self.assertIn(("Engine_Do", "global", None), observed)
        self.assertIn(("Branch_Call", "global", None), observed)
        self.assertIn(("Else_Call", "global", None), observed)
        self.assertIn(("Loop_Call", "global", None), observed)
        self.assertIn(("Receiver_Call", "method", None), observed)
        self.assertIn((None, None, "dynamic call expression"), observed)
        self.assertIn((None, None, "script-local function"), observed)
        self.assertFalse(any(c.symbol in {"Comment_Call", "String_Call"} for c in calls))
        alias_call = next(c for c in calls if c.symbol == "Engine_Do")
        self.assertGreaterEqual(alias_call.line or 0, 10)
        self.assertGreaterEqual(alias_call.column or 0, 1)

    def test_malformed_source_is_an_error(self) -> None:
        with self.assertRaises(LuaError):
            parse_source_calls(b'Broken("unterminated')
        with self.assertRaises(LuaError):
            parse_source_calls(b'x("bad\nstill")')

    def test_inventory_records_malformed_source_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "game"
            mod = Path(temp) / "mod" / "Data"
            for data_root in (root / "GameData" / "Data", root / "corruption" / "Data", mod):
                data_root.mkdir(parents=True)
                (data_root / "MegaFiles.xml").write_text(
                    "<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
            broken = root / "GameData" / "Data" / "Scripts" / "broken.lua"
            broken.parent.mkdir(parents=True)
            broken.write_bytes(b'x("bad\nstill")')
            out = Path(temp) / "out"
            summary = generate_lua(root, mod, ROOT / "plan" / "inventories" / "lua-declarations.json", out)
            self.assertEqual(summary["parse_failures"], 3)
            manifest = __import__("json").loads((out / "lua-manifest.json").read_text(encoding="utf-8"))
            for profile in manifest["profiles"]:
                failures = [r for r in profile["file_outcomes"] if not r["parsed"]]
                self.assertEqual(len(failures), 1)
                self.assertEqual(failures[0]["diagnostic"]["code"], "EAWR-INV-LUA-0001")


class CompiledContractTests(unittest.TestCase):
    def test_cfg_merge_tailcall_debug_and_unsupported_opcode(self) -> None:
        merge = [
            instruction(5, 0, bx=0),       # R0 = Alpha
            instruction(24, 1),             # branch to PCs 2/3
            instruction(20, bx=131074),     # true path -> PC 6
            instruction(5, 0, bx=1),       # false path: R0 = Beta
            instruction(20, bx=131072),     # false path -> PC 6
            instruction(0, 1, 1),           # unreachable filler
            instruction(25, 0, 1, 1),       # merged callable
            instruction(27),
        ]
        calls, detail = analyze_pglua(PGLUA_HEADER + prototype(1, [b"Alpha", b"Beta"], merge))
        self.assertEqual(detail["prototype_count"], 1)
        self.assertEqual(len(calls), 1)
        self.assertEqual((calls[0].symbol, calls[0].reason, calls[0].prototype_id, calls[0].pc),
                         (None, "merged control-flow callable", 1, 6))

        tail, _ = analyze_pglua(PGLUA_HEADER + prototype(
            1, [b"Tail"], [instruction(5, 0, bx=0), instruction(26, 0, 1, 1)]))
        self.assertEqual([(c.symbol, c.opcode, c.prototype_id, c.pc) for c in tail],
                         [("Tail", "TAILCALL", 1, 1)])

        unsupported, detail = analyze_pglua(PGLUA_HEADER + prototype(
            1, [], [instruction(32), instruction(27)]))
        self.assertEqual(detail["unsupported_instructions"],
                         [{"opcode": "SETLISTO", "pc": 0, "prototype_id": 1}])
        self.assertEqual(unsupported, [])

    def test_register_lifetime_kills_all_loadnil_destinations(self) -> None:
        calls, _ = analyze_pglua(PGLUA_HEADER + prototype(
            1, [b"Should_Not_Survive"], [
                instruction(5, 1, bx=0),       # R1 = global
                instruction(3, 0, 1),           # LOADNIL R0 through R1
                instruction(25, 1, 1, 1),       # R1 is no longer callable
            ]))
        self.assertEqual(len(calls), 1)
        self.assertIsNone(calls[0].symbol)
        self.assertEqual(calls[0].reason, "unknown callable provenance")

    def test_register_lifetime_kills_all_call_result_destinations(self) -> None:
        calls, _ = analyze_pglua(PGLUA_HEADER + prototype(
            1, [b"Should_Not_Survive", b"Callee"], [
                instruction(5, 1, bx=0),       # R1 = global
                instruction(5, 0, bx=1),       # R0 = callee
                instruction(25, 0, 1, 3),      # two returned values overwrite R0/R1
                instruction(25, 1, 1, 1),      # R1 is no longer callable
            ]))
        self.assertEqual([(c.pc, c.symbol, c.reason) for c in calls], [
            (2, "Callee", None), (3, None, "unknown callable provenance")])


SCHEMA = {
    "revision": "synthetic-independent",
    "files": [],
    "metafiles": [],
    "types": [{"name": "Unit", "game": "eaw"}, {"name": "Ship", "game": "eaw"}],
    "tags": [
        {"available_since": None, "deprecated": False, "game": "eaw", "object_type": "Unit",
         "schema_ref": "eaw/Unit.yaml#L2", "tag": "Value"},
        {"available_since": "FoC 1.0", "deprecated": False, "game": "foc", "object_type": "Unit",
         "schema_ref": "foc/Unit.yaml#L3", "tag": "Value"},
        {"available_since": None, "deprecated": True, "game": "eaw", "object_type": "Unit",
         "schema_ref": "eaw/Unit.yaml#L4", "tag": "Old"},
    ],
}


def xml_source(data: bytes) -> Source:
    return Source("data/xml/independent.xml", "independent:loose:Data/XML/Independent.xml",
                  "loose", "independent", data, "independent-hash")


class XmlContractTests(unittest.TestCase):
    def test_context_order_attributes_lines_and_schema_status(self) -> None:
        schema = SchemaLookup(SCHEMA)
        data = b'''<Root>\n  <Unit Name="A"><Group><Value>1</Value><Value>2</Value></Group><Old/></Unit>\n  <Ship Name="B"><Group><Value>3</Value></Group></Ship>\n</Root>'''
        rows = parse_xml(xml_source(data), "eaw", schema)
        values = [r for r in rows if r.tag_name == "Value"]
        self.assertEqual([(r.object_type, r.tag_path, r.order) for r in values], [
            ("Unit", "Unit/Group/Value", 5), ("Unit", "Unit/Group/Value", 6),
            ("Ship", "Ship/Group/Value", 11)])
        self.assertEqual(next(r for r in rows if r.node_kind == "attribute").tag_path, "Unit/@Name")
        self.assertEqual(schema.status("eaw", "Unit", "Old")[0], "deprecated")
        self.assertEqual(schema.status("foc", "Unit", "Value")[2], ["FoC 1.0", "eaw", "foc"])

    def test_malformed_xml_and_external_doctype_fail(self) -> None:
        schema = SchemaLookup(SCHEMA)
        with self.assertRaises(XmlInventoryError):
            parse_xml(xml_source(b"<Root>"), "eaw", schema)
        with self.assertRaises(XmlInventoryError):
            parse_xml(xml_source(b'<!DOCTYPE x SYSTEM "file:///x"><Root/>'), "eaw", schema)
        utf16_doctype = '<!DOCTYPE x SYSTEM "file:///x"><Root/>'.encode("utf-16")
        with self.assertRaises(XmlInventoryError):
            parse_xml(xml_source(utf16_doctype), "eaw", schema)


class CorpusContractTests(unittest.TestCase):
    def test_missing_data_root_is_not_an_empty_success(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "GameData").mkdir()
            with self.assertRaises(CorpusError):
                list(Corpus(root).iter_sources("eaw", ".lua", "raw"))


if __name__ == "__main__":
    unittest.main()
