"""Regression for #363 review 1: corpus_association_foc.py must reject a false cause.

Runs the checker on temporary copies of corpus_association_foc.tsv: the committed file passes;
a hand-edited cause fails on the receipt; the same edit with the receipt recomputed still fails
on the cause rules, as do a bound row whose clip type is not a reading of its path and a
never_loaded row with the other never_loaded cause; a missing receipt fails.

Usage: python corpus_association_foc_tamper.py <frozen-metadata.tsv> <corpus_association_foc.tsv> <unit_clips.hpp>
"""
import contextlib
import hashlib
import io
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import corpus_association_foc as checker  # noqa: E402


def with_receipt(lines):
    body = "".join(line + "\n" for line in lines)
    return lines + ["receipt\tsha256\t" + hashlib.sha256(body.encode("utf-8")).hexdigest()]


def edit_cause(lines, disposition, cause):
    """Replace the cause of the first row with this disposition."""
    edited = list(lines)
    for position, line in enumerate(edited):
        fields = line.split("\t")
        if fields[0] == "row" and fields[4] == disposition:
            edited[position] = "\t".join(fields[:6] + [cause])
            return edited
    raise AssertionError(f"no {disposition} row")


def run(frozen, header, lines, directory, name):
    path = pathlib.Path(directory) / name
    path.write_text("".join(line + "\n" for line in lines), encoding="utf-8", newline="")
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        code = checker.main(frozen, str(path), header)
    return code, output.getvalue()


def main(frozen, foc, header):
    lines = pathlib.Path(foc).read_text(encoding="utf-8").splitlines()
    body = lines[:-1]
    bound_row = next(line for line in body if line.split("\t")[4:5] == ["bound"])
    bound_cause = bound_row.split("\t")[6]
    wrong_type = bound_cause.replace(bound_cause.split(" ")[1], "TURNL_HALF" if " IDLE " not in bound_cause else "MOVE")
    cases = [
        ("committed", lines, 0, None),
        ("hand-edited cause", edit_cause(body, "binding_failed", "entirely false cause") + lines[-1:], 1, "receipt"),
        ("false cause, receipt recomputed", with_receipt(edit_cause(body, "binding_failed", "entirely false cause")),
         1, "diagnostic"),
        ("bound cause names another clip type", with_receipt(edit_cause(body, "bound", wrong_type)), 1,
         "not a reading of the path"),
        ("not_in_foc cause swapped", with_receipt(edit_cause(body, "not_in_foc", checker.NO_TYPE)), 1,
         "not the not_in_foc cause"),
        ("no receipt", body, 1, "receipt"),
    ]
    failures = 0
    with tempfile.TemporaryDirectory(prefix="eawr-foc-association-") as directory:
        for number, (name, case, wanted, text) in enumerate(cases):
            code, output = run(frozen, header, case, directory, f"case{number}.tsv")
            if code != wanted or (text is not None and text not in output):
                failures += 1
                print(f"FAILED: {name}: exit {code}, wanted {wanted} with '{text}'\n{output}")
    if failures == 0:
        print(f"foc association tamper cases passed: {len(cases)}")
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3]))
