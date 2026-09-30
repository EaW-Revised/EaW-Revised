"""Keep standard-library names globally qualified inside the Lua namespace."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
OPEN = "namespace eawr::script::EAWR_SFLUA_NAMESPACE {"
CLOSE = "} // namespace eawr::script::EAWR_SFLUA_NAMESPACE"
IMPORT = "using namespace ::eawr::script::sflua;"
UNQUALIFIED_STD = re.compile(r"(?<!:)\bstd::")


def main() -> int:
    sources = list((ROOT / "src/script/sflua").rglob("*.cpp"))
    sources += list((ROOT / "src/script/sflua").rglob("*.hpp"))
    sources += list((ROOT / "src/script/authoritative").glob("*.cpp"))
    sources += list((ROOT / "tests/script/numeric/bench").glob("*.cpp"))
    errors = []
    for path in sources:
        text = path.read_text(encoding="utf-8")
        if OPEN in text:
            before, remainder = text.split(OPEN, 1)
            body = remainder.split(CLOSE, 1)[0]
            first_line = before.count("\n") + 2
        elif IMPORT in text:
            body = text
            first_line = 1
        else:
            continue
        for line_number, line in enumerate(body.splitlines(), first_line):
            if UNQUALIFIED_STD.search(line.split("//", 1)[0]):
                errors.append(f"{path.relative_to(ROOT)}:{line_number}: use ::std:: in the Lua namespace")
    if errors:
        print("\n".join(errors))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
