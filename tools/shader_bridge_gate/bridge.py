"""Bounded experiment: parse SPIRV-Cross GLSL, then emit Godot spatial source.

This is deliberately not a general GLSL compiler. Unknown declarations,
statements, resources and stage links fail closed. All binding policy is an
explicit local manifest; effect metadata is not inferred from register names.
Generated player source belongs in a private output directory, never in git.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re


class Unsupported(ValueError):
    pass


TOKEN = re.compile(r"\s+|//[^\n]*|/\*.*?\*/|[A-Za-z_]\w*|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?u?|==|!=|<=|>=|&&|\|\||\+=|-=|\*=|/=|[{}()\[\].,;?:+*/%<>=!\-]", re.S)
TYPES = {"float", "int", "uint", "bool", "vec2", "vec3", "vec4", "bvec2", "bvec3", "bvec4", "mat4", "sampler2D"}
CALLS = TYPES | {"dot", "inversesqrt", "sqrt", "abs", "min", "max", "clamp", "isnan", "texture", "pow", "exp2", "log2", "floor", "fract", "mix", "normalize", "length"}
PRECEDENCE = {"||": 1, "&&": 2, "==": 3, "!=": 3, "<": 4, ">": 4, "<=": 4, ">=": 4, "+": 5, "-": 5, "*": 6, "/": 6, "%": 6}


@dataclass
class Expr:
    kind: str
    value: str
    args: tuple = ()


class Parser:
    def __init__(self, source):
        # Preprocessor directives are parsed as source metadata, never emitted.
        source = "\n".join(line for line in source.splitlines() if not line.lstrip().startswith("#"))
        self.tokens = []
        offset = 0
        while offset < len(source):
            match = TOKEN.match(source, offset)
            if not match:
                raise Unsupported(f"unrecognized token at {offset}")
            word = match.group()
            if not word.isspace() and not word.startswith(("//", "/*")):
                self.tokens.append(word)
            offset = match.end()
        self.tokens.append("<end>")
        self.pos = 0

    def peek(self):
        return self.tokens[self.pos]

    def take(self, expected=None):
        word = self.peek()
        if expected is not None and word != expected:
            raise Unsupported(f"expected {expected}, found {word}")
        self.pos += 1
        return word

    def expression(self, minimum=0):
        word = self.take()
        if word in ("-", "+", "!"):
            left = Expr("unary", word, (self.expression(7),))
        elif word == "(":
            left = self.expression()
            self.take(")")
        elif re.fullmatch(r"[A-Za-z_]\w*|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?u?", word):
            left = Expr("atom", word)
        else:
            raise Unsupported(f"expression token {word}")
        while True:
            op = self.peek()
            if op == ".":
                self.take()
                left = Expr("member", self.take(), (left,))
            elif op == "[":
                self.take()
                index = self.expression()
                self.take("]")
                left = Expr("index", "", (left, index))
            elif op == "(":
                if left.kind != "atom" or left.value not in CALLS:
                    raise Unsupported("unsupported function call")
                self.take()
                args = []
                if self.peek() != ")":
                    while True:
                        args.append(self.expression())
                        if self.peek() != ",":
                            break
                        self.take()
                self.take(")")
                left = Expr("call", left.value, tuple(args))
            elif op in PRECEDENCE and PRECEDENCE[op] >= minimum:
                self.take()
                left = Expr("binary", op, (left, self.expression(PRECEDENCE[op] + 1)))
            else:
                break
        if minimum == 0 and self.peek() == "?":
            self.take()
            yes = self.expression()
            self.take(":")
            left = Expr("select", "", (left, yes, self.expression()))
        return left

    def variable(self):
        kind, name = self.take(), self.take()
        if kind not in TYPES or not re.fullmatch(r"[A-Za-z_]\w*", name):
            raise Unsupported("unsupported declaration")
        size = None
        if self.peek() == "[":
            self.take()
            size = int(self.take())
            if not 1 <= size <= 4096:
                raise Unsupported("experiment uniform array budget")
            self.take("]")
        return kind, name, size

    def parse(self):
        declarations, statements = [], []
        while self.peek() != "<end>":
            if self.peek() == "layout":
                self.take()
                self.take("(")
                while self.peek() != ")":
                    if self.peek() == "<end>":
                        raise Unsupported("unterminated layout")
                    self.take()
                self.take(")")
            if self.peek() == "void":
                self.take(); self.take("main"); self.take("("); self.take(")"); self.take("{")
                while self.peek() != "}":
                    if self.peek() in TYPES:
                        declaration = self.variable()
                        self.take("=")
                        expression = self.expression()
                        statements.append(("declare", declaration, expression))
                    else:
                        left = self.expression()
                        op = self.take()
                        if op not in ("=", "+=", "-=", "*=", "/="):
                            raise Unsupported(f"statement operator {op}")
                        statements.append((op, left, self.expression()))
                    self.take(";")
                self.take("}")
                if self.peek() != "<end>":
                    raise Unsupported("multiple entry points")
                return declarations, statements
            qualifier = self.take() if self.peek() in ("uniform", "in", "out") else "local"
            # A uniform block has a type identifier followed by an opening brace.
            if qualifier == "uniform" and self.tokens[self.pos + 1] == "{":
                block = self.take(); self.take("{")
                members = []
                while self.peek() != "}":
                    members.append(self.variable()); self.take(";")
                self.take("}")
                instance = self.take(); self.take(";")
                declarations.append(("block", block, instance, members))
            else:
                variable = self.variable(); self.take(";")
                declarations.append((qualifier, *variable))
        raise Unsupported("missing main")


def zero(kind):
    if kind == "bool":
        return "false"
    if kind == "float":
        return "0.0"
    return f"{kind}(" + ("false" if kind.startswith("bvec") else "0.0" if kind.startswith(("vec", "mat")) else "0") + ")"


def emit_expression(node, names, blocks):
    emit = lambda expr: emit_expression(expr, names, blocks)
    if node.kind == "atom":
        return names.get(node.value, node.value)
    if node.kind == "member":
        if node.args[0].kind == "atom" and (node.args[0].value, node.value) in blocks:
            raise Unsupported("whole constant-buffer array expression")
        return f"({emit(node.args[0])}).{node.value}"
    if node.kind == "index":
        base = node.args[0]
        if base.kind == "member" and base.args[0].kind == "atom":
            key = (base.args[0].value, base.value)
            if key in blocks:
                index = node.args[1]
                if index.kind != "atom" or not re.fullmatch(r"\d+u?", index.value):
                    raise Unsupported("dynamic constant-buffer indexing needs a later binding policy")
                count = blocks[key][1]
                at = int(index.value.removesuffix("u"))
                if not 0 <= at < count:
                    raise Unsupported("constant-buffer index out of bounds")
                return blocks[key][0] + "_" + str(at)
        return f"{emit(node.args[0])}[{emit(node.args[1])}]"
    if node.kind == "unary":
        return f"({node.value}{emit(node.args[0])})"
    if node.kind == "binary":
        return f"({emit(node.args[0])} {node.value} {emit(node.args[1])})"
    if node.kind == "select":
        return f"({emit(node.args[0])} ? {emit(node.args[1])} : {emit(node.args[2])})"
    if node.kind == "call":
        return f"{node.value}({', '.join(map(emit, node.args))})"
    raise Unsupported(node.kind)


def bridge(vertex, pixel, vertex_reflection, pixel_reflection, policy):
    parsed = [Parser(vertex).parse(), Parser(pixel).parse()]
    reflections = [vertex_reflection, pixel_reflection]
    vs_outputs = {row["location"]: row for row in vertex_reflection.get("outputs", [])}
    links = []
    for row in pixel_reflection.get("inputs", []):
        output = vs_outputs.get(row["location"])
        if not output:
            raise Unsupported(f"missing varying location {row['location']}")
        links.append((output, row))
    source = ["shader_type spatial;", "render_mode unshaded, cull_disabled, skip_vertex_transform;"]
    for output, _ in links:
        source.append(f"varying {output['type']} gate_link_{output['location']};")
    bodies = []
    for stage, (declarations, statements), reflection in zip(("vs", "ps"), parsed, reflections):
        names = {"gl_Position": "gate_position", "gl_PointSize": "gate_point_size"}
        blocks = {}
        body = ["vec4 gate_position = vec4(0.0);", "float gate_point_size = 1.0;"] if stage == "vs" else []
        for declaration in declarations:
            qualifier, kind, name, extra = declaration
            if qualifier == "block":
                for member_type, member, size in extra:
                    key = f"{stage}_{name}_{member.lstrip('_')}"
                    if not size:
                        raise Unsupported("experiment requires array constant-buffer members")
                    blocks[name, member] = (key, size)
                    # MaterialBinding has scalar/vector values but no array value.
                    # Static-register lowering preserves this production boundary.
                    for index in range(size):
                        source.append(f"uniform {member_type} {key}_{index};")
            elif qualifier == "uniform":
                key = policy[stage].get("samplers", {}).get(name)
                if kind != "sampler2D" or not key:
                    raise Unsupported(f"unbound resource {stage}:{name}")
                names[name] = key
                source.append(f"uniform sampler2D {key} : filter_nearest, repeat_disable;")
            else:
                if extra:
                    raise Unsupported("local arrays")
                body.append(f"{kind} {name} = {zero(kind)};")
        if stage == "vs":
            for row in reflection.get("inputs", []):
                value = policy[stage].get("inputs", {}).get(row["name"])
                if not value:
                    raise Unsupported(f"unbound vertex input {row['name']}")
                body.append(f"{row['name']} = {value};")
        else:
            for output, row in links:
                link = f"gate_link_{output['location']}"
                if row["type"] != output["type"]:
                    if row["type"] == "vec4" and output["type"] == "vec3":
                        link = f"vec4({link}, 1.0)"
                    elif row["type"] == "vec4" and output["type"] == "vec2":
                        link = f"vec4({link}, 0.0, 1.0)"
                    else:
                        raise Unsupported("varying width conversion")
                body.append(f"{row['name']} = {link};")
        for op, left, right in statements:
            if op == "declare":
                kind, name, size = left
                if size:
                    raise Unsupported("entry arrays")
                body.append(f"{kind} {name} = {emit_expression(right, names, blocks)};")
            else:
                body.append(f"{emit_expression(left, names, blocks)} {op} {emit_expression(right, names, blocks)};")
        if stage == "vs":
            body.append("POSITION = gate_position;")
            for output, _ in links:
                body.append(f"gate_link_{output['location']} = {output['name']};")
        else:
            outputs = reflection.get("outputs", [])
            if len(outputs) != 1 or outputs[0]["location"] != 0 or outputs[0]["type"] != "vec4":
                raise Unsupported("fragment output shape")
            # Production renderer applies stored_output::backend_source at upload.
            body.append(f"ALBEDO = {outputs[0]['name']}.rgb;")
        bodies.append("void " + ("vertex" if stage == "vs" else "fragment") + "() {\n    " + "\n    ".join(body) + "\n}")
    return "\n".join(source + bodies) + "\n"


def main():
    args = argparse.ArgumentParser(description=__doc__)
    for name in ("vs", "ps", "vs-reflection", "ps-reflection", "policy", "out"):
        args.add_argument("--" + name, type=Path, required=True)
    ns = args.parse_args()
    source = bridge(ns.vs.read_text(), ns.ps.read_text(), json.loads(ns.vs_reflection.read_text()),
                    json.loads(ns.ps_reflection.read_text()), json.loads(ns.policy.read_text()))
    ns.out.write_text(source, encoding="utf-8")


if __name__ == "__main__":
    main()
