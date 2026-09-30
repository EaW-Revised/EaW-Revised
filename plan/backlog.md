# Backlog and timeline

Ticket format: `ID  Title` then size (S under 2 days, M under a week, L under three weeks),
dependencies, and acceptance criteria. Phases 0 and 1 keep their original ticket summaries;
Phase 2 has its own plan, and later phases remain epics.

Assumptions from [the estimate](../docs/estimate.md): one person full time with agents.
Weeks are booked effort; milestone dates are the GitHub due dates. The working dates below
are the 2026-09-26 [board EAWR-7](https://github.com/users/Thom-Ernst/projects/7) replan snapshot
in the estimate; the live board is authoritative when dates move.

Status legend: `[ ]` open, `[~]` in progress, `[x]` done, `[-]` dropped.

---

## Phase 0: Foundations (W1, half a week: 2026-09-21 to 2026-09-25)

Goal: engine-independent core proven on three targets, engine decision made.
Exit criteria: P0-04 replay hashes identical on Windows x64, Linux x64, Linux ARM64 in CI;
one ALO model drawn by both render prototypes; ADR-001 resolved.
Status: M0 done 2026-09-22; all fourteen tickets are done. P0-05 was accepted on
2026-09-24 with the twelve missing EaW CIN includes as a known gap.

- [x] **P0-01  Repository skeleton and three-target CI**  (M)
  Deps: none.
  Accept: CMake project builds a `sim_headless` and a `tests` target with MSVC, Clang on
  Windows, GCC and Clang on Linux x64, GCC on Linux ARM64 (GitHub Actions hosted runners).
  A lint or compile rule fails the build if `float` or `double` appears inside the `sim`
  namespace. Warnings as errors.

- [x] **P0-02  Virtual file system**  (M)
  Deps: P0-01. Source: alo-viewer `MegaFile.cpp`, tools/megx.py.
  Accept: opens every MEG in the base game and the Remake folder; resolves paths
  case-insensitively with either separator; loose files override MEG entries; MODPATH
  layering order matches the original (mod, expansion, base); test enumerates all 1311
  Remake XML files and all base `.xml` through the layered view.

- [x] **P0-03  Fixed-point math library**  (M)
  Deps: P0-01. See ADR-003.
  Accept: `Fixed` type, `Vec2`/`Vec3`/`Quat`/`Mat3x4` over it, deterministic sqrt, sin,
  cos, atan2, normalise. Range analysis documented against the largest map extents and
  smallest damage values in the Remake XML. Golden-value tests (hashes of a million-op
  sequence) pass identically on all three targets.

- [x] **P0-04  Deterministic sim harness and replay test**  (M)
  Deps: P0-03. See ADR-005.
  Accept: fixed tick, command log format (versioned, binary), per-tick state hash,
  `sim_headless --replay file --hash-out`. Entity library chosen (EnTT vs flecs, record
  reasons). CI job compares hash output across the three targets and fails on divergence.
  Sorted-iteration rule documented and enforced in the entity layer.

- [x] **P0-05  XML data model and variant inheritance**  (M)
  Deps: P0-02. Source: eaw-schema, LSP `Variant_Of_Existing_Type` resolver.
  Accept: loads every GameObject, hardpoint, ability, faction, campaign and SFX file from
  vanilla and Remake without error; effective-object resolution matches the LSP's "Show
  Effective Object" on 20 sampled variants; unknown tags logged with file and line.
  P0-only exception: the authored `Freighter_Acclamator_E` self-inheritance remains an
  error diagnostic with no inferred runtime values; E5-1 retains its runtime-measurement
  compatibility follow-up. Owner decision 2026-09-24: the twelve required EaW CIN includes, which
  the pinned EaW build does not ship, are accepted as a known gap. They stay an explicit
  diagnostic with EaW `full_pass=false` and no invented values.

- [x] **P0-06  ALO, ALA, DDS, TGA loaders**  (S)
  Deps: P0-02. Source: alo-viewer `Assets/`.
  Accept: headless test loads all 4140 Remake ALO and 5603 ALA files, reports mesh, bone,
  material and animation counts; zero parse failures or a documented list of failures.

- [x] **P0-07  Lua runtime embedded, script model**  (M)
  Deps: P0-01. Studied as documentation: GlyphX-Reference `PGLua/`; implemented from behaviour notes.
  Accept: Lua version identified from the reference source and embedded; the script
  wrapper model reproduced (script instances, coroutine threads, `Function_Call`, event
  dispatch, wide strings); runs a vanilla story script far enough to call into a stubbed
  API and fail on the first missing function with a clear message.

- [x] **P0-08  Lua API surface inventory**  (S)
  Deps: none (reads files only).
  Accept: script walks vanilla and Remake Lua, emits every engine function called with
  call count and example call site, cross-referenced to the visible names GlyphX-Reference declares.
  Output checked into `plan/inventories/`.

- [x] **P0-09  XML tag inventory**  (S)
  Deps: none.
  Accept: same as P0-08 for XML tags per object type, vanilla and Remake, with the eaw-schema
  status of each tag (known, deprecated, unknown).

- [x] **P0-10  Shader translation spike**  (S)
  Deps: none. Source: shaders/petroglyph-foc, LSP `hlsl.ts`.
  Accept: script strips the effect wrapper (technique, pass, sampler_state) and compiles
  MESHGLOSS, MESHBUMPCOLORIZE, RSKINGLOSSCOLORIZE, MESHSHIELD and PRIMALPHA to SPIR-V with
  DXC or glslang; parameter and semantic list extracted per shader; corpus issues listed.

- [x] **P0-11  Render prototype A: SDL3 + wgpu**  (M)
  Deps: P0-06, P0-10.
  Accept: window on Windows and Linux, one ALO drawn with translated MESHGLOSS, orbit
  camera, frame time logged. Notes on effort and friction.

- [x] **P0-12  Render prototype B: Godot RenderingServer via GDExtension**  (M)
  Deps: P0-06, P0-10.
  Accept: same scene as P0-11 drawn through RenderingServer with no scene-tree nodes for the
  mesh; sim data passed from a C++ extension; export template built for Linux. Notes on
  effort and friction.

- [x] **P0-13  Behaviour-note workflow and first note**  (S)
  Deps: none.
  Accept: template for behaviour notes (observable decisions, discarded implementation
  details, replay test cases); two-context procedure written; first note produced for
  space targeting from Ghidra, with at least three replay-style test cases.

- [x] **P0-14  Engine decision (resolve ADR-001)**  (S)
  Deps: P0-11, P0-12.
  Accept: ADR entry with the choice, the measured friction from both prototypes, and the
  losing prototype deleted.

---

## Phase 1: Render the world (2 booked weeks, due 2026-10-09; done 2026-09-25)

Milestone M1: a FoC space map and land map render with the real shaders,
units placed from XML, particles playing. Screenshot-fidelity milestone: recognisably the
same scene next to the original, judged by the owner, not pixel or metric equality.

Rescoped 2026-09-24 and signed off 2026-09-25. Full checklists and acceptance rules are in
[the Phase 1 plan](phase-1/README.md), which replaces the earlier issue checklists.
Reference maps: `_mp_land_naboo.ted`
and `_mp_space_coruscant.ted` (owner decision 2026-09-24). EAWR-5 is accepted and does not gate Phase 1. Deferred fidelity work is
listed at the end of that file.

- [x] **P1-01  Production renderer**  (M)  EAWR-22
  Accept: both reference maps render through `apps/viewer` on Windows and Linux x64;
  unsupported materials fail with a diagnostic; draw order correct; no leaks on reload.
- [x] **P1-02  Full material shader corpus translation**  (M)  EAWR-23
- [x] **P1-03  Skinned meshes and animation playback**  (M)  EAWR-24
  Accept: infantry, vehicle and capital-ship turrets animate correctly by eye; ALA
  failures counted; attachment points follow the hierarchy.
- [x] **P1-04  Spherical-harmonics lighting and shadow maps**  (S)  EAWR-25
  Accept: SH with unit tests; shadows on land terrain and a real hull; plausible next to
  the original.
- [x] **P1-05  TED map loader**  (M)  EAWR-26
  Accept: every map loads or is listed; resolution rate per profile, vanilla at least 99% or
  shortfall explained; synthetic tests; coordinate note.
- [x] **P1-06  Terrain, water, skydome and nebula rendering**  (M)  EAWR-27
  Accept: both reference maps render without units next to original screenshots; unused
  families listed; one Windows frame-time number.
- [x] **P1-07  Fog of war texture path**  (S)  EAWR-28
  Accept: painted grid darkens terrain and units; replay hashes unchanged on five targets.
- [x] **P1-08  Particle system port from alo-viewer**  (M)  EAWR-29
  Accept: reference-map emitters plus engine glow, laser hit, explosion and smoke play and
  look right by eye; families counted; fixed-seed tests; no particle state in sim.
- [x] **P1-09  Tactical camera and input**  (S)  EAWR-30
  Accept: zoom, pitch and pan feel like the original; constants listed with XML source.
- [x] **P1-10  Mega texture reader and icon atlas**  (S)  EAWR-31
- [x] **P1-11  Units placed from XML on loaded maps**  (M)  EAWR-32
  Accept: both reference maps populated with team colour, animation and effects; every
  vanilla map attempted; import fixture hash identical on five targets.
- [x] **P1-12  M1 sign-off**  (M)  EAWR-33
  Accept: owner-taken original screenshots beside remake screenshots and signed off; M1 CI
  batch green with a Linux software-render screenshot; Phase 2 ticketed and re-estimated.

## M1.5: Phase 2 prerequisites (due 2026-09-30; board plan 2026-09-28)

Added between M1 and M2. The structural refactor (EAWR-98–EAWR-107 and EAWR-116), build offload
(EAWR-123, EAWR-162 and EAWR-289), Forward+ switch (EAWR-149–EAWR-153), and UI foundation prepare Phase 2.
The integration branch merged to `main` on 2026-09-26 via
EAWR-305.
Open follow-ups on 2026-09-26: EAWR-116 (T11) and EAWR-153 (FP-5).
After FP-5, remove Compatibility-only shader compensation and GL sampler limits
when the first new RenderingDevice-only feature lands; the emergency output has
no pinned fidelity target.

## Phase 2: Space skirmish (5 booked weeks, due 2026-10-13; likely sign-off 2026-10-08 as of 2026-09-29)

Milestone M2: a vanilla space skirmish is playable against a scripted AI.

Phase 2 is in progress; its status and dates as of 2026-09-29 are in the plan. Scope, rules, and tickets live in [the Phase 2 plan](phase-2/README.md)
and [board EAWR-7](https://github.com/users/Thom-Ernst/projects/7).

## Phase 3: Galactic conquest (6 booked weeks, due 2026-12-18; board 2026-09-30 to 2026-10-30)

Milestone M3: a vanilla campaign is completable with AI, saves and story events.

This section is the Phase 3 plan until EAWR-85 tickets the phase. Since the 2026-09-29 triage, the
first engine-only slices run beside the M2 work: EAWR-679 (E3-1a), EAWR-680 (E3-1b), EAWR-681 (E3-6a),
EAWR-682 (E3-2a) and EAWR-683 (E3-3a), all sub-issues of EAWR-18, with EAWR-273, EAWR-560 and MOD-4/MOD-5
(EAWR-236, EAWR-237). Their order and dates are in [the estimate](../docs/estimate.md#status-and-re-estimate-2026-09-29).

- **E3-1 Galaxy model**: planets, trade routes, fleets, movement, hyperspace.
- **E3-2 Economy and production**: income, build queues, tech levels, heroes.
- **E3-3 Story mode**: the story event and sub-plot model, implemented from behaviour notes
  (GlyphX-Reference studied as documentation only); vanilla campaign scripts run.
- **E3-4 Galactic UI**.
- **E3-5 Galactic AI**.
- **E3-6 Save and load**: versioned, deterministic round trip.
- **E3-7 Auto-resolve and land-space transitions**.

## Phase 4: Land skirmish (5 booked weeks, due 2027-01-29; board 2026-11-02 to 2026-11-29)

Milestone M4: a vanilla land skirmish is playable.

- **E4-1 Pathfinding and passability**: highest-risk item; behaviour notes first, Recast
  only if it matches.
- **E4-2 Infantry, vehicles, buildings, build pads, garrisons**.
- **E4-3 Bombing runs, orbital bombardment, weather, hazards**.
- **E4-4 Land AI**.
- **E4-5 Lua API, land subset**.

The owner chose this phase and milestone order in EAWR-160
on 2026-09-26.

## Phase 5: Mod compatibility (12 booked weeks, due 2027-04-23; board 2026-11-30 to 2027-02-23)

Milestone M5: Empire at War Remake 4.0 loads and plays acceptably.

- **E5-1 Remake load-through**: every XML and Lua file loads; the unknown-tag and
  missing-function lists from P0-08 and P0-09 driven to zero.
  Measure the original runtime binding, effective values and instantiation of
  `Freighter_Acclamator_E` on the pinned Remake/FoC build; adjudicate compatibility
  using those observations. Its approved P0 data exception supplies no runtime oracle.
- **E5-2 Remake shaders**: obtain the 11 mod-specific `.fx` sources or translate their
  bytecode.
- **E5-3 Fidelity tail**: bug reports from mod teams, behaviour notes, replay fixtures.
- **E5-4 Second mod** (Thrawn's Revenge or Republic at War) as a regression fixture.

## Phase 6: Multiplayer (8 booked weeks, due 2027-05-07; board 2026-12-26 to 2027-02-23, overlaps Phase 5)

- **E6-1 Lockstep networking** over GameNetworkingSockets, command log is the wire format.
- **E6-2 Lobby and Steam**.
- **E6-3 Desync diagnosis** using the per-tick hashes.
- **E6-4 Cross-platform play**: Windows x64 vs Linux ARM64 in the same match, in CI.

---

## Tracking

Live status and working dates are on [board EAWR-7](https://github.com/users/Thom-Ernst/projects/7).
The GitHub milestones carry the booked due dates; [the estimate](../docs/estimate.md#milestones-solo-full-time)
records the dated board snapshot and measured effort. M0 was done 2026-09-22, M1 was
signed off 2026-09-25, M1.5 merged 2026-09-26 with two follow-ups open, and Phase 2 is
in progress. EAWR-85 re-checks later dates against measured Phase 2 effort.

Schedule history: the 2026-09-22 rebaseline and its old phase estimates are retained
only as dated records in [the estimate](../docs/estimate.md#rebaseline-after-phase-0).
The 2026-09-26 replan supersedes them for current working dates.
