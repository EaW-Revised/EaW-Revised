# Validation tiers

Before a PR merges, it runs the gate for its **tier**, not the whole four-compiler matrix every
time. The owner chose this on 2026-10-02. The compilers a tier leaves out still run every night
on `main` (`.github/workflows/ci.yml`, 01:00 UTC). Pushes to `main` do not start CI;
the coordinator also runs batch checks on merged changes. A red nightly
opens an issue labelled `ci-nightly`. The nightly covers the four x64 compilers only: macOS and
arm64 run in the public repository's cloud CI (`tools/oss/public/.github/workflows/ci.yml`).
In this repository, native Linux arm64 runs only when someone dispatches the CI workflow by hand
with `arm64` set (a GitHub-hosted runner); no pull request, push or nightly runs it.

## Which tier

`tools/ci/validation_tier.py` reads `git diff --name-only BASE...HEAD`. It prints the tier, the
area of each changed path and the exact commands to run:

```powershell
python tools/ci/validation_tier.py                          # this branch against origin/main
python tools/ci/validation_tier.py --base origin/main --json
gh pr diff 1234 --name-only | python tools/ci/validation_tier.py --stdin
```

| Tier | Changed paths | Per-PR gate | Left to the nightly / main CI |
| --- | --- | --- | --- |
| `docs` | Markdown (`*.md`) anywhere, walks included | the reader reruns: cleanroom check, doc references, tag registry check, `unit_census.py --check`, the whole Python suite (`tools/ci/python_suite.py`) and every CTest with game data | - |
| `tooling` | Python files anywhere outside the viewer areas and build steps (tools, tests, test helpers such as `tests/ui/sfnt_fixture.py`), `tools/`, `tests/ci/`, `.github/`, `config/` (except the compiler cache pin); tool data: every non-Markdown file under `docs/`, `plan/` and `analysis/` that no build step reads | cleanroom check, doc references, tag registry check, the whole Python suite and the whole Windows CTest with game data (it holds every CTest that runs Python); with tool data, also `unit_census.py --check` | Linux |
| `data` | `data/`, test fixtures, pinned replays and hashes | game-data CTest (unit census included) with one compiler, `windows-msvc-ninja` on the host with the verified FoC data copy, and the whole Python suite | the other compilers |
| `sim` | gameplay and sim C++ (`src/`, `include/`, `apps/` except the viewer, C++ tests) | MSVC with game-data CTest (it runs the 1/2/4/8 worker hash tests) and Linux GCC | clang-cl, Clang |
| `viewer` | `apps/viewer/`, the presentation library, GPU suite tests | the viewer build and the affected GPU suites; the presentation library also builds and tests in the root MSVC tree | the root presets, Linux |
| `full` | the build system (every `CMakeLists.txt`, `*.cmake`, `CMakePresets*.json` and toolchain file anywhere in the tree, `cmake/`), compiler, toolchain and dependency pins (`tools/shaders/toolchain.json`, `config/compiler-cache.json`, the Godot and godot-cpp pins in `dependencies.json`, `requirements-dev.txt`, any `*toolchain*` data file), platform and foundation code (`core`, `platform`, `vfs`, `third_party`), headers with 50 or more direct includers, unknown paths | both presets per platform, the viewer, game-data CTest, the whole Python suite | - |

Only Markdown is prose. Any other file wherever it lives can feed a build:
`plan/inventories/xml-tags.json` generates the C++ schema contract of the root build and the
viewer, and the roster gate header is generated at configure time from
`data/skirmish/roster-gate.json` by `tools/inventory/roster_gate.py`. So a file that a build step
reads takes at least `sim` (`viewer` when only the viewer reads it, `full` when both builds do),
whatever its directory says. The classifier finds those files by scanning, not by a list:

