"""Check the FoC disposition of the frozen association failures (#81).

corpus_association_foc.tsv is written by animation_corpus_association_foc from
the installed game (it cannot run in CI; animation_corpus_association_foc_rederive
re-derives it when EAWR_EAW_GAME_ROOT is set). This check needs no game:

- the receipt, the last line, is the SHA-256 the tool wrote over every line
  before it, so a hand edit of any row fails;
- every frozen failure row has exactly one FoC row with the same index, stage
  and path, and the totals add up;
- each cause is the one the tool writes for its disposition and path: the
  fixed texts, a diagnostic code for a failure, and for a bound row the clip
  type and index the path's own <base>_<TYPE>_NN.ala name gives
  (unit_clips.hpp's type table).

Usage: python corpus_association_foc.py <frozen-metadata.tsv> <corpus_association_foc.tsv> <unit_clips.hpp>
"""
import hashlib
import re
import sys
from collections import Counter

DISPOSITIONS = {"bound", "binding_failed", "model_failed", "index_gap", "never_loaded", "not_in_foc"}
WITH_MODEL = {"bound", "binding_failed", "model_failed", "index_gap"}
NOT_IN_FOC = "Remake mod file: absent from the FoC view (FoC-only target)"
NO_TYPE = "the name is not <base>_<TYPE>_NN.ala for any retail type"
NO_OWNER = "no FoC model or anim override name is the base of this <base>_<TYPE>_NN name"
INDEX_GAP = "a lower index of this type is missing"
DIAGNOSTIC = re.compile(r"EAWR-[A-Z]+-\d{4} \S")
BOUND = re.compile(r"(model_stem|anim_override:\S+) ([A-Z0-9_]+) index (\d+)")


def clip_types(header_path):
    """The retail clip type table, in order, from unit_clips.hpp."""
    with open(header_path, encoding="utf-8") as handle:
        text = handle.read()
    table = re.search(r"clip_type_names\{(.*?)\};", text, re.S)
    return re.findall(r'"([A-Z0-9_]+)"', table.group(1)) if table else []


def clip_names(path, types):
    """Every (base, TYPE, index) a <base>_<TYPE>_NN.ala path can be read as."""
    match = re.fullmatch(r"(.+)_(\d\d)\.ala", path.lower())
    if not match:
        return []
    names = []
    for clip_type in types:
        suffix = "_" + clip_type.lower()
        if match.group(1).endswith(suffix) and len(match.group(1)) > len(suffix):
            names.append((match.group(1)[: -len(suffix)], clip_type, int(match.group(2))))
    return names


def cause_error(disposition, path, model, cause, types):
    """Why the cause is not the one the tool writes for this row; None when it is."""
    names = clip_names(path, types)
    if disposition == "not_in_foc":
        return None if cause == NOT_IN_FOC else "not the not_in_foc cause"
    if disposition == "never_loaded":
        wanted = NO_OWNER if names else NO_TYPE
        return None if cause == wanted else f"a never_loaded row of this name has the cause '{wanted}'"
    if disposition == "index_gap":
        return None if cause == INDEX_GAP else "not the index_gap cause"
    if not names:
        return f"{disposition}, but the name is not <base>_<TYPE>_NN.ala"
    if not model.endswith(".alo"):
        return f"model {model} is not a .alo"
    if disposition in ("binding_failed", "model_failed"):
        body = cause[len("clip: "):] if disposition == "binding_failed" and cause.startswith("clip: ") else cause
        return None if DIAGNOSTIC.match(body) else "not a diagnostic (EAWR-<AREA>-NNNN message)"
    bound = BOUND.fullmatch(cause)
    if not bound:
        return "not '<rule> <TYPE> index N'"
    rule, clip_type, index = bound.group(1), bound.group(2), int(bound.group(3))
    readings = [(base, t, i) for base, t, i in names if t == clip_type and i == index]
    if not readings:
        return f"{clip_type} index {index} is not a reading of the path"
    if rule == "model_stem" and all(base + ".alo" != model for base, _, _ in readings):
        return f"model_stem, but {model} is not the path's base model"
    return None


def rows(lines, kind):
    return [line.split("\t") for line in lines if line.startswith(kind + "\t")]


def main(frozen_path, foc_path, header_path):
    errors = []
    types = clip_types(header_path)
    if len(types) != 119:
        errors.append(f"expected 119 clip types in {header_path}, read {len(types)}")
    with open(frozen_path, encoding="utf-8", newline="") as handle:
        frozen = {r[1]: (r[2], r[5]) for r in rows([l.rstrip("\r\n") for l in handle], "failure")}
    with open(foc_path, encoding="utf-8", newline="") as handle:
        lines = [line.rstrip("\r\n") for line in handle]

    receipt = lines[-1].split("\t") if lines else []
    body = "".join(line + "\n" for line in lines[:-1])
    if len(receipt) != 3 or receipt[:2] != ["receipt", "sha256"]:
        errors.append("the last line is not the tool's receipt")
    elif hashlib.sha256(body.encode("utf-8")).hexdigest() != receipt[2]:
        errors.append("the receipt does not match the rows: the file was edited by hand; "
                      "re-derive it with animation_corpus_association_foc")

    foc = rows(lines, "row")
    seen = Counter(r[1] for r in foc)
    for index, count in seen.items():
        if count != 1:
            errors.append(f"index {index} appears {count} times")
    if set(seen) != set(frozen):
        errors.append(f"indices differ: {len(set(frozen) - set(seen))} missing, {len(set(seen) - set(frozen))} extra")
    counts = Counter()
    for r in foc:
        if len(r) != 7:
            errors.append(f"row {r[1] if len(r) > 1 else '?'} has {len(r)} fields")
            continue
        _, index, stage, path, disposition, model, cause = r
        if frozen.get(index) != (stage, path):
            errors.append(f"row {index} does not match the frozen stage and path")
        if disposition not in DISPOSITIONS:
            errors.append(f"row {index} has unknown disposition {disposition}")
            continue
        if (disposition in WITH_MODEL) != (model != "-"):
            errors.append(f"row {index}: {disposition} and model {model} disagree")
        elif (problem := cause_error(disposition, path, model, cause, types)) is not None:
            errors.append(f"row {index} cause '{cause}': {problem}")
        counts[disposition] += 1
    totals = {r[1]: int(r[2]) for r in rows(lines, "total")}
    if totals != dict(counts):
        errors.append(f"totals {totals} are not the row counts {dict(counts)}")
    if len(foc) != 356 or len(frozen) != 356:
        errors.append(f"expected 356 rows, frozen {len(frozen)}, foc {len(foc)}")
    for error in errors:
        print("FAILED:", error)
    if not errors:
        print("foc association dispositions:", dict(sorted(counts.items())))
    return 1 if errors else 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3]))
