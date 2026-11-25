import pathlib
import json
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as directory:
    output = pathlib.Path(directory) / "output.jsonl"
    subprocess.run([sys.argv[1], "console", "--script", str(root / "commands.txt"),
                    "--ticks", "3", "--out", str(output)], check=True)
    assert output.read_bytes() == (root / "transcripts/core.jsonl").read_bytes()
    script = pathlib.Path(directory) / "source.txt"
    script.write_text("help\n", encoding="utf-8")
    duplicate = subprocess.run([sys.argv[1], "console", "--script", str(script), "--out", str(script)],
                               capture_output=True)
    assert duplicate.returncode != 0 and script.read_text(encoding="utf-8") == "help\n"
    repeated = subprocess.run([sys.argv[1], "console", "--script", str(script), "--out", str(output),
                               "--ticks", "1", "--ticks", "2"], capture_output=True)
    assert repeated.returncode != 0
    script.write_text("version\ncheats\nlist invalid\n", encoding="utf-8")
    subprocess.run([sys.argv[1], "console", "--script", str(script), "--out", str(output)], check=True)
    rows = [json.loads(line) for line in output.read_text(encoding="utf-8").splitlines()]
    assert rows[0]["output"] and rows[1]["output_keys"] == ["TEXT_EAWR_CONSOLE_NO_ACTIVE_GAME"]
    assert rows[1]["output"] == ""
    assert rows[2]["output_keys"] == ["TEXT_EAWR_CONSOLE_ERROR_ARGUMENT"]
