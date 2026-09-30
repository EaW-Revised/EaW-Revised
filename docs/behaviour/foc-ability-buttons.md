# FoC ability buttons in the tactical command bar (EAWR-454)

## Applicability

The ability buttons of the Forces of Corruption space tactical command bar on the pinned FoC build:
which buttons a selection shows, what each draws (icon, recharge dial, autofire mark, disabled
state), the ability marks on the unit cards, and what a click or hotkey on them does. The rules were
read in the FoC debug build (the command bar's tactical selection update, its per-group ability
button update, the ability state and recharge queries, the button and icon components' render and
animation, the build dial geometry, the component action table, the ability activation and autofire
toggles, and the default key map and its dispatch) unless marked as project policy. Evidence IDs
ABE-1 to ABE-10 map to research notes kept outside the repository. Textures and offsets are from the
FoC `CommandBarComponents.xml`.

The remake implements it in `presentation/ui/ability_buttons.hpp` (buttons, card marks, hotkeys,
clicks), `presentation/ui/hud_shell.hpp` (the button components), the Godot `EawrAbilityButtons`
control and the viewer's battle input. It is presentation only until the simulation has ability
state (EAWR-76): see "Interface to the simulation" below.

## Interface

Input: the unit card layout (docs/behaviour/foc-unit-cards.md), each card unit's first and second
unit ability, and each unit's ability state (status, recharge completion, autofire).
Output: the ability buttons and card marks to draw; on a click or hotkey, an activation or autofire
request for the units of one ability group.

## Buttons

- AB-01 (ABE-1). Each ability group of the cards (docs/behaviour/foc-unit-cards.md L-5) gets one
  button, `special_button_<first column + last column>`, centred under its border. A group whose
  units have a second ability gets a second button, the next component, and both are shifted 14
  shell units left.
- AB-02 (ABE-2). A group with ability none gets no button. A button is hidden when a unit of its
  group has no state for the ability; it is disabled (the `Disabled_Texture_Name` art) when every unit
  of the group has it disabled.
- AB-03 (ABE-3). A unit's ability state is, in order: recharging (with its completion from 0 to 1)
  while its countdown runs; disabled when the ability cannot be used; active; ready.
- AB-04 (ABE-4). The icon is the unit type's ability `Alternate_Icon_Name`, else the engine's icon
  for the ability type (a built-in table: DEFEND `i_sa_defend_mode.tga`, TURBO
  `i_sa_power_to_engines.tga`, POWER_TO_WEAPONS `i_sa_power_to_weapons.tga`, ION_CANNON_SHOT
  `i_sa_ion_cannon_shot.tga`, SPOILER_LOCK `i_sa_s_foil_mode.tga`, ...). An ability without an icon
  in the table draws none.
- AB-05 (ABE-2, ABE-7). The recharge dial is the button's `Build_Texture_Name`
  (`i_button_sa_recharge.tga`) at the group's largest recharge completion. A completion of 0 (no unit
  recharging) counts as 1, which draws no dial. The dial is the texture cut to a clock sweep: from
  twelve o'clock clockwise, the part from the completion to the full turn is drawn, additively.
  While a timed ability (one with an expiration) is on, its countdown runs backwards from the start of
  its duration, so the dial's completion is the share of the duration left, falling from 1 to 0: the
  drawn part of the dial grows as the ability runs, and the recharge countdown then takes over and
  shrinks it again (debug build: the ability countdown's service and expiration timer, and the card
  update that shows the icon and dial for the countdown's completion). An untimed ability that is on
  (S-foils, `SPOILER_LOCK`) draws no dial. The card's dial (AB-08) reads the same completion.
- AB-06 (ABE-2, ABE-5). When every unit of the group has the ability on autofire, the button's
  upper effect cycles through its `Icon_Alternate_Texture_Name` frames (`sa_ami_outline_00` to `_09`)
  at `Anim_FPS` (5), looping; otherwise it has no upper effect.
- AB-07 (ABE-5, ABE-6). Draw order: base (`Blank_Texture_Name`), icon, mouse-over, the 0.2 s press
  flash (`Selected_Texture_Name`, fading), disabled, the upper effect, the recharge dial. Each quad is
  its texture's size times `Scale` around the component's bone.

## Card marks

- AB-08 (ABE-9). A card whose unit has its first ability active, recharging or on autofire shows the
  ability's icon at 0.75 scale at the card's `Icon_Offset` (-12, 15); recharging also draws the card's dial there
  (`Build_Dial_Offset`). On autofire the card shows its `Overlay_Texture_Name` (`i_sa_ami_box.tga`) at
  `Overlay_Offset`. The second ability does the same on the right: `Upper_Effect_Offset`,
  `Build_Dial2_Offset`, `Overlay2_Offset` (12, 15).

## Clicks and hotkeys

- AB-09 (ABE-8). A left release on a button requests the ability for its group: when every unit of
  the group has it active the request switches it off, otherwise on for the units that can. The
  abilities aimed at a target (ION_CANNON_SHOT, TRACTOR_BEAM, ENERGY_WEAPON, LUCKY_SHOT,
  CONCENTRATE_FIRE, SUPER_LASER, ...) enter targeting instead. A right release toggles autofire for
  the group: off when every unit has it on, else on.
- AB-10 (ABE-10). FoC's default key map presses every button whose ability matches (Shift, Ctrl and
  Alt are exact modifiers): DEFEND Shift+O, TURBO Shift+E, SPOILER_LOCK Shift+W, ION_CANNON_SHOT
  Shift+I, MISSILE_SHIELD Shift+M, TRACTOR_BEAM Shift+T, INTERDICT Shift+G, BARRAGE Shift+B, HUNT
  Shift+H, LURE Shift+L, SENSOR_JAMMING Shift+J, STEALTH Shift+C, LEECH_SHIELDS Shift+N, BUZZ_DROIDS
  Shift+Z, SUPER_LASER Shift+A, CLUSTER_BOMB Shift+K, LASER_DEFENSE Shift+P, SELF_DESTRUCT Shift+S,
  FULL_SALVO Shift+F, DEPLOY_SQUAD Shift+Q, POWER_TO_WEAPONS Ctrl+B, MAXIMUM_FIREPOWER Ctrl+M,
  SPREAD_OUT Ctrl+Z, INVULNERABILITY Alt+], CONCENTRATE_FIRE Alt+; and the land and hero keys.