- every CMake file for the tracked paths it names outside comments, `message()` text and test
  registrations (`add_test` and the project's test helpers);
- every Python script a build step runs (`execute_process`, `add_custom_command`,
  `add_custom_target`, configure dependencies), its module-level imports, and every data path
  literal in them (`"plan" / "inventories"` counts as `plan/inventories`; a directory takes all
  of its files);
- every C/C++ source for string literals naming a tracked file under `docs/`, `plan/`,
  `analysis/` or `data/`.

A test (`test_no_build_input_classifies_below_sim`) repeats that search independently and fails
when a path a build reads would classify below `sim`.

Markdown, tool data and game data have readers no path rule lists reliably: the unit census
parses the walks, and `test_live_session.py` collects a mixin from a helper module that asserts
a key of `docs/ui/perf-overlay.md` while naming no Markdown itself. A change to them therefore
runs the whole Python suite and every CTest with game data, not a computed subset. The same
holds for Python: a test imports a helper beside it (`test_extract_eaw_fonts.py`, CTest
`ui_font_extraction`, builds its fonts with `tests/ui/sfnt_fixture.py`), and a CTest runs a script
from anywhere in the tree. So any changed `*.py` file, in any tier, adds the whole Python suite
and the whole Windows CTest with game data; the classifier does not compute which tests consume
it. That CTest runs every registration, Python or not: the offload host builds the whole preset
before CTest anyway, and a regex of every Python CTest name would neither fit the remote command
line nor catch names a CMake loop builds.
`tools/ci/python_suite.py` runs one pytest per test directory (a single `pytest tests` cannot
collect the tree: files in different directories share base names). Test files written as
scripts that take CTest arguments are left out of it and run in that CTest. Together the
suite takes about ten minutes.

Mixed changes take the stricter tier. Simulation or data changes together with viewer or
presentation changes take `full`. The cheaper checks of the other areas are added to the
list: a sim PR that also edits a tool also runs the whole Python suite. Every CMake
edit takes `full`, including source-entry edits in `apps/viewer/CMakeLists.txt` or the
shared `src/**/sources.cmake` lists. The root libraries and viewer consume the shared
lists, so adding or removing a source requires validation of both builds.

The only computed test list left is the GPU suites of a viewer change: the changed
`tests/presentation` suite files, otherwise the suites of the changed views.

The classifier only chooses commands. The merge rules stay the same: if it says `full`, run
everything; if a command fails, the PR is not ready.

## Rebases keep their receipts only when nothing moved under them

A rebase keeps the compiled validation results ("receipts") of the head that was validated only
when the PR's own files are unchanged and `main` moved only in Markdown. Check it and record the
line it prints in the PR:

```powershell
python tools/ci/validation_tier.py --equivalent <validated head> [--head HEAD] [--base origin/main]
```

It compares the set of files the PR changes against `BASE` before and after the rebase, and the
mode and blob of every one of those files. It then lists what `main` changed between the old
and the new merge base. A PR's unchanged code can still break on a header, CMake option, pin,
tool, data file or test that `main` changed under it, so any path that is not a regular `*.md`
file voids every receipt: exit 1, it names what moved, and you validate again for the tier
before the merge. No later CI run stands in for that.

Markdown is not always prose either: tools and tests read it as data, some of them through
helper modules (see [Which tier](#which-tier)). When `main` moved only in Markdown, the tool
exits 0 and keeps only the compiled receipts: the compiler builds, the viewer, the GPU suites
and the replay hashes, which read no Markdown. It prints
`Source-equivalent rebase: <new> carries the N PR files of <old> byte-identically ...` and the
reader reruns for the new head, the same list a Markdown PR runs; it does not try to compute
which tests read the moved files:

- the cleanroom check, the doc references, the tag registry check, and
  `unit_census.py --check` (with `EAWR_EAW_GAME_ROOT`, on the local lane);
- the whole Python suite, `python tools/ci/python_suite.py`;
- every CTest on the host with the verified FoC data copy (`windows_build.py --preset
  windows-msvc-ninja --host laptop`), which also runs the script-style tests and the C++ tests
  that read Markdown.

Record their results with the line.

## Nightly coverage

The nightly runs the four x64 compilers (MSVC, clang-cl, GCC 14, Clang 18) with CTest and the
warnings probe. It also runs the determinism comparators, the Linux software-render viewer, and
the Windows viewer extension (built in the MSVC job of every full run). Everything runs on the
self-hosted guests; GitHub-hosted minutes stay at zero, and ARM64 runs only when a dispatch asks
for it. The CI guests have no FoC game data, so the game-data CTest stays in the per-PR gate of
the `data`, `sim` and `full` tiers and of every change to Markdown, tool data or Python.

The `nightly-alert` job runs after every scheduled run on the Linux support runner. A red
nightly opens one issue labelled `ci-nightly`, or comments on the one that is already open. The
next green nightly comments and closes it. A nightly that skipped because `main` has not
changed since the last green one leaves the issue as it is.
