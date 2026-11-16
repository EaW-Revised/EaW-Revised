# Space tactical super weapon: debug-build walk answers (Death Star, tactical part)

Companion to the Death Star tactical plan (legacy EAWR-6941). This note holds the answers of five read-only walks of the FoC
debug build (2026-11-13) that the tactical sequence, its switch button and its battle-end cues depend on. The sequence
itself (states, timings, gate, command) is the subject of space-tactical-superweapon.md once the first simulation slice
lands (legacy EAWR-6950); the rows here carry their own ids (`TSW-W..`) so they can be folded in without renumbering.

Evidence: "debug build" is the read-only walk or a played debug-build run, "XML" the stock data read in place. The played runs
(legacy EAWR-6949) are in the second table; what they did not settle is listed after it.

## Answers

| Rule | Behaviour | Evidence |
|---|---|---|
| TSW-W1 | **Unauthored planet default.** A planet type that does not author `Planet_Is_Destroyable_By_TSW` is destroyable: the type's constructor sets the flag to yes before any XML is read. Only a planet that authors No refuses the super weapon (stock: three planets). The same constructor defaults `TSW_Start_Pos_Match_Targets_Z` to yes. | Debug build (type constructor defaults); XML. |
| TSW-W2 | **Facing.** The "facing" state lasts one service frame: the object's full 3D facing (heading and elevation) is set at once to point from itself at its target, and the state moves on. There is no turn rate and no stop condition. The same instant facing is applied again at the frame the attack animation starts. During approach, ready and power-up countdown, the only turning is a slow turn toward a "super laser target" that exists only when a script has enabled it (default off; rate is a fixed constructor value of 10, no XML tag feeds it); stock battles never enable it, so the heading stays the one the facing state set. A defender's start (and an object with no target) faces the battlefield centre, yaw only. | Debug build (the helpers the service calls in each state). |
| TSW-W3 | **Approach step order.** Each approach frame the speed changes by the acceleration (`Max_Thrust` while approaching, minus that once ready), is clamped to `[0, Max_Speed]`, and the position advances along the line to the target by the speed, set directly. The in-range test (distance to target at or below `TSW_Attack_Distance_From_Target`) uses the distance measured before that frame's move. The speech threshold check only runs while the speed equals `Max_Speed` and range is not reached. | Debug build. |
| TSW-W4 | **Target prop destruction reads the prop's own tags.** There is no generic death path: the destruction step takes the target type's `Death_Explosions` list, picks one entry with the non-synchronised random stream (presentation only), creates it for the neutral player in the non-synchronised object manager at the prop's pose and triggers camera jitter there; plays the type's `Death_SFXEvent_Start_Die` as a plain, non-positional event; then removes the prop. Both tags are parsed straight into the fields the destruction step reads (checked by the tag table). Stock: all 37 planet backdrop props author `Death_Explosions` = one particle effect, `Death_SFXEvent_Start_Die` = one event and `Death_By_TSW_Replacements` = two remains props. A prop type without these tags destroys silently. | Debug build (destruction step, tag table); XML (`spaceprops.xml`). |
| TSW-W5 | **Leader-kill test.** At the same step, after the prop is gone and when a parent galactic mode and conflict exist, the planet is looked up; the leader-kill victory test passes only when the planet's current owner's faction leader type sits inside one of the transports landed on that planet (and, when the victory condition lists names, matches one of them). If it passes: a pending galactic victory is registered, a story-mode-triggered tactical victory condition is added and tested, both carrying a 30-frame value (its meaning was not read); then the planet is destroyed to ruins (destroy on) whether or not the test passed. | Debug build. |
| TSW-W6 | **Battle-end cues (`..._TSW_Present`).** The four faction cues (won/lost x land/space) are chosen in the battle-end sound routine, which is given the winner and the local player. It scans all players: for the local player it records whether a live Super-Weapon-Killer object (type flag) exists (not dead, not a death clone, not pending delete); for every OTHER player that is an enemy of the local player it records whether that player owns at least one object with the tactical super weapon behaviour (no liveness test). The scan stops at the first such enemy, so the killer flag is only evaluated when the local player comes earlier in the player order. Local player won: land with a TSW present -> `SFXEvent_HUD_Won_Land_Battle_Enemy_TSW_Present`; space with a TSW present -> `..._Won_Space_Battle_Enemy_TSW_Present` if the local player has a live killer object, otherwise the faction's "forced retreat due to super weapon" event; no TSW present -> the corruption-mission win cue if a corruption mission is active, else the plain land/space win cue. Winner is an enemy of the local player: land with TSW -> `..._Lost_Land_Battle_Enemy_TSW_Present`, space with TSW -> `..._Lost_Space_Battle_Enemy_TSW_Present` (no killer split), else the corruption-mission loss cue or the plain loss cue. The cue is the LOCAL player's faction's, played as a plain event. | Debug build (battle-end sound routine). |
| TSW-W7 | **Switch visibility and states.** Each UI update the switch is shown only when the command bar is in space mode, the local player's faction is the command bar's "evil faction" name (value not read here; presumably Empire: unverified), at least one tactical super weapon object exists (the first in the object list is used, whoever owns it) and no command-bar hide counter is set; otherwise it and the countdown readout are hidden. While shown, the switch's shell anchor is lifted by 8 units on Z (user bone transform). State ready and not yet fired: button enabled with the first alternate icon (`..._00`); any other state: base icon (`..._05`) and the button disabled; once the click has fired: the icon is left to the click animation and the button stays disabled. | Debug build (switch update routine); XML (`commandbarcomponents.xml`). |
| TSW-W8 | **Switch click and countdown readout.** A click (or the key command "destroy planet", which exists as a game command; whether the stock key map binds it is data and was not checked) runs one routine: it starts the button's animation (the five alternate icons `..._00..04` at 5 frames per second, no loop) with the click-request routine as the completion callback, sets a per-battle "fired" flag (never cleared in the walked code, so the button fires once) and hides the countdown readout. The request routine is the one in the plan (first object, no-op unless the local player owns it, next stage; a refusal plays the generic negative-feedback sound). The countdown readout is the command bar's super weapon display widget, a text button: almost certainly the `DS_Countdown_Timer` component (it carries the font and text offset; `deathstar_switch` carries none); its text and colour are set each UI update while approaching or ready, and it is a no-op once the flag is set. Its own render hook is empty. | Debug build (click routine, readout routine, update loop); XML. |

