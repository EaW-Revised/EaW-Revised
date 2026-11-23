"""d3d9_bytecode: the vs_1_1 / ps_1_x token reader, on synthetic streams built here."""

from __future__ import annotations

from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "shaders"))

import d3d9_bytecode as bc  # noqa: E402


def dst(reg_type: int, number: int, mask: int = 0xF) -> int:
    return 0x80000000 | ((reg_type & 7) << 28) | (((reg_type >> 3) & 3) << 11) | (mask << 16) | number


def src(reg_type: int, number: int, swizzle: int = 0xE4, modifier: int = 0) -> int:
    return 0x80000000 | ((reg_type & 7) << 28) | (((reg_type >> 3) & 3) << 11) | (swizzle << 16) | (modifier << 24) | number


def words(*tokens: int) -> list:
    return list(tokens)


class SyntheticStreams(unittest.TestCase):
    def test_pixel_shader_disassembles(self) -> None:
        stream = words(0xFFFF0101, 66, dst(3, 0), 4, dst(0, 0), src(3, 0), src(1, 0), src(0, 0, 0xE4, 4),
                       5, dst(0, 0, 0x7), src(0, 0), src(2, 1), 0xFFFF)
        # opcode 66 (tex) has no sources; opcode 4 (mad) has three.
        shader, nxt = bc.read_shader(stream, 0)
        self.assertEqual(nxt, len(stream))
        self.assertEqual(shader.name, "ps_1_1")
        self.assertEqual(shader.opcodes(), ["tex", "mad", "mul"])
        self.assertEqual(shader.instructions[0].text, "tex t0")
        self.assertEqual(shader.instructions[1].text, "mad r0, t0, v0, r0_bx2")
        self.assertEqual(shader.instructions[2].text, "mul r0.xyz, r0, c1")

    def test_def_and_comment_and_dcl(self) -> None:
        one = struct.unpack("<I", struct.pack("<f", 1.0))[0]
        stream = words(0xFFFE0101, 0xFFFE | (2 << 16), 0x41424344, 0, 31, 0x80000000, dst(1, 0), 81, dst(2, 5),
                       one, 0, 0, 0, 1, dst(1, 0), src(1, 1), 0xFFFF)
        shader, _ = bc.read_shader(stream, 0)
        self.assertEqual(shader.constants[0][0], "c5")
        self.assertEqual(shader.constants[0][1], (1.0, 0.0, 0.0, 0.0))
        self.assertEqual(shader.opcodes(), ["dcl", "def", "mov"])
        self.assertEqual(len(shader.comments), 1)

    def test_fail_closed(self) -> None:
        with self.assertRaises(bc.BytecodeError):  # opcode outside the table
            bc.read_shader(words(0xFFFF0101, 200, dst(0, 0), 0xFFFF), 0)
        with self.assertRaises(bc.BytecodeError):  # no end token
            bc.read_shader(words(0xFFFF0101, 1, dst(0, 0), src(1, 0)), 0)
        with self.assertRaises(bc.BytecodeError):  # truncated instruction
            bc.read_shader(words(0xFFFF0101, 4, dst(0, 0), src(1, 0)), 0)
        with self.assertRaises(bc.BytecodeError):  # destination token without the high bit
            bc.read_shader(words(0xFFFF0101, 1, 0x00000000, src(1, 0), 0xFFFF), 0)
        with self.assertRaises(bc.BytecodeError):  # pixel shader 2.0 is not read
            bc.read_shader(words(0xFFFF0200, 0xFFFF), 0)
        with self.assertRaises(bc.BytecodeError):  # relative addressing in a pixel shader
            bc.read_shader(words(0xFFFF0101, 1, dst(0, 0), src(1, 0) | (1 << 13), 0xFFFF), 0)

    def test_scan_skips_coincidental_patterns(self) -> None:
        good = words(0xFFFF0101, 66, dst(3, 0), 0xFFFF)
        noise = words(0xFFFF0101, 999, 0)
        blob = struct.pack("<%dI" % (len(noise) + len(good)), *(noise + good))
        found = list(bc.find_shaders(blob))
        self.assertEqual([s.opcodes() for s in found], [["tex"]])


def comment_words(raw: bytes) -> list:
    """A comment instruction carrying ``raw`` (padded to whole dwords)."""
    padded = raw + bytes(-len(raw) % 4)
    return [0xFFFE | ((len(padded) // 4) << 16)] + list(struct.unpack("<%dI" % (len(padded) // 4), padded))


def constant_table_comment(entries: list, size: int = 28, name_cut: bool = False, count_extra: int = 0) -> list:
    """A synthetic constant table: ``entries`` are (name, first register, register count)."""
    info_at = 28
    names_at = info_at + 20 * len(entries)
    names = b""
    rows = b""
    for name, register, number in entries:
        rows += struct.pack("<IHHHHII", names_at + len(names), 2, register, number, 0, 0, 0)
        names += name.encode("ascii") + (b"" if name_cut else b"\x00")
    header = struct.pack("<7I", size, 0, 0x0101, len(entries) + count_extra, info_at, 0, 0)
    return comment_words(b"CTAB" + header + rows + names)


def vertex_shader_with(*comment_streams: list) -> list:
    stream = [0xFFFE0101]
    for comment in comment_streams:
        stream += comment
    return stream + [0xFFFF]


class ConstantTable(unittest.TestCase):
    def table(self, stream: list) -> list:
        shader, _ = bc.read_shader(stream, 0)
        return bc.constant_table(shader)

    def test_reads_names_registers_and_counts(self) -> None:
        stream = vertex_shader_with(constant_table_comment([("ConstantA", 4, 4), ("ConstantB", 8, 1)]))
        self.assertEqual(self.table(stream), [("ConstantA", 4, 4), ("ConstantB", 8, 1)])

    def test_no_table_is_empty(self) -> None:
        self.assertEqual(self.table(vertex_shader_with()), [])
        self.assertEqual(self.table(vertex_shader_with(comment_words(b"NOTC" + bytes(40)))), [])
        self.assertEqual(self.table(vertex_shader_with(comment_words(b"CTAB" + bytes(8)))), [])  # shorter than a header

    def test_inconsistent_header_fails_closed(self) -> None:
        with self.assertRaises(bc.BytecodeError):  # header size is not 28
            self.table(vertex_shader_with(constant_table_comment([("ConstantA", 0, 1)], size=32)))
        with self.assertRaises(bc.BytecodeError):  # more entries declared than the table holds
            self.table(vertex_shader_with(constant_table_comment([("ConstantA", 0, 1)], count_extra=50)))

    def test_name_without_a_terminator_fails_closed(self) -> None:
        with self.assertRaises(bc.BytecodeError):
            self.table(vertex_shader_with(constant_table_comment([("Cnst", 0, 1)], name_cut=True)))


class TruncatedInput(unittest.TestCase):
    def test_truncated_comment(self) -> None:
        # The comment announces 40 dwords but the stream ends after two.
        with self.assertRaises(bc.BytecodeError):
            bc.read_shader([0xFFFE0101, 0xFFFE | (40 << 16), 0x41424344, 0x45464748], 0)

    def test_truncated_dcl(self) -> None:
        with self.assertRaises(bc.BytecodeError):
            bc.read_shader([0xFFFE0101, 31, 0x80000000], 0)

    def test_phase_token_is_not_accepted_in_a_pixel_shader(self) -> None:
        with self.assertRaises(bc.BytecodeError):
            bc.read_shader(words(0xFFFF0101, 0xFFFD, 66, dst(3, 0), 0xFFFF), 0)


if __name__ == "__main__":
    unittest.main()
