# Engine guard: only our patched Godot

Owner rule (2026-11-03): EAWR never runs on the official Godot download. Our engine is the fork Thom-Ernst/godot,
branch eawr/4.7.2-stable, built with `BUILD_NAME=eawr`.

- **What it checks.** `tools/engine_pins.json` lists the SHA-256 of every accepted patched exe (`engines`; the `default`
  one is the exe the player zip ships) and of known unpatched builds (`known_unpatched`).
  - The scripts compare hashes: rig drivers (`tools/rig/RigCommon.ps1`), `remake_bench.py`, `Start-Preview.ps1`,
    the mods-kit smoke, and the release pin in `package_release.py`.
  - The viewer checks `Engine.get_version_info()` at startup (build `eawr`, a pinned commit;
    `include/eawr/presentation/engine_signature.hpp`).
- **What happens on an unpatched engine.**
  - Test, gate, perf and rig runs refuse: they exit non-zero (the viewer exits with code 7; `--quit-after` smokes
    count as tests). The single refusal line names both hashes, e.g.
    `official Godot detected, not our patched engine: UNPATCHED ENGINE: found sha256 c8f0a6bc… expected 96c03783…
    (tools/engine_pins.json); <exe>`. The viewer prints the same `UNPATCHED ENGINE: found … expected …` part after
    `EAWR-ENGINE-UNPATCHED:`.
  - Normal play only warns: a log line, "UNPATCHED ENGINE (...)" in the window title and a red row on the perf overlay. It never aborts a match.
  - The player launcher passes `--eawr-engine-unverified` when the exe does not match runtime.json; the viewer then
    treats the engine as unpatched.
- **Override.** `EAWR_ALLOW_UNPATCHED_ENGINE=ab-control` (exact value) is the only bypass. It is meant for a labelled
  control arm of an engine A/B. The run is then labelled (`engineOverride` in rig.json, `ENGINE CONTROL ARM` in bench
  results, `override=ab-control` in preview logs) and never counts as a patched result.
- **Adding a new patched engine.**
  1. Build it from the fork (`tools/offload/godot_build.py`) and record the exe and console SHA-256.
  2. Add an entry under `engines` with `commit`, `build`, `role` and `files`. Move `default` only for the shipped
     player exe; update these at the same time:
     - `PINNED_GODOT_SHA256` in `tools/release/package_release.py`;
     - the zip pins in `tools/release/fetch_pinned_godot.ps1`;
     - `pinned_player_engine_sha256` in `include/eawr/presentation/engine_signature.hpp` (the GUI exe's hash).
  3. For a new fork commit, add its short hash to `patched_engine_commits` in `engine_signature.hpp`
     (`tests/ci/test_engine_signature_pins.py` keeps the two in sync), then run `ci_engine_guard`,
     `ci_engine_signature_pins` and `ui_engine_signature_contracts`.
