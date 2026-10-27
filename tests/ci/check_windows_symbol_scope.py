"""Check linked PE/PDB pairs, including warm trees containing stale test PDBs."""
import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import symbol_store


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--with-symbols", type=Path, action="append", default=[])
    parser.add_argument("--without-symbols", type=Path, action="append", default=[])
    args = parser.parse_args()
    for binary in args.with_symbols:
        name, key, _ = symbol_store.pe_identity(binary.read_bytes())
        assert symbol_store.pdb_identity((binary.parent / name).read_bytes()) == key, binary
    for binary in args.without_symbols:
        try:
            symbol_store.pe_identity(binary.read_bytes())
        except ValueError as error:
            if str(error) not in ("PE image has no PDB identity", "missing PE debug directory"):
                raise
        else:
            raise AssertionError(f"{binary} unexpectedly links an external PDB")
    print(f"Symbol scope passed: {len(args.with_symbols)} pairs, "
          f"{len(args.without_symbols)} images without PDB references")


if __name__ == "__main__":
    main()
