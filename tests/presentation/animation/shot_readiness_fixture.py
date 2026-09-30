"""Writes a synthetic read-only FoC-profile installation for the
shot-readiness probe's success-path CLI test: one boxed single-bone ALO in the
base layer and, in both layers, an archive manifest whose one declared
archive is absent (the accepted missing-archive contract). Prints its SHA-256.

    python shot_readiness_fixture.py <game-root>
"""
import hashlib
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "assets" / "fixtures"))
import scene_fixture  # noqa: E402  (shared, reviewed ALO byte builder)


def main() -> int:
    game = pathlib.Path(sys.argv[1])
    base = game / "GameData" / "Data"
    expansion = game / "corruption" / "Data"
    (base / "Art" / "Models").mkdir(parents=True, exist_ok=True)
    expansion.mkdir(parents=True, exist_ok=True)
    for data in (base, expansion):
        (data / "MegaFiles.xml").write_text("<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
    model = scene_fixture.alo_bytes([("MeshAlpha.fx", None, 1.0, 2.0, 3.0)])
    (base / "Art" / "Models" / "EAWR_PROBE_BOX.ALO").write_bytes(model)
    print(hashlib.sha256(model).hexdigest())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
