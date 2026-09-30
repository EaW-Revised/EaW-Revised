"""Derive, or re-check, the pinned pair list of the declaration probe.

The pair list is the metadata-only frozen input of
`animation_corpus_declarations`.  It holds every `compatible_unapproved`
alternative of the pinned association receipt (schema v2, the fixed runner's
fix-run receipt) and nothing else: no asset bytes and no host path.

* the ALA identity (path, SHA-256, effective-VFS provenance, size);
* the frozen R0-selected model and its run-4 SHA-256;
* the compatible candidate's identity as the receipt recorded it;
* the priority flag (`ei_armytrooper_heavy` rows).

Every pair is cross-checked against the tracked frozen metadata manifest: the
ALA identity, the selected model and the candidate's membership of the ordered
R0 list must match, and a candidate with a run-4 identity must match it too.

Usage:
    python corpus_declaration_pairs.py generate <association-candidates.json> <frozen-metadata.tsv> <pairs.tsv>
    python corpus_declaration_pairs.py check    <association-candidates.json> <frozen-metadata.tsv> <pairs.tsv>

`generate` refuses unless both sources hash to their pinned values; `check`
regenerates in memory and requires byte equality with the pair list.  Output is
UTF-8 with LF line endings, sorted by ALA path.
"""
import hashlib
import json
import re
import sys

SCHEMA = "eawr.animation-declaration-pairs"
SCHEMA_VERSION = 1
PINNED_RECEIPT_SHA256 = "751723ed8f86757121f805668501b9d40c99ccfe919b451225b64db1994d927b"
PINNED_FROZEN_METADATA_SHA256 = "2c3456374eed6a5dff02cecbfd727c0ad0942ca8e7d932b8fb5142e52a5678e2"
EXPECTED_PAIRS = 63
EXPECTED_PRIORITY = 58
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


def pinned_bytes(path, pinned, label):
    with open(path, "rb") as stream:
        data = stream.read()
    digest = hashlib.sha256(data).hexdigest()
    require(digest == pinned, f"{label} is stale: sha256 {digest} is not pinned {pinned}")
    return data


def frozen_failures(text):
    failures = {}
    models = {}
    for line in text.split("\n"):
        cells = line.split("\t")
        if cells[0] == "failure":
            # failure, index, stage, code, cause, identity(7), selected, r0...
            failures[cells[5]] = {"sha256": cells[6], "selected": cells[12], "r0": cells[13:]}
        elif cells[0] == "model":
            models[cells[1]] = cells[2]
    return failures, models


def render(receipt_path, metadata_path):
    receipt = json.loads(pinned_bytes(receipt_path, PINNED_RECEIPT_SHA256, "association receipt").decode("utf-8"))
    metadata = pinned_bytes(metadata_path, PINNED_FROZEN_METADATA_SHA256, "frozen metadata").decode("utf-8")
    require(receipt.get("schema") == "eawr.animation-association-candidates", "unexpected receipt schema")
    require(receipt.get("schema_version") == 2, "unexpected receipt schema version")
    require(receipt.get("association_approved") is False and receipt.get("associations_promoted") == 0,
            "receipt claims an approval")
    require(receipt["frozen_metadata"]["sha256"] == PINNED_FROZEN_METADATA_SHA256, "receipt names another manifest")
    failures, models = frozen_failures(metadata)

    pairs = []
    for row in receipt["rows"]:
        for alternative in row.get("alternatives", []):
            if alternative["status"] != "compatible_unapproved":
                continue
            require(alternative.get("approved") is False, "compatible alternative is approved")
            animation = row["animation"]
            frozen = failures.get(animation["path"])
            require(frozen is not None, f"{animation['path']} is not a frozen failure")
            require(frozen["sha256"] == animation["sha256"], f"{animation['path']}: ALA hash drift")
            require(frozen["selected"] == row["baseline_selected_model"], f"{animation['path']}: selected drift")
            require(frozen["r0"] == row["r0_candidates"], f"{animation['path']}: R0 list drift")
            require(alternative["path"] in frozen["r0"] and alternative["path"] != frozen["selected"],
                    f"{animation['path']}: candidate is not an additional R0 candidate")
            if alternative["path"] in models:
                require(models[alternative["path"]] == alternative["sha256"],
                        f"{alternative['path']}: candidate hash differs from run-4")
            selected_sha = models.get(frozen["selected"])
            require(selected_sha == row["selected"]["sha256"], f"{animation['path']}: selected hash drift")
            pairs.append([
                "pair",
                *(field(animation[key]) for key in ("path",) + IDENTITY),
                field(frozen["selected"]), field(selected_sha),
                *(field(alternative[key]) for key in ("path",) + IDENTITY),
                "1" if row["priority"] else "0",
            ])
    pairs.sort(key=lambda cells: cells[1])
    require(len(pairs) == EXPECTED_PAIRS, f"expected {EXPECTED_PAIRS} compatible pairs, found {len(pairs)}")
    require(len({cells[1] for cells in pairs}) == len(pairs), "an ALA has two compatible pairs")
    priority = sum(1 for cells in pairs if cells[-1] == "1")
    require(priority == EXPECTED_PRIORITY, f"expected {EXPECTED_PRIORITY} priority pairs, found {priority}")

    lines = [
        f"{SCHEMA}\t{SCHEMA_VERSION}",
        f"source\tassociation_receipt_sha256\t{PINNED_RECEIPT_SHA256}",
        f"source\tfrozen_metadata_sha256\t{PINNED_FROZEN_METADATA_SHA256}",
        f"counts\t{len(pairs)}\t{priority}",
    ]
    lines.extend("\t".join(cells) for cells in pairs)
    return ("\n".join(lines) + "\n").encode("utf-8")


def main(argv):
    if len(argv) != 5 or argv[1] not in ("generate", "check"):
        print(__doc__, file=sys.stderr)
        return 2
    try:
        output = render(argv[2], argv[3])
        if argv[1] == "generate":
            with open(argv[4], "wb") as stream:
                stream.write(output)
            print(f"wrote {argv[4]} sha256 {hashlib.sha256(output).hexdigest()}")
            return 0
        with open(argv[4], "rb") as stream:
            existing = stream.read()
        require(existing == output, "pair list differs from a fresh derivation")
        print(f"pair list matches sha256 {hashlib.sha256(output).hexdigest()}")
        return 0
    except (Refused, OSError, KeyError, ValueError) as error:
        print(f"refused: {error}", file=sys.stderr)
        return 4


if __name__ == "__main__":
    sys.exit(main(sys.argv))
