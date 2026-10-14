"""Record the installed fixture commands through sim_headless's native session recorder."""
from __future__ import annotations
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
from compare_installed import compare, digest, INSTALLED, validate_inputs

# docs/traces.md: position and degree values are signed Q24 raw integers.
FIXED_SCALE = 1 << 24

def banked_releases(trace, fired):
    bank = {r['tick']: abs(int(r['value'])) for r in trace
            if r['object'] == 'source' and r['field'] == 'bank.roll'}
    return sum(bank.get(r[0], 0) > FIXED_SCALE * 5 for r in fired if r[2] == 'source')

def waypoint_ticks(spec, trace):
    positions = {}
    for r in trace:
        if r['object'] == 'source' and r['field'] in ('pos.x', 'pos.y'):
            positions.setdefault(int(r['tick']), {})[r['field']] = int(r['value'])
    route = [e['position'] for e in spec['events'] if e['action'] == 'move']
    reached, after = [], 0
    for x, y, _ in route:
        ticks = [t for t, point in positions.items() if t > after and
                 (point['pos.x'] - x * FIXED_SCALE) ** 2 +
                 (point['pos.y'] - y * FIXED_SCALE) ** 2 < (100 * FIXED_SCALE) ** 2]
        if not ticks:
            raise ValueError('queued route did not reach its waypoints in order')
        after = min(ticks)
        reached.append(after)
    return reached

