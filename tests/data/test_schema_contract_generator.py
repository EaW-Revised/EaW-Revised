#!/usr/bin/env python3
"""Focused integrity and reproducibility checks for the packed XML contract."""
from __future__ import annotations

import argparse
import importlib.util
import json
import sys
import tempfile
from pathlib import Path


def load_generator(path: Path):
    spec = importlib.util.spec_from_file_location("eawr_xml_schema_generator", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot import generator at {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generator", required=True, type=Path)
    parser.add_argument("--inventory", required=True, type=Path)
    args = parser.parse_args()

    generator = load_generator(args.generator.resolve())
    value = json.loads(args.inventory.read_text(encoding="utf-8"))
    rows, object_types = generator.selected_rows(value)
    packed = generator.pack_contract(rows, object_types)

    profile_ids = {"eaw": 0, "foc": 1, "remake": 2}
    node_kind_ids = {"element": 0, "attribute": 1}
    status_ids = {"unknown": 0, "known": 1, "deprecated": 2}
    expected_selected = {}
    expected_object_types = set()
    for row in value["rows"]:
        key = (row["profile"], row["object_type"].lower(), row["tag_path"].lower(),
               row["node_kind"], row["tag_name"].lower())
        previous = expected_selected.get(key)
        if previous is None or status_ids[row["status"]] > status_ids[previous["status"]]:
            expected_selected[key] = row
        expected_object_types.add(row["object_type"])
    expected_source_rows = [expected_selected[key] for key in sorted(expected_selected)]
    assert rows == expected_source_rows, "generator dropped or changed a selected schema row"
    assert object_types == sorted(expected_object_types, key=str.lower), \
        "generator dropped or changed an object type"

    blobs = [b"".join(text.encode("utf-8") + b"\0" for text in chunk)
             for chunk in packed.string_chunks]

    def unpack(locator: int) -> str:
        chunk = locator >> generator.STRING_CHUNK_OFFSET_BITS
        offset = locator & ((1 << generator.STRING_CHUNK_OFFSET_BITS) - 1)
        end = blobs[chunk].index(0, offset)
        return blobs[chunk][offset:end].decode("utf-8")

    unpacked_rows = []
    for row in packed.rows:
        unpacked_rows.append((
            *(unpack(offset) for offset in row[:4]),
            row[4], row[5], row[6],
        ))
    expected_rows = [(
        row["object_type"], row["tag_path"], row["tag_name"], row["schema_ref"] or "",
        profile_ids[row["profile"]], node_kind_ids[row["node_kind"]], status_ids[row["status"]],
    ) for row in rows]
    assert unpacked_rows == expected_rows, "packed rows changed a key, classification, or schema reference"
    assert [unpack(locator) for locator in packed.object_type_locators] == object_types, \
        "packed object-type normalization table changed"

    rendered = generator.render_contract(value)
    with tempfile.TemporaryDirectory(prefix="eawr-schema-generator-") as temporary:
        first = Path(temporary) / "first.inc"
        second = Path(temporary) / "second.inc"
        with first.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(rendered)
        with second.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(generator.render_contract(value))
        assert first.read_bytes() == second.read_bytes(), "generator output is not byte-reproducible"

    blob_size = sum(len(blob) for blob in blobs)
    assert all(len(blob) <= generator.STRING_CHUNK_LIMIT for blob in blobs), "string chunk is too large"
    print(f"packed XML schema integrity passed: {len(rows)} rows, {blob_size} string bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
