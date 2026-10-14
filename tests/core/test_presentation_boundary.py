#!/usr/bin/env python3
"""Negative UI-07 cases include aliases, wrappers and member-function pointers."""
import importlib.util
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest

spec = importlib.util.spec_from_file_location("boundary", Path(__file__).parents[2] / "tools/check_presentation_boundary.py")
boundary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boundary)


class PresentationBoundary(unittest.TestCase):
    def test_galactic_owners(self):
        for name in ("GalacticSession", "GameSession"):
            for source in (f"using Alias = {name};", f"auto bypass = &{name}::submit;", '#include "wrapper.hpp"'):
                with self.subTest(name=name, source=source), tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary).resolve()
                    entry = root / "ui.cpp"
                    entry.write_text(source)
                    (root / "wrapper.hpp").write_text('#include "session.hpp"')
                    (root / "session.hpp").write_text(f"class {name} {{}};")
                    self.assertTrue(boundary.violations(root, [entry]))

    def test_live_game_header_is_safe(self):
        root = Path(__file__).resolve().parents[2]
        entry = root / "include/eawr/platform/live_game_session.hpp"
        self.assertEqual([], boundary.violations(root, [entry]))

    def test_cli_violation_is_a_failed_gate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for directory in boundary.ROOTS:
                (root / directory).mkdir(parents=True)
            entry = root / boundary.ROOTS[0] / "invalid.cpp"
            entry.write_text("void click(TacticalSession& session);", encoding="utf-8")
            result = subprocess.run([sys.executable, str(Path(boundary.__file__)), "--root", str(root)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("UI-07 live TacticalSession access", result.stdout)
            entry.write_text("void click(CommandSink& sink);", encoding="utf-8")
            result = subprocess.run([sys.executable, str(Path(boundary.__file__)), "--root", str(root)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    def test_live_session_paths(self):
        for source in (
            "void click(TacticalSession& s) { s.submit(command); }",
            "using Alias = eawr::sim::tactical::TacticalSession;",
            "auto bypass = &eawr::sim::tactical::TacticalSession::submit;",
            "namespace eawr::sim::tactical { class TacticalSession; }",
            '#include "wrapper.hpp"',
            '#define SESSION_HEADER "session.hpp"\n#include SESSION_HEADER',
        ):
            with self.subTest(source=source), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                entry = root / "ui.cpp"
                entry.write_text(source)
                (root / "wrapper.hpp").write_text('#include "session.hpp"')
                (root / "session.hpp").write_text("class TacticalSession {};")
                self.assertTrue(boundary.violations(root, [entry]))

    def test_command_sink_and_snapshot(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            entry = root / "ui.cpp"
            entry.write_text("// TacticalSession::submit is forbidden here\n"
                             "void click(CommandSink& sink) { sink.submit(command); }\n"
                             "const TacticalSnapshot& snapshot();")
            self.assertEqual([], boundary.violations(root, [entry]))


if __name__ == "__main__":
    unittest.main()
