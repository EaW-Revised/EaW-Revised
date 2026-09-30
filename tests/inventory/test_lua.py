from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "inventory"))
from lua_calls import (LuaError, PGLUA_HEADER, PGLuaDecoder, analyze_pglua,  # noqa: E402
                       parse_source_calls)


def instruction(op: int, a: int = 0, b: int = 0, c: int = 0, bx: int | None = None) -> int:
    return op | (a << 24) | ((bx << 6) if bx is not None else (b << 15) | (c << 6))


def string(value: bytes | None) -> bytes:
    return struct.pack("<I", 0) if value is None else struct.pack("<I", len(value) + 1) + value + b"\0"


def prototype(pid: int, constants: list[object], code: list[int], nested: list[bytes] | None = None,
              debug: bool = False, source: bytes | None = b"@synthetic") -> bytes:
    nested = nested or []; value = string(source) + struct.pack("<iiBBBB", 1, pid, 0, 0, 0, 4)
    lines = [1] * len(code) if debug else []
    value += struct.pack("<i", len(lines)) + b"".join(struct.pack("<i", n) for n in lines)
    locals_ = [(b"local_value", 0, len(code))] if debug else []
    value += struct.pack("<i", len(locals_))
    for name, start, end in locals_: value += string(name) + struct.pack("<ii", start, end)
    upnames = [b"captured"] if debug else []
    value += struct.pack("<i", len(upnames)) + b"".join(string(name) for name in upnames)
    value += struct.pack("<i", len(constants))
    for item in constants:
        if item is None: value += b"\0"
        elif isinstance(item, float): value += b"\3" + struct.pack("<d", item)
        else: value += b"\4" + string(item)
    value += struct.pack("<i", len(nested)) + b"".join(nested)
    value += struct.pack("<i", len(code)) + b"".join(struct.pack("<I", word) for word in code)
    return value


class LuaSourceTests(unittest.TestCase):
    def test_parser_handles_scopes_aliases_calls_and_noncode(self) -> None:
        source = br'''
-- Fake_Call("comment")
local text = "String_Call()"
function helper(x) return x end
local alias = Engine_Do
alias(
  7
)
unit:Move_To("A")
math.floor(2)
local Engine_Shadow = 3
Engine_Shadow()
helper()
factory()[key]()
'''
        calls, helpers = parse_source_calls(source)
        observed = [(c.symbol, c.call_kind, c.reason) for c in calls]
        self.assertIn(("Engine_Do", "global", None), observed)
        self.assertIn(("Move_To", "method", None), observed)
        self.assertIn(("math.floor", "dotted", None), observed)
        self.assertTrue(any(c.reason == "dynamic or shadowed local callable" for c in calls))
        self.assertTrue(any(c.reason == "dynamic call expression" for c in calls))
        self.assertEqual(helpers, {"helper"})
        self.assertFalse(any(c.symbol == "Fake_Call" for c in calls))

    def test_short_string_line_endings_require_an_escape(self) -> None:
        for malformed in (b'Bad("raw\nnewline")', b'Bad("raw\rnewline")',
                          b'Bad("raw\r\nnewline")'):
            with self.subTest(malformed=malformed):
                with self.assertRaises(LuaError):
                    parse_source_calls(malformed)
        for valid in (b'Escaped("slash-n\\n")', b'Escaped("continued\\\nline")',
                      b'Escaped("continued\\\r\nline")'):
            with self.subTest(valid=valid):
                calls, _ = parse_source_calls(valid)
                self.assertEqual([(call.symbol, call.call_kind) for call in calls],
                                 [("Escaped", "global")])


class PGLuaTests(unittest.TestCase):
    S01 = bytes.fromhex("""
        1b 4c 75 70 51 01 04 04 04 06 08 09 09 08 b6 09
        93 68 e7 f5 7d 41 08 00 00 00 3d 28 6e 6f 6e 65
        29 00 00 00 00 00 01 00 00 00 00 00 00 02 00 00
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
        00 00 02 00 00 00 1b 80 00 00 1b 80 00 00
    """)

    def fixture(self) -> bytes:
        nested = prototype(2, [b"Nested"], [instruction(5, 0, bx=0), instruction(25), instruction(27)],
                           debug=True, source=None)
        constants = [b"Engine_Do", b"unit", b"Move_To"]
        code = [instruction(5, 0, bx=0), instruction(25),
                instruction(5, 0, bx=1), instruction(6, 0, 0, 252), instruction(25),
                instruction(34, 1, bx=0), instruction(27)]
        return PGLUA_HEADER + prototype(1, constants, code, [nested])

    def test_nested_debug_fixture_and_call_provenance(self) -> None:
        calls, detail = analyze_pglua(self.fixture())
        self.assertEqual(detail["prototype_count"], 2)
        self.assertEqual([(c.symbol, c.call_kind, c.prototype_id, c.pc) for c in calls],
                         [("Engine_Do", "global", 1, 1), ("Move_To", "method", 1, 4),
                          ("Nested", "global", 2, 1)])

    def test_contract_rejections(self) -> None:
        valid = self.S01
        self.assertEqual(PGLuaDecoder(valid).decode().persistence_id, 1)
        mutations = []
        for offset, value in ((3, 0x61), (4, 0x52), (5, 0), (7, 8), (13, 4), (33, 1)):
            changed = bytearray(valid); changed[offset] = value; mutations.append(bytes(changed))
        identifier = bytearray(valid); identifier[38:42] = b"\0\0\0\0"; mutations.append(bytes(identifier))
        mutations.extend([valid[:-1], valid + b"\0"])
        for value in mutations:
            with self.subTest(size=len(value)):
                with self.assertRaises(LuaError): PGLuaDecoder(value).decode()
        unknown = bytearray(PGLUA_HEADER + prototype(1, [b"X"], [instruction(27)]))
        unknown[65] = 1
        with self.assertRaises(LuaError): PGLuaDecoder(bytes(unknown)).decode()

    def test_open_call_results_kill_every_possible_destination(self) -> None:
        chunk = PGLUA_HEADER + prototype(1, [b"Callee", b"Stale"], [
            instruction(5, 3, bx=1),
            instruction(5, 0, bx=0),
            instruction(25, 0, 1, 0),
            instruction(25, 3, 1, 1),
        ])
        calls, _ = analyze_pglua(chunk)
        self.assertEqual([(call.pc, call.symbol, call.reason) for call in calls], [
            (2, "Callee", None), (3, None, "unknown callable provenance")])


if __name__ == "__main__": unittest.main()
