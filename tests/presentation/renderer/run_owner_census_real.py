"""Real VFS/cache/ModelUpload identities and ownership lifetimes, without Godot."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from tests.assets.fixtures.scene_fixture import alo_bytes


def main():
    with tempfile.TemporaryDirectory(prefix="owner-census-real-") as directory:
        model = Path(directory) / "art/models/owner.alo"
        model.parent.mkdir(parents=True)
        model.write_bytes(alo_bytes([("MeshGloss.fx", "", 1, 1, 1)]))
        result = subprocess.run([sys.argv[1], directory], check=True,
                                capture_output=True, text=True, timeout=30)
        evidence = json.loads(result.stdout)
    geometry = None
    for stage, mask, derived in (("lru", 2, "cache_only"), ("shared", 3, None),
                                  ("renderer", 1, "renderer_only"),
                                  ("raw", 5, "renderer_emitter")):
        census = evidence[stage]
        assert census["models_complete"] and len(census["models"]) == 1, census
        row = census["models"][0]
        assert row["owner_mask"] == mask, row
        current = row["vertex_capacity"] + row["index_capacity"]
        assert current > 0 and (geometry is None or current == geometry), row
        geometry = current
        assert row["listed_payload_capacity"] >= current
        for name, value in census["derived_geometry_bytes"].items():
            assert value == (current if name == derived else 0), (stage, name, value)
        assert census["cache_only_listed_payload_bytes"] == (
            row["listed_payload_capacity"] if stage == "lru" else 0)
        assert bool(row["raw_wrapper_mask"]) == (stage == "raw")
    assert evidence["raw"]["raw_wrappers"][0]["role"] == "unit_emitters"
    assert evidence["released"]["models"] == [] and evidence["lifetime_expired"]
    print("real VFS/cache/ModelUpload masks, capacities, deduplication and lifetimes PASS")


if __name__ == "__main__":
    main()
