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


def requires_game(program, path, runner):
    if carries_identity(path.read_bytes()) or path.stem == 'm2-start':
        return True
    # Native fixtures outside the built-in closures fail closed without mounted data.
    with tempfile.TemporaryDirectory() as temporary:
        result = runner([str(program), '--replay', str(path), '--workers', '1',
                         '--hash-out', str(Path(temporary) / 'probe.csv')],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
    return result.returncode == 3 and b'--game-root is required' in result.stdout


def run(program, root, baseline, game_root, out, head, runner=subprocess.run):
    names = fixture_names(root)
    expected = baseline_rows(baseline, names)
    out.mkdir(parents=True, exist_ok=True)
    if (out / 'receipt.json').exists():
        raise ValueError('choose a fresh output directory')
    rows, failures, ones = [], [], {}
    for name in names:
        path = root / name
        if digest(path) != expected[name, 1]['replay_sha256']:
            raise ValueError(f'baseline fixture changed: {name}')
    # The mounted mirror is verified separately by the offload host driver.
    try:
        for name in names:
            path = root / name
            game = requires_game(program, path, runner)
            for workers in WORKERS:
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
                    if hashes != expected[name, workers]['sha256']:
                        failures.append([name, workers, 'baseline mismatch'])
                rows.append(dict(replay=name, replay_sha256=digest(path), workers=workers,
                                 requires_game_root=game, exit_code=result.returncode, sha256=hashes))
                print(f'{name} workers={workers} exit={result.returncode}', flush=True)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        failures.append(['driver error', str(error)])
    receipt = dict(head=head, baseline_head=baseline.get('head'), program_sha256=digest(program),
                   runs=len(rows), workers=list(WORKERS), rows=rows, failures=failures,
                   all_baseline_and_worker_bytes_equal=len(rows) == 76 and not failures)
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--program', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, required=True, help='baseline receipt.json')
    parser.add_argument('--game-root', type=Path, required=True)
    parser.add_argument('--fixtures-root', type=Path, default=ROOT)
    parser.add_argument('--head', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    result = run(args.program, args.fixtures_root, json.loads(args.baseline.read_text()),
                 args.game_root, args.out, args.head)
    if not result['all_baseline_and_worker_bytes_equal']:
        return 1
    print('PASS: 76 replay runs match baseline and workers.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
