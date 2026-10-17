# Hardpoint presentation data

Audit of the marker path at `da75e162`, using the effective FoC XML and the
preview's FFT, CW Holdouts, Remake load order. Retail consumers below refer to
the reviewed rules in [battle world UI](foc-battle-world-ui.md#hardpoint-reticles),
not a new binary trace. Locations describe the code before the fix.

| XML input | FoC consumer | Project consumer at audit | Preview data and result before fix |
| --- | --- | --- | --- |
| `HardPoint_Target_Reticle_Enemy_Texture`, `HardPoint_Target_Reticle_Enemy_Tracked_Texture` | WU-32 enemy marker, normal / hovered | `src/presentation/ui/world_ui.cpp:393` hardcoded type table; `apps/viewer/src/world_ui_bars.cpp:313` appends `_tracked` | Laser maps to `I_HARD_POINT_RETICLE_WEAPONS_TURBO`, missile to `..._MISSILE`, torpedo to `..._TORPEDO`, ion to `..._ION`, mass driver to `..._MASS_DRIVER`; gravity well to `I_HARD_POINT_RETICLE_GRAV_WELL`. We instead request stock weapon/generic art. |
| `HardPoint_Target_Reticle_Friendly_Texture`, `HardPoint_Target_Reticle_Friendly_Tracked_Texture` | WU-32 friendly marker, normal / hovered | Same table and suffix as enemy | Same changed type mappings; authored team distinctions ignored. |
| `HardPoint_Target_Reticle_Friendly_Repairing_Texture`, `HardPoint_Target_Reticle_Friendly_Disabled_Texture`, `HardPoint_Target_Reticle_Friendly_Disabled_Tracked_Texture` | Friendly repair and disabled marker states (WU-51) | Same base texture plus tint in `apps/viewer/src/world_ui_bars.cpp:318` | All seven tables are authored, including separate repair textures; ignored. |
| `HardPoint_Target_Reticle_Enemy_Screen_Size`, `HardPoint_Target_Reticle_Friendly_Screen_Size` | WU-31 screen fraction | `src/presentation/ui/world_ui.cpp:424`; `apps/viewer/src/world_ui_prepare.cpp:127`, `world_ui_bars.cpp:287` | Stock 0.03, all three mod layers 0.022. Already applied: at 1280×720 the mod rectangle is 28.16×21.12. Friendly size is used at appearance, enemy size subsequently, as documented. |
| HardPoint `Type`, `Is_Targetable`, `Is_Destroyable` | Lookup, admission and destruction, WU-30/32 | `src/units/unit_tables_profiles.cpp:18`; `world_ui_bars.cpp:267` | Authored roles and targetability reach the viewer. |
| HardPoint `Attachment_Bone`, parent `Scale_Factor` | WU-34 marker anchor | `src/units/unit_combat.cpp:313`; `world_ui_prepare.cpp:287` | Model attachment is scaled and transformed with drawn pose. The bind-pose rather than animated-bone limitation remains WU-35. |
| HardPoint `Tooltip_Text` | WU-51 localized title | `world_ui_prepare.cpp:274` DAT lookup, `world_ui_bars.cpp:335` percentage suffix | The mod authors damage wording in the localized title (e.g. Quad Standard Turbolaser Battery - 120 DMG); already displayed. This path does not calculate damage from a projectile tag. |
| HardPoint `Health`, `Object_Max_Health_Multiplier_Space` | WU-51 health fraction / bar | Durability profile, `world_ui_prepare.cpp:291`, `world_ui_bars.cpp:318` | Authored health is applied; percentage is current / effective maximum. |
| `Tool_Tip_Font_Name`, `Tool_Tip_Font_Size`, `Tool_Tip_Small_Font_Name`, `Tool_Tip_Small_Font_Size` | WU-52 title and bar row | `world_ui_prepare.cpp:134` | Stock and each mod layer use Arial 7 / 5 points; already applied. |
| `Tooltip_Delay` | Other tooltip paths, explicitly excluded by WU-51 | Not read by the immediate hardpoint tooltip | No fix needed in this path. |
| Parent ability `Alternate_Icon_Name`, `Alternate_Name_Text`, `Alternate_Description_Text`; HardPoint `Type=HARD_POINT_ENABLE_SPECIAL_ABILITY` | Ability icon/name and hardpoint availability gate; not marker art | Parent overrides in `src/presentation/ui/hud_shell.cpp:843`; command icon callback in `src/presentation/ui/ability_buttons.cpp:164`; world overlay defaults at `world_ui_prepare.cpp:244` | No per-hardpoint ability-icon field was found in this marker schema. World ability overlay defaults need a separate audit against the parent ability overrides. |

The 4/3 rectangle height factor, health tint thresholds, percentage suffix, tooltip
colours, health-bar width/height, padding and pointer offset are reviewed retail
code constants (WU-31/33/51–53). No XML controls for those constants are established
by the reviewed evidence; do not turn them into invented tags. Damage art tags
(`Damage_Decal`, `Damage_Particles`, model variants) are separate from the marker
and covered by [space hardpoints](space-hardpoints.md).

## Fix list

1. Replace the type-to-stock-art table and tracked suffix with all seven authored
   GameConstants tables, resolved through the mounted VFS. Keep the existing
   authored size path. Missing mappings have no invented stock fallback. Preserve
   stock pixels for ordinary normal/hover markers; explicitly exercise independent
   friendly, enemy, tracked, disabled and repairing art names in synthetic data.
2. Cover icon and size together with a synthetic mod overlay; compare stock and an
   identical stock-data overlay pixel for pixel, then change one texture and size.
3. Route the seven tag-coverage rows to the actual consumers and regenerate the
   presentation report.
4. Follow up separately on parent ability world-overlay art. No marker fix should
   change unrelated ability behavior or the reviewed tooltip constants.

## Routed behavior and remaining seams

All seven mappings now load from the mounted GameConstants document. The normal,
tracked and repairing mappings preserve each authored texture sequence: the health
fraction chooses a sequence entry, capped at the last entry at full health. Disabled
mappings select their single texture. Repeated rows for distinct types remain
independent; no hardpoint-type art list or tracked-name convention remains.
A damaged tracked friendly hardpoint uses disabled-tracked art when the local
player is not repairing it; tracked repair uses friendly-tracked art. Repair
membership is checked for the local player, including allied stations.

The targeted retail read confirmed two selection details beyond this data-routing
change. Enemy art depends on whether the current selection can hit through the
target's shields; the viewer retains its existing hostile admission policy here.
Nontracked repair alternates repairing and normal friendly art; the selector has
an explicit repair-phase input, held on repairing art pending qualification through
the frame-service schedule programme. The coordinator owns these two follow-ups.
Neither is a reason to ignore an authored texture mapping in the meantime.

