# Battle mouse pointers (UI-09)

Evidence was read from the debug build and the effective FoC data on 2026-10-01.
Research receipts remain in ignored `out/research/`; no retail art is distributed.

| Evidence | Source |
|---|---|
| CU-E1 | debug build: pointer initialization, state switching and animation service |
| CU-E2 | debug build: battle action to pointer resolution |
| CU-E3 | debug build: battle hover service and action classification |
| CU-E4 | FoC `MousePointerFiles.xml` and its `MousePointers.xml` include |
| CU-E5 | debug build: ion-shot target admission |

## Data and display

- **CU-01** (CU-E1, CU-E4): the file list supplies pointer definitions. `Name` identifies
  the pointer; `Base_Texture`, `Hot_X`, `Hot_Y` and `Anim_Frame_Delay` supply its
  art, click offset and animation delay. Load these through the player's VFS.
  Numbered art starts at 00 and ends at the first unavailable frame, at most 100.
  DDS counterparts satisfy TGA names. A name without a numeric slot is static.
- **CU-02** (CU-E1): changing pointer restarts at frame zero. Each animation service
  interval is 16 ms; the delay counts down before advancing, so a frame lasts
  `(Anim_Frame_Delay + 1) * 16 ms`. Pointer animation uses wall time, including pause.
- **CU-03** (CU-E1): dimensions and hot spots are texture pixels, independent of HUD
  reference scaling. Dividing these by viewport dimensions only positions the software
  pointer in normalized screen coordinates; increasing resolution does not enlarge it.
  Use OS hardware pointers with frame swaps (UI design D6). Diagnostic viewport captures
  may composite the same frame at the reported pointer position minus its hot spot.
  The viewer observes pointer events before GUI ownership, recording their position
  without issuing a world action; HUD-consumed motion therefore keeps the cursor current.

The viewer retains diagnostic strings only when cursor or hover values change; the
unchanged resolution/history path has an allocation-count regression after warm-up
and after the sample cap. If requested art is unusable, presentation uses normal art.
If normal art is also unusable, it restores the system arrow and hides the diagnostic
overlay. Captures and reports use the same effective art as the hardware pointer.
These are presentation fallback and performance guarantees, not claims about the
debug build's response to a broken pointer catalogue.

## Resolution order

The existing input hover owns the world pick. Reticles take priority over squadron icons,
which take priority over world meshes (WU-41, WU-23); cursors consume that result.

- **CU-04** (CU-E2): selection drag shows `POINTER_DRAG_SELECT_MODE`, ahead of camera
  scrolling (`POINTER_MAP_SCROLLING_MODE`) and rotating (`POINTER_MAP_ROTATING_MODE`).
  Edge scrolling alone supplies no alternate art: resolve the ordinary action.
  A held middle grab scrolls without Ctrl and rotates with Ctrl; changing Ctrl
  during the grab changes the pointer mode too (CU-E3).
- **CU-05** (CU-E2, CU-E3): a HUD component's explicit pointer wins when no ability
  targeting is active; over ordinary HUD/minimap chrome the ordinary pointer is used.
  World attack/move modes are overridden only outside the command bar. During targeting,
  HUD hover still uses its HUD action, rather than the enemy hidden beneath it.
- **CU-06** (CU-E2): attack-only mode (A) shows
  `POINTER_ATTACK_ONLY_MODE_UNIT_TARGETED` for an attackable hostile contact and
  `POINTER_ATTACK_ONLY_MODE_NO_UNIT_TARGETED` otherwise. Move-only mode shows
  `POINTER_MOVE_ONLY_MODE_PASSABLE_TARGETED` or its `NO_PASSABLE_TARGETED` counterpart.
