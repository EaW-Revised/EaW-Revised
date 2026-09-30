"""Read ``$Global`` member offsets of the private t1 vertex stage via ``glslang -q``."""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys

FLAGS = ["-D", "-V", "--target-env", "vulkan1.1", "--hlsl-dx9-compatible",
         "--auto-map-bindings", "--auto-map-locations"]


def globals_offsets(glslang: Path, manifest_dir: Path) -> dict:
    manifest = json.loads((manifest_dir / "manifest.json").read_text(encoding="utf-8"))
    entry = manifest["vertex"]["entry"]
    result = subprocess.run([str(glslang), *FLAGS, "-S", "vert", "-e", entry, "-q", "-o", "reflect.spv", "vert.hlsl"],
                            cwd=manifest_dir, capture_output=True, text=True, check=True)
    offsets: dict[str, int] = {}
    for line in result.stdout.splitlines():
        member = re.match(r"(\w+): offset (\d+),", line)
        if member:
            offsets[member.group(1)] = int(member.group(2))
        block = re.match(r"\$Global: offset -1, type \w+, size (\d+),", line)
        if block:
            offsets["$size"] = int(block.group(1))
    if "$size" not in offsets:
        raise RuntimeError("glslang reflection did not report the $Global block")
    return offsets


if __name__ == "__main__":
    print(json.dumps(globals_offsets(Path(sys.argv[1]), Path(sys.argv[2]))))
