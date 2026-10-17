# Campaign space retreat: cues, countdown line and station explosion

Presentation companion to the [retreat frame schedule](gc-retreat-frame-schedule.md) (WR-42..56). Evidence is read-only
**debug build** reading (the retreat coordinator's start and cancel paths, the command bar's retreat press, the countdown
line of the play UI and the station self-destruct service) plus the effective XML. WR-42 keeps skirmish and multiplayer
tactical retreat out; these rows apply to the campaign tactical battle.

| ID | Rule | Evidence |
|---|---|---|
| RP-01 | An accepted player retreat start (not the automatic forced retreat) plays its start cue for the local player once. The cue is non-positional and starts at once; nothing is attached to a unit. | debug build; WR-44 |
| RP-02 | The retreating local player hears the first roster member's type cue `SFXEvent_Retreat_Start` (the roster in start order, the first member with one); with none, the **local** faction's `Space_Retreat_Begin_SFXEvent`. A local player who is not the retreating one but is its enemy hears the local faction's `Space_Retreat_Enemy_Begin_SFXEvent`; an ally hears nothing. Both faction cues belong to the local player's faction, not the retreating one's. | debug build; WR-44 |
| RP-03 | A cancel of an underway countdown plays one cue. A local player's own cancel prefers the first roster member's type cue `SFXEvent_Retreat_Cancel`; every other cancel, an enemy's included, plays the **retreating** player's faction `Space_Retreat_Cancel_SFXEvent`. A cancel made by the game (not a player command) skips the type cue. The automatic forced retreat plays nothing. | debug build; WR-44 |
| RP-04 | The cues are heard by the local player wherever the camera is: fog, distance and the retreating player's visibility do not gate them. | debug build |
| RP-05 | A refused local press of the retreat button plays one cue of the local faction: the reason cue of the refusal when it has one (`Space_Retreat_Not_Allowed_Reason_1` gravity well, `_2` a hero preventing, `_3` a tactical superweapon), else the generic `Space_Retreat_Not_Allowed_SFXEvent`. An authored blank reason cue still replaces the generic one and plays nothing (stock Rebel, Empire reason 3). The generic cue covers the other refusals of the request (opening lockout, a retreat already underway). | debug build |
| RP-06 | While a retreat is underway (countdown or escape) and no cinematic plays, the local screen shows one line at the tactical countdown position: x and y are the shares `GUI_Tactical_Countdown_Timers_Screen_X` and `_Y` of the screen (stock 0.005 and 0.10), in the countdowns font. | debug build; `GameConstants` |
| RP-07 | The line is the **retreating** player's faction text `Space_Retreat_Countdown_Text_ID`, a space, then the remaining countdown as whole seconds and thousandths, each padded with zeros to three digits (`012.345`). The remaining time is the 32-bit float quotient of the remaining frames over the logical frame rate; the thousandths are the truncated fraction times 1000. The escape phase reads `000.000`. | debug build |
| RP-08 | The line's colour is the retreating faction's `Space_Retreat_Countdown_Color_RGBA` (four 0..255 channels; stock white). | debug build |
| RP-09 | At the station-wait boundary of FS-R06 each living owned station with a `Retreat_Self_Destruct_Explosion` creates that object at its z-adjusted target position, facing the station's facing, before it receives its lethal damage. The object is a projectile whose shooter is the station: its instant area blast (stock `Self_Destruct_Star_Base_Level_N_*`: range and damage 1200 + 200 per level, damage type `Damage_StarBase_Self_Destruct`) damages what is near, so the explosion is **gameplay**, not presentation only. The projectile's detonation particle and sound are the authored ones. | debug build; FS-R06 |
| RP-10 | `Space_Surrender_SFXEvent` has a tag-table row but no accessor or reader in the debug build; it plays nothing (as does `Land_Surrender_SFXEvent`). | debug build (no reader found; unverified beyond a name search) |

## Remake

- `ui::retreat_start_cue`, `retreat_cancel_cue`, `retreat_refusal_cue`, `retreat_countdown_visible` and
  `retreat_countdown_line` (`include/eawr/presentation/ui/retreat_presentation.hpp`) hold RP-02/03/05..08 as pure functions.
- The viewer reads the Faction tags through the catalog layers (a mod's override wins) and plays the cues from the retreat
  state each published snapshot carries.
- RP-09 is a sim event: the station's `CombatProfile::retreat_explosion` launches the authored projectile at the retreat's
  station boundary. A campaign battle with a retreating station therefore moves its tactical hashes from that tick on;
  nothing else moves (a type without the tag, or any battle without a retreat, is byte-identical).

## Unverified and open

- RP-05 reason 1 (gravity well): the remake's retreat refusals have no gravity-well source yet, so that cue is not played.
- Which refusal the original maps to reasons 2 and 3 beyond their enum names (hero preventing, superweapon present) is read
  from names; a rig recording would confirm it.
- A refusal by a mod that changes the opening lockout still plays the generic cue (the original's too-soon refusal does).
- A cancel that comes from a newly active prevention ability (FD-R01) is not distinguishable from a player cancel in the
  published retreat state; the type cue (none in stock data) would play for the retreating local player in that case.
- The line's font and exact baseline use the pause banner font; the original's countdowns font is not wired separately.
