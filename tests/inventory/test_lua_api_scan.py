"""The Lua API coverage scan (tools/lua_api_scan): C++ binding reader, collection, matrix and baseline.

The scan itself runs on a small synthetic fixture (tests/inventory/fixtures/lua_api_scan/stock) against the real
host binding tables in src/, so a host row that stops matching its source is caught here. The assertions about the
real tables never name a gap that a later fix is meant to close.
"""
from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.lua_api_scan import __main__ as cli  # noqa: E402
from tools.lua_api_scan import bindings, cxx, extract, scan  # noqa: E402

FIXTURE = ROOT / "tests/inventory/fixtures/lua_api_scan/stock"
CONFIG = ROOT / "tools/lua_api_scan/scan_config.json"

SNIPPET = r'''
#include <x>
namespace n {
void other() { scripts.register_binding("NotThisOne", nullptr); }
void host(Scheduler& scripts) {
    const auto add = [&](std::string_view name, Binding b) { return scripts.register_binding(name, std::move(b)); };
    const auto method = [&](std::string_view name, Binding b) { return scripts.register_method(kind, name, std::move(b)); };
    add("Plain_Global", f);
    method("Plain_Method", f);
    scripts.register_binding("Dotted.Name", f); // register_binding("In_Comment", f)
    scripts.register_method(handle_game_object, "Direct_Method", f);
    for (const auto& [name, direction] : std::array<std::pair<std::string_view, std::string_view>, 2>{{{"Loop_A", "in"}, {"Loop_B", "out"}}}) {
        scripts.register_binding(std::string(name), f);
    }
    const char* text = "register_binding(\"In_String\", f)";
}
std::vector<std::string_view> implemented{"Listed_One", "Listed_Two"};
}
'''


class CxxReaderTests(unittest.TestCase):
    def test_registrations_in_a_function_body_only(self):
        found = cxx.read_registrations(SNIPPET, "x.cpp", {"add": "global", "method": "member", "register_binding": "global",
                                                           "register_method": "member"}, "host")
        names = {(r.name, r.kind) for r in found}
        self.assertIn(("Plain_Global", "global"), names)
        self.assertIn(("Plain_Method", "member"), names)
        self.assertIn(("Dotted.Name", "global"), names)
        self.assertIn(("Direct_Method", "member"), names)
        self.assertIn(("Loop_A", "global"), names)
        self.assertIn(("Loop_B", "global"), names)
        self.assertNotIn("NotThisOne", {r.name for r in found})
        # kind-first lambdas (name is the second argument) are read with member@2
        kind_first = "void h() { const auto m = [&](int k, std::string_view n, B b) {}; m(kind, \"Second_Arg\", f); }"
        self.assertEqual([r.name for r in cxx.read_registrations(kind_first, "y.cpp", {"m": "member@2"}, "h")], ["Second_Arg"])
        self.assertNotIn("In_Comment", {r.name for r in found})
        self.assertNotIn("In_String", {r.name for r in found})

    def test_method_handle_kind_and_name_lists(self):
        found = cxx.read_registrations(SNIPPET, "x.cpp", {"register_method": "member"}, "host")
        self.assertEqual([(r.name, r.handle) for r in found], [("Direct_Method", "handle_game_object")])
        self.assertEqual(cxx.read_name_list(SNIPPET, "implemented"), ["Listed_One", "Listed_Two"])
        self.assertIn("register_binding", cxx.called_identifiers(SNIPPET, "other"))

    def test_loop_over_a_braced_name_list_registers_each_name(self):
        text = r'''void f(S& s) {
            const auto add = [&](std::string_view n, B b) { return s.register_binding(n, b); };
            for (const char* name : {"Sink_A", "Sink_B"}) add(name, noop());
            for (const char* name : {"Sink_C"}) { add(name, noop()); }
            add("Plain", f);
        }'''
        names = {r.name for r in cxx.read_registrations(text, "z.cpp", {"add": "global"}, "f")}
        self.assertEqual(names, {"Sink_A", "Sink_B", "Sink_C", "Plain"})

    def test_missing_function_is_reported(self):
        with self.assertRaises(ValueError):
            cxx.read_registrations(SNIPPET, "x.cpp", {"add": "global"}, "no_such_function")


