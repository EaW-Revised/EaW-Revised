#!/usr/bin/env python3
"""List literal/anchor misses and select a reproducible sample for consumer review.

Textual misses are review candidates, not evidence of missing application: loaders
often rename fields, construct keys, or apply values in another source file.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import random
import re

if __package__ in (None, ""):
    import tag_registry as tr
else:
    from . import tag_registry as tr


def verify(rows: list[dict], root: Path) -> list[dict]:
    results = []
    for row in rows:
        if row['status'] != 'applied':
            continue
        targets = []
        leaf = row['tag'].rsplit('/', 1)[-1].lstrip('@')
        for target in row.get('applied', []):
            relative, identifier = tr.code_location(target['code'])
            path = root / relative
            body = tr.uncomment_cpp(path.read_text(encoding='utf-8')) if path.is_file() else ''
            targets.append({'code': target['code'], 'file_exists': path.is_file(),
                            'tag_literal': bool(re.search(r'"' + re.escape(leaf) + r'"', body, re.I)),
                            'identifier_exists': bool(identifier and tr.identifier_pattern(identifier).search(body))})
        results.append({'classes': row['classes'], 'tag': row['tag'], 'area': row['area'], 'targets': targets})
    return results


def sample_rows(rows: list[dict], size: int, seed: int) -> list[dict]:
    candidates = sorted((r for r in rows if r['status'] == 'applied'),
                        key=lambda r: (r['tag'].casefold(), tuple(r['classes'])))
    return random.Random(seed).sample(candidates, min(size, len(candidates)))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--statuses', type=Path, default=tr.DEFAULT_STATUSES)
    parser.add_argument('--root', type=Path, default=tr.ROOT)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--seed', type=int, default=1021)
    parser.add_argument('--sample-size', type=int, default=100)
    args = parser.parse_args(argv)
    rows = tr.load_registry(args.statuses)['rows']
    checked = verify(rows, args.root)
    misses = [r for r in checked if any(not t['tag_literal'] or not t['identifier_exists']
                                       or not t['file_exists'] for t in r['targets'])]
    result = {'seed': args.seed, 'checked_rows': len(checked), 'misses': misses,
              'sample': sample_rows(rows, args.sample_size, args.seed)}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f"{len(checked)} applied rows checked; {len(misses)} textual misses; "
          f"{len(result['sample'])} rows selected for hand review (seed {args.seed})")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
