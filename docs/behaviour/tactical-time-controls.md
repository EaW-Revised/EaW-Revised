# Tactical time controls: pause and game speed

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, a tactical battle in skirmish or single
  player. The claims come from the FoC debug build, read on 2026-09-28 under the
  [clean-room rule](../clean-room.md); the private evidence map (IDs TE-01 to TE-12, with the (legacy EAWR-617) flash/final-pose reread) is under
  the ignored `out/research/`. Data claims name the file. Pause and speed controls, gap 10 of the Phase 2 plan.
- Bounded question: what the time panel's pause and fast-forward buttons and the game-speed
  setting do, which keys drive them, and what still works while the battle is paused.
- Source tags: **research** (debug build), **data** (a FoC file), **project** (a remake choice)
  and **unverified** (not established; the remake takes the least visible behaviour).
- Related: the logical rate and the speed steps in [tactical tick rate](tactical-tick-rate.md)
  (TR-01, TR-02); the HUD art in [ui-layer](../ui/ui-layer.md) section 1.3.

## Interface

- Input: presses of the time panel's two buttons (`b_play_pause_t`, `b_fast_forward_t`), the
  tactical speed setting (a profile option, five steps), and the game mode (skirmish or single
  player against network multiplayer).
- Output: how many logical frames run per wall-clock second, or none while paused. One logical
  frame is always 1/30 s of game time (TR-01): nothing here changes what a tick does.
- Retained state: the time state (play, paused or fast forward) and the speed setting. Neither
  is simulation state; neither enters a command, a replay or a state hash.

## Rules

| Rule | Behaviour | Source |
|---|---|---|
| TM-01 | The tactical speed setting has five steps, 0 to 4, default 2. A step selects a target of 10, 20, 30, 45 or 60 logical frames per wall-clock second. | research TE-01, TE-02 (TR-02) |
| TM-02 | The game waits at least `1000 / target` whole milliseconds (integer division) between two logical frames, so the steps run at most 10, 20, 30.3, 45.5 and 62.5 frames a second. | research TE-03 |
| TM-03 | Fast forward replaces the target with 120 frames a second (an 8 ms wait, at most 125 frames a second) while it is on. A playing cinematic forces 30. | research TE-03 |
| TM-04 | The time state is play, paused or fast forward. It starts at play. Pause and fast forward refuse to work in network multiplayer (LAN, internet, Steam); in skirmish and single player they work. | research TE-04, TE-05 |
| TM-05 | The pause button is a toggle (`Toggle` yes in `CommandBarComponents.xml`; tooltip "Pause/Play the game"). Selecting it pauses; deselecting it plays. Pausing from fast forward ends fast forward first. Playing from paused returns to play at the speed setting, never to fast forward. | research TE-04, TE-06; data |
| TM-06 | The fast-forward button is a toggle too (tooltip "Click to toggle accelerated game time"). It acts on the mouse press, not the release. It turns fast forward on only from play, and off again from fast forward; while paused it does nothing. | research TE-05, TE-07; data |
| TM-07 | Paused means no logical frame runs: the frame counter holds, so every unit, projectile, timer and the victory countdown stop. The display, the camera and the interface keep running. | research TE-08 |
| TM-08 | While paused the pause button flashes, and the fast-forward button is disabled and tinted grey (128, 128, 128). Playing stops the flash, enables the fast-forward button and takes the tint away. | research TE-04 |
| TM-09 | Pausing shows the pause banner: the `pause_shell` shell (`i_attack_mode.alo`) with the text `TEXT_GAME_PAUSED` ("Game Paused") in `Battle_Pending_Message_Color` (255, 32, 32, 240, `GameConstants.xml`) and a button reading `TEXT_BUTTON_RESUME_GAME` ("Resume Game") that plays again. In a tactical battle the banner's show animation jumps to its end. Playing hides it. | research TE-04, TE-09; data |
| TM-10 | Orders given while paused are accepted and wait: no order path looks at the pause, and a command waits in the command queue for the next frame that runs (one tactical action, selling a structure, is refused while paused). | research TE-10 |
| TM-11 | While paused the tactical camera's smoothed distance, field of view, height and spline position jump straight to their targets instead of easing towards them. | research TE-11 |
| TM-12 | No key drives pause, play or fast forward: the default key map has no time command, and only the two buttons (and restoring a saved game) call them. The Escape menu has a pause of its own (the in-game menu, battle dialogs). | research TE-12 |
| TM-13 | Pausing also pauses movies, positional and looping sound, speech and weather audio; playing resumes them. | research TE-04 |

