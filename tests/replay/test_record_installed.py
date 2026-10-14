"""Native trace audit regression: Q24 angles and positions, plus route order."""
import importlib.util
from pathlib import Path
import sys
import pytest
import os
import subprocess

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/replay'))
spec = importlib.util.spec_from_file_location('record_installed', ROOT / 'tools/replay/record_installed.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

def row(tick, field, raw):
    return dict(tick=str(tick), object='source', field=field, value=str(raw))

def test_banked_release_uses_documented_q24_degrees():
    # 12.5 degrees from a native Q24 trace is 209715200, far below a Q32 threshold.
    fired = [['20', 'fired', 'source', 'HP', 'target', '1']]
    assert tool.banked_releases([row(20, 'bank.roll', -209715200)], fired) == 1
    assert tool.banked_releases([row(20, 'bank.roll', 50331648)], fired) == 0
    assert tool.banked_releases([row(19, 'bank.roll', 209715200)], fired) == 0

def test_waypoints_use_q24_positions_and_require_route_order():
    goals = [[-2500, -2600, 0], [-2100, -2600, 0], [-2100, -3000, 0]]
    scenario = dict(events=[dict(action='move', position=p) for p in goals])
    trace = []
    for tick, (x, y, _) in zip([100, 200, 300], goals):
        trace += [row(tick, 'pos.x', x * 16777216), row(tick, 'pos.y', y * 16777216)]
    assert tool.waypoint_ticks(scenario, trace) == [100, 200, 300]
    for r in trace:
        if r['tick'] == '200': r['tick'] = '400'
    with pytest.raises(ValueError, match='waypoints in order'):
        tool.waypoint_ticks(scenario, trace)

def test_required_recording_fails_without_game_data(tmp_path):
    program = tmp_path / 'sim'; program.write_bytes(b'program')
    env = dict(os.environ); env.pop('EAWR_EAW_GAME_ROOT', None)
    result = subprocess.run([sys.executable, str(ROOT / 'tools/replay/record_installed.py'),
                             '--program', str(program), '--out', str(tmp_path / 'out'), '--check'],
                            env=env, capture_output=True, text=True)
    assert result.returncode != 0
    assert '--game-root or EAWR_EAW_GAME_ROOT' in result.stderr
    assert 'SKIPPED' not in result.stdout + result.stderr
    assert not (tmp_path / 'out').exists()
