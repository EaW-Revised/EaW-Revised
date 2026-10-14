"""Build a publish-safe catalog from a private dump and reviewed owner/type translations.

The review JSON maps raw table tokens to {kind, evidence}, numeric parser tokens to
value-type descriptions, and supplies evidence descriptions and extra rows for
non-leaf dispatch tags. Raw tokens and binary locations are never emitted.
"""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

try:
    from .engine_tags import EngineTags, ascii_fold
except ImportError:
    from engine_tags import EngineTags, ascii_fold


def build(rows, review):
    result = {}
    for row in rows:
        owner = review["tables"][row["table"]]
        value_type = review["value_types"][row["parser"]]
        public = {"tag": row["name"], "table_kind": owner["kind"],
                  "value_type": value_type, "evidence": owner["evidence"]}
        result[(ascii_fold(public["tag"]), public["table_kind"], value_type)] = public
    for row in review.get("extra_rows", []):
        result[(ascii_fold(row["tag"]), row["table_kind"], row["value_type"])] = row
    value = {"schema_version": 1, "note": "Debug-build name recognition only; table membership does not establish context acceptance or remake application.",
             "evidence": review["evidence"], "rows": [result[k] for k in sorted(result)]}
    EngineTags(value)
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rows", type=Path, required=True)
    parser.add_argument("--review", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    with args.rows.open(encoding="utf-8") as stream:
        value = build(csv.DictReader(stream, delimiter="\t"), json.loads(args.review.read_text(encoding="utf-8")))
    # One row per line keeps catalog changes reviewable, as in the authored registry.
    header = json.dumps({k: v for k, v in value.items() if k != "rows"}, indent=2, ensure_ascii=False)
    rows_text = ",\n".join("    " + json.dumps(row, ensure_ascii=False) for row in value["rows"])
    args.out.write_text(header[:-2] + ',\n  "rows": [\n' + rows_text + "\n  ]\n}\n", encoding="utf-8", newline="\n")


if __name__ == "__main__": main()
