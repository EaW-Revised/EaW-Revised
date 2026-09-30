"""Independent synthetic PGLua boundary fixtures for the P0-07 review.

This module deliberately does not import eawr_script or copy its parser.  It
builds the fixed-width PGLua records from the approved byte contract and
performs an independent structural walk.  Runtime and retail evidence is
executed separately by the acceptance command/report; these tests are meant
to catch a fixture or contract regression without mirroring implementation
helpers.
"""

from __future__ import annotations

import hashlib
import struct
import unittest


HEADER = bytes.fromhex(
    "1b 4c 75 70 51 01 04 04 04 06 08 09 09 08 "
    "b6 09 93 68 e7 f5 7d 41"
)
RETURN = 0x0000801B  # opcode 27, A=0, B=1, C=0


def pglua_string(value: bytes) -> bytes:
    """Encode one PGLua string independently of the production reader."""
    if not value:
        return struct.pack("<I", 0)
    return struct.pack("<I", len(value) + 1) + value + b"\0"


def make_chunk(
    *,
    persistence_id: int = 1,
    max_stack: int = 2,
    constants: bytes = b"",
    constant_count: int | None = None,
    code: tuple[int, ...] = (RETURN, RETURN),
    source: bytes = b"=(none)",
) -> bytes:
    """Create a small, synthetic PGLua root prototype.

    The record order is written out here rather than delegated to a project
    serializer.  Constants are supplied as already-tagged records so tests
    can exercise unknown tags without teaching this writer the implementation
    format.
    """
    if constant_count is None:
        constant_count = 0 if not constants else 1
    body = bytearray(HEADER)
    body += pglua_string(source)
    body += struct.pack("<ii", 0, persistence_id)
    body += bytes((0, 0, 0, max_stack))
    # source lines, locals, upvalue names, then constant count and payload;
    # the nested-prototype count follows the variable-length constants.
    body += struct.pack("<iiii", 0, 0, 0, constant_count)
    body += constants
    body += struct.pack("<ii", 0, len(code))
    body += b"".join(struct.pack("<I", word) for word in code)
    return bytes(body)


class ChunkError(ValueError):
    pass


def independent_walk(chunk: bytes) -> dict[str, int]:
    """Validate the fixed synthetic subset without using the implementation."""
    if len(chunk) > 64 * 1024 * 1024:
        raise ChunkError("input quota")
    if len(chunk) < len(HEADER) or chunk[: len(HEADER)] != HEADER:
        raise ChunkError("header")
    at = len(HEADER)

    def take(n: int) -> bytes:
        nonlocal at
        if n < 0 or at + n > len(chunk):
            raise ChunkError("truncated")
        result = chunk[at : at + n]
        at += n
        return result

    def u32() -> int:
        return struct.unpack("<I", take(4))[0]

    def i32() -> int:
        return struct.unpack("<i", take(4))[0]

    def count(label: str) -> int:
        value = i32()
        if value < 0 or value > 1_000_000:
            raise ChunkError(label)
        return value

    string_size = u32()
    if string_size == 0:
        raise ChunkError("root source must be present")
    if string_size > 16 * 1024 * 1024:
        raise ChunkError("string quota")
    raw = take(string_size)
    if raw[-1:] != b"\0":
        raise ChunkError("string terminator")

    first_line = i32()
    persistence_id = i32()
    upvalues, parameters, vararg, max_stack = take(4)
    if first_line < 0 or persistence_id != 1 or max_stack == 0:
        raise ChunkError("prototype scalar")

    line_count = count("line count")
    take(4 * line_count)
    local_count = count("local count")
    for _ in range(local_count):
        size = u32()
        if size:
            local_name = take(size)
            if local_name[-1:] != b"\0":
                raise ChunkError("local terminator")
        take(8)
    upvalue_name_count = count("upvalue name count")
    for _ in range(upvalue_name_count):
        size = u32()
        if size:
            name = take(size)
            if name[-1:] != b"\0":
                raise ChunkError("upvalue terminator")

    constant_count = count("constant count")
    for _ in range(constant_count):
        tag = take(1)[0]
        if tag == 0:
            continue
        if tag == 3:
            take(8)
            continue
        if tag == 4:
            size = u32()
            if size > 16 * 1024 * 1024:
                raise ChunkError("constant string quota")
            value = take(size)
            if size and value[-1:] != b"\0":
                raise ChunkError("constant terminator")
            continue
        raise ChunkError("constant tag")

    if count("nested count") != 0:
        raise ChunkError("nested fixture not supported by this independent walk")
    instruction_count = count("instruction count")
    words = [u32() for _ in range(instruction_count)]
    if not words or any((word & 0x3F) > 34 for word in words):
        raise ChunkError("opcode")
    if at != len(chunk):
        raise ChunkError("trailing bytes")
    return {
        "source_bytes": string_size,
        "persistence_id": persistence_id,
        "instruction_count": instruction_count,
        "sha256": int(hashlib.sha256(chunk).hexdigest()[:16], 16),
    }


class P007IndependentFixtureTests(unittest.TestCase):
    def test_minimal_contract_shape_and_identity(self) -> None:
        chunk = make_chunk()
        self.assertEqual(len(chunk), 78)
        self.assertEqual(
            hashlib.sha256(chunk).hexdigest(),
            "cb38e204cf10865666e03cf7a23242629499dff189cd213e41906ba2cf4f06c7",
        )
        self.assertEqual(independent_walk(chunk)["instruction_count"], 2)

    def test_rejection_boundaries(self) -> None:
        valid = make_chunk()
        cases = {
            "signature": bytes((valid[0] ^ 1,)) + valid[1:],
            "version": valid[:4] + b"P" + valid[5:],
            "truncated": valid[:-1],
            "trailing": valid + b"\0",
            "bad_source_terminator": valid[:33] + b"X" + valid[34:],
            "negative_line_count": valid[:46] + struct.pack("<i", -1) + valid[50:],
            "negative_persistence": valid[:38] + struct.pack("<i", -1) + valid[42:],
            "zero_stack": valid[:45] + b"\0" + valid[46:],
            "unknown_constant_tag": make_chunk(constants=b"\x02", constant_count=1),
            "unknown_opcode": make_chunk(code=(0x00000023, RETURN)),
        }
        for name, candidate in cases.items():
            with self.subTest(name=name):
                with self.assertRaises(ChunkError):
                    independent_walk(candidate)

    def test_supported_constant_tags_remain_structurally_distinct(self) -> None:
        numeric = make_chunk(constants=b"\x03" + struct.pack("<d", 7.5))
        text = make_chunk(constants=b"\x04" + pglua_string(b"synthetic"))
        self.assertEqual(independent_walk(numeric)["instruction_count"], 2)
        self.assertEqual(independent_walk(text)["instruction_count"], 2)
        self.assertNotEqual(hashlib.sha256(numeric).digest(), hashlib.sha256(text).digest())


if __name__ == "__main__":
    unittest.main()
