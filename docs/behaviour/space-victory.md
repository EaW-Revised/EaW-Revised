# Space skirmish victory and defeat (fixed forces)

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, multiplayer-style space skirmish with
  the lobby's default win condition, the M2 fixture ([m2-skirmish.md](../../plan/phase-2/m2-skirmish.md),
  SK-33; [skirmish start](../skirmish-start.md)). Fixed-force victory and defeat (legacy EAWR-77).
- Bounded question: which objects decide a fixed-force space battle, when a player wins or loses,
  what happens with more players and teams, and in which order destructions within one frame are
  judged. The outcome comes from battle state alone: no production, credits, income or
  population cap takes part.
- Source tags: **research** (the FoC debug build, read under the clean-room rule; evidence IDs
  VE-nn are opaque and their map stays private), **data** (a tag or value in the FoC files),
  **project** (a remake decision) and **unverified** (not established; the remake picks the least
  visible behaviour).
- Out of scope: the other lobby win conditions, story and campaign victories, retreat, the
  victory text, music and dialog ([battle end](battle-end.md)), and what the 7-second countdown
  does to the battle (VT-11).

## Interface

- Content: the victory rules (`sim::tactical::VictoryRules`): the condition, the star base types
  that count, the contenders (the lobby players) and which of them are human, and the countdown.
  For M2, `skirmish::victory_rules` builds them from the start and the unit tables. Like the
  sensor and durability tables they are passed to the session and are neither replay data nor
  state; a session without them never decides an outcome.
- State, once decided: the outcome (`BattleOutcome`: condition, winner, winner's team, the frame
  of the deciding destruction, the star base whose destruction decided it, and the frame the
  retail battle ends). It is a tagged block of the state hash and of the snapshot
  ([replay format](../replay-format.md)); an undecided session hashes exactly as before the fixed-force victory and defeat work.
- Outputs: a `victory` event in the frame that decides (player: the winner; unit: the deciding
  star base) and the outcome in that and every later snapshot. `sim_headless --replay` prints it
  (`outcome ... winner ...`) and the viewer's live-session report lists it with the local
  player's result.
- Cadence: once per tick, after every destruction of the tick and before the commit, serially.

## Rules

