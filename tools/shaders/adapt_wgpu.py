#!/usr/bin/env python3
"""Mechanical private-output HLSL adaptation for WebGPU SPIR-V ingestion.

The source and adapted shader bodies remain under an ignored output directory.  This
tool only repairs the D3D9 POSITION output semantic and expands legacy sampler2D into
the separate texture/sampler bindings required by WebGPU; it never rewrites equations.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess


def write_text_lf(path: pathlib.Path, text: str) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


def adapt_vertex(text: str) -> str:
    match = re.search(r"(struct\s+VS_OUTPUT\s*\{)(.*?)(\};)", text, re.DOTALL)
    if not match:
        raise ValueError("VS_OUTPUT structure not found")
    body, count = re.subn(r"(\bfloat4\s+Pos\s*:)\s*POSITION\s*;", r"\1 SV_POSITION;", match.group(2))
    if count != 1:
        raise ValueError(f"expected one VS_OUTPUT POSITION, found {count}")
    return text[: match.start(2)] + body + text[match.end(2) :]


def adapt_fragment(text: str) -> str:
    # The shared VS_OUTPUT declaration appears in both generated stages. Mapping
    # POSITION in both stages keeps the remaining auto-mapped varyings aligned.
    text = adapt_vertex(text)
    declaration = re.compile(r"\bsampler2D\s+BaseSampler\s*;", re.DOTALL)
    replacement = (
        "[[vk::binding(1, 0)]] Texture2D BaseTexture;\n"
        "[[vk::binding(2, 0)]] SamplerState BaseSamplerState;"
    )
    text, declarations = declaration.subn(replacement, text)
    text, samples = re.subn(
        r"\btex2D\s*\(\s*BaseSampler\s*,\s*([^\)]+)\)",
        r"BaseTexture.Sample(BaseSamplerState, \1)",
        text,
    )
    if declarations != 1 or samples < 1:
        raise ValueError(f"expected one BaseSampler and at least one tex2D use, got {declarations}/{samples}")
    return text


def run(command: list[str]) -> None:
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError("command failed: " + " ".join(command) + "\n" + result.stdout)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vertex", type=pathlib.Path, required=True)
    parser.add_argument("--fragment", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--glslang", type=pathlib.Path, required=True)
    parser.add_argument("--spirv-val", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    vert = args.out / "vert.hlsl"
    frag = args.out / "frag.hlsl"
    write_text_lf(vert, adapt_vertex(args.vertex.read_text(encoding="utf-8")))
    write_text_lf(frag, adapt_fragment(args.fragment.read_text(encoding="utf-8")))
    for stage, entry, source in (("vert", "sph_vs_main", vert), ("frag", "gloss_ps_main", frag)):
        output = args.out / f"{stage}.spv"
        run([str(args.glslang), "-D", "-V", "--target-env", "vulkan1.1", "--hlsl-dx9-compatible", "--auto-map-bindings", "--auto-map-locations", "-S", stage, "-e", entry, "-o", str(output), str(source)])
        run([str(args.spirv_val), "--target-env", "vulkan1.1", str(output)])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
