# Tactical logical tick rate

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption. The claims come from the FoC debug
  build's frame synchronizer, read on 2026-09-25 under the [clean-room rule](../clean-room.md).
  The private evidence map (IDs TR-E1 to TR-E4) is under the ignored `out/research/`.
- Bounded question: how much game time one simulation tick covers, and whether the game-speed
  setting changes it.
- Status: **pinned, awaiting confirmation.** original-game behaviour recordings records the retail executable every logical
  frame and measures its cadence. That measurement confirms or replaces TR-01. Until then, the
  pin rests on the debug build alone.

## Interface

- Input: the number of completed ticks in a tactical session.
- Output: game time. One tick is one logical frame.
- Retained state: none. The rate is a constant of the tactical rules, not session data.

## Rules

| Rule | Behaviour | Source |
|---|---|---|
| TR-01 | The logical frame rate is **30 frames per second**, so one tick covers exactly 1/30 s of game time. The original sets this rate on reset and again for every game mode it initialises: single player, skirmish, LAN, internet and Steam multiplayer. | research (TR-E1, TR-E2) |
| TR-02 | The game-speed settings do not change the logical rate. They select how many logical frames run per wall-clock second. The tactical and strategic modes each have their own setting, with five steps, and both default to the middle step. | research (TR-E3) |
| TR-03 | Game time in seconds is the completed frame count divided by 30. | research (TR-E4) |
| TR-04 | A duration in seconds becomes a frame count by rounding seconds × 30 to the nearest integer, halves away from zero, with the product and the half added in binary32. This is the general conversion; some subsystems convert on their own and truncate instead: weapon recharge (whole hundredths of a second times 30, divided by 100) and pulse delay, and the half-second opportunity rescan (TC-03). Each behaviour note states its own conversion. | research (TR-E4; [audit](debug-build-audit.md) AU-63, AU-64) |
| TR-05 | The remake pins the tick in tactical rules v1. Replay v2 and the tactical state hash carry the rational 1/30. A replay with any other rational fails with `EAWR-SIM-0302`, so a new rate needs a new rules version. | project |
| TR-06 | The simulation has no wall clock. How many ticks run per real second (TR-02) is decided by the host that drives the session, and it never changes simulation results. | project |

## Cases

| Case | Input | Expected |
|---|---|---|
| TC-01 | 30 and 45 completed ticks | 1 s and 1.5 s of game time |
| TC-02 | The 10 s squadron delay in the M2 lock (SK-23) | 300 ticks |
| TC-03 | The half-second opportunity rescan in [space targeting](space-targeting.md) R-04 | truncation of 30 × 0.5 = 15 frames, so a scan needs 16 elapsed frames (case C-05) |
| TC-04 | `tests/replay/fixtures/tactical-v2-mutated-tick-rate.eawr-replay`, a replay-v2 header with a 1/60 tick | rejected with `EAWR-SIM-0302` |

## Unknowns

| ID | Unknown | Effect |
|---|---|---|
| TR-U1 | The retail executable's measured cadence (original-game behaviour recordings). | If it differs from 30, TR-01, the tactical rules version and the replay-v2 fixtures change together. |
| TR-U2 | Resolved by the [debug-build audit](debug-build-audit.md): ties round away from zero, after a binary32 multiply and a binary32 add of one half (TR-04). | A later ticket that converts XML durations to ticks reproduces both binary32 steps; durations whose ticks do not fall near a tie are unaffected. |
| TR-U3 | Resolved by [time controls](tactical-time-controls.md) TM-01 to TM-03: 10, 20, 30, 45 and 60 frames a second, 120 in fast forward, each paced by whole-millisecond waits. | None for the simulation (TR-06). |
| TR-U4 | Subsystems that run only every N frames, or convert seconds to frames by truncation (TR-04). | Each behaviour note states its own cadence and conversion in logical frames. |
