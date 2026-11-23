"""Bounded Direct3D 9 shader-bytecode token reader (vs_1_1 and ps_1_x).

The public token format: a version token (``0xFFFE0101`` vs_1_1, ``0xFFFF01xx`` ps_1_x),
instructions made of an opcode token plus destination and source parameter tokens, comment
blocks (opcode ``0xFFFE``) and an ``0x0000FFFF`` end token.  Shader models below 2.0 do not
encode an instruction length, so the operand count comes from the opcode table below.

Fail closed: an opcode outside the table, a truncated stream, a register type outside the
shader model's set, relative addressing in a pixel shader or a missing end token raises
``BytecodeError`` naming the offset; nothing is guessed.

Only a disassembly (text of registers and operations) leaves this module; shaders are read
from the player's own installed effect files and nothing of them is stored in the repository.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import re
import struct
from typing import Iterator, List, Tuple

END = 0x0000FFFF
COMMENT = 0xFFFE
PHASE = 0xFFFD

# opcode -> (name, destination count, source count, extra dwords after the operands)
OPCODES = {
    0: ("nop", 0, 0, 0), 1: ("mov", 1, 1, 0), 2: ("add", 1, 2, 0), 3: ("sub", 1, 2, 0),
    4: ("mad", 1, 3, 0), 5: ("mul", 1, 2, 0), 6: ("rcp", 1, 1, 0), 7: ("rsq", 1, 1, 0),
    8: ("dp3", 1, 2, 0), 9: ("dp4", 1, 2, 0), 10: ("min", 1, 2, 0), 11: ("max", 1, 2, 0),
    12: ("slt", 1, 2, 0), 13: ("sge", 1, 2, 0), 14: ("exp", 1, 1, 0), 15: ("log", 1, 1, 0),
    16: ("lit", 1, 1, 0), 17: ("dst", 1, 2, 0), 18: ("lrp", 1, 3, 0), 19: ("frc", 1, 1, 0),
    20: ("m4x4", 1, 2, 0), 21: ("m4x3", 1, 2, 0), 22: ("m3x4", 1, 2, 0), 23: ("m3x3", 1, 2, 0),
    24: ("m3x2", 1, 2, 0), 64: ("texcoord", 1, 0, 0), 65: ("texkill", 1, 0, 0), 66: ("tex", 1, 0, 0), 67: ("texbem", 1, 1, 0),
    68: ("texbeml", 1, 1, 0), 69: ("texreg2ar", 1, 1, 0), 70: ("texreg2gb", 1, 1, 0),
    71: ("texm3x2pad", 1, 1, 0), 72: ("texm3x2tex", 1, 1, 0), 73: ("texm3x3pad", 1, 1, 0),
    74: ("texm3x3tex", 1, 1, 0), 76: ("texm3x3spec", 1, 2, 0), 77: ("texm3x3vspec", 1, 1, 0),
    78: ("expp", 1, 1, 0), 79: ("logp", 1, 1, 0), 80: ("cnd", 1, 3, 0), 81: ("def", 1, 0, 4),
    82: ("texreg2rgb", 1, 1, 0), 83: ("texdp3tex", 1, 1, 0), 84: ("texm3x2depth", 1, 1, 0),
    85: ("texdp3", 1, 1, 0), 86: ("texm3x3", 1, 1, 0), 87: ("texdepth", 1, 0, 0),
    88: ("cmp", 1, 3, 0), 89: ("bem", 1, 2, 0),
}
# vs_1_1 input declarations: usage token then register token (vertex shaders only).
DCL = 31
USAGES = {0: "position", 1: "blendweight", 2: "blendindices", 3: "normal", 4: "psize", 5: "texcoord",
          6: "tangent", 7: "binormal", 8: "tessfactor", 9: "positiont", 10: "color", 11: "fog",
          12: "depth", 13: "sample"}

VERTEX_REGISTERS = {0: "r", 1: "v", 2: "c", 3: "a", 4: "oPos", 5: "oD", 6: "oT"}
PIXEL_REGISTERS = {0: "r", 1: "v", 2: "c", 3: "t"}
SOURCE_MODIFIERS = {0: "{}", 1: "-{}", 2: "{}_bias", 3: "-{}_bias", 4: "{}_bx2", 5: "-{}_bx2",
                    6: "1-{}", 7: "-{}", 8: "{}_x2", 9: "-{}_x2", 10: "{}_dz", 11: "{}_dw"}
RESULT_MODIFIERS = {0: "", 1: "_sat", 2: "_pp", 4: "_centroid"}
SHIFTS = {0: "", 1: "_x2", 2: "_x4", 3: "_x8", 13: "_d8", 14: "_d4", 15: "_d2"}


class BytecodeError(ValueError):
    """The stream is not a shader this reader accepts."""


@dataclass
class Instruction:
    offset: int
    text: str
    opcode: str


@dataclass
class Shader:
    offset: int
    kind: str  # "vs" or "ps"
    version: Tuple[int, int]
    instructions: List[Instruction] = field(default_factory=list)
    constants: List[Tuple[str, Tuple[float, float, float, float]]] = field(default_factory=list)
    comments: List[bytes] = field(default_factory=list)

    @property
    def name(self) -> str:
        return "%s_%d_%d" % (self.kind, self.version[0], self.version[1])

    def disassemble(self) -> str:
        lines = [self.name]
        lines.extend(ins.text for ins in self.instructions)
        return "\n".join(lines)

    def opcodes(self) -> List[str]:
        return [ins.opcode for ins in self.instructions]


def _register(token: int, kind: str, offset: int) -> Tuple[str, int]:
    reg_type = ((token >> 28) & 0x7) | (((token >> 11) & 0x3) << 3)
    number = token & 0x7FF
    table = VERTEX_REGISTERS if kind == "vs" else PIXEL_REGISTERS
    if reg_type not in table:
        raise BytecodeError("register type %d at dword %d is outside the %s_1 register set" % (reg_type, offset, kind))
    if (token >> 13) & 1 and kind == "ps":
        raise BytecodeError("relative addressing in a pixel shader at dword %d" % offset)
    if kind == "vs" and reg_type == 4:
        return ("oPos", 0) if number == 0 else _fail_rastout(number, offset)
    return table[reg_type], number


def _fail_rastout(number: int, offset: int):
    if number == 1:
        return "oFog", 0
    if number == 2:
        return "oPts", 0
    raise BytecodeError("rasterizer output %d at dword %d is not defined" % (number, offset))


def _mask(token: int) -> str:
    bits = (token >> 16) & 0xF
    if bits == 0xF:
        return ""
    return "." + "".join(c for i, c in enumerate("xyzw") if bits & (1 << i))


def _swizzle(token: int) -> str:
    sw = (token >> 16) & 0xFF
    comps = "".join("xyzw"[(sw >> (2 * i)) & 3] for i in range(4))
    if comps == "xyzw":
        return ""
    if comps[0] == comps[1] == comps[2] == comps[3]:
        return "." + comps[0]
    return "." + comps


def _format_dest(token: int, kind: str, offset: int) -> str:
    base, number = _register(token, kind, offset)
    name = base if base in ("oPos", "oFog", "oPts") else "%s%d" % (base, number)
    return name + _mask(token)


def _format_source(token: int, kind: str, offset: int) -> str:
    base, number = _register(token, kind, offset)
    modifier = SOURCE_MODIFIERS.get((token >> 24) & 0xF)
    if modifier is None:
        raise BytecodeError("source modifier %d at dword %d is not defined" % ((token >> 24) & 0xF, offset))
    name = "%s%d" % (base, number) if base not in ("oPos", "oFog", "oPts") else base
    return modifier.format(name + _swizzle(token))


def read_shader(words: List[int], start: int) -> Tuple[Shader, int]:
    """Read one shader whose version token is at ``start``; return it and the next index."""
    version = words[start]
    major, minor = (version >> 8) & 0xFF, version & 0xFF
    if version >> 16 == 0xFFFE and major == 1:
        kind = "vs"
    elif version >> 16 == 0xFFFF and major == 1:
        kind = "ps"
    else:
        raise BytecodeError("version token 0x%08x at dword %d is not vs_1_x or ps_1_x" % (version, start))
    if kind == "ps" and minor >= 4:
        raise BytecodeError("ps_1_%d at dword %d has its own operand forms and is not read" % (minor, start))
    shader = Shader(offset=start, kind=kind, version=(major, minor))
    index = start + 1
    while True:
        if index >= len(words):
            raise BytecodeError("shader at dword %d has no end token" % start)
        token = words[index]
        if token == END:
            return shader, index + 1
        op = token & 0xFFFF
        if op == COMMENT:
            length = (token >> 16) & 0x7FFF
            if index + 1 + length > len(words):
                raise BytecodeError("comment at dword %d runs past the stream" % index)
            raw = b"".join(struct.pack("<I", w) for w in words[index + 1:index + 1 + length])
            shader.comments.append(raw)
            index += 1 + length
            continue
        if op == PHASE:
            # The phase marker belongs to ps_1_4, which is refused above: in a shader this reader accepts it is malformed.
            raise BytecodeError("phase token at dword %d is not valid in %s" % (index, shader.name))
        if op == DCL and kind == "vs":
            if index + 3 > len(words):
                raise BytecodeError("dcl at dword %d is truncated" % index)
            usage_token, register_token = words[index + 1], words[index + 2]
            usage = USAGES.get(usage_token & 0x1F)
            if usage is None or not (usage_token >> 31) & 1 or not (register_token >> 31) & 1:
                raise BytecodeError("dcl at dword %d has an undefined usage or register token" % index)
            usage_index = (usage_token >> 16) & 0xF
            text = "dcl_%s%s %s" % (usage, str(usage_index) if usage_index else "",
                                    _format_dest(register_token, kind, index + 2))
            shader.instructions.append(Instruction(index, text, "dcl"))
            index += 3
            continue
        entry = OPCODES.get(op)
        if entry is None:
            raise BytecodeError("opcode %d at dword %d is not in the vs_1/ps_1 table" % (op, index))
        name, dsts, srcs, extra = entry
        need = 1 + dsts + srcs + extra
        if index + need > len(words):
            raise BytecodeError("instruction %s at dword %d is truncated" % (name, index))
        cursor = index + 1
        parts: List[str] = []
        suffix = ""
        coissue = bool(token & 0x40000000)
        for _ in range(dsts):
            dtoken = words[cursor]
            if not (dtoken >> 31) & 1:
                raise BytecodeError("destination token at dword %d lacks the high bit" % cursor)
            rmod = RESULT_MODIFIERS.get((dtoken >> 20) & 0xF)
            shift = SHIFTS.get((dtoken >> 24) & 0xF)
            if rmod is None or shift is None:
                raise BytecodeError("result modifier or shift at dword %d is not defined" % cursor)
            suffix = shift + rmod
            parts.append(_format_dest(dtoken, kind, cursor))
            cursor += 1
        for _ in range(srcs):
            stoken = words[cursor]
            if not (stoken >> 31) & 1:
                raise BytecodeError("source token at dword %d lacks the high bit" % cursor)
            parts.append(_format_source(stoken, kind, cursor))
            cursor += 1
        if name == "def":
            values = tuple(struct.unpack("<f", struct.pack("<I", words[cursor + i]))[0] for i in range(4))
            number = _register(words[index + 1], kind, index + 1)[1]
            shader.constants.append(("c%d" % number, values))
            parts.extend("%g" % v for v in values)
            cursor += 4
        text = ("+ " if coissue else "") + name + suffix + " " + ", ".join(parts)
        shader.instructions.append(Instruction(index, text.strip(), name))
        index = cursor


_VERSION_RE = re.compile(rb"(?:\x01\x01\xFE\xFF|\x00\x01\xFE\xFF|[\x00-\x04]\x01\xFF\xFF)")


def find_shaders(blob: bytes) -> Iterator[Shader]:
    """Every vs_1_x and ps_1_x program embedded in ``blob`` (a compiled effect, or a bare shader).

    The scan anchors on version tokens and keeps only candidates that parse to an end token
    without error, so a coincidental byte pattern is dropped rather than guessed at; the
    result of every accepted program is complete.
    """
    padded = blob + b"\x00" * (-len(blob) % 4)
    words = list(struct.unpack("<%dI" % (len(padded) // 4), padded))
    index = 0
    while index < len(words):
        version = words[index]
        if version >> 16 in (0xFFFE, 0xFFFF) and ((version >> 8) & 0xFF) == 1 and (version & 0xFF) <= 4:
            try:
                shader, nxt = read_shader(words, index)
            except BytecodeError:
                index += 1
                continue
            if shader.instructions:
                yield shader
                index = nxt
                continue
        index += 1


def constant_table(shader: Shader) -> List[Tuple[str, int, int]]:
    """The CTAB comment's constants as (name, first register, register count); empty when there is none."""
    for raw in shader.comments:
        if raw[:4] != b"CTAB" or len(raw) < 32:
            continue
        table = raw[4:]
        size, _creator, _version, count, info, _flags, _target = struct.unpack_from("<7I", table, 0)
        if size != 28 or info + 20 * count > len(table):
            raise BytecodeError("constant table header is inconsistent")
        result = []
        for index in range(count):
            name_at, _regset, reg, number, _reserved, _type, _default = struct.unpack_from(
                "<IHHHHII", table, info + 20 * index)[:7]
            end = table.find(bytes(1), name_at)
            if end < 0:
                raise BytecodeError("constant name runs past the table")
            result.append((table[name_at:end].decode("ascii", "replace"), reg, number))
        return result
    return []


def disassemble_file(path: str) -> str:
    with open(path, "rb") as handle:
        blob = handle.read()
    return "\n\n".join(shader.disassemble() for shader in find_shaders(blob))


if __name__ == "__main__":
    import sys

    for argument in sys.argv[1:]:
        print("; " + argument)
        print(disassemble_file(argument))
