"""Compare installed or synthetic replay groups on 1/2/4/8 workers.

Installed content is a batch/release check. Baselines are created explicitly
from native replay runs with --record-baseline, never by inventing replay bytes.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
INSTALLED = ROOT / 'tests/skirmish/fixtures/installed'
WORKERS = (1, 2, 4, 8)
KINDS = {'hashes': '--hash-out', 'events': '--events-out', 'snapshots': '--snapshot-out'}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def validate_inputs(program, game_root, required=False):
    if not program.is_file():
        raise ValueError(f'--program does not exist: {program}')
    if required and not game_root:
        raise ValueError('installed replays require --game-root or EAWR_EAW_GAME_ROOT')
    if game_root:
        parts = ('GameData/Data', 'GameData/StarWarsG.exe', 'corruption/Data', 'corruption/StarWarsG.exe')
        if not all((game_root / part).exists() for part in parts):
            raise ValueError(f'--game-root / EAWR_EAW_GAME_ROOT is missing installed data: {game_root}')

def fixtures(group, root):
    if group == 'installed':
        paths = sorted((root / 'tests/skirmish/fixtures/installed').glob('*.eawr-replay'))
        if len(paths) != 4:
            raise ValueError('installed group requires all four native fixtures')
        return paths
    names = subprocess.check_output(['git', '-C', str(root), 'ls-files', '*.eawr-replay'], text=True).splitlines()
    return [root / name for name in names if 'mutated' not in name and '/installed/' not in name.replace('\\', '/')]

def compare(program, paths, game_root, out, baseline=None, group='installed', runner=subprocess.run):
    out.mkdir(parents=True, exist_ok=True)
    if (out / 'receipt.json').exists():
        raise ValueError('choose a fresh output directory')
    expected = {(Path(r['replay'].replace('\\', '/')).name, r['workers']): r for r in baseline['rows']} if baseline else {}
    if baseline and baseline.get('group', 'synthetic') != group:
        raise ValueError('baseline group differs')
    rows, failures, ones = [], [], {}
    for path in paths:
        name = path.name
        raw = path.read_bytes()
        # Native extension headers and m2-start need mounted content even in the synthetic group.
        game = group == 'installed' or path.stem == 'm2-start' or 'tests/fidelity/fixtures' in path.as_posix() or carries_identity(raw)
        if game and not game_root:
            raise ValueError('installed/match-identity replays require --game-root or EAWR_EAW_GAME_ROOT')
        for workers in WORKERS:
            stem = out / f'{path.stem}-{workers}'
            kinds = {'hashes': '--hash-out'} if 'v1' in path.stem else KINDS
            outputs = {k: Path(str(stem) + '.' + k + '.csv') for k in kinds}
            command = [str(program), '--replay', str(path), '--workers', str(workers)]
            if game:
                command += ['--game-root', str(game_root)]
            for kind, flag in kinds.items():
                command += [flag, str(outputs[kind])]
            result = runner(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
            Path(str(stem) + '.log').write_bytes(result.stdout)
            if result.returncode:
                raise RuntimeError(f'{name}/{workers}: exit {result.returncode}: {result.stdout.decode(errors="replace")}')
            hashes = {}
            for kind, output in outputs.items():
                data = output.read_bytes()
                hashes[kind] = hashlib.sha256(data).hexdigest()
                key = (name, kind)
                if workers == 1:
                    ones[key] = data
                elif data != ones[key]:
                    failures.append([name, workers, kind, 'worker mismatch'])
            row = dict(replay=name, replay_sha256=hashlib.sha256(raw).hexdigest(), workers=workers,
                       requires_game_root=game, sha256=hashes)
            if baseline:
                prior = expected.get((name, workers))
                if not prior or prior['replay_sha256'] != row['replay_sha256']:
                    failures.append([name, workers, 'fixture missing or changed'])
                elif prior['sha256'] != hashes:
                    failures.append([name, workers, 'baseline mismatch'])
            rows.append(row)
            print(f'{group}: {name} workers={workers}', flush=True)
    if baseline and set(expected) != {(r['replay'], r['workers']) for r in rows}:
        failures.append(['baseline fixture/worker set differs'])
    receipt = dict(group=group, program_sha256=digest(program), workers=list(WORKERS), runs=len(rows),
                   all_baseline_and_worker_bytes_equal=not failures, failures=failures, rows=rows)
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if failures:
        raise ValueError(failures)
    return receipt

def carries_identity(data):
    import struct
    if len(data) < 12 or data[:8] != b'EAWRPLY\0':
        raise ValueError('invalid replay header')
    version, size = struct.unpack_from('<HH', data, 8)
    if version not in (4, 5):
        return False
    if not 108 <= size <= len(data):
        raise ValueError('invalid replay extension header')
    count = struct.unpack_from('<I', data, 104)[0]
    offset, found = 108, False
    for _ in range(count):
        if offset + 4 > size:
            raise ValueError('truncated replay extension')
        tag, length = struct.unpack_from('<HH', data, offset)
        offset += 4
        if offset + length > size:
            raise ValueError('truncated replay extension payload')
        found |= tag == 7
        offset += length
    if offset != size:
        raise ValueError('invalid replay extension size')
    return found

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--program', type=Path, required=True)
    p.add_argument('--group', choices=('installed', 'synthetic'), default='installed')
    p.add_argument('--fixtures-root', type=Path, default=ROOT)
    p.add_argument('--game-root', type=Path, default=os.environ.get('EAWR_EAW_GAME_ROOT'))
    p.add_argument('--out', type=Path, required=True)
    b = p.add_mutually_exclusive_group()
    b.add_argument('--baseline', type=Path)
    b.add_argument('--record-baseline', type=Path)
    a = p.parse_args()
    baseline = a.baseline
    if not baseline and not a.record_baseline and a.group == 'installed':
        baseline = a.fixtures_root / 'tests/skirmish/fixtures/installed/baseline.json'
    if a.record_baseline and a.record_baseline.exists():
        p.error('baseline exists; choose a new path and review before replacing the tracked baseline')
    try:
        validate_inputs(a.program, a.game_root, a.group == 'installed')
        if baseline and not baseline.is_file():
            raise ValueError(f'baseline does not exist: {baseline}; use --record-baseline for explicit generation')
        receipt = compare(a.program, fixtures(a.group, a.fixtures_root), a.game_root, a.out,
                          json.loads(baseline.read_text()) if baseline else None, a.group)
    except (ValueError, OSError, RuntimeError) as error:
        p.error(str(error))
    if a.record_baseline:
        a.record_baseline.parent.mkdir(parents=True, exist_ok=True)
        a.record_baseline.write_text(json.dumps(receipt, indent=2) + '\n')
    print(f'PASS: {receipt["runs"]} {a.group} replay runs equal on 1/2/4/8 workers')

if __name__ == '__main__':
    main()
