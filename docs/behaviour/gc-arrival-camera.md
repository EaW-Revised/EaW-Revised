# Campaign space battle arrival camera

## Applicability

FoC campaign (Galactic Conquest) space battles, read from the **debug build** on 2026-11-12 (read-only). It adds the
camera half of the hyperspace arrival; the movement half (the 149-frame arrival move, hidden for its first 25 frames, the
frame-35 arrival sound) is PB-13 and [reinforcements walk](walks/reinforcements.md) WR-33..40. The retreat counterpart is
[gc-retreat-frame-schedule.md](gc-retreat-frame-schedule.md). Skirmish has no arrival camera (SK-ARR-01), and a
reinforcement arrival mid-battle starts none (no start path found: the cinematic is started once from the battle's
post-load step; the reinforcement path was not walked end to end). Rule IDs SK-ARR-nn are ours.

A live retail recording of the arrival has not been captured yet (open: see the Open list). Everything below is the debug
build's code and the shipped data; "unverified" marks what only a recording can settle.

## Rules

- **SK-ARR-01 Gate.** When a campaign battle finishes loading (not multiplayer; the debug build skips the step for a loaded save, see below), the pre-battle step runs
  the arrival cinematic if the `In_Game_Cinematics` game constant allows cinematics, pre-battle cinematics are enabled and the
  arrival is not held. It needs a parent (galactic) mode and a conflict lead object. A multiplayer battle builds no cinematic.
  Held arrival: a story-linked battle whose pre-battle mode is "held" (LINK_TACTICAL option 9 = 2, WST-50) waits for the
  story script's `Resume_Hyperspace_In` (GA-07) and plays no camera. If no cinematic starts the ordinary pre-battle fade-in
  runs instead.
  **Fresh battles only (debug build, loaded saves skip the step).** A battle restored from a save past its start plays no
  arrival. We admit a battle whose first ready tick is 0 (a fresh start, or the prebattle autosave, whose prebattle
  presentation the walk restores, WGS-64); a battle ready at tick > 0 is refused. The cinematic waits for the battle's loading
  screen to go; if the camera is not drawable at tick 0 it starts late and plays the remainder (it is a function of the
  presented tick).
- **SK-ARR-02 Geometry and timer.** The destination is the conflict lead's destination. The entry point is the destination
  plus the departure direction times the full-speed span plus the slow-to-target span of the hyperspace table (WR-38):
  75 frames x 8.125 = 609.375, plus the eased last 30 frames (sum of 8.125 - 8.125 E((k-119)/30) for k = 120..149, E the
  WR-38 ease), about 704 together. The departure direction is the lead's facing reversed (the default direction is -X turned
  by the facing's Y then Z angles). The entry point uses only the last two phases of the table (the debug build's two
  distance fields), not the 750-per-frame jump of frames 25..34 or the slow-down of 35..44: it is where the fleet leaves the
  jump, about 700 units out, not its hyperspace start. The cinematic timer is **5.0 s = 150 logical frames**; the camera keys end at frame 120.
