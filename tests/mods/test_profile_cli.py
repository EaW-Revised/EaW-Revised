"""Exercise the real sim_headless entry point with synthetic mod directories only."""
import json
import pathlib
import subprocess
import sys
import tempfile


def main():
    with tempfile.TemporaryDirectory(prefix="eawr-profile-cli-") as directory:
        root = pathlib.Path(directory)
        layers = [root / "leaf", root / "parent"]
        data_roots = [layer / "Data" for layer in layers] + [root / "corruption/Data", root / "GameData/Data"]
        for data in data_roots:
            data.mkdir(parents=True)
            # A manifest must declare at least one file; missing archives are valid for an otherwise loose tree.
            (data / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
        profile = root / "profile.json"
        profile.write_text(json.dumps({"format": "eawr-mod-profile", "version": 1,
            "id": "synthetic", "name": "Synthetic", "layers": [
                {"key": f"mods:{layer.name}", "root": str(layer)} for layer in layers]}), encoding="utf-8")
        results = []
        for arguments in (["--eawr-mod-profile", str(profile)],
                          ["--eawr-mod-root", ";".join(map(str, layers))],
                          ["--mod-root", ";".join(map(str, layers))]):
            # Observe the specs passed to the actual successful VFS mount, before the catalog load fails.
            run = subprocess.run([sys.argv[1], *arguments, "--skirmish", "m2", "--game-root", str(root),
                "--census-out", str(root / "census.json"), "--eawr-mod-mount-report"],
                text=True, capture_output=True, check=False)
            assert run.returncode != 0
            assert "expected --replay" not in run.stderr, run.stderr
            prefix = "sim_headless: mounted roots="
            reports = [line.removeprefix(prefix) for line in run.stderr.splitlines() if line.startswith(prefix)]
            assert reports, run.stderr
            roots = json.loads(reports[0])
            assert roots == [data.as_posix() for data in data_roots], roots
            downstream = [line for line in run.stderr.splitlines()
                          if not line.startswith(("EAWR mods:", prefix))]
            assert downstream, run.stderr
            results.append((roots, downstream, run.returncode))
        assert results[0] == results[1] == results[2], results
        # A dropped profile root pair would mount only the two retail layers; prove this observer sees it.
        vanilla = subprocess.run([sys.argv[1], "--skirmish", "m2", "--game-root", str(root),
            "--census-out", str(root / "census.json"), "--eawr-mod-mount-report"],
            text=True, capture_output=True, check=False)
        vanilla_reports = [line.removeprefix(prefix) for line in vanilla.stderr.splitlines() if line.startswith(prefix)]
        assert vanilla_reports, vanilla.stderr
        vanilla_roots = json.loads(vanilla_reports[0])
        assert vanilla_roots == [data.as_posix() for data in data_roots[2:]]
        quiet = subprocess.run([sys.argv[1], "--mod-root", str(layers[0]), "--invalid-mode"],
            text=True, capture_output=True, check=False)
        assert "EAWR mods:" not in quiet.stderr and "mounted roots=" not in quiet.stderr, quiet.stderr


if __name__ == "__main__":
    main()
