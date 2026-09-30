"""Cross-check compiled .FXO names (from analysis/shader-archive-listing.txt)
against the .fx sources in shaders/petroglyph-foc. Run from the project root."""
import os
import re

SRC_DIR = "shaders/petroglyph-foc"
LISTING = "analysis/shader-archive-listing.txt"

src = {
    f[:-3].upper()
    for _r, _d, fs in os.walk(SRC_DIR)
    for f in fs
    if f.lower().endswith(".fx")
}
txt = open(LISTING, encoding="utf-8").read()


def leaf(line: str) -> str:
    return line.strip().replace("\\", "/").split("/")[-1][:-4]


all_fxo = set()
for sec in re.split(r"^== ", txt, flags=re.M)[1:]:
    name = "/".join(sec.split(":")[0].split("/")[-3:])
    fxo = {leaf(l) for l in sec.splitlines() if l.strip().endswith(".FXO")}
    all_fxo |= fxo
    missing = sorted(fxo - src)
    print(f"{name}: {len(fxo)} fxo, {len(missing)} without source: {missing}")

print(f"sources total: {len(src)}; sources with no fxo in any archive: {len(src - all_fxo)}")
