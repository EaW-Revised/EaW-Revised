# Performance deviations from retail

Owner approved this policy on 2026-10-25: "This could be another option, retail deviation toggle for performance."
The registry is `include/eawr/presentation/ui/performance_deviations.hpp`.
Every registered deviation must also have its own behaviour rule, owner approval and measured evidence.
PD-01 is an owner-requested readability feature approved on 2026-10-27 (legacy EAWR-5136). Its measured evidence below supports the gameplay frame-time gate; it makes no speedup claim.

## Policy

- A presentation optimization with no visible difference is always on and listed for transparency, without a toggle.
- A visible presentation deviation uses a per-player **Retail-exact / Faster** preference under **Options > Performance**, initially **Retail-exact**.
- A simulation deviation requires a host-selected match option shared by all peers, included in match identity and recorded in replays. Clients see the host's choice read-only; skirmish setup uses the same option. It initially selects **Retail-exact**. A later default change needs owner approval and before/after balance measurements.

This registry introduces no simulation option. Its first simulation entry must bring the match/lobby/replay implementation with it.

## Registry

The settings unit test checks this table exactly against the compiled metadata, including rule ID, setting key and default.
Update the header and table together when adding an approved deviation.
PD-01 is an explicitly approved exception to the initial Retail-exact default: its checkbox lives in **Options > Gameplay** and defaults on.

<!-- performance-deviations:begin -->
| Rule | Setting key | Name | Scope | Default | Retail behaviour | Our behaviour |
|---|---|---|---|---|---|---|
| PD-01 | gameplay.reinforcement_areas_only_while_placing | Reinforcement areas only while placing | presentation | 1 | Panel or data-authored drag rule | Only while placing |
<!-- performance-deviations:end -->

## PD-01 measured evidence

The owner request (legacy EAWR-5136) approves the Gameplay checkbox and its default on. On 2026-10-27, the same viewer binary was compared OFF/ON/ON/OFF on the RTX 4060 laptop rig using official Godot 4.7.2 (`ed1daf0bf`), Safe mode. The release default (patched Separate) was not separately measured. The extension was built from `2a2205689488f5eef3e59aaab0f77b9857223c86` (DLL SHA-256 `d2a3fad99e3688b88888ef0d1d14b8eebba67b634cf1286bb3bcd39c9b4ef05b`).

M2 ran with vision revealed, AI off, driven step 1, vsync off and no frame cap. Each run sampled 1,000 frames at ticks 1–1000, including squadron launches at ticks 12 and 162. Render timing queries were disabled equally in both modes.

| Preference | p99 frame time, repeats (ms) | Worst frame time, repeats (ms) |
|---|---|---|
| OFF | 7.992, 8.118 | 25.354, 22.705 |
| ON | 8.217, 8.212 | 22.807, 22.838 |

Both pass the declared gate: mean repeated p99 at most OFF × 1.15 + 0.5 ms; maximum worst at most OFF × 1.25 + 2 ms. All four runs drew 11 launched craft, matched their headless simulation, and ended with the same state hash. Four separate visual cells verify ON/OFF while idle/placing; ON idle performs no blocked-grid overlay queries, while OFF retains the pane/data-authored rule. Enemy-circle drop rejection remains active.

Receipts are retained in the worker's `out/pf-5136/`: `gpu/rig/20261027-033033-3ddd66` contains the timing summary and raw CSV text, and `gpu-feature-pass-timing-failed/rig/20261027-031827-1f1978` retains the green visual cells and initial failed proof drivers. The initial timing driver hid enemy launches behind fog; the rerun reveals them. The broad Options geometry check exposed the pre-existing checkbox overlap tracked separately (legacy EAWR-5219); the added checkbox passes focused geometry against all other controls.

## Performance options

The main and in-game Options hubs expose **Performance**. Its **Render thread (restart)** selector offers **Separate (default)** and **Safe**.
Apply and Accept save only that preference; Cancel discards it; Defaults selects Separate without saving until Apply or Accept.
The engine mode changes after restarting. Explicit launch flags and manager-selected profile modes retain their startup precedence.
Frame-rate limit and vsync stay under **Video**. See [player settings](../ui/ui-settings.md).
