# XML tag coverage

Generated from [statuses.json](../statuses.json). For how coverage is assessed, see the
[registry guide](../../tag-coverage.md). These are registry claims, not a runtime test of every tag.

Counts are **(object class, tag) pairs**, not unique tag names or JSON rows. A row shared by
three classes contributes three pairs. Space skirmish excludes `land-or-galactic`,
`multiplayer` and `presentation-later` rows; it includes `deferred` rows so postponed battle
work remains visible. Mixed classes retain the registry's classification. This is a scope
view, not a census of tags used in a particular battle.

## What is used and what is not

The registry covers 8,208 class/tag pairs. It records 1,928 as applied and 1,059 as partly applied; 1,252 still need implementation. Parsing a value alone does not count as using it.

The remaining pairs are 457 presentation-later, 251 foc-ignores, 75 deferred, 10 by-design, 3,172 land-or-galactic, 4 multiplayer. Scope labels describe postponed work, not proof that the game ignores those values.

By area (all scopes):

- **ai**: 112 pairs; 37 applied, 31 partial, 5 todo, 39 ignored, replaced, deferred or outside space skirmish.
- **combat**: 3,184 pairs; 485 applied, 359 partial, 433 todo, 1,907 ignored, replaced, deferred or outside space skirmish.
- **data**: 74 pairs; 74 applied, 0 partial, 0 todo, 0 ignored, replaced, deferred or outside space skirmish.
- **economy**: 1,109 pairs; 338 applied, 287 partial, 215 todo, 269 ignored, replaced, deferred or outside space skirmish.
- **fighters**: 89 pairs; 16 applied, 13 partial, 16 todo, 44 ignored, replaced, deferred or outside space skirmish.
- **movement**: 491 pairs; 133 applied, 105 partial, 134 todo, 119 ignored, replaced, deferred or outside space skirmish.
- **presentation**: 3,149 pairs; 845 applied, 264 partial, 449 todo, 1,591 ignored, replaced, deferred or outside space skirmish.

The largest outstanding groups are:

- **presentation / todo**: 386 pairs, including @Name, Activated_System_Spy_Ability_Names, Allow_Particle_Model_Culling (legacy EAWR-653).
- **combat / todo**: 302 pairs, including *, */*, @Description (legacy EAWR-650).
- **combat / partial**: 194 pairs, including Abilities/Blast_Ability/Activation_Style, Abilities/Buzz_Droids_Ability/Activation_Max_Range, Abilities/Buzz_Droids_Ability/Activation_Min_Range (legacy EAWR-4532).
- **economy / partial**: 159 pairs, including Abilities/Combat_Bonus_Ability/Stacking_Category, Abilities/Maximum_Firepower_Attack_Ability/Activation_Style, Abilities/Maximum_Firepower_Attack_Ability/Applicable_Unit_Categories (legacy EAWR-4532).
- **presentation / partial**: 128 pairs, including Abilities/Blast_Ability/SFXEvent_Activate, Abilities/Buzz_Droids_Ability/Activate_SFX, Abilities/Combat_Bonus_Ability/@Name (legacy EAWR-4532).
- **movement / todo**: 119 pairs, including Apply_Y_Turret_Rotate_To_Axis, Apply_Z_Turret_Rotate_To_Axis, Attack_Move_Response_Range (legacy EAWR-649).
- **combat / todo**: 113 pairs, including Abilities/@SubObjectList, Abilities/Base_Power_Ability/Activation_Style, Abilities/Battlefield_Modifier_Ability/Disable_Reinforcement_Vulnerability (legacy EAWR-760).
- **economy / todo**: 103 pairs, including Blob_Shadow_Below_Detail_Level, Bombardment_Blob_Size, Bombardment_Required_Orbital_Ships (legacy EAWR-654).

Each area's tables below name the missing kinds, current consumers and tracking ticket. An applied claim remains subject to consumer review; these totals are not runtime coverage.

## Status legend

| Status | Meaning |
|---|---|
| applied | The value reaches our simulation or presentation; parsing alone does not count. |
| partial | Applied for the listed kinds, with missing kinds still to implement. |
| todo | A behaviour remains to implement; the ticket tracks the work. |
| presentation-later | Presentation work outside the current space battle scope. |
| foc-ignores | The registry records evidence that FoC ignores this value. |
| deferred | Explicitly postponed; see the note or tracking ticket. |
| by-design | Our implementation intentionally replaces this mechanism; the evidence cites the rule that says so. |
| land-or-galactic | Land battle or galactic campaign work outside space skirmish. |
| multiplayer | Multiplayer work outside the current scope. |

## Totals by status

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 1928 | 1928 |
| partial | 1059 | 1059 |
| todo | 1252 | 1252 |
| presentation-later | 0 | 457 |
| foc-ignores | 251 | 251 |
| deferred | 75 | 75 |
| by-design | 10 | 10 |
| land-or-galactic | 0 | 3172 |
| multiplayer | 0 | 4 |
| **Total** | **4575** | **8208** |

## Totals by area

| Area | Space skirmish | Everything |
|---|---:|---:|
| [ai](ai.md) | 102 | 112 |
| [combat](combat.md) | 1319 | 3184 |
| [data](data.md) | 74 | 74 |
| [economy](economy.md) | 899 | 1109 |
| [fighters](fighters.md) | 61 | 89 |
| [movement](movement.md) | 414 | 491 |
| [presentation](presentation.md) | 1706 | 3149 |

Regenerate with `python tools/inventory/tag_report.py --batch`, then verify with `python tools/inventory/tag_report.py --check`. Do not edit generated pages by hand.