def record(program, game_root, scenarios, out):
    out.mkdir(parents=True, exist_ok=True)
    coverage = {}
    for source in sorted(scenarios.glob('*.json')):
        if source.name == 'baseline.json':
            continue
        spec = json.loads(source.read_text())
        if spec.get('format') != 'eawr-scenario':
            continue
        stem = out / source.stem
        if stem.with_suffix('.eawr-replay').exists():
            raise ValueError('recording exists; choose a fresh output directory')
        command = [str(program), '--scenario', str(source), '--game-root', str(game_root),
                   '--trace-out', str(stem.with_suffix('.trace.csv')),
                   '--combat-out', str(stem.with_suffix('.combat.csv')),
                   '--replay-out', str(stem.with_suffix('.eawr-replay'))]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
        stem.with_suffix('.record.log').write_bytes(result.stdout)
        if result.returncode or b'warning:' in result.stdout:
            raise RuntimeError(f'{source.name}: exit {result.returncode}: {result.stdout.decode(errors="replace")}')
        header = json.loads(stem.with_suffix('.trace.json').read_text())
        if header.get('format') != 'eawr-installed-replay-audit' or header.get('fixed_fractional_bits') != 24:
            raise ValueError('installed recording requires the Q24 audit trace format')
        trace = list(csv.DictReader(stem.with_suffix('.trace.csv').open()))
        with stem.with_suffix('.combat.csv').open() as stream:
            combat = list(csv.reader(stream))
        fired = [r for r in combat if len(r) > 1 and r[1] == 'fired']
        if not fired:
            raise ValueError(f'{source.stem}: no native weapon releases')
        item = dict(command=command, scenario_sha256=digest(source),
                    replay_sha256=digest(stem.with_suffix('.eawr-replay')), weapon_releases=len(fired))
        if source.stem in ('banked-hardpoints', 'special-blast'):
            item['banked_weapon_releases'] = banked_releases(trace, fired)
            if not item['banked_weapon_releases']:
                raise ValueError(f'{source.stem}: no release during a substantial bank')
        if source.stem == 'special-blast':
            item['charged_releases'] = sum(int(r['value']) for r in trace
                                          if r['object'] == 'source' and r['field'] == 'charged.shots')
            if not item['charged_releases']:
                raise ValueError('BLAST did not release a charged shot')
        if source.stem == 'leech-shields':
            item['active_leech_ticks'] = sum(r['value'] == '1' for r in trace
                                            if r['object'] == 'source' and r['field'] == 'ability.LEECH_SHIELDS.active')
            if not item['active_leech_ticks']:
                raise ValueError('Leech Shields never became active')
        if source.stem == 'carrier-waypoints':
            item['waypoints_reached_ticks'] = waypoint_ticks(spec, trace)
            alive = [r for r in trace if r['object'] == 'fighter' and r['field'] == 'alive' and r['value'] == '1']
            item['fighter_first_alive_tick'] = int(alive[0]['tick']) if alive else None
            item['fighter_weapon_releases'] = sum(r[2].startswith('fighter.') for r in fired)
            if not alive or not item['fighter_weapon_releases']:
                raise ValueError('carrier fighters did not launch and engage')
        coverage[source.stem] = item
    if len(coverage) != 4:
        raise ValueError('record all four installed command scripts')
    (out / 'recording.json').write_text(json.dumps(dict(program_sha256=digest(program), fixtures=coverage), indent=2) + '\n')
    return coverage

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--program', type=Path, required=True)
    p.add_argument('--game-root', type=Path, default=os.environ.get('EAWR_EAW_GAME_ROOT'))
    p.add_argument('--scenarios', type=Path, default=INSTALLED)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--check', action='store_true', help='also replay the native recordings on 1/2/4/8 workers')
    a = p.parse_args()
    try:
        validate_inputs(a.program, a.game_root, required=True)
    except ValueError as error:
        p.error(str(error))
    if a.check:
        a.out.mkdir(parents=True, exist_ok=True)
        parent = a.out
        a.out = Path(tempfile.mkdtemp(prefix='run-', dir=parent))
        shutil.copy2(a.program, a.out / a.program.name)
        a.program = (a.out / a.program.name).resolve()
        (parent / 'latest.json').write_text(json.dumps({'output': str(a.out)}) + '\n')
    if a.check:
        synthetic = INSTALLED.parents[2] / 'replay/fixtures/tactical-v2.eawr-replay'
        result = subprocess.run([str(a.program), '--replay', str(synthetic),
                                 '--hash-out', str(a.out / 'synthetic.hashes.csv')],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        if result.returncode:
            raise ValueError('built-in synthetic replay failed without --game-root')
        original = json.loads((a.scenarios / 'banked-hardpoints.json').read_text())
        standard = dict(original, installed_replay_audit=False, duration_ticks=31, events=original['events'][:1])
        standard_path = a.out / 'ordinary-trace.json'
        standard_path.write_text(json.dumps(standard))
        result = subprocess.run([str(a.program), '--scenario', str(standard_path), '--game-root', str(a.game_root),
                                 '--trace-out', str(a.out / 'ordinary.trace.csv')],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        (a.out / 'ordinary-trace.log').write_bytes(result.stdout)
        header = json.loads((a.out / 'ordinary.trace.json').read_text())
        fields = {'alive', 'fwd.x', 'fwd.y', 'fwd.z', 'hull', 'pos.x', 'pos.y', 'pos.z', 'shield', 'shots', 'target'}
        with (a.out / 'ordinary.trace.csv').open() as stream:
            rows = list(csv.DictReader(stream))
        if result.returncode or header.get('format') != 'eawr-trace' or header.get('format_version') != 1 or any(r['field'] not in fields for r in rows):
            raise ValueError('ordinary scenario trace contract changed')
        for action, append in [('move', 'yes'), ('attack', True)]:
            original['events'][0]['action'] = action
            original['events'][0]['append'] = append
            invalid = a.out / ('invalid-append-' + action + '.json')
            invalid.write_text(json.dumps(original))
            result = subprocess.run([str(a.program), '--scenario', str(invalid), '--game-root', str(a.game_root),
                                     '--trace-out', str(a.out / 'invalid.trace.csv')],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            if result.returncode == 0 or b'append is a boolean for move events only' not in result.stdout:
                raise ValueError('invalid append accepted or failed for an unrelated reason')
    record(a.program, a.game_root, a.scenarios, a.out)
    if a.check:
        for replay in sorted(a.out.glob('*.eawr-replay')):
            result = subprocess.run([str(a.program), '--replay', str(replay),
                                     '--hash-out', str(a.out / 'missing-root.hashes.csv')],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            if result.returncode != 3 or b'--game-root' not in result.stdout or (a.out / 'missing-root.hashes.csv').exists():
                raise ValueError('installed replay did not fail closed without --game-root')
        baseline_path = a.scenarios / 'baseline.json'
        baseline = json.loads(baseline_path.read_text()) if baseline_path.exists() else None
        receipt = compare(a.program, sorted(a.out.glob('*.eawr-replay')), a.game_root, a.out / 'comparison', baseline)
        (a.out / 'baseline.json').write_text(json.dumps(receipt, indent=2) + '\n')
        for path in (a.out / 'comparison').glob('*.events.csv'):
            events = list(csv.DictReader(path.open()))
            rejected = [r for r in events if 'rejected' in r['event']]
            if rejected:
                raise ValueError(f'{path}: commands rejected: {rejected}')
        print('PASS: native installed recordings and 1/2/4/8 replay equality')

if __name__ == '__main__':
    main()