## Captured on the debug build (two played runs, 2026-11-13)

Both runs: galactic conquest (Equal Footing, Empire as the local player) on the debug build, lit, windowed 1280x720, the
engine's own battle HUD. A tactical super weapon is staged from the debug Lua harness: the scoring script spawns the
Death Star type for the Empire (at an enemy planet it opens a "space battle imminent" panel at the next update; during a
defence it is spawned beside an own station), then the battle is played with the real switch click. The harness read
`Is_Tactical_Superweapon_Ready` once a second and the engine log gave the frame numbers.

| Rule | Observation | Evidence |
|---|---|---|
| TSW-C1 | **Ready start.** In both runs the Death Star was ready (readiness true) from the first sample after the battle loaded, with the countdown readout at `000.000` in red. A Lua-spawned Death Star therefore behaves like the defender case of TSW-03 (the owner does not have to fly in). The attacker case (approach, ETA readout, the disabled switch) was **not** captured: it needs a real fleet move into the conflict, which did not fit the session (see "Capture notes"). | Debug build, played run; Lua readiness samples. |
| TSW-C2 | **Switch art, ready.** The switch sits in the first slot of the lower command panel (left end) while a super weapon object exists; with none the slot is empty. Ready look: lever up, one green lamp lit, four small lights dark. The countdown readout is a red `000.000` on a dial at the top centre beside the force-balance bar (so the readout widget is NOT drawn on the switch). | Debug build, lit stills (listed below). |
| TSW-C3 | **Switch art, fired.** After the click the lever is down, the lamp is dark and the four small lights are lit yellow; the hover text is the same single "fire the Death Star weapon" tooltip as before the click, so the three tooltip keys (TSW-W7) were not told apart (only the ready and fired looks were seen, the too-far look not). The fired look stayed for at least 170 s in one run; in the other a still taken about 3 minutes after the click showed the lever up and the lamp green again while readiness was false: unresolved (unverified whether the switch resets after the sequence). | Debug build, lit stills. |
| TSW-C4 | **Click to cinematic.** The click's command executes 4 frames after the click frame (the normal command latency). In that frame the power-up cinematic starts at once: the camera goes to a close shot of the Death Star's dish with the HUD hidden, a destruction-theme music stream starts (fade in 0.25 s), and readiness goes false within the next second. About 570 frames (19 s: 571 and 568 in the two runs) after the command the battle-music event for the Death Star starts and the normal camera and HUD are back. This is longer than the sequence's own timings (power-up 270 + attack 75 + debris delay 33 = 378 frames), so the cinematic or the post-wait defines it: not split further (unverified). The activation voice line named in the data is missing from the debug install's sound archive (logged, no sound). | Debug build, engine log frame numbers (two runs). |
| TSW-C5 | **After the planet goes (open item U1).** The planet prop is gone from every still after the sequence, the remaining ships keep fighting and no battle-end happens: at least 3 minutes in each run with enemy and own units still engaged and the objective panel unchanged. The sequence therefore does not end the battle by itself. The galactic side was not played on to the return. | Debug build, lit stills taken up to 3.5 minutes after the click. |
| TSW-C6 | **Target look after the destruction.** About 60 s after the click a faint blue ring glow still marks the old target position in one still; the replacement remains props were not identified in any still. | Debug build, lit still. |

