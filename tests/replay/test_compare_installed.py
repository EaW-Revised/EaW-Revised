"""Regression checks for replay grouping, evidence comparison, and content mounting."""
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('installed', ROOT / 'tools/replay/compare_installed.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

def replay(path):
    path.write_bytes(b'EAWRPLY\0' + struct.pack('<HH', 2, 104) + bytes(92))
    return path

def runner(mutate_worker=None):
    def run(command, **kwargs):
        worker = command[command.index('--workers') + 1]
        for kind, flag in tool.KINDS.items():
            if flag in command:
                Path(command[command.index(flag) + 1]).write_bytes(b'tick,sha256\n1,a\n' +
                    (b'changed\n' if worker == mutate_worker else b''))
        return subprocess.CompletedProcess(command, 0, b'')
    return run

def test_installed_mounts_content_for_every_worker(tmp_path):
    program = tmp_path / 'sim'; program.write_bytes(b'program')
    path = replay(tmp_path / 'bank.eawr-replay')
    commands = []
    def run(command, **kwargs):
        commands.append(command)
        return runner()(command, **kwargs)
    receipt = tool.compare(program, [path], tmp_path / 'game', tmp_path / 'out', runner=run)
    assert [int(c[c.index('--workers') + 1]) for c in commands] == [1, 2, 4, 8]
    assert all('--game-root' in c for c in commands)
    assert receipt['all_baseline_and_worker_bytes_equal']
    with pytest.raises(ValueError, match='require --game-root'):
        tool.compare(program, [path], None, tmp_path / 'missing', runner=run)

def test_worker_mismatch_writes_failure_receipt(tmp_path):
    program = tmp_path / 'sim'; program.write_bytes(b'program')
    with pytest.raises(ValueError, match='worker mismatch'):
        tool.compare(program, [replay(tmp_path / 'bank.eawr-replay')], tmp_path, tmp_path / 'out', runner=runner('8'))
    assert json.loads((tmp_path / 'out/receipt.json').read_text())['failures']

def test_baseline_detects_changed_sim_and_changed_fixture(tmp_path):
    program = tmp_path / 'sim'; program.write_bytes(b'program')
    path = replay(tmp_path / 'bank.eawr-replay')
    baseline = tool.compare(program, [path], tmp_path, tmp_path / 'base', runner=runner())
    baseline['rows'][0]['sha256']['hashes'] = '0' * 64
    with pytest.raises(ValueError, match='baseline mismatch'):
        tool.compare(program, [path], tmp_path, tmp_path / 'changed', baseline, runner=runner())
    path.write_bytes(path.read_bytes() + b'changed')
    with pytest.raises(ValueError, match='fixture missing or changed'):
        tool.compare(program, [path], tmp_path, tmp_path / 'fixture', baseline, runner=runner())

def test_synthetic_group_excludes_installed_and_mutations(tmp_path, monkeypatch):
    monkeypatch.setattr(subprocess, 'check_output', lambda *a, **k:
        'tests/replay/fixtures/tactical-v2.eawr-replay\ntests/skirmish/fixtures/installed/bank.eawr-replay\ntests/replay/fixtures/mutated-order.eawr-replay\n')
    assert [p.name for p in tool.fixtures('synthetic', tmp_path)] == ['tactical-v2.eawr-replay']
    with pytest.raises(ValueError, match='four native fixtures'):
        tool.fixtures('installed', tmp_path)

def test_match_identity_still_mounts_content_in_synthetic_group(tmp_path):
    header = bytearray(b'EAWRPLY\0' + struct.pack('<HH', 4, 144) + bytes(96))
    struct.pack_into('<I', header, 104, 1)
    header += struct.pack('<HH', 7, 32) + bytes(32)
    assert tool.carries_identity(header)
    struct.pack_into('<H', header, 110, 33)
    with pytest.raises(ValueError, match='truncated replay extension payload'):
        tool.carries_identity(header)

def test_input_validation_reports_missing_program_root_and_layout(tmp_path):
    program = tmp_path / 'sim'
    with pytest.raises(ValueError, match='--program'):
        tool.validate_inputs(program, None, True)
    program.write_bytes(b'program')
    with pytest.raises(ValueError, match='EAWR_EAW_GAME_ROOT'):
        tool.validate_inputs(program, None, True)
    with pytest.raises(ValueError, match='missing installed data'):
        tool.validate_inputs(program, tmp_path, True)
    for part in ('GameData/Data', 'GameData/StarWarsG.exe', 'corruption/Data', 'corruption/StarWarsG.exe'):
        target = tmp_path / part
        target.parent.mkdir(parents=True, exist_ok=True)
        target.mkdir() if part.endswith('/Data') else target.write_bytes(b'')
    tool.validate_inputs(program, tmp_path, True)


def test_wide_native_identity_scan():
    body = struct.pack('<HH', 7, 32) + bytes(32) + struct.pack('<HI', 15, 70000) + bytes(70000)
    header = b'EAWRPLY\0' + struct.pack('<HI', 6, 110 + len(body)) + bytes(92) + struct.pack('<I', 2) + body
    assert tool.carries_identity(header)
    with pytest.raises(ValueError, match='truncated replay extension payload'):
        tool.carries_identity(header[:-70000-4] + struct.pack('<I', 70001) + bytes(70000))