### Project choices

| Rule | Choice |
|---|---|
| TP-01 | The simulation has no wall clock (TR-06). The live session's host paces it: in real time, the thread steps one tick per TM-02 interval of the current target and holds while paused. Changing the target or pausing restarts the pacing clock at the current tick, so no tick is made up or skipped. |
| TP-02 | The time state and the speed setting are presentation state. They never enter a command, the replay or a hash, so a run with pauses and speed changes has the hashes of the same commands at the same ticks without them (`tactical_live_session_contracts`, pause and speed case). |
| TP-03 | Orders given while paused are stamped for the next tick to run (the UI-07 scheduler's open tick does not advance while no tick runs), which is TM-10. |
| TP-04 | Each change is recorded in a time track: the tick the next step would run and the new state and target. The live session report lists it (`live_session.time`), and `--eawr-live-replay-out <file>` writes it beside the replay as `<file>.time.csv` (`tick,state,ticks_per_second`). The replay file itself is unchanged. |
| TP-05 | `--eawr-live-speed 0..4` sets the tactical speed setting (default 2), since the remake has no profile options page. |
| TP-06 | A driven capture run advances its presentation tick by `--eawr-live-step` times target / 30 per frame, and not at all while paused, so a scripted pause or fast forward shows the same ticks on every host. |
| TP-07 | The mounted pause shell is held at the final `IDLE_00` pose at the top centre, with the text at its authored bone and Resume Game in its authored button mesh. The additional Quit Game caption underneath remains project policy ((legacy EAWR-952)). Missing shell/clip data retains the diagnostic text fallback. |

## Cases

| Case | Input | Expected |
|---|---|---|
| TMC-01 | Speed setting 2, play | 30 ticks per nominal second; at most 30.3 a wall-clock second (TM-01, TM-02) |
| TMC-02 | Fast forward pressed in play | Target 120; pressed again: back to the speed setting (TM-03, TM-06) |
| TMC-03 | Paused, fast forward pressed | Nothing happens; the button is disabled (TM-06, TM-08) |
| TMC-04 | Fast forward, then pause pressed | Paused, fast forward off; play returns to the speed setting (TM-05) |
| TMC-05 | Paused at tick N, an order given, play | The order runs at tick N (the next tick) (TM-10, TP-03) |
| TMC-06 | A run paused and fast-forwarded against a plain run with the same commands at the same ticks | Equal per-tick hashes (TP-02) |

## Unknowns

| ID | Unknown | Effect |
|---|---|---|
| TM-U1 | Resolved by the (legacy EAWR-617) debug reread and (legacy EAWR-514) bright/dim captures: an additive `Flash_Texture_Name` quad, falling back to selected art, fades from full brightness to zero every 0.5 seconds on UI time while the base button keeps its normal/hover state. | Implemented by the presentation clock; play removes the flash. |
| TM-U2 | Resolved by the (legacy EAWR-617) debug reread, mounted `i_attack_mode_idle_00.ala` and (legacy EAWR-514) stills: tactical pause holds the final clip pose. | Native frame, text bone, button bounds, rollover art and 7-point component fonts are drawn. |
| TM-U3 | Whether a paused frame freezes the particle and animation clocks as well as the simulation. | The remake freezes everything the presented tick drives (effects, death clips) with the simulation. |

## Fidelity list

- TM-U1/TM-U2 are resolved by (legacy EAWR-617); lit remake captures are compared beside (legacy EAWR-514) retail stills.
- Audio pause and resume (TM-13) waits for the battle audio (battle audio foundation).
- The camera's smoothing snap while paused (TM-11): the remake's camera keeps easing its distance
  while paused.
- Multiplayer refusal (TM-04) is not modelled: the remake has no network game.