- **CU-07** (CU-E2, CU-E3): selectable objects show `POINTER_SELECT` with no controllable
  selection; enemy hover alone stays normal under the current selection policy (S-1).
  Own movable units selected over empty passable space show `POINTER_MOVE`.
  A hostile ship, hardpoint reticle, or squadron icon shows `POINTER_ATTACK` (OR-26).
  Outside the inclusive playable map bounds the action is cannot-move, even without
  a selection. Armed attack/move modes retain their no-target/no-passable pointers.
  The adapter uses the hover ray's plane point and existing PU-31 bounds.
  Out-of-range ordinary attacks use `POINTER_ATTACK_OUT_OF_RANGE`, which FoC authors
  with the same art and hot spot as ordinary attack. Nonselectable empty space with no
  selection or enemy-only selection stays `POINTER_NORMAL` inside the map; a selected stationary
  object cannot imply a move. Own selectable contacts show select; selection alone
  never grants control of enemies.
- **CU-08** (CU-E2, CU-E3): Ctrl on a move action or armed attack-move shows
  `POINTER_ATTACK_MOVE`; Ctrl+Alt or armed guard shows `POINTER_GUARD` on guard actions.
  Ordinary hostile attacks still show attack. Alt on move actions shows
  `POINTER_WAYPOINT_PLACEMENT`. Guard/escort therefore does have distinct FoC art.
- **CU-09** (CU-E2, CU-E3): targeted enemy abilities, including the Y-wing ion shot,
  show `POINTER_TARGET_SPECIAL_ABILITY_TO_ENEMY_OBJECT` or the `_INVALID` variant.
  Friendly-object, passable-terrain and space-position targeting have corresponding
  valid/invalid definitions. Validity includes the ability's target predicate, fog and
  authored range, where applicable; the pointer must use the activation's predicate.
  The ion-shot predicate checks hostility and the authored type filter, rejects a
  target in hyperspace or an incompatible hero duel, and requires a target model
  (CU-E5, AB-62). These checks belong to ability admission, not an extra cursor pick.
- **CU-10** (CU-E2, CU-E3): a valid reinforcement point shows
  `POINTER_REINFORCEMENTS_LANDING_POINT`; an invalid move location shows
  `POINTER_CANT_MOVE`. Use the placement preview's already evaluated predicate (WR-13).
- **CU-11** (CU-E2, OR-26): there is no hostile hardpoint-specific cursor. Own damaged
  hardpoints not already repairing can show `POINTER_REPAIR_HARDPOINT`; that requires
  a repair action and must not be implied by hovering any friendly ship.

- **CU-12** (CU-E2, CU-E4, BARR-01): BARRAGE and WEAKEN_ENEMY arm space-position
  targeting, using `POINTER_TARGET_SPECIAL_ABILITY_TO_SPACE_POSITION` and its
  `_INVALID` variant. Empty space can be valid without an enemy under the pointer.
  Hover and click use one point query. BARRAGE reads logical fog cells, including
  rejection outside the grid; without a grid it uses published allied sensor ranges.
  The presentation reveal switch does not bypass this admission. WEAKEN_ENEMY
  retains the simulation's authored-radius check and zero-point fallback (U-10).
  Invalid point clicks cancel targeting with refusal feedback and submit no command.

## Scope

All definitions are loaded, including land/galactic, wait, drag/drop, superweapon,
beacon, garrison and repair art. Their actions are selected only when input exposes
those modes; loading art does not add commands. Existing M2 targets are enemy-object
abilities and world-point BARRAGE/WEAKEN_ENEMY. Friendly/terrain target routing must be
supplied by future input modes. CU-09 is only partially implemented: the current input layer classifies
ability hover and clicks by hostility. Simulation's private AB-62 ion-shot admission
also requires a combat profile, but there is no shared admission query including
the authored type filter, hyperspace, duel and other applicable restrictions. A
shared evaluated predicate must cover these before full CU-09 parity can be claimed;
the ordinary hostile-ship and empty-space GPU pair does not establish that parity
(legacy EAWR-578). These restrictions must not be invented by the cursor adapter.
CU-12 matches current simulation point admission. BARRAGE's separate debug-build
firing predicate checks weapon reach, movement, weapon-hit and turret state; those
checks do not establish an activation range gate. WEAKEN_ENEMY's U-10 placement
range remains unverified.
The adapter uses the ordinary attack ID for hostile hover: FoC's out-of-range
definition has identical art and click offset, so the cursor adds no range query.
