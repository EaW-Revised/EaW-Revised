#!/usr/bin/env python3
"""Cut a labelled owner kit using the Windows build pool."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile
import tarfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/common'))
import hidden_console
hidden_console.install()
sys.path.insert(0, str(ROOT / 'tools/release'))
sys.path.insert(0, str(ROOT / 'tools/offload'))
import build_hosts
import host_package
import kit_engine
import kit_manifest
import launcher_artifact
import release_version
import rig_transfer

TARGETS = ('sim_headless sim_bench path_bench asset_scan asset_validate xml_scan scene_scan sky_scan unit_scan '
           'script_smoke hud_movie_prepare eawr_launcher eawr_mods_start eawr_mods_service eawr_loadcheck').split()
LABEL = re.compile(r'^[A-Za-z0-9._-]{1,32}$')


def git(*args, env=None, data=None):
    return subprocess.run(['git', '-C', str(ROOT), *args], input=data, capture_output=True,
                          env=env, check=True).stdout.decode().strip()


def kit_commit(base, label, pin, extras):
    files = {'tools/release/kit-label.txt': (label + '\n').encode(),
             'config/launcher-pin.json': pin.read_bytes()}
    for source, destination in extras:
        path = PurePosixPath(destination)
        if path.is_absolute() or '..' in path.parts or not path.parts or '\\' in destination or ':' in destination:
            raise ValueError(f'unsafe extra path: {destination}')
        if destination in files:
            raise ValueError(f'duplicate extra path: {destination}')
        files[destination] = source.read_bytes()
    with tempfile.TemporaryDirectory(prefix='eawr-kit-index-') as temp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(temp) / 'index'))
        git('read-tree', base, env=env)
        for name, content in files.items():
                # --path applies the repository's line-ending rules, so a CRLF working-tree file is stored as the LF blob a commit has.
            blob = git('hash-object', '-w', '--path', name, '--stdin', data=content)
            git('update-index', '--add', '--cacheinfo', f'100644,{blob},{name}', env=env)
        tree = git('write-tree', env=env)
    env = dict(os.environ)
    env.setdefault('GIT_AUTHOR_NAME', 'EAWR Kit Builder')
    env.setdefault('GIT_AUTHOR_EMAIL', 'noreply@example.invalid')
    env.setdefault('GIT_COMMITTER_NAME', env['GIT_AUTHOR_NAME'])
    env.setdefault('GIT_COMMITTER_EMAIL', env['GIT_AUTHOR_EMAIL'])
    return git('commit-tree', tree, '-p', base, '-m', f'Kit {label}: label and launcher pin', env=env)


def engine_identity(base):
    """The pinned engine label and file hashes of the base commit."""
    return kit_engine._default_files(json.loads(git('show', base + ':tools/engine_pins.json')))


def verify_engine(source, base):
    pins = json.loads(git('show', base + ':tools/engine_pins.json'))
    label, files = kit_engine._default_files(pins)
    for name, digest in files.items():
        path = source / name
        if not path.is_file():
            raise FileNotFoundError(f'missing pinned engine input: {path}')
        identity = kit_engine.guard.identify(path, pins)
        if not identity['patched'] or identity['label'] != label or identity['sha256'] != digest:
            raise ValueError(f'engine input does not match the cut pin: {name}')


def parse_files(path):
    raw = path.read_bytes()
    if raw.startswith(b'\xef\xbf\xbd\xef\xbf\xbd'):
        # A previously fetched UTF-16 log may have had its BOM damaged by an older client.
        data = raw.decode('utf-8', 'replace').replace('\x00', '')
    elif raw.startswith((b'\xff\xfe', b'\xfe\xff')):
        data = raw.decode('utf-16')
    elif len(raw) >= 4 and raw[1::2].count(0) > len(raw) // 4:
        data = raw.decode('utf-16le')
    else:
        data = raw.decode('utf-8-sig')
    return re.findall(r'EAWR-RELEASE FILE (\S.*?) (\d+) ([0-9a-f]{64})\s*$', data, re.M)


def parse_receipt(path):
    """The one-line host receipt a pool host prints (host_package.py receipt) in the package log."""
    raw = path.read_bytes()
    if raw.startswith((b'\xff\xfe', b'\xfe\xff')):
        data = raw.decode('utf-16')
    elif len(raw) >= 4 and raw[1::2].count(0) > len(raw) // 4:
        data = raw.decode('utf-16le')
    else:
        data = raw.decode('utf-8-sig', 'replace')
    lines = [line[len(host_package.RECEIPT_LINE):] for line in data.splitlines() if line.startswith(host_package.RECEIPT_LINE)]
    if len(lines) != 1:
        raise ValueError(f'the package log must hold exactly one host receipt line, found {len(lines)}')
    return json.loads(lines[0])


def extract_cross_player(bundle, fetched):
    fetched.mkdir()
    with tarfile.open(bundle, 'r:gz') as archive:
        players = [m for m in archive.getmembers() if m.isfile() and
                   re.fullmatch(r'package/[^/]+-player\.zip', m.name)]
        sums = [m for m in archive.getmembers() if m.isfile() and m.name == 'package/SHA256SUMS.txt']
        if len(players) != 1 or len(sums) != 1:
            raise ValueError('cross package bundle must contain one player zip and SHA256SUMS.txt')
        target = fetched / PurePosixPath(players[0].name).name
        with archive.extractfile(players[0]) as src, target.open('xb') as dst:
            for block in iter(lambda: src.read(1024 * 1024), b''):
                dst.write(block)
        expected = archive.extractfile(sums[0]).read().decode('utf-8')
        found = [m for m in archive.getmembers() if m.isfile() and m.name == 'host-receipt.json']
        if len(found) != 1:
            raise ValueError('cross package bundle must contain host-receipt.json')
        receipt = json.loads(archive.extractfile(found[0]).read().decode('utf-8'))
    with target.open('rb') as stream:
        actual = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            actual.update(block)
    digest = actual.hexdigest()
    if f'{digest}  {target.name}' not in expected.splitlines():
        target.unlink(missing_ok=True)
        raise ValueError('player zip does not match package SHA256SUMS.txt')
    return target, digest, receipt


def record_host(manifest, path, host_name, source, player, digest, receipt, attested=True, assets=None):
    """Write the host section: the player archive as it arrived, hashed here. `attested` is False when the pool reported no
    host name and the name is only the one the cut asked for. `receipt` is the host's own record (host_package.py): the tree
    check, the input hashes, the outputs it hashed and its toolchain; verify compares it with the plan and the outputs."""
    output = {'name': player.name, 'size': player.stat().st_size, 'sha256': digest}
    if output not in receipt.get('outputs', []):
        raise ValueError('the host receipt does not list the player archive that arrived')
    outputs = [output]
    if assets:  # a release package: every file the host wrote, not only the player archive
        outputs = [{'name': a.name, 'size': a.stat().st_size, 'sha256': kit_manifest.sha256_file(a)} for a in assets]
        if output not in outputs:
            raise ValueError('the player archive is not among the fetched assets')
    kit_manifest.add_host(manifest, {'host': host_name, 'host_attested': attested, 'received_from': source,
                                     'outputs': outputs, 'receipt': receipt})
    problems = kit_manifest.receipt_problems(manifest, manifest['host'])
    if problems:
        raise ValueError('host receipt refused: ' + '; '.join(problems))
    kit_manifest.write(path, manifest)


def base_release_version(base, extras):
    """The release version the kit commit will carry: an --extra config/release-version.json, else the base's own; None when
    neither exists (a game head older than the version file, cut with a plain --label)."""
    for source, destination in extras:
        if destination == 'config/release-version.json':
            return release_version.read(source)
    try:
        data = subprocess.run(['git', '-C', str(ROOT), 'show', f'{base}:config/release-version.json'], capture_output=True, check=True).stdout
    except subprocess.CalledProcessError:
        return None
    with tempfile.TemporaryDirectory() as scratch:
        path = Path(scratch) / 'release-version.json'
        path.write_bytes(data)
        return release_version.read(path)


def package_version(label, commit, version, release):
    """The version the host packager is given. A release is named like every release so far, v<release version>: archives
    eaw-revised-v0.3.3-..., tag v0.3.3, stable channel, no kit suffix. A kit carries its label and its kit commit."""
    if release:
        return 'v' + version
    return release_version.kit_version(label, commit, version) if version else f'{label}-{commit[:8]}'


def resolve_label(label, suffix, version):
    """One kit label: version + suffix, or a given label that starts with the release version."""
    if (label is None) == (suffix is None):
        raise ValueError('give exactly one of --label and --suffix')
    if version is None:
        if suffix is not None:
            raise ValueError('--suffix needs config/release-version.json in the base (or as an --extra); use --label for older game heads')
        if not LABEL.fullmatch(label):
            raise ValueError('invalid kit label')
        return label
    if suffix is not None:
        return release_version.kit_label(suffix, version)
    release_version.check_label(label, version)
    return label


def subprocess_blob(commit, path, directory):
    """A file of a commit written to `directory`, for tools that read paths."""
    target = Path(directory) / PurePosixPath(path).name
    target.write_bytes(subprocess.run(['git', '-C', str(ROOT), 'show', f'{commit}:{path}'], check=True, capture_output=True).stdout)
    return target


def layer_zip(layer, reviewed_manifest, target, date_time):
    """Zip the reviewed Revised layer reproducibly after checking it against the reviewed manifest. The package host unpacks
    it and the packager compares it again with --public."""
    import finish_kit
    state = finish_kit.check_layer(layer, reviewed_manifest, {'inputs': {'revised_layer': {'manifest_sha256': ''}}}, False)
    if state['problems']:
        raise ValueError('layer: ' + '; '.join(state['problems']))
    layer = Path(layer)
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(p for p in layer.rglob('*') if p.is_file()):
            info = zipfile.ZipInfo(path.relative_to(layer).as_posix(), date_time=date_time)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, path.read_bytes())
    return target


def extract_cross_assets(bundle, fetched):
    """Every file of a cross package bundle, each checked against the bundle's SHA256SUMS.txt; also the host receipt."""
    fetched.mkdir()
    with tarfile.open(bundle, 'r:gz') as archive:
        members = {m.name: m for m in archive.getmembers() if m.isfile()}
        if 'package/SHA256SUMS.txt' not in members or 'host-receipt.json' not in members:
            raise ValueError('cross package bundle must contain package/SHA256SUMS.txt and host-receipt.json')
        listed = {}
        for line in archive.extractfile(members['package/SHA256SUMS.txt']).read().decode('utf-8').splitlines():
            value, _, name = line.partition('  ')
            listed[name] = value
        paths = []
        for name, member in sorted(members.items()):
            if not re.fullmatch(r'package/[^/]+', name) or name == 'package/SHA256SUMS.txt':
                continue
            target = fetched / PurePosixPath(name).name
            digest = hashlib.sha256()
            with archive.extractfile(member) as source, target.open('xb') as sink:
                for block in iter(lambda: source.read(1024 * 1024), b''):
                    digest.update(block)
                    sink.write(block)
            if listed.get(target.name) != digest.hexdigest():
                target.unlink(missing_ok=True)
                raise ValueError(f'{target.name} does not match package SHA256SUMS.txt')
            paths.append(target)
        sums_copy = fetched / 'SHA256SUMS.txt'
        with archive.extractfile(members['package/SHA256SUMS.txt']) as source, sums_copy.open('xb') as sink:
            sink.write(source.read())
        receipt = json.loads(archive.extractfile(members['host-receipt.json']).read().decode('utf-8'))
    return paths + [sums_copy], receipt


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sha')
    parser.add_argument('--release-tag', metavar='vX.Y.Z',
                        help='cut the release package: the signed annotated tag must point at --sha (default: the tag\'s commit), '
                             'the label is the release version, and --layer is the reviewed Revised layer')
    parser.add_argument('--layer', type=Path, help='release only: the reviewed Revised layer directory')
    parser.add_argument('--label', help='the full kit label; it must start with the release version (config/release-version.json)')
    parser.add_argument('--suffix', help='the part after the release version, e.g. c or c-clang; the label is version + suffix')
    parser.add_argument('--pin', type=Path)
    parser.add_argument('--engine-dir', type=Path, required=True)
    parser.add_argument('--launcher-artifact', type=Path, required=True)
    parser.add_argument('--changes', type=Path, required=True)
    parser.add_argument('--extra', action='append', default=[], metavar='FILE=REPO_PATH')
    parser.add_argument('--host', action='append', help='pool host or any; repeat to restrict candidates')
    parser.add_argument('--cross-host', help='use this Linux cross-build host instead of the Windows pool')
    parser.add_argument('--wine', help='Wine executable relative to the cross host base')
    parser.add_argument('--wine-lib-dir', help='optional Wine library directory relative to the cross host base')
    parser.add_argument('--allow-dirty-tooling', action='store_true',
                        help='cut with uncommitted changes in the orchestrating scripts; the manifest records it')
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args(argv)
    scratch = tempfile.TemporaryDirectory(prefix='eawr-release-')  # holds the tagged commit's pin and manifest of a release cut
    try:
        args.work = args.work.resolve()
        if args.cross_host and (args.host or not args.wine):
            raise ValueError('--cross-host needs --wine and cannot be combined with --host')
        if not args.cross_host and (args.wine or args.wine_lib_dir):
            raise ValueError('--wine and --wine-lib-dir require --cross-host')
        if not args.changes.is_file():
            raise FileNotFoundError(f'changes file missing: {args.changes}')
        release = None
        if args.release_tag:
            for flag, given in (('--label', args.label), ('--suffix', args.suffix), ('--pin', args.pin), ('--extra', args.extra),
                                ('--allow-dirty-tooling', args.allow_dirty_tooling)):
                if given:
                    raise ValueError(f'a release package takes exactly the tagged commit: {flag} is not allowed with --release-tag')
            if args.layer is None:
                raise ValueError('--release-tag needs --layer, the reviewed Revised layer directory')
            args.sha = args.sha or args.release_tag + '^{commit}'
        else:
            if args.layer is not None:
                raise ValueError('--layer is for --release-tag (a kit gets its layer from finish_kit.py)')
            args.sha = args.sha or 'HEAD'
            args.pin = args.pin or ROOT / 'config/launcher-pin.json'
        base = git('rev-parse', '--verify', args.sha + '^{commit}')
        if args.release_tag:
            version = base_release_version(base, [])
            if version is None:
                raise ValueError('the tagged commit has no config/release-version.json')
            release_version.check_tag(args.release_tag, version)
            release = kit_manifest.tag_facts(ROOT, args.release_tag)
            if release['tag_commit'] != base:
                raise ValueError(f"{args.release_tag} points at {release['tag_commit'][:12]}, not at --sha {base[:12]}")
            release.update(version=version, public=True)
            args.label = version
            args.pin = Path(scratch.name) / 'launcher-pin.json'  # the tagged commit's own pin: the overlay changes nothing
            args.pin.write_bytes(subprocess.run(['git', '-C', str(ROOT), 'show', f'{base}:config/launcher-pin.json'],
                                                check=True, capture_output=True).stdout)
        pin = launcher_artifact.read_pin(args.pin)
        launcher_artifact.verify_local(args.launcher_artifact, pin)
        verify_engine(args.engine_dir, base)
        extras = []
        for entry in args.extra:
            source, sep, destination = entry.partition('=')
            if not sep or not Path(source).is_file():
                raise ValueError(f'bad extra: {entry}')
            extras.append((Path(source), destination))
        if not release:
            version = base_release_version(base, extras)
            args.label = resolve_label(args.label, args.suffix, version)
        args.work.mkdir(parents=True, exist_ok=True)
        meta_path = args.work / 'cut.json'
        if meta_path.exists():
            raise ValueError(f'cut already exists: {meta_path}')
        commit = kit_commit(base, args.label, args.pin, extras)
        config = {'version': package_version(args.label, commit, version, bool(release)),
                  'allow_missing_revised_layer': True,
                  'local_inputs': {'engine_dir': str(args.engine_dir.resolve()),
                                   'launcher_artifact': str(args.launcher_artifact.resolve())}}
        if release:
            # The reviewed layer goes in as a reproducible archive; the host packages with --public and the symbols kept.
            when = time.gmtime(max(int(git('show', '-s', '--format=%ct', base)), 315532800))
            stamp = (when.tm_year, when.tm_mon, when.tm_mday, when.tm_hour, when.tm_min, when.tm_sec - when.tm_sec % 2)
            archive = layer_zip(args.layer, subprocess_blob(base, 'tools/release/revised-layer-manifest.json', scratch.name),
                                args.work / 'revised-layer.zip', stamp)
            config.update(public=True, allow_missing_revised_layer=False)
            config['local_inputs']['revised_layer'] = str(archive.resolve())
        if args.cross_host:
            config['wine'] = args.wine
            if args.wine_lib_dir:
                config['wine_lib_dir'] = args.wine_lib_dir
        operator = kit_manifest.orchestrator(ROOT)
        if not operator['clean'] and not args.allow_dirty_tooling:
            raise ValueError('the orchestrating scripts in this checkout have uncommitted changes (commit them, or pass '
                             '--allow-dirty-tooling; the manifest then records orchestrator.clean false)')
        route = {'kind': 'linux-xbuild' if args.cross_host else 'windows-pool',
                 'hosts': [args.cross_host] if args.cross_host else (args.host or ['any'])}
        plan = kit_manifest.build_plan(
            ROOT, base=base, kit=commit, label=args.label, kit_version=config['version'],
            overlay=['tools/release/kit-label.txt', 'config/launcher-pin.json', *[p for _, p in extras]],
            launcher_pin=pin, engine=engine_identity(base),
            changes_sha256=kit_manifest.sha256_file(args.changes), route=route, operator=operator)
        if release:
            plan['release'] = release
        manifest = kit_manifest.new_manifest(plan, 'release' if release else 'kit')
        if release and (refused := kit_manifest._release_problems(manifest, False)):
            raise ValueError('release plan refused: ' + '; '.join(refused))
        # The hosts verify their tree and every staged input against this file, and its hash is a literal in the host script.
        plan_path = args.work / 'plan.json'
        plan_path.write_text(json.dumps(plan, indent=2, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
        config['plan'] = str(plan_path)
        cfg = args.work / 'pkg.json'
        cfg.write_text(json.dumps(config, indent=2), encoding='utf-8')
        manifest_path = args.work / kit_manifest.MANIFEST_NAME
        kit_manifest.write(manifest_path, manifest)
        meta = {'base_sha': base, 'kit_sha': commit, 'label': args.label, 'pin': str(args.pin.resolve()),
                'changes': str(args.changes.resolve()), 'extras': [p for _, p in extras], 'ok': False,
                'manifest': str(manifest_path), 'plan_sha256': manifest['plan_sha256']}
        meta_path.write_text(json.dumps(meta, indent=2), encoding='utf-8')
        build = args.work / 'build'
        if args.cross_host:
            cmd = [sys.executable, str(ROOT / 'tools/offload/linux_cross_build.py'), '--ref', commit, '--kit',
                   '--no-test', '--host', args.cross_host, '--package-on-host', str(cfg), '--out', str(build)]
        else:
            cmd = [sys.executable, str(ROOT / 'tools/offload/windows_build.py'), '--ref', commit, '--viewer',
                   '--viewer-relay-only', '--preset', 'windows-clang-ninja-relay', '--require-symbols', '--no-test',
                   '--no-probe-build', '--no-werror-probe', '--package-on-host', str(cfg), '--out', str(build)]
            for target in TARGETS:
                cmd += ['--target', target]
            for host in args.host or ['any']:
                cmd += ['--host', host]
        cmd += ['--lock-timeout', '28800']
        with (args.work / 'client.log').open('wb') as log:
            result = subprocess.run(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f'pool build failed ({result.returncode}); see {args.work / "client.log"}')
        summary = json.loads((build / 'summary.json').read_text(encoding='utf-8-sig'))
        if args.cross_host:
            if release:
                assets, receipt = extract_cross_assets(build / 'package.tar.gz', args.work / 'fetched')
                (target,) = [a for a in assets if a.name.endswith('-player.zip')]
                digest = kit_manifest.sha256_file(target)
            else:
                assets = None
                target, digest, receipt = extract_cross_player(build / 'package.tar.gz', args.work / 'fetched')
            record_host(manifest, manifest_path, summary.get('hostName') or args.cross_host, 'package.tar.gz', target, digest,
                        receipt, attested=bool(summary.get('hostName')), assets=assets)
            meta.update(ok=True, player_zip=str(target.resolve()), player_sha256=digest, host=summary.get('hostName') or args.cross_host)
            meta_path.write_text(json.dumps(meta, indent=2), encoding='utf-8')
            print(f'Cut {commit} on {meta["host"]}; run finish_kit.py --work {args.work}')
            return 0
        hosts = [h for h in build_hosts.load() if h.name == summary.get('hostName')]
        if len(hosts) != 1:
            raise ValueError('selected host missing or ambiguous in pool configuration')
        listed = parse_files(build / 'release-package.log')
        players = [item for item in listed if item[0].endswith('-player.zip')]
        if len(players) != 1:
            raise ValueError('package log must name exactly one player zip')
        fetched = args.work / 'fetched'
        fetched.mkdir()
        # A kit takes the player archive; a release takes every file the host wrote (tools, viewer, sums, update manifest).
        wanted = listed if release else players
        downloaded = {}
        for remote, size, want in wanted:
            local = fetched / remote.replace('\\', '/').rsplit('/', 1)[-1]
            if local.exists() or local.is_symlink():
                raise ValueError(f'fetched archive already exists: {local}')
            options = argparse.Namespace(source=remote, destination=str(local), download=True, recurse=False)
            command, cwd = rig_transfer.build_scp(hosts[0], options)
            subprocess.run(command, cwd=cwd, timeout=rig_transfer.transfer_timeout(int(size)), check=True)
            if kit_manifest.sha256_file(local) != want or local.stat().st_size != int(size):
                raise ValueError(f'downloaded {local.name} does not match its receipt')
            downloaded[remote] = local
        target = downloaded[players[0][0]]
        digest = players[0][2]
        record_host(manifest, manifest_path, hosts[0].name, 'release-package.log', target, digest,
                    parse_receipt(build / 'release-package.log'), assets=list(downloaded.values()) if release else None)
        meta.update(ok=True, player_zip=str(target.resolve()), player_sha256=digest, host=hosts[0].name)
        meta_path.write_text(json.dumps(meta, indent=2), encoding='utf-8')
        print(f'Cut {commit} on {hosts[0].name}; run finish_kit.py --work {args.work}')
        return 0
    except (OSError, ValueError, RuntimeError, kit_engine.KitEngineError, subprocess.SubprocessError) as error:
        print(f'cut-kit: {error}', file=sys.stderr)
        return 1
    finally:
        scratch.cleanup()


if __name__ == '__main__':
    sys.exit(main())