- **SK-ARR-03 The local player is the ATTACKER.** The start pose C is the tactical camera's position when the cinematic
  starts (read after one render service, so it is the map's start pose). A random bit chooses one of two shots; both share
  the target keys: target = entry point from frame 0 to 23, then (linear, eased) to the destination at frame 120.
  - Shot 0: camera key 0 = destination + 0.05 x (entry - destination), plus 300 units sideways on the map plane (the span's
    horizontal turned a quarter turn, the side chosen by the span's X sign), plus 70 in Z. Held (cut) to frame 35, then a linear
    move to C at frame 120. Ease: speed up over the first half of the segment, slow down over the last half (E(0.5, 0.5)).
    Target ease is (0.5, 0.5) here.
  - Shot 1: camera key 0 = entry + (entry - destination), Z = entry Z - 70. Held to frame 35, then a Hermite spline to C at
    frame 120 (SK-ARR-12). Segment ease (0.5, 0.5). Target ease is (0, 0.5) here.
  - After frame 120 the camera stays on C until the timer ends at 150.
- **SK-ARR-04 The local player is the DEFENDER.** The look-at point T is the defender's first star base object (found by its
  behaviour, ground height 0); without one it is the camera's current look-at point and keeps its own height. Camera key 0 =
  T plus the horizontal offset (T - C) with its X and Y **swapped**, plus 20 in Z. A rotation about T (SK-ARR-11) in the
  reverse (long, clockwise) direction runs with ease-out 1/2 to key 1 = C at frame 120 with ease-in 1/2. The target is T throughout.
  In the debug build the camera is first pointed at the star base at the closest zoom before C is read (unverified what C is
  then; see UAR-02).
- **SK-ARR-05 Screen state.** While the cinematic plays: letterbox bands on and the screen faded out then in (the pre-battle
  step sets both "on delayed"; the fade-in timing is unverified); the command bar, radar map and galactic radar, fog-of-war
  rendering and occlusion silhouettes are off, the mouse pointer is hidden in space, the camera is saved and its frustum set
  to near 1.0 / far 10000, the frame synchronizer is told a cinematic runs, and in space the local selection is cleared. All
  restore when it ends. Objects are forced visible (arriving units are hidden in their first frames, PB-13).
- **SK-ARR-06 Input.** The pre-battle cinematic runs with the manager's "in-game" mode and its **ignore-input flag set**: every
  mouse and key event is consumed, including Esc. The player cannot skip or steer it. In the viewer the galactic layer's
  escape menu and quick save/load keys (Esc, F5, F6) also wait while it plays, as does all other input; an escape menu
  already open keeps working. The dropped input never unfocuses the battle camera, so camera input works as soon as the arrival ends. (Verified in the manager's event
  handlers; the player-visible feel is unverified without a recording.)
- **SK-ARR-07 Data or code.** Camera keys, 300, 70, 0.05, 20, the frames 23/35/120/150, the ease values, near 1 / far 10000 and
  the random bit are code constants. Nothing is loaded from XML or an animation file by this starter. The only authored input is
  the `In_Game_Cinematics` constant. No tag-registry row changes.
- **SK-ARR-08 Death Star.** A campaign battle whose invader owns a Death Star with a potential target gets a separate
  superweapon-attack cinematic instead. Not modelled.
- **SK-ARR-09 Shot choice.** The debug build draws the bit from a shared cinematic random stream the simulation never reads.
  We use a presentation-only hash of the conflict identity: a replayed battle shows the same shot. The simulation never reads it.
- **SK-ARR-10 Key lookup.** The first key whose frame is above the presented frame ends the segment; the segment's transition
  (the first key's type) and eases decide the position: cut = the first key, linear, spline, or rotation. After the last key the
  camera sits on it. The ease is a trapezoid velocity profile: ramp up over the first key's ease-out, hold, ramp down over the
  second key's ease-in, area one.
- **SK-ARR-11 Rotation.** Positions are taken relative to T. The horizontal angle from key 0 to key 1 is in [0, 360),
  counter-clockwise. A normal sweep turns angle x e; the reverse sweep turns 360 - (360 - angle) x e (clockwise, the long
  way). Radius and height are interpolated by the same e. Zero horizontal length falls back to a linear move.
- **SK-ARR-12 Spline.** Hermite basis with tangent = 0.5 x (next - previous) in the interior and 0.5 x (neighbour difference) at
  the ends (the default key tension is 0.5).
- **SK-ARR-13 Held and story-linked battles.** The held-arrival mode is the story engine's (WST-50, GA-07) and is not
  published to the viewer yet; **we play no arrival camera for a battle with story-linked options** until it is.
- **SK-ARR-14 Pacing and layer.** Presentation only: the camera is a function of the presented battle tick (30 frames per
  second, FC-R03 pacing while it plays) and the sim never hears of it. Hashes, replays and checkpoints are unchanged.

## Our deviations (list, presentation-only)

- **UAR-01** Destination = the attacker entry marker nearest the first defending marker (the retreat cinematic's geometry);
  the debug build uses the conflict lead's destination (the lead's formation position), unverified equal.
- **UAR-02** The defender's rest pose C is the player's tactical start pose; the debug build points the camera at the star base
  first. Least visible reading until a recording settles it.
- **UAR-03** No fade-in from black, no sound change (SK-ARR-05 fade timing unverified).
- **UAR-04** The units are not hidden for the first frames: the campaign arrival move itself is not implemented (PB-13), so the
  fleet is already in place while the camera shows its entry point.
- **UAR-06** The mouse pointer is not hidden and occlusion silhouettes are not switched off (SK-ARR-05): the viewer's
  battle UI hide covers the command bar and radar only.
- **UAR-05** Shot choice from a hash (SK-ARR-09). Story-linked battles play nothing (SK-ARR-13).

## Open

- Retail recording of a campaign space battle start (attacker and defender) to settle letterbox/fade timing, the defender's C
  and the shot look (SK-ARR-04/05, UAR-01/02/03).
