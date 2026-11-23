"""Engine guard: the viewer's accepted fork commits (include/eawr/presentation/engine_signature.hpp) mirror the patched
engines of tools/engine_pins.json (PR #5791), and the viewer's startup hook stays in place."""
import json
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "include/eawr/presentation/engine_signature.hpp"
PINS = ROOT / "tools/engine_pins.json"


class EngineSignaturePinsTests(unittest.TestCase):
    def header_commits(self):
        match = re.search(r"patched_engine_commits\{([^}]*)\}", HEADER.read_text(encoding="utf-8"))
        self.assertIsNotNone(match, "patched_engine_commits not found")
        return sorted(re.findall(r'"([0-9a-f]+)"', match.group(1)))

    @unittest.skipUnless(PINS.is_file(), "tools/engine_pins.json arrives with #5791")
    def test_commits_match_the_pins_file(self):
        pins = json.loads(PINS.read_text(encoding="utf-8"))
        expected = sorted({entry["commit"][:9] for entry in pins["engines"].values()})
        self.assertEqual(self.header_commits(), expected)

    def test_pinned_player_hash_matches_the_default_engine(self):
        pins = json.loads(PINS.read_text(encoding="utf-8"))
        gui = pins["engines"][pins["default"]]["files"]["Godot_v4.7.2-stable_win64.exe"]
        self.assertIn(f'"{gui}"', HEADER.read_text(encoding="utf-8").replace("\n", ""))

    def test_pinned_linux_hash_matches_the_linux_engine_entry(self):
        pins = json.loads(PINS.read_text(encoding="utf-8"))
        linux = pins["engines"]["eawr-5b733a90f-linux-x64"]["files"]["godot.linuxbsd.editor.x86_64"]
        text = HEADER.read_text(encoding="utf-8")
        match = re.search(r"pinned_player_engine_sha256_linux_x64\s*=\s*\"([0-9a-f]{64})\"", text)
        self.assertIsNotNone(match, "pinned_player_engine_sha256_linux_x64 not found")
        self.assertEqual(match.group(1), linux)
        # The platform selector: Linux builds print the Linux hash, everything else the Windows one.
        self.assertRegex(text, r"#if defined\(__linux__\)\s+inline constexpr std::string_view pinned_player_engine_sha256 = "
                               r"pinned_player_engine_sha256_linux_x64;")

    def test_commits_are_short_hex_and_the_hook_is_wired(self):
        commits = self.header_commits()
        self.assertTrue(commits and all(len(commit) >= 7 for commit in commits))
        ready = (ROOT / "apps/viewer/src/viewer_host_ready.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "engine_guard_startup.hpp"', ready)
        body = ready[ready.index("void ViewerHost::_ready() try {"):]
        first = body.splitlines()[1].strip()
        self.assertTrue(first.startswith("if (!apply_engine_guard(*this)) return;"), "the guard runs first in _ready")


if __name__ == "__main__":
    unittest.main()
