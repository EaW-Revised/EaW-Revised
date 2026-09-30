"""Bounded ps_1_1 assembly parser, validator and HLSL lowering.

Only a deliberately small subset of pixel shader 1.1 is accepted: ``tex``, ``texbem``,
``mov``, ``add``, ``mul`` and ``mad`` on ``r0``/``r1``, ``t0``-``t3``, ``v0``/``v1``,
with ``.rgba``, ``.rgb`` or ``.a`` destination masks and no source modifiers,
swizzles, constants or co-issue.  Everything else fails closed with a named
``EAWR-SHD-PS11-*`` diagnostic instead of being approximated.

Semantics (public Direct3D 9 reference, verified against a recorded native D3D9 draw,
see docs/shaders.md#terrainwater-t1-and-ps11):

* ``tex tN`` samples stage N at the (u, v) of texture-coordinate set N.
* ``texbem tM, tN`` reads ``tN.r``/``tN.g`` as signed (du, dv), perturbs set M by an
  external per-stage 2x2 transform and samples stage M.  The transform is a shader
  input (``eawr_bem[M]`` = (a, b, c, d): du' = a*du + b*dv, dv' = c*du + d*dv); how
  the four D3D9 BUMPENVMAT states map onto it is decided at binding time, never here.
* Arithmetic is per component; a mask leaves the other components unchanged.
* ``v0``/``v1`` are colours in [0, 1]; ``r0`` is saturated on output.
* Intermediate values outside [-1, 1] depend on the implementation-defined
  PixelShader1xMaxValue.  Interval analysis finds every such write; the lowering
  clamps those to +/-``eawr_range.x`` and records that the value is required.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
import re
from typing import Optional, Tuple


class Ps11Error(ValueError):
    def __init__(self, code: str, message: str, line: int = 0):
        super().__init__(f"{code}: {message}" + (f" (statement {line})" if line else ""))
        self.code = code
        self.line = line


ARITHMETIC = {"mov": 1, "add": 2, "mul": 2, "mad": 3}
TEXTURE_OPS = {"tex": 0, "texbem": 1}
REGISTER_LIMITS = {"r": 2, "t": 4, "v": 2}
MASKS = {"": "rgba", "rgba": "rgba", "xyzw": "rgba", "rgb": "rgb", "xyz": "rgb", "a": "a", "w": "a"}
COMPONENTS = {"rgba": (0, 1, 2, 3), "rgb": (0, 1, 2), "a": (3,)}
UNIT = (-1.0, 1.0)

# Fixed inter-stage interface: TEXCOORDn -> location n, COLORn -> 8 + n.
COLOR_LOCATION_BASE = 8
BINDING_CBUFFER = 1


def texture_binding(stage: int) -> int:
    return 2 + 2 * stage


def sampler_binding(stage: int) -> int:
    return 3 + 2 * stage


@dataclass(frozen=True)
class Register:
    file: str
    index: int

    def __str__(self) -> str:
        return f"{self.file}{self.index}"


@dataclass
class Instruction:
    opcode: str
    dst: Register
    mask: str
    sources: tuple[Register, ...]
    line: int


@dataclass
class Program:
    version: str
    instructions: list[Instruction]
    sampled_stages: list[int] = field(default_factory=list)
    bump_stages: dict[int, int] = field(default_factory=dict)  # sampled stage -> du/dv source stage

    def describe(self) -> list[dict]:
        return [{"opcode": item.opcode, "dst": str(item.dst), "mask": item.mask,
                 "sources": [str(source) for source in item.sources]} for item in self.instructions]


def _statements(text: str) -> list[tuple[int, str]]:
    body = text.strip()
    if body[:3].casefold() == "asm":
        body = body[3:].strip()
        if not (body.startswith("{") and body.endswith("}")):
            raise Ps11Error("EAWR-SHD-PS11-SYNTAX", "asm block must be braced")
        body = body[1:-1]
    statements = []
    for raw in body.splitlines():
        line = raw.split("//", 1)[0].split(";", 1)[0].strip()
        if line:
            statements.append(line)
    return list(enumerate(statements, start=1))


def _register(token: str, line: int) -> Register:
    match = re.fullmatch(r"([rtv])(\d+)", token.strip())
    if not match:
        if re.fullmatch(r"c\d+", token.strip()):
            raise Ps11Error("EAWR-SHD-PS11-UNSUPPORTED-REGISTER", f"constant register {token} is outside the subset", line)
        raise Ps11Error("EAWR-SHD-PS11-UNSUPPORTED-OPERAND", f"operand {token!r} has a modifier or swizzle", line)
    file, index = match.group(1), int(match.group(2))
    if index >= REGISTER_LIMITS[file]:
        raise Ps11Error("EAWR-SHD-PS11-UNSUPPORTED-REGISTER", f"{token} exceeds ps_1_1 limits", line)
    return Register(file, index)


def _destination(token: str, line: int) -> tuple[Register, str]:
    name, _, mask = token.strip().partition(".")
    if mask.casefold() not in MASKS:
        raise Ps11Error("EAWR-SHD-PS11-UNSUPPORTED-MASK", f"write mask .{mask} is not legal in the subset", line)
    return _register(name, line), MASKS[mask.casefold()]


def parse(text: str) -> Program:
    statements = _statements(text)
    if not statements:
        raise Ps11Error("EAWR-SHD-PS11-SYNTAX", "empty program")
    _, version = statements[0]
    if re.sub(r"\s+", "", version).casefold() != "ps.1.1":
        raise Ps11Error("EAWR-SHD-PS11-VERSION", "only ps.1.1 is accepted", 1)
    instructions = []
    for line, statement in statements[1:]:
        if "+" in statement.split()[0] or statement.startswith("+"):
            raise Ps11Error("EAWR-SHD-PS11-UNSUPPORTED-MODIFIER", "co-issue is outside the subset", line)
        opcode, _, rest = statement.partition(" ")
        opcode = opcode.casefold()
        if "_" in opcode:
            raise Ps11Error("EAWR-SHD-PS11-UNSUPPORTED-MODIFIER", f"instruction modifier on {opcode}", line)
        operands = [item.strip() for item in rest.split(",")] if rest.strip() else []
        if opcode in TEXTURE_OPS:
            if len(operands) != 1 + TEXTURE_OPS[opcode]:
                raise Ps11Error("EAWR-SHD-PS11-SYNTAX", f"{opcode} operand count", line)
            if "." in operands[0]:
                raise Ps11Error("EAWR-SHD-PS11-UNSUPPORTED-MASK", f"{opcode} takes no write mask", line)
            dst = _register(operands[0], line)
            sources = tuple(_register(item, line) for item in operands[1:])
            if dst.file != "t" or any(source.file != "t" for source in sources):
                raise Ps11Error("EAWR-SHD-PS11-SYNTAX", f"{opcode} operates on t registers", line)
            instructions.append(Instruction(opcode, dst, "rgba", sources, line))
        elif opcode in ARITHMETIC:
            if len(operands) != 1 + ARITHMETIC[opcode]:
                raise Ps11Error("EAWR-SHD-PS11-SYNTAX", f"{opcode} operand count", line)
            dst, mask = _destination(operands[0], line)
            if dst.file != "r":
                raise Ps11Error("EAWR-SHD-PS11-UNSUPPORTED-REGISTER", "arithmetic writes r registers only", line)
            instructions.append(Instruction(opcode, dst, mask,
                                            tuple(_register(item, line) for item in operands[1:]), line))
        else:
            raise Ps11Error("EAWR-SHD-PS11-UNSUPPORTED-INSTRUCTION", f"{opcode} is outside the subset", line)
    program = Program("ps_1_1", instructions)
    validate(program)
    return program


def validate(program: Program) -> None:
    """Enforce the ps_1_1 ordering and register-read rules the subset relies on."""
    written: dict[Register, set[int]] = {Register("v", 0): {0, 1, 2, 3}, Register("v", 1): {0, 1, 2, 3}}
    consumed_by_bem: set[Register] = set()
    arithmetic_seen = False
    for item in program.instructions:
        if item.opcode in TEXTURE_OPS:
            if arithmetic_seen:
                raise Ps11Error("EAWR-SHD-PS11-ORDER", "texture instructions must precede arithmetic", item.line)
            if item.dst in written:
                raise Ps11Error("EAWR-SHD-PS11-ORDER", f"{item.dst} is sampled twice", item.line)
            if item.opcode == "texbem":
                source = item.sources[0]
                if source not in written:
                    raise Ps11Error("EAWR-SHD-PS11-UNWRITTEN-READ", f"{source} read before tex", item.line)
                if source.index >= item.dst.index:
                    raise Ps11Error("EAWR-SHD-PS11-ORDER", "texbem requires dst stage > src stage", item.line)
                consumed_by_bem.add(source)
                program.bump_stages[item.dst.index] = source.index
            program.sampled_stages.append(item.dst.index)
            written[item.dst] = {0, 1, 2, 3}
            continue
        arithmetic_seen = True
        for source in item.sources:
            if source in consumed_by_bem:
                raise Ps11Error("EAWR-SHD-PS11-BEM-SOURCE-REREAD",
                                f"{source} was read by texbem and cannot be read again", item.line)
            if written.get(source, set()) != {0, 1, 2, 3}:
                raise Ps11Error("EAWR-SHD-PS11-UNWRITTEN-READ", f"{source} is not fully written", item.line)
        written.setdefault(item.dst, set()).update(COMPONENTS[item.mask])
    if written.get(Register("r", 0), set()) != {0, 1, 2, 3}:
        raise Ps11Error("EAWR-SHD-PS11-OUTPUT", "r0 must be fully written")


Interval = Tuple[float, float]


def _mul(a: Interval, b: Interval) -> Interval:
    products = [x * y for x in a for y in b]
    return min(products), max(products)


def _add(a: Interval, b: Interval) -> Interval:
    return a[0] + b[0], a[1] + b[1]


def analyse_ranges(program: Program, stage_ranges: dict[int, Interval],
                   colour_ranges: Optional[dict[int, Interval]] = None) -> list[dict]:
    """Per-component interval analysis; returns every write that can leave [-1, 1]."""
    colours = colour_ranges or {0: (0.0, 1.0), 1: (0.0, 1.0)}
    ranges: dict[Register, list[Interval]] = {Register("v", n): [colours[n]] * 4 for n in (0, 1)}
    escapes = []
    for item in program.instructions:
        if item.opcode in TEXTURE_OPS:
            stage = item.dst.index
            if stage not in stage_ranges:
                raise Ps11Error("EAWR-SHD-PS11-STAGE-RANGE-UNRESOLVED", f"no value range declared for stage {stage}", item.line)
            ranges[item.dst] = [stage_ranges[stage]] * 4
            continue
        args = [ranges[source] for source in item.sources]
        result = []
        for c in range(4):
            if item.opcode == "mov":
                value = args[0][c]
            elif item.opcode == "add":
                value = _add(args[0][c], args[1][c])
            elif item.opcode == "mul":
                value = _mul(args[0][c], args[1][c])
            else:
                value = _add(_mul(args[0][c], args[1][c]), args[2][c])
            result.append(value)
        target = ranges.setdefault(item.dst, [(0.0, 0.0)] * 4)
        leaves = []
        for c in COMPONENTS[item.mask]:
            target[c] = result[c]
            if result[c][0] < UNIT[0] or result[c][1] > UNIT[1]:
                leaves.append(c)
        if leaves:
            escapes.append({"line": item.line, "opcode": item.opcode, "dst": str(item.dst),
                            "components": leaves, "interval": [list(result[c]) for c in leaves]})
    return escapes


def _operand(register: Register) -> str:
    return str(register)


def _expression(item: Instruction) -> str:
    a = [_operand(source) for source in item.sources]
    if item.opcode == "mov":
        return a[0]
    if item.opcode == "add":
        return f"({a[0]} + {a[1]})"
    if item.opcode == "mul":
        return f"({a[0]} * {a[1]})"
    return f"({a[0]} * {a[1]} + {a[2]})"


def lower_to_hlsl(program: Program, entry: str, escapes: list[dict], header: str = "") -> str:
    """Emit an HLSL pixel entry with explicit bindings and inter-stage locations."""
    escape_lines = {item["line"] for item in escapes}
    lines = [
        "// Generated from a bounded ps_1_1 program by tools/shaders/ps11.py.",
        "// Private output: keep below ignored out/.",
    ]
    if header:
        lines.append(header)
    lines += [
        f"[[vk::binding({BINDING_CBUFFER}, 0)]] cbuffer EAWR_PS11_State",
        "{",
        "    float4 eawr_bem[4];   // per stage: du' = x*du + y*dv, dv' = z*du + w*dv",
        "    float4 eawr_range;    // x = PixelShader1xMaxValue",
        "};",
    ]
    for stage in program.sampled_stages:
        lines.append(f"[[vk::binding({texture_binding(stage)}, 0)]] Texture2D eawr_tex{stage};")
        lines.append(f"[[vk::binding({sampler_binding(stage)}, 0)]] SamplerState eawr_smp{stage};")
    lines.append("struct EAWR_PS11_Input")
    lines.append("{")
    for stage in sorted(program.sampled_stages):
        lines.append(f"    [[vk::location({stage})]] float2 tc{stage} : TEXCOORD{stage};")
    for colour in (0, 1):
        lines.append(f"    [[vk::location({COLOR_LOCATION_BASE + colour})]] float4 v{colour} : COLOR{colour};")
    lines += ["};", f"float4 {entry}(EAWR_PS11_Input i) : SV_Target0", "{",
              "    float4 v0 = saturate(i.v0);",
              "    float4 v1 = saturate(i.v1);",
              "    float4 r0 = float4(0.0, 0.0, 0.0, 0.0);",
              "    float4 r1 = float4(0.0, 0.0, 0.0, 0.0);"]
    for item in program.instructions:
        stage = item.dst.index
        if item.opcode == "tex":
            lines.append(f"    float4 t{stage} = eawr_tex{stage}.Sample(eawr_smp{stage}, i.tc{stage});")
        elif item.opcode == "texbem":
            src = f"t{item.sources[0].index}"
            lines.append(f"    float2 bem{stage} = float2(dot(eawr_bem[{stage}].xy, {src}.rg), "
                         f"dot(eawr_bem[{stage}].zw, {src}.rg));")
            lines.append(f"    float4 t{stage} = eawr_tex{stage}.Sample(eawr_smp{stage}, i.tc{stage} + bem{stage});")
        else:
            expression = _expression(item)
            if item.line in escape_lines:
                expression = f"clamp({expression}, -eawr_range.xxxx, eawr_range.xxxx)"
            swizzle = {"rgba": "", "rgb": ".rgb", "a": ".a"}[item.mask]
            lines.append(f"    {item.dst}{swizzle} = ({expression}){swizzle};")
    lines += ["    return saturate(r0);", "}", ""]
    return "\n".join(lines)


def program_record(program: Program, escapes: list[dict]) -> dict:
    return {"version": program.version, "instructions": program.describe(),
            "sampled_stages": program.sampled_stages,
            "bump_stages": {str(k): v for k, v in program.bump_stages.items()},
            "range_escapes": escapes}


__all__ = ["Instruction", "Program", "Ps11Error", "Register", "analyse_ranges", "asdict",
           "lower_to_hlsl", "parse", "program_record", "sampler_binding", "texture_binding"]