## Still open (needs another capture or owner footage)

| Item | State |
|---|---|
| The attacker case: approach start, ETA readout (green), the disabled switch, the "too far" tooltip, speech at 60/30/15/5 s | Not captured (TSW-C1). A real fleet move into the conflict is needed. |
| Which of the three tooltip keys shows when | The too-far look was not seen (TSW-C3). |
| Whether the switch resets after the sequence | Contradicting stills (TSW-C3). |
| A look-up frame at the sky during the battle | Not captured: the step tooling has no camera pitch input and the session video helper failed. |
| Frames before and after the explosion at frame resolution | Not captured: a still takes 25-35 s; only the click frame (cinematic start) and later stills exist. |
| Placement of the readout, shell anchor | Settled for the readout (top centre, TSW-C2); the switch anchor is the first panel slot. |
| Whether the stock key map binds the "destroy planet" command | Key map is data; not checked. |

## Capture notes (for the next capture)

- The adapter's game task has a 15-minute execution limit by default and ends the game silently (the Lua harness then reports
  a lost connection): pass the longest session option (30 minutes) for a galactic run.
- The debug build's "all campaigns" toggle does not list the stock Death Star test campaigns in the menu; staging by Lua spawn
  worked instead. An engine-level script error (for example a wrong object-list filter) ends the scoring script and then the
  harness cannot run chunks: keep staging chunks to calls already proven.
- The session video helper failed with a transfer error on the host in both runs; stills only.
- Each lit still is a window screenshot, 25-35 s per step; the click step's own screenshot is not taken at the click
  instant, so the cinematic frame is luck.

## Capture index

Stills, Lua readiness samples and logs stay in the ignored `out/eyecheck/sk-6949/` of the coordinating checkout and are
never committed. `stills/`: `01` cinematic close shot (run 1, at the click), `02`-`04` run 1 after the click, `05`/`05b` run 1
switch and tooltip (client pixels and crop), `06` run 1 battle still going; run 2: `10`/`10b` ready, `11` ready (client
pixels), `12`/`12b` fired, `13` wide view during the sequence, `14` planet gone with a blue ring, `15`/`15b` and `16`/`16b`
fired look at +95 s and +170 s. `run1/` and `run2/` hold the Lua readiness logs.
