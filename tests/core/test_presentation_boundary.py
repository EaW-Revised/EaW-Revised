#!/usr/bin/env python3
"""Negative UI-07 cases include aliases, wrappers and member-function pointers."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("boundary", Path(__file__).parents[2] / "tools/check_presentation_boundary.py")
boundary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boundary)


class PresentationBoundary(unittest.TestCase):
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
