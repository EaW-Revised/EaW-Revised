# Selected-ship path line

Scope: the space battle (skirmish and galactic-conquest space), the local player's selection. Mods switch the
overlay on with one GameConstants tag. Presentation reads published snapshot data and never changes commands or
canonical state (legacy EAWR-6932).

| Rule | Behavior | Evidence |
| --- | --- | --- |
| PL-01 | The overlay is an engine feature that one game-wide boolean switches on: `ShouldDisplayPredictionPaths` in `Data/Xml/GameConstants.xml`. Stock FoC data sets it `false`; the Remake chain (Remake, Holdouts, FFT) sets it `true`. With it false nothing is created and nothing is drawn. It is not a per-unit tag, not Lua and not an authored model. | Debug build (the flag is read each time a unit's display state is set); mod data (four data sets compared). |
| PL-02 | Only objects in the local player's selection get a line, and only ships that fly a move plan: a unit at rest, turning in place, a fighter or squadron member, or a unit without a locomotor has none. Our default is own units only (a selected enemy's line would show an AI's intent through fog). | Debug build for "selected only" and for the stored plan per tracked object; own-units-only and the fighter exclusion are our least-visible choice where retail is unverified (see Unverified). |
| PL-03 | The line is the path the ship will actually fly (turn arcs included), from where it was when the plan was made to the end of the plan, not its waypoint list. We take the plan's nodes the movement already holds (MV-30: cubic Hermite between nodes) and sample it with the same rule, so the line matches the ship's own flight. | Debug build (the line is built from the stored predicted track). |
| PL-04 | Light green, RGBA 128,255,128,128, a hard-coded constant (not data), multiplied by the first texel of the `WaypointLineTextureName` texture, the same texel rule as OF-05. Alpha blended, one hardware line (one pixel wide at any zoom), depth test always passing, no culling: the line is drawn over ships and scenery. | Debug build (constant, depth state, line list with all UVs at 0,0); mod data (the texture is white in all chains). |
| PL-05 | A segment ends once the track has moved 20 world units from the last segment start; the last, shorter piece also ends the line. A 3000-unit trip is roughly 150 segments before the cap (PL-08). | Debug build (segment cut by distance from the last segment start). |
| PL-06 | The line shortens as the ship flies and is gone at arrival: the part of the line whose plan frame is already past is not drawn. It is cleared on deselection, replaced when the ship gets a new plan (the new line starts where the ship was when the plan was made) and removed when the ship is destroyed. Retail trims by whole segments; ours trims smoothly inside a segment. | Debug build (segments deleted by elapsed frames since the plan started; cleared on deselect, replan and removal). |
| PL-07 | The overlay costs nothing on the CPU per segment per frame. One mesh with one fixed-capacity triangle surface carries every selected ship; the vertex buffer is written in place (no mesh rebuild) and only when the selection or a ship's plan changes. A plan is copied once when it is made, into an immutable record shared by every snapshot, and sampled into line points once. The trim is one material parameter per frame. A frame with no change touches no buffer. | Project rule; the original builds, draws (one draw call each) and frees every segment of every selected ship individually, which is why it was slow with large selections. |
| PL-08 | Caps (our deviation, to bound the cost of a select-all fleet): at most 256 ships (the earliest selected win), 64 segments per ship and 16,384 segments in all; a longer track is thinned evenly, raising the step above 20 units, and never truncated, so the end of the plan is always drawn. A real rebuild happens at most 4 times a second however many plans change (a replan storm after a select-all order lands over several ticks). | Project rule. The caps and the rebuild limit were agreed against the high-end performance budget. |
| PL-09 | Readability override to PL-04: each segment is a screen-space ribbon, one pixel wide within 1000 world units of the camera, smoothly widening to 2.5 pixels at 1900 units and beyond. Colour, alpha, trim and ON/OFF semantics stay as PL-01..PL-06. One material and one draw, six vertices per segment, no extra pass or glow. The vertex shader uses the camera matrices; zoom adds no CPU work or uploads. OFF still creates no surface and skips the frame service. | Owner kit1 feedback, 2026-11-17 (legacy EAWR-7581); project presentation rule, not a claim about retail. |

## What the code does

- Sim: `TacticalSnapshot::predicted_path(id)` reads the enabled-only sidecar's pointer to a moving ship's immutable plan copy (PL-01, PL-07).
  A ship with a path plan, a motion profile and no craft state gets one copy per plan: the session keeps the copy
  while the plan's nodes are unchanged and publishes the same pointer in every enabled snapshot. The sidecar is presentation
  only: outside canonical bytes, checkpoints and the state hash.
- Presentation: `PathLineBuilder` (engine-free) turns the selected ships' plans into line vertices with a plan frame
  each, applies the caps and the rebuild limit, and counts its work (`PathLineStats`).
- Viewer: `PathLineView` owns the fixed-capacity surface and the material. `WorldUiView` reads the tag with the other
  GameConstants world-UI tags and passes the local player's selected ships.

## Tags near the feature that are not wired

| Tag | State | Why |
| --- | --- | --- |
| `ShouldDisplaySyncedPaths` | not wired | `false` in all four data sets; it adds the network-synchronisation ticks to the same overlay. A single-player skirmish has no such frames to mark (unverified). |
| `ShouldDisplayPotentialPath` | not wired | `false` in all four data sets; it belongs to the land path finder's debug display, not this overlay. |
| `LoopWaypointLineTextureName` | not wired here | Unrelated to this overlay; the waypoint display reads `WaypointLineTextureName` (OF-05). |

## Unverified (no retail capture yet)

1. Which type flag removes a unit from path display in space (assumed: fighters and small craft) and which XML tag sets it.
2. Whether selecting an enemy ship shows its path in retail.
3. Whether the synchronisation ticks are present in a single-player skirmish.
4. Whether land units show the line in a land battle. Land is left out until confirmed.
5. The on-screen alpha impression of the colour with the white texture, to be judged on a lit retail capture.

## Fidelity list

PL-01 load gate: `ShouldDisplayPredictionPaths` is read once with the immutable game constants.
Absent or false leaves the simulation's prediction phase disabled: no unit visits, stamp checks,
plan copies or per-unit snapshot handles. Enabled content publishes the same immutable plans in
an optional snapshot sidecar. The flag and sidecar do not participate in content identity,
canonical simulation bytes, replay data or checkpoints. The viewer creates the overlay only
when enabled and skips its frame service otherwise.

- Retail draws the line per segment per frame; ours is one batched fixed-capacity buffer with caps (deviation, PL-07, PL-08).
- Retail trims the line by whole segments; ours trims smoothly.
- Far zoom widens the line to 2.5 screen pixels for readability (PL-09); retail uses one pixel.
- Retail synchronisation ticks: not drawn.
- Land-battle path lines: not drawn (unverified in retail).
