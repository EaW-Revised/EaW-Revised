"""Compare the 19 tracked synthetic/native fixtures against a 76-run receipt."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from compare_installed import WORKERS, KINDS, carries_identity, digest

ROOT = Path(__file__).resolve().parents[2]


def fixture_names(root):
    manifest = root / 'fixture-manifest.json'
    if manifest.is_file():
        names = json.loads(manifest.read_text())
    else:
        names = subprocess.check_output(['git', '-C', str(root), 'ls-files', '*.eawr-replay'], text=True).splitlines()
    return sorted(n for n in names if 'mutated' not in n and '/installed/' not in n)


def baseline_rows(receipt, names):
    expected = {}
    for row in receipt['rows']:
        key = (row['replay'].replace('\\', '/'), row['workers'])
        if key in expected:
            raise ValueError('duplicate baseline fixture/worker')
        expected[key] = row
    if set(expected) != {(n, w) for n in names for w in WORKERS} or len(names) != 19:
        raise ValueError('replay76 baseline must cover exactly 19 fixtures on 1/2/4/8')
    if receipt.get('runs') != 76 or receipt.get('all_baseline_and_worker_bytes_equal') is not True:
        raise ValueError('baseline receipt did not pass all 76 runs')
    for name in names:
        kinds = {'hashes'} if 'v1' in Path(name).stem else set(KINDS)
        for w in WORKERS:
            if set(expected[name, w]['sha256']) != kinds:
                raise ValueError('baseline output kinds differ')
            if expected[name, w]['sha256'] != expected[name, 1]['sha256']:
                raise ValueError('baseline workers differ')
    return expected


def accept_changes(raw, baseline, names, accepted, reason, raw_sha256):
    """Produce a baseline only from a complete, independently compared remote run."""
    if not reason.strip() or not accepted or len(accepted) != len(set(accepted)):
        raise ValueError('a reason and distinct accepted fixture names are required')
    expected = baseline_rows(baseline, names)
    if not set(accepted) <= set(names):
        raise ValueError('accepted fixture is not in the baseline')
    groups = [g for g in raw.get('groups', []) if g.get('name') == 'replay76']
    if len(groups) != 1:
        raise ValueError('expected one replay76 group')
    result = groups[0].get('receipt', {})
    if (not raw.get('head') or not raw.get('program_sha256') or result.get('runs') != 76
            or result.get('workers') != list(WORKERS)
            or result.get('full_set') is not True or result.get('head') != raw.get('head')
            or result.get('program_sha256') != raw.get('program_sha256')
            or result.get('baseline_head') != baseline.get('head')):
        raise ValueError('run identity or coverage differs')
    for failure in result.get('failures', []):
        if len(failure) != 3 or failure[2] != 'baseline mismatch' or failure[0] not in accepted:
            raise ValueError('run has an unaccepted failure')
    rows = {}
    changed = set()
    for row in result.get('rows', []):
        name = row['replay'].replace('\\', '/')
        key = (name, row['workers'])
        if key in rows or key not in expected or row.get('exit_code') != 0:
            raise ValueError('duplicate, unexpected, or failed replay row')
        old = expected[key]
        kinds = {'hashes'} if 'v1' in Path(name).stem else set(KINDS)
        hashes = row.get('sha256', {})
        if (row.get('replay_sha256') != old.get('replay_sha256') or set(hashes) != kinds
                or any(not isinstance(h, str) or len(h) != 64 or any(c not in '0123456789abcdef' for c in h)
                       for h in hashes.values())):
            raise ValueError('fixture or output identity differs')
        if hashes != old['sha256']:
            changed.add(name)
            if name not in accepted:
                raise ValueError('unlisted baseline mismatch: ' + name)
        rows[key] = row
    if set(rows) != set(expected):
        raise ValueError('incomplete replay rows')
    for name in names:
        if any(rows[name, worker]['sha256'] != rows[name, 1]['sha256'] for worker in WORKERS):
            raise ValueError('worker mismatch: ' + name)
    if changed != set(accepted):
        raise ValueError('accepted fixtures must match the changed fixtures')
    mirror = raw.get('game_data_identity', {})
    previous = baseline.get('game_data_identity', {})
    if any(not previous.get(key) or mirror.get(key) != previous.get(key)
           for key in ('manifestSha256', 'files', 'bytes')):
        raise ValueError('game-data mirror differs')
    output = dict(result)
    output.update(failures=[], all_selected_runs_equal=True, all_baseline_and_worker_bytes_equal=True,
                  game_data_identity=mirror, accepted_changes=sorted(accepted), acceptance_reason=reason,
                  accepted_failures=result.get('failures', []), source_receipt_sha256=raw_sha256)
    return output


def requires_game(program, path, runner):
    if carries_identity(path.read_bytes()) or path.stem == 'm2-start':
        return True
    # Native fixtures outside the built-in closures fail closed without mounted data.
    with tempfile.TemporaryDirectory() as temporary:
        result = runner([str(program), '--replay', str(path), '--workers', '1',
                         '--hash-out', str(Path(temporary) / 'probe.csv')],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
    return result.returncode == 3 and b'--game-root is required' in result.stdout


def parse_workers(text):
    """'1,8' -> [1, 8]: a subset of the full 1/2/4/8 set that includes worker 1 (the reference run)."""
    parts = text.split(',')
    if not all(p.isascii() and p.isdigit() and p == str(int(p)) for p in parts):
        raise ValueError(f'--workers must be a comma list of plain integers: {text!r}')
    chosen = [int(p) for p in parts]
    if len(set(chosen)) != len(chosen) or any(w not in WORKERS for w in chosen) or 1 not in chosen:
        raise ValueError(f'--workers must be distinct values from {WORKERS} and include 1: {text!r}')
    return sorted(chosen)


def run(program, root, baseline, game_root, out, head, runner=subprocess.run, subset=None):
    selected = list(WORKERS) if subset is None else parse_workers(','.join(map(str, subset)))
    names = fixture_names(root)
    expected = baseline_rows(baseline, names) if baseline is not None else None
    if len(names) != 19:
        raise ValueError("replay76 requires exactly 19 fixtures")
    out.mkdir(parents=True, exist_ok=True)
    if (out / 'receipt.json').exists():
        raise ValueError('choose a fresh output directory')
    rows, failures, ones = [], [], {}
    for name in names:
        path = root / name
        if expected is not None and digest(path) != expected[name, 1]['replay_sha256']:
            raise ValueError(f'baseline fixture changed: {name}')
    # The mounted mirror is verified separately by the offload host driver.
    try:
        for name in names:
            path = root / name
            game = requires_game(program, path, runner)
            for workers in selected:
                stem = Path(name).parent.parent.name + '-' + path.stem + '-' + str(workers)
                kinds = {'hashes': KINDS['hashes']} if 'v1' in path.stem else KINDS
                outputs = {k: out / (stem + '.' + k + '.csv') for k in kinds}
                command = [str(program), '--replay', str(path), '--workers', str(workers)]
                if game:
                    command += ['--game-root', str(game_root)]
                for kind, flag in kinds.items():
                    command += [flag, str(outputs[kind])]
                result = runner(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
                (out / (stem + '.log')).write_bytes(result.stdout)
                hashes = {}
                if result.returncode:
                    failures.append([name, workers, 'exit', result.returncode])
                else:
                    for kind, output in outputs.items():
                        data = output.read_bytes()
                        hashes[kind] = hashlib.sha256(data).hexdigest()
                        if workers == 1:
                            ones[name, kind] = data
                        elif data != ones.get((name, kind)):
                            failures.append([name, workers, kind, 'worker mismatch'])
                    if expected is not None and hashes != expected[name, workers]['sha256']:
                        failures.append([name, workers, 'baseline mismatch'])
                rows.append(dict(replay=name, replay_sha256=digest(path), workers=workers,
                                 requires_game_root=game, exit_code=result.returncode, sha256=hashes))
                print(f'{name} workers={workers} exit={result.returncode}', flush=True)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        failures.append(['driver error', str(error)])
    receipt = dict(head=head, baseline_head=baseline.get('head') if baseline else None, program_sha256=digest(program),
                   runs=len(rows), workers=selected, full_set=selected == list(WORKERS), rows=rows, failures=failures)
    receipt['all_selected_runs_equal'] = len(rows) == len(names) * len(selected) and not failures
    # The release gate reads this flag: only the full 1/2/4/8 set can credit a replay76.
    receipt['all_baseline_and_worker_bytes_equal'] = (
        receipt['full_set'] and receipt['all_selected_runs_equal'] and len(rows) == 76)
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--program', type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--baseline', type=Path, help='baseline receipt.json')
    mode.add_argument('--record', action='store_true', help='record a full baseline without comparison')
    parser.add_argument('--game-root', type=Path)
    parser.add_argument('--fixtures-root', type=Path, default=ROOT)
    parser.add_argument('--head')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--workers', default=','.join(map(str, WORKERS)),
                        help='comma list, a subset of 1,2,4,8 that includes 1; a subset receipt is NOT a full replay76')
    parser.add_argument('--accept-receipt', type=Path, help='raw remote replay76 receipt to review without rerunning')
    parser.add_argument('--accept-changes', help='comma-separated exact fixture paths whose differences are reviewed')
    parser.add_argument('--reason', help='reason for accepting these changes')
    args = parser.parse_args()
    if args.accept_receipt:
        if not args.baseline:
            parser.error('accept mode requires --baseline')
        if not args.accept_changes or not args.reason or args.program or args.head or args.game_root:
            parser.error('accept mode needs --accept-changes and --reason, without run options')
        raw_bytes = args.accept_receipt.read_bytes()
        raw = json.loads(raw_bytes)
        baseline_bytes = args.baseline.read_bytes()
        if raw.get('package', {}).get('baseline_sha256', {}).get('replay76') != hashlib.sha256(baseline_bytes).hexdigest():
            raise ValueError('raw receipt names a different baseline')
        accepted = args.accept_changes.split(',')
        output = accept_changes(raw, json.loads(baseline_bytes), fixture_names(args.fixtures_root), accepted,
                                args.reason, hashlib.sha256(raw_bytes).hexdigest())
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open('x', encoding='utf-8') as file:
            json.dump(output, file, indent=2)
            file.write('\n')
        print('PASS: reviewed replay76 baseline written to ' + str(args.out))
        return 0
    if not args.program or not args.head or not args.game_root or args.accept_changes or args.reason:
        parser.error('run mode needs --program, --head and --game-root')
    if args.record and args.workers != ','.join(map(str, WORKERS)):
        parser.error('record mode requires all 1,2,4,8 workers')
    result = run(args.program, args.fixtures_root, json.loads(args.baseline.read_text()) if args.baseline else None,
                 args.game_root, args.out, args.head, subset=parse_workers(args.workers))
    if not result['all_selected_runs_equal']:
        return 1
    if not result['full_set']:
        print(f'PASS (SUBSET, not a full replay76): {result["runs"]} runs on workers {result["workers"]} match baseline.')
        return 0
    print('PASS: 76 replay runs equal across workers.' if args.record else 'PASS: 76 replay runs match baseline and workers.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