- AB-11 (EAWR-561). Targeting. A left release (or the hotkey) on a targeted ability's button that
  would switch it on waits for a target instead of sending the request: the next left click on an
  enemy unit sends it with that unit (a squadron: the craft clicked) and acknowledges it like an
  attack; a left click on empty space or on an own or neutral unit, a right click or Esc cancels
  it and orders nothing. The selection does not change. FoC changes the mouse pointer while it
  waits (its `POINTER_TARGET_SPECIAL_ABILITY_TO_ENEMY_OBJECT` pointers); the remake has no pointer
  art yet (EAWR-578). The report's `battle_input.ability_bar` counts `targeted` requests and
  `target_cancels`, and says whether one is `targeting`.

## Interface to the simulation (EAWR-76)

The buttons read and write abilities only through two interfaces in
`include/eawr/presentation/ui/ability_buttons.hpp`; EAWR-76 implements them and hands them to
`BattleInput::set_abilities(state, commands)`.

- `AbilityState::state(unit, ability) -> std::optional<UnitAbilityState>`: for a card unit (a unit,
  or a squadron's container) and a `UnitAbilityType` value, its `status` (ready, active, recharging,
  disabled), `recharge` (the countdown's completion, 0 to 1, while recharging) and `autofire`; nothing
  when the unit lacks the ability (AB-02 then hides the group's button).
- `AbilityCommands::request(AbilityRequest)`: `kind` (activate, deactivate, autofire_on,
  autofire_off), `ability`, `units` (the group's card units in card order) and `targeted` (an
  activation that first takes a target, AB-09). The simulation decides which units can act.

Since EAWR-76 the live view hands `LiveSessionView::Abilities` to both. It reads the latest snapshot's
ability status ([space abilities](space-abilities.md) AB-50; a squadron's container stands for its
craft). A unit that is on is active; one that recharges shows the least-recharged holder's dial; one
held by its gate (a depleted shield, lost engines) is disabled; a cut ability (`HUNT`, AB-03) is
always disabled. A team ability (`ION_CANNON_SHOT`, space-abilities AB-60) reads the container's own
status instead of its craft's. A request becomes one ability command for the group's card units
through the order scheduler, so it enters the replay; the simulation rejects the units that cannot
act. A targeted request goes out only with the target AB-11 gave it. The report's
`live_session.ability_requests` counts issued and refused requests. `--eawr-live-ability-demo on`
keeps the stand-in `ReadyAbilities` with its staged active, recharging, disabled and autofire
states so they can be looked at; its requests only add a report line.

## Unverified / fidelity list

- The retail look of a cut ability's button (`HUNT`) is its normal one; the remake shows it disabled
  because it cannot act (space-abilities AB-03).
- The targeting pointer of AB-11 (EAWR-578, battle cursors), the activation and deactivation sounds, the
  tooltip and the hero and land key bindings are not implemented.
- Whether FoC's targeting also ends on a click on a friendly unit, or keeps waiting, is not traced;
  the remake cancels (the least visible choice: nothing is ordered).
- A remapped key map (the options' keyboard page) is not read; the defaults are used.
- The retail stills show only idle buttons (the X-wing squadron's S-foil button matches). The
  recharge dial, disabled and autofire looks have not been compared with retail footage.
