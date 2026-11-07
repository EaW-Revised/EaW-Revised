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
| SH-01 | keyboard.selection_hotkeys | Selection hotkeys | presentation | 0 | Existing selection gestures | Optional bulk selection keys |
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

The main and in-game Options hubs expose **Performance**. Its **Render thread (restart)** selector offers **Separate (default)**; a previously saved Safe choice remains visible, and Safe is available through the launcher.
Apply and Accept save the render-thread preference and selection-hotkey toggle; Cancel discards it; Defaults selects Separate without saving until Apply or Accept.
The engine mode changes after restarting. Explicit launch flags and manager-selected profile modes retain their startup precedence.
Frame-rate limit and vsync stay under **Video**. See [player settings](../ui/ui-settings.md).

## SH-01 optional selection hotkeys

Owner request (legacy EAWR-5131) (2026-10-28) authorizes this convenience deviation, with no performance claim.
Options > Performance enables it (default off); Options > Controls binds both actions independently.
The defaults are physical Ctrl+Q (all own units) and Q (own units on screen). E, Shift+E,
Shift+Q and Ctrl+E are reserved by the existing command/ability map. The editor offers Q,
Ctrl+Q, Ctrl+U and Ctrl+Y, the verified free tactical chords, and rejects duplicate assignments.
The development land free camera owns its chords while active (space-map cameras reject free-flight bindings); these optional selection hotkeys are suspended until it exits. Modifiers match exactly; bindings and the toggle are local player preferences.

Both land and space use the same live-snapshot collection, independent of render-node and fog
admission. Own selectable mobile non-decoration units are admitted only with local command
permission. Heroes, including authored named, generic and team hero flags, join both selections by default
(owner decision (legacy EAWR-5558); supersedes the earlier hero exclusion); structures have no
locomotion and decorations stay excluded. Fighters/bombers select their squadron once.
For the screen action, any overlap of the projected soft-radius world bounds with the camera
viewport counts, including partly visible units; fully off-screen or behind-camera units do not.
Selection uses the ordinary selection object and command path without submitting a simulation
command. Replay bytes and simulation state are unaffected until the player issues an order.

## SH-02 Revised selection advisor events

The optional Revised layer supplies normal SFX events for bulk selection, including presses
that select no units. Samples rotate with Play_Sequentially and use the existing Localize,
Is_HUD_VO, Speech volume, pause and attract-silence policies. Samples preload through the
ordinary audio barrier. Absent event mappings retain the selected unit response.
No voice assets ship in the repository or release.

## SH-03 XML selection controls and additive SFX layers

Owner request (2026-10-28) defines project data in the regular Revised layer, enabled and
ordered per launcher profile using the existing folder layer controls. Its optional
SFXEventFilesAdditions.xml uses the regular SFXEvent_Files root and File includes to extend
the winning SFXEventFiles.xml. Primary registry replacement keeps normal retail/mod semantics;
omitted older primary registries stay inactive. Each layer contributes its winning additions
registry, repeated supplemental includes are omitted through normal VFS priority, and strongest-layer event
definitions win. Primary presets load before layer additions. Without additions, primary include
repetition and sequential registration retain their existing behavior.

The optional Data/XML/SelectionControls.xml document contains Selection_Advisor_Enabled
(default No) and optional Selection_Action entries with Name SELECT_OWN_ALL or SELECT_OWN_SCREEN.
Name is required only on an authored action. Optional Default_Key retains Ctrl+Q / Q; optional
Faction_SFXEvent contains explicit faction,event overrides from layers above Revised (or from any layer when Revised is absent); Advisor_SFXEvent maps advisor events to cues and Default_Faction_SFXEvent supplies the name-based fallback. Rows are repeatable and keys are case-insensitive.
Missing mappings preserve unit responses. Invalid/occupied keys retain safe defaults; duplicate
bindings fail closed. Settings rebinding and the explicit player toggle override XML defaults. Unrelated Options saves
do not persist the XML-derived enable default; Defaults removes that player override.
Each Selection_Action accepts optional Include_Heroes (Yes by default): No leaves heroes out of
that bulk selection. Structures and decorations stay excluded regardless. The tag is ours,
not an FoC object tag.
A hero in a bulk selection takes part in the ordinary speaker choice: the speaker is the unit in
the best-ranked category of Unit_Command_Rankings_By_Category, which lists Hero first, so a
selected hero speaks its own select line (a squadron or team container speaks through its
leading craft). A configured advisor event for the action still plays instead of any unit line.
