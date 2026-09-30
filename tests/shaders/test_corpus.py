from __future__ import annotations

import unittest
from pathlib import Path
import tempfile
import zipfile

from tools.shaders.corpus import (
    MATERIAL_EFFECTS, PIPELINES, REMAKE_BYTECODE_ONLY, classify_public_exclusion,
    descriptor_from_manifest, verify_public_source_tree,
)


class CorpusContractTests(unittest.TestCase):
    def test_requested_and_deferred_corpora_are_complete_and_unique(self):
        paths = [item["path"].casefold() for item in MATERIAL_EFFECTS]
        self.assertEqual(46, len(paths))
        self.assertEqual(len(paths), len(set(paths)))
        self.assertEqual(11, len(REMAKE_BYTECODE_ONLY))
        self.assertEqual(11, len(set(REMAKE_BYTECODE_ONLY)))
        self.assertIn("Terrain/TerrainWater.fx".casefold(), paths)
        self.assertTrue(all(item["pipeline"] in PIPELINES for item in MATERIAL_EFFECTS))

    def test_non_material_classification_has_explicit_reason(self):
        cases = {
            "Engine/StencilDarken.fx": "fixed_function_pipeline",
            "Dev/alMissingShader.fx": "developer_sample",
            "SceneComposite/Scene_default.fx": "post_process",
            "Engine/PrimAlpha.fx": "primitive_or_particle",
        }
        for path, expected in cases.items():
            classification, reason = classify_public_exclusion(path)
            self.assertEqual(expected, classification)
            self.assertTrue(reason)

    def test_extracted_source_tree_must_match_archive_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            (source / "Synthetic.fx").write_bytes(b"synthetic effect")
            archive_path = root / "synthetic.zip"
            with zipfile.ZipFile(archive_path, "w") as archive:
                archive.writestr("Shaders/Synthetic.fx", b"synthetic effect")
            result = verify_public_source_tree(source, archive_path)
            self.assertEqual("byte_exact_to_pinned_archive", result["status"])
            self.assertEqual(1, result["files"])
            (source / "Synthetic.fx").write_bytes(b"tampered")
            with self.assertRaises(ValueError):
                verify_public_source_tree(source, archive_path)

    def test_descriptor_retains_stage_hashes_and_unsupported_constructs(self):
        spec = {"path": "MeshFixture.fx", "family": "MESH", "pipeline": "rigid_mesh",
                "role": "material"}
        manifest = {
            "source": {"root": {"sha256": "a" * 64}, "includes": []},
            "parameters": [
                {"name": "World", "type": "float4x4", "semantic": "WORLD"},
                {"name": "binary", "type": "pixelshader", "semantic": None,
                 "default": "synthetic shader body must not survive"},
            ],
            "resources": [], "structs": {}, "techniques": [],
            "runtime_compatibility_order": ["runtime"],
            "translation_selection": {"excluded_techniques": []},
            "manifest_id": "b" * 64,
            "unsupported_constructs": [{"kind": "legacy_shader_assembly"}],
            "artifacts": [{
                "technique": "runtime", "pass": "draw", "draw": True,
                "stages": [{
                    "stage": "vert", "entry_point": "main",
                    "source_sha256": "c" * 64, "spirv_sha256": "d" * 64,
                    "exit_code": 0, "validation": {"exit_code": 0},
                }],
            }],
        }
        descriptor = descriptor_from_manifest(spec, manifest)
        self.assertEqual("partial", descriptor["translation"]["status"])
        self.assertEqual("d" * 64,
                         descriptor["translation"]["stages"][0]["spirv_sha256"])
        self.assertEqual("WORLD", descriptor["semantics"][0])
        self.assertEqual(["World"], [item["name"] for item in descriptor["parameters"]])


if __name__ == "__main__":
    unittest.main()
