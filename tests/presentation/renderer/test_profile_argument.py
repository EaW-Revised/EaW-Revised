"""Structural regression checks for --eawr-profile parsing in viewer modes."""

import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import mode_source  # noqa: E402


class ProfileArgumentContracts(unittest.TestCase):
    def test_shared_host_rejects_missing_and_unknown_values_before_dispatch(self):
        source = mode_source("viewer_host")
        branch_start = source.index('} else if (argument == "--eawr-profile") {')
        branch_end = source.index('} else if (argument == "--eawr-scene") {', branch_start)
        profile_branch = source[branch_start:branch_end]
        self.assertIn("missing value for --eawr-profile", profile_branch)
        self.assertIn("options_->profile_error", profile_branch)

        dispatch = source.index("// Interaction runs on the host tactical camera path")
        validation = source[source.index("if (options_->profile_error.empty()", branch_end):dispatch]
        for token in ('options_->profile != "eaw"', 'options_->profile != "foc"',
                      'options_->profile != "remake"', 'write_report("failed")',
                      "printerr(String(status_message_.c_str()))", "stop(2)"):
            self.assertIn(token, validation, token)

    def test_effect_parser_rejects_a_trailing_flag_and_unknown_values(self):
        source = mode_source("effect_mode")
        header = (ROOT / "apps/viewer/src/effect_mode.hpp").read_text(encoding="utf-8")
        parser_start = source.index("EffectMode::Options EffectMode::from_command_line()")
        parser_end = source.index("EffectMode::EffectMode(", parser_start)
        parser = source[parser_start:parser_end]
        self.assertIn("std::string profile_error;", header)
        self.assertIn("index < arguments.size(); ++index", parser)
        self.assertIn('argument == "--eawr-profile" && index + 1 >= arguments.size()', parser)
        self.assertIn("options.profile_error = \"missing value for --eawr-profile", parser)

        ready_start = source.index("bool EffectMode::ready(")
        ready_end = source.index("std::optional<int> EffectMode::process(", ready_start)
        ready = source[ready_start:ready_end]
        self.assertIn("if (!options.profile_error.empty())", ready)
        self.assertIn("UtilityFunctions::printerr(String(options.profile_error.c_str()))", ready)
        for token in ('options.profile != "eaw"', 'options.profile != "foc"',
                      'options.profile != "remake"',
                      '"--eawr-profile must be eaw, foc or remake"'):
            self.assertIn(token, ready, token)

    def test_map_mode_rejects_unknown_profiles(self):
        source = mode_source("map_mode")
        self.assertIn('state.profile != "eaw" && state.profile != "foc" && state.profile != "remake"', source)
        self.assertIn('return give_up("--eawr-profile must be eaw, foc or remake")', source)


if __name__ == "__main__":
    unittest.main()
