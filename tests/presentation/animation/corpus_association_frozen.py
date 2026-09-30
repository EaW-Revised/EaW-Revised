"""Derive, or re-check, the frozen association metadata manifest.

The manifest is the metadata-only frozen input of the P1-03 association audit
(`animation_corpus_validation --association-audit`).  It is derived from the
retained run-4 diagnostic receipt and its v1 playback ledger, and holds no
asset bytes and no host path:

* every frozen failure: index, stage, code, cause, the ALA identity (path,
  SHA-256, effective-VFS provenance, size), the selected model and the full
  ordered R0 candidate list;
* every model named by a failure (selected or R0 candidate) for which run-4
  recorded an identity: SHA-256, provenance, size and bone count;
* every other named candidate, listed as unrecorded.

Usage:
    python corpus_association_frozen.py generate <run4-diagnostics.json> <run4-ledger.json> <manifest.tsv>
    python corpus_association_frozen.py check    <run4-diagnostics.json> <run4-ledger.json> <manifest.tsv>

`generate` refuses unless both sources hash to their pinned values; `check`
regenerates in memory and requires byte equality with the manifest.  Output is
UTF-8 with LF line endings and deterministic ordering.
"""
import hashlib
import json
import re
import sys

SCHEMA = "eawr.animation-association-frozen-metadata"
SCHEMA_VERSION = 1
RULE = "R0-same-directory-longest-underscore-prefix"
PINNED_DIAGNOSTICS_SHA256 = "0bd26f2e297f1a571b32ab80c0b6beeb43395534988a49142391c5f9ed81e1f3"
PINNED_LEDGER_SHA256 = "d49aa6cd1d718de39d255f3eee4740a695cc6f13a1d9d64a870895a5f7bbd5a8"
COUNTS = (7685, 7329, 356)
IDENTITY = ("sha256", "layer_id", "origin", "source_id", "original_path", "size")
HOST_PATH = re.compile(r"(^|[^A-Za-z])[A-Za-z]:[\\/]|^[\\/]")


class Refused(Exception):
    pass


def require(condition, message):
    if not condition:
        raise Refused(message)


def field(value):
    text = str(value)
    require(not any(ord(c) < 0x20 or ord(c) == 0x7F for c in text), f"control byte in field {text!r}")
    require(not HOST_PATH.search(text), f"host path in field {text!r}")
    return text


def hex64(value):
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value), f"not a SHA-256: {value!r}")
    return value


def read_pinned(path, pinned, label):
    data = open(path, "rb").read()
    digest = hashlib.sha256(data).hexdigest()
    require(digest == pinned, f"{label} is stale: sha256 {digest} is not pinned {pinned}")
    return json.loads(data.decode("utf-8"), strict=True)


def build(diagnostics_path, ledger_path):
    diagnostics = read_pinned(diagnostics_path, PINNED_DIAGNOSTICS_SHA256, "run-4 diagnostics")
    ledger = read_pinned(ledger_path, PINNED_LEDGER_SHA256, "run-4 ledger")
    require(diagnostics["schema"] == "eawr.animation-corpus-diagnostics" and diagnostics["schema_version"] == 1,
            "diagnostics schema")
    require(diagnostics["association_rule"] == RULE, "association rule")
    counts = diagnostics["counts"]
    require((counts["animation_count"], counts["playback_passed"], counts["failure_count"]) == COUNTS,
            "diagnostic counts")
    require((ledger["animation_count"], ledger["playback_passed"], ledger["failure_count"]) == COUNTS, "ledger counts")

    rows = diagnostics["rows"]
    require(len(rows) == COUNTS[0], "diagnostic row count")
    failures = sorted((r for r in rows if r["stage"] != "passed"), key=lambda r: r["baseline_failure_index"])
    require([r["baseline_failure_index"] for r in failures] == list(range(1, COUNTS[2] + 1)),
            "failure indices are not contiguous from 1")
    require(len(ledger["failures"]) == COUNTS[2], "ledger failure count")
    # The ledger is the pinned v1 baseline; every failure it names must be the
    # diagnostic row with the same index.
    for row, entry in zip(failures, ledger["failures"]):
        model_path = (row["model"] or {}).get("path", "")
        require((entry["path"], entry["sha256"], entry["model_path"], entry["stage"], entry["code"], entry["cause"])
                == (row["path"], row["sha256"], model_path, row["stage"], row["code"], row["cause"]),
                f"ledger failure {row['baseline_failure_index']} differs from run-4 diagnostics")

    # Model identities recorded anywhere in run-4 (each row records its
    # selected model).  A model recorded twice must be recorded identically.
    recorded = {}
    for row in rows:
        model = row["model"]
        if not model:
            continue
        record = tuple(model[key] for key in IDENTITY) + (model["bone_count"],)
        require(recorded.setdefault(model["path"], record) == record, f"inconsistent run-4 model {model['path']}")

    named = set()
    lines = [f"{SCHEMA}\t{SCHEMA_VERSION}",
             f"rule\t{RULE}",
             f"source\tdiagnostics_sha256\t{PINNED_DIAGNOSTICS_SHA256}",
             f"source\tledger_sha256\t{PINNED_LEDGER_SHA256}",
             "counts\t" + "\t".join(str(n) for n in COUNTS)]
    for row in failures:
        selected = (row["model"] or {}).get("path", "")
        candidates = row["r0_qualifying_models"]
        require(len(set(candidates)) == len(candidates), f"duplicate R0 candidate in {row['path']}")
        require(selected == "" or selected == candidates[0], f"selected model is not the first R0 candidate: {row['path']}")
        require((selected == "") == (row["stage"] == "model_match"), f"selected model does not match stage: {row['path']}")
        named.update(candidates)
        values = [row["baseline_failure_index"], row["stage"], row["code"], row["cause"], row["path"],
                  hex64(row["sha256"]), row["layer_id"], row["origin"], row["source_id"], row["original_path"],
                  row["size"], selected] + candidates
        lines.append("failure\t" + "\t".join(field(v) for v in values))
    for path in sorted(named):
        if path in recorded:
            sha, layer, origin, source, original, size, bones = recorded[path]
            values = [path, hex64(sha), layer, origin, source, original, size, "null" if bones is None else bones]
            lines.append("model\t" + "\t".join(field(v) for v in values))
    for path in sorted(named):
        if path not in recorded:
            lines.append("unrecorded\t" + field(path))
    return ("\n".join(lines) + "\n").encode("utf-8")


def main(argv):
    if len(argv) != 5 or argv[1] not in ("generate", "check"):
        print(__doc__, file=sys.stderr)
        return 2
    try:
        data = build(argv[2], argv[3])
    except (Refused, OSError, ValueError, KeyError, TypeError) as error:
        print(f"refused: {error}", file=sys.stderr)
        return 4
    if argv[1] == "generate":
        with open(argv[4], "wb") as output:
            output.write(data)
    else:
        try:
            existing = open(argv[4], "rb").read()
        except OSError as error:
            print(f"refused: {error}", file=sys.stderr)
            return 4
        if existing != data:
            print("refused: manifest differs from its regenerated bytes", file=sys.stderr)
            return 4
    lines = data.count(b"\n")
    print(f"manifest sha256={hashlib.sha256(data).hexdigest()} bytes={len(data)} lines={lines}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