class RealHostTableTests(unittest.TestCase):
    def test_every_galactic_host_row_still_matches_its_source(self):
        config = scan.load_config(CONFIG)
        read = bindings.read_all(ROOT, config["hosts"], config["follow"])
        for host in config["hosts"]:
            if host.get("reader") in {"pending", "parked"}:
                self.assertFalse(host["bindings"], host["id"])
                continue
            self.assertIn(host["id"], read)
            self.assertEqual(read[host["id"]].problems, [], host["id"])
            self.assertGreater(len(read[host["id"]].globals) + len(read[host["id"]].members), 3, host["id"])
        story = read["galactic_story"]
        self.assertTrue(story.knows_global("Get_Game_Mode"))
        self.assertFalse(story.stubs & set(story.globals), "a name cannot be both bound and stubbed")
        self.assertTrue(story.infrastructure, "scheduler infrastructure names are read from the host table")

    def test_config_profiles_resolve_to_layers(self):
        from tools.lua_api_scan import collect
        rows = collect.load_profiles(ROOT, scan.load_config(CONFIG))
        ids = {row["id"] for row in rows}
        self.assertTrue({"remake-chain", "bfc", "raw", "aaw", "stock"} <= ids)
        self.assertTrue(next(row for row in rows if row["id"] == "remake-chain")["owner_stack"])


class CommittedBaselineTests(unittest.TestCase):
    def test_committed_baseline_is_well_formed_and_current_with_the_config(self):
        data = json.loads((ROOT / "tools/lua_api_scan/baseline.json").read_text(encoding="utf-8"))
        keys = data["unbound"]
        self.assertEqual(keys, sorted(set(keys)), "sorted and unique, so a baseline refresh is a clean diff")
        config = scan.load_config(CONFIG)
        profiles = {p["id"] for p in config["profiles"]}
        read_hosts = {h["id"] for h in config["hosts"] if h.get("bindings")}
        for key in keys:
            profile, host, kind, name = key.split("|")
            self.assertIn(profile, profiles, key)
            self.assertIn(host, read_hosts, key)
            self.assertIn(kind, {"global", "member"}, key)
            self.assertTrue(cxx.API_NAME.match(name), key)

    def test_baseline_never_lists_a_name_the_host_registers(self):
        config = scan.load_config(CONFIG)
        read = bindings.read_all(ROOT, config["hosts"], config["follow"])
        data = json.loads((ROOT / "tools/lua_api_scan/baseline.json").read_text(encoding="utf-8"))
        for key in data["unbound"]:
            _profile, host, kind, name = key.split("|")
            if kind == "global":
                bound = read[host].knows_global(name) and name not in read[host].stubs
            else:
                bound = read[host].knows_member(name)
            self.assertFalse(bound, f"{key} is bound by the host table now: refresh the baseline")


_SCANNED: dict = {}


def fixture_result() -> dict:
    if not _SCANNED:
        _SCANNED["result"] = scan.run_scan(ROOT, scan.load_config(CONFIG), {"stock": FIXTURE}, ["stock"])
    return _SCANNED["result"]


class FixtureScanTests(unittest.TestCase):
    def setUp(self):
        self.result = fixture_result()
        self.rows = {(r["host"], r["kind"], r["name"]): r for r in self.result["rows"]}

    def test_hosts_follow_paths_and_plot_files(self):
        roots = self.result["profiles"][0]["script_roots"]
        self.assertEqual(roots["galactic_story"], 1)  # named by a campaign's plot file
        self.assertEqual(roots["tactical_story"], 2)  # the same script (mixed mode) plus the one no plot names
        self.assertEqual(roots["galactic_ai"], 1)
        self.assertEqual(roots["galactic_object"], 1)
        self.assertEqual(self.result["profiles"][0]["story_scripts_not_in_a_campaign_plot"], 1)

    def test_stubbed_story_function_is_critical_at_start(self):
        row = self.rows[("galactic_story", "global", "Find_Path")]
        self.assertEqual(row["status"], "stub-raises-unbound")
        self.assertEqual((row["severity"], row["timing"]), ("critical", "start"))
        # the AI host binds it
        self.assertNotIn(("galactic_ai", "global", "Find_Path"), self.rows)
        # the story host binds its planet list now (GA-08)
        self.assertNotIn(("galactic_story", "global", "FindPlanet.Get_All_Planets"), self.rows)

    def test_library_call_is_blocked_through_the_requiring_script(self):
        row = self.rows[("galactic_story", "global", "Find_All_Objects_Of_Type")]
        cell = row["per_profile"]["stock"]
        self.assertEqual((cell["call_sites"], cell["scripts_blocked"], cell["scripts_blocked_directly"]), (1, 1, 0))
        self.assertIn(("galactic_ai", "global", "Lock_Controls"), self.rows)

    def test_script_defined_helpers_and_forbidden_names_are_not_gaps(self):
        names = {name for (_h, _k, name) in self.rows}
        for helper in ("Fixture_Start", "Free_Local_Helper", "Lib_Helper", "Fixture_Table.Method", "Definitions"):
            self.assertNotIn(helper, names)
        self.assertNotIn("GameRandom.Free_Random", names)
        self.assertFalse([r for r in self.result["unresolved"] if r["name"] in {"Lib_Helper", "Fixture_Start"}])

    def test_method_calls_on_locals_and_ground_parking(self):
        self.assertIn(("galactic_object", "member", "Play_SFX_Event"), self.rows)
        self.assertNotIn(("galactic_object", "member", "Is_Human"), self.rows)  # bound in the object host (#7964)
        ground = self.rows[("galactic_story", "member", "Is_In_Garrison")]
        self.assertTrue(ground["ground_only"])
        self.assertEqual(ground["severity"], "parked")
        self.assertNotIn(("galactic_story", "global", "Find_Player"), self.rows)  # bound in the story host

    def test_tactical_hosts_are_read_and_ground_stays_parked(self):
        # The tactical lane's rows read the shared tactical scheduler's registration sites.
        self.assertIn(("tactical_story", "global", "Point_Camera_At"), self.rows)
        self.assertNotIn(("tactical_story", "global", "Find_Nearest"), self.rows)  # bound by the tactical host
        self.assertFalse([r for r in self.result["demand_for_unread_hosts"] if r["host"] != "tactical_plan_land"])

    def test_runtime_unbound_lines_are_all_accounted_for(self):
        by_name = {c["name"]: c for c in self.result["runtime_crosscheck"]}
        self.assertEqual(by_name["FindPlanet.Get_All_Planets"]["static"], "explained")  # bound since GA-08
        self.assertEqual(by_name["Is_Human"]["explain"][:5], "bound")
        self.assertEqual(by_name["Find_All_Objects_Of_Type"]["explain"][:5], "bound")


