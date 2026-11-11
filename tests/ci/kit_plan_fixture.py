"""A plan for tests that run package-on-host configurations against this checkout's HEAD."""
import json
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]
HOST_SCRIPT = 'tools/release/host_package.py'


def git(*args):
    return subprocess.run(['git', '-C', str(ROOT), *args], capture_output=True, text=True, check=True).stdout.strip()


def make_plan(directory, engine=None, launcher_sha256='0' * 64):
    """Write plan.json for HEAD of this checkout and return its path. HEAD must hold tools/release/host_package.py."""
    head = git('rev-parse', 'HEAD')
    plan = {'source': {'kit_sha': head, 'base_sha': head},
            'payload': [{'path': HOST_SCRIPT, 'git_blob': git('rev-parse', f'{head}:{HOST_SCRIPT}')}],
            'inputs': {'engine': {'label': 'test', 'files': dict(engine or {})},
                       'launcher': {'sha256': launcher_sha256}}}
    path = pathlib.Path(directory) / 'plan.json'
    path.write_text(json.dumps(plan, indent=2, sort_keys=True), encoding='utf-8')
    return path