| Rule | Behaviour | Source |
|---|---|---|
| VT-01 | The lobby gives every player one victory condition, the lobby's win condition; FoC's default for space is `SKIRMISH_SPACE_ENEMY_STARBASE_DESTROYED` (M2 uses it). The other lobby conditions (all units, base components, command HQ, control point domination) are not modelled. | research VE-01; data VE-02 (`MP_Default_Space_Tactical_Win_Condition`); owner SK-33 |
| VT-02 | The condition is tested when an object whose type is victory relevant in space is destroyed, at that moment, once for each destruction. No per-frame test looks at star bases. Only a destroyed star base (`DUMMY_STAR_BASE` behaviour) tests this condition. Ships, squadrons and map objects never decide the battle, however many are lost. | research VE-04, VE-05, VE-11 |
| VT-03 | A star base counts when it is standing (not dead, not a death clone), its type is `Victory_Relevant` in space and its owner's faction is playable. Each FoC `StarBase` type authors `DUMMY_STAR_BASE` and `Victory_Relevant` yes. The Coruscant map objects never count: the gravity-well station is an orbital structure with `Victory_Relevant` no, and the Neutral and Hutt objects belong to factions that are not playable. So in M2 exactly the two skirmish stations count. | research VE-06, VE-10; data (`starbases.xml`, `secondarystructures.xml`) |
| VT-04 | When a counted star base is destroyed, every player that is an enemy of its owner is tested in ascending player ID. | research VE-05 |
| VT-05 | A tested player wins when no counted star base that it does not own is standing. A star base of an ally counts too: only the player's own star bases are left out. | research VE-06, VE-07 |
| VT-06 | A tested player that is not human also wins when none of those star bases is an ally of a human player. With one human and one AI this is the same as VT-05. | research VE-06 |
| VT-07 | Retail gives the non-playable players (Pirates, Neutral, Hostile, Sarlacc, Hutts) the condition too. The remake tests only the contenders (the lobby players, which have the command flag): a non-playable player never wins, and its star bases never count (VT-03), so it never blocks a win. Every contender has a lower ID than every non-playable player, and in the tested line-ups a contender decides first; whether retail can ever let a non-playable player win was not traced (**unverified**, G-V2). | research VE-03; project |
| VT-08 | Destructions in one frame are judged one at a time in the order they happen. When a destruction is judged, the star bases destroyed later in the same frame are still standing. In the remake the order is the tick's event order: projectile hits in ascending projectile ID, then commands in canonical order and each command's units in list order, then the durability service in ascending unit ID. | research VE-04; project (the remake's event order stands for retail's destruction order) |
| VT-09 | The first winner decides. A pending victory rejects every later one, so a later destruction, in the same frame or after, never changes the outcome. There is no draw: when both star bases fall in one frame, the player whose star base was destroyed first loses and the other wins. | research VE-08 |
| VT-10 | The winner's allies win with it and every player that is its enemy loses: retail shows the local player the win text when it is the winner or the winner's ally and the lose text when it is the winner's enemy. The outcome names the winner and its team. | research VE-08 |
| VT-11 | The battle ends 210 frames (7 s) after the deciding destruction (`end_tick`). The simulation records that frame and keeps stepping; the viewer's live session halts at it and shows the end of the battle ([battle end](battle-end.md), victory display and battle-end delay). Anything else that happens during the countdown (FoC also scales damage while a victory is pending, space-damage DG-02) is not modelled. | research VE-01, VE-09; project |
| VT-12 | A unit removed by scenario staging (`stage_remove`, the recorder's deletion) is not destroyed and decides nothing; a staged star base counts from the tick it is staged. | project |

### Project choices

| Rule | Choice |
|---|---|
| VP-01 | The evaluation is one serial pass at the end of the tick over the tick's `unit_destroyed` events and the counted star bases (at most a few), never over every unit, so it is not a per-entity loop ([phase map](../simulation.md#phase-map)). It reads only the committed destruction order, so its result is the same for any worker count. |
| VP-02 | Enemies and allies are teams: a player on another team is an enemy, a player on the same team an ally, and a player is its own ally (**unverified**: the retail self-ally status was not traced, G-V3). Retail puts the non-playable players on no team; the remake gives each its own team ([skirmish start](../skirmish-start.md)). |
| VP-03 | The replay format does not record which players are human. The skirmish start knows it (slot 1 in M2); `sim_headless --replay` with `--game-root` takes the pinned fixture's human slots. In a one-against-one battle the result does not depend on it (VT-06). |

## Cases

`tests/replay/victory_tests.cpp` (`tactical_victory_contracts`) checks VC-01 to VC-05 on the
evaluation and replays VF-1 to VF-5 (a Rebel human, an Empire AI and a non-playable Pirates
player, each lobby player with a star base and a ship) with 1, 2 and 4 workers, scrambled storage
and a written and parsed replay; the final hashes are pinned in
`tests/replay/fixtures/tactical-victory.hashes.csv`. With `EAWR_EAW_GAME_ROOT`,
`skirmish_start_contracts` also destroys each M2 station in turn.

| Case | Input | Expected |
|---|---|---|
| VC-01 | One human against one AI; either star base falls | The other player wins (VT-05) |
| VC-02 | A non-playable player's star base falls; the AI's stands | Nothing is decided (VT-03, VT-07) |
| VC-03 | Human and two AIs, each on its own team: an AI's star base falls, then the other AI's | Undecided (a human's and an AI's star base stand), then the human wins; had the human's fallen first, the lowest AI would win at once (VT-06) |
| VC-04 | Three AIs, no human; one star base falls | The lowest enemy AI wins at once (VT-06) |
| VC-05 | Human and AI against two AIs | An enemy loss decides nothing while the human's ally stands; the human's own loss decides nothing either; once the human's team has no star base the lowest enemy AI wins (VT-05, VT-06, VP-02) |
| VF-1 | The Pirates' star base and container, the Empire ship, then at tick 5 the Empire star base; the Rebel star base at tick 8 | The Rebel wins at tick 5, ending at 215; the tick-8 loss changes nothing (VT-09) |
| VF-2 | The Rebel star base at tick 4 | The Empire wins (defeat of the human) |
| VF-3 | Both star bases in one command, Rebel's listed first | The Empire wins (VT-08, VT-09) |
| VF-4 | Both star bases at tick 6, by two commands (player 1 hits the Empire's first) | The Rebel wins |
| VF-5 | Ships and the Pirates' objects only | No outcome; hashes equal a session without victory rules |

## Unknowns

| Gate | Unknown | Effect |
|---|---|---|
| G-V1 | Retail tests at the moment an object is destroyed; the remake's destruction order within a frame is its event order (VT-08). | A frame with two star base losses may pick the other winner than retail if retail destroys them in another order. |
| G-V2 | Whether retail ever lets a non-playable player win (VT-07). | None in the tested line-ups. |
| G-V3 | Retail's ally relation of a player to itself and of the non-playable players (VP-02). | Team games with a human: if a player is not its own ally in retail, an AI ally of a human wins earlier there (VC-05). |
| G-V4 | The countdown's effect on the battle (VT-11). | The remake keeps fighting after the outcome; nothing can change it. |