class BaselineCliTests(unittest.TestCase):
    def run_cli(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = cli.main(["--stock-root", str(FIXTURE), "--profiles", "stock", *args])
        return code, out.getvalue() + err.getvalue()

    def test_new_unbound_call_fails_the_baseline_and_a_fix_does_not(self):
        with tempfile.TemporaryDirectory() as tmp:
            baseline = Path(tmp) / "baseline.json"
            self.assertEqual(self.run_cli("--baseline", str(baseline), "--update-baseline")[0], 0)
            self.assertEqual(self.run_cli("--baseline", str(baseline), "--check-baseline")[0], 0)
            data = json.loads(baseline.read_text(encoding="utf-8"))
            dropped = data["unbound"].pop()
            baseline.write_text(json.dumps(data), encoding="utf-8")
            code, text = self.run_cli("--baseline", str(baseline), "--check-baseline")
            self.assertEqual(code, 1)
            self.assertIn("NEW unbound call: " + dropped, text)
            data["unbound"].append(dropped)
            data["unbound"].append("stock|galactic_story|global|Was_Fixed_Since")
            baseline.write_text(json.dumps(data), encoding="utf-8")
            code, text = self.run_cli("--baseline", str(baseline), "--check-baseline")
            self.assertEqual(code, 0)
            self.assertIn("fixed (drop from baseline): stock|galactic_story|global|Was_Fixed_Since", text)

    def test_markdown_and_json_outputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(self.run_cli("--out-dir", tmp)[0], 0)
            matrix = json.loads((Path(tmp) / "matrix.json").read_text(encoding="utf-8"))
            self.assertEqual(matrix["schema_version"], 1)
            text = (Path(tmp) / "matrix.md").read_text(encoding="utf-8")
            self.assertIn("FindPlanet.Get_All_Planets", text)
            self.assertNotIn(str(ROOT), text, "no local path in a report")


class ExtractTests(unittest.TestCase):
    def test_approximate_fallback_still_finds_calls(self):
        parsed = extract.parse_script(b"function Definitions()\n  x = = 1\n  FindPlanet.Get_All_Planets()\n  p:Is_Human()\nend\n")
        self.assertIsNotNone(parsed.parse_error)
        self.assertIn(("FindPlanet.Get_All_Planets", "dotted"), {(c.symbol, c.kind) for c in parsed.calls})
        self.assertIn(("Is_Human", "method"), {(c.symbol, c.kind) for c in parsed.calls})

    def test_call_context_is_the_outermost_named_function(self):
        parsed = extract.parse_script(b"Top()\nfunction A()\n  local f = function() B() end\nend\n")
        by_symbol = {c.symbol: c.function for c in parsed.calls}
        self.assertIsNone(by_symbol["Top"])
        self.assertEqual(by_symbol["B"], "A")


if __name__ == "__main__":
    unittest.main()
