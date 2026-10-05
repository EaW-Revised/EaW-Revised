# Space mass drivers

| Rule | Behaviour | Evidence |
|---|---|---|
| MD-01 | `HARD_POINT_WEAPON_MASS_DRIVER` is a weapon hardpoint. Its firing bones, cone, range, recharge, pulse count, scatter and damage type use the ordinary hardpoint weapon path (W-06, W-07, DG-12, DG-21 to DG-25). Losing it disables its weapon (HD-05). | Stock hardpoint and projectile definitions; debug build's generic flight and shield services. |
| MD-02 | The two stock space rounds have `Projectile_Category` DEFAULT, `Max_Speed` 70 and `Max_Rate_Of_Turn` 0. They fly directly, with no gravity or homing. Hardpoint range controls their travel, rather than their projectile type's shorter `Projectile_Max_Flight_Distance` (DG-23, DG-33). | Stock data; debug build advances along facing by stored speed and calls generic projectile service, without adding gravity. |
| MD-03 | `Projectile_Does_Shield_Damage` No bypasses shield absorption and shield collision meshes (DG-06, DG-38). `Projectile_Does_Hitpoint_Damage` Yes sends the hit through diminishing firepower, defense and the target's hull armor table. `Projectile_Does_Energy_Damage` No leaves energy intact. Hardpoint aim routing remains DG-39; destroying a shield generator can consequently drop shields (DG-17). | Stock data; debug build returns zero shield absorption immediately when the projectile disables shield damage. Existing damage and collision evidence DG-05 to DG-12 and DG-38 to DG-39. |
| MD-04 | Both space rounds use `Damage_Mass_Driver_Space`: craft and transports x2; corvettes, missile cruisers, interdictors and MC30 x1.5; frigates and Vengeance x1.25; capitals and stations x1. Missing pairs default to x1 (DG-12). | Stock `Damage_To_Armor_Mod` rows. |
| MD-05 | Ship and station batteries fire eight pulses six logical frames apart, with 3–4 seconds recharge (W-06). Ship ranges are 1700 for Vengeance and 1800 for Kedalbe; station range is 2000. Visuals and sound follow the same data-driven presentation hooks as other rounds: the authored projectile model and scale, hardpoint `Fire_SFXEvent`, object impact particle and detonation sound, and armor-reduced alternatives (BP-12, BP-13). | Stock XML; existing generic firing and presentation evidence. |
| MD-06 | A projectile model can contain only a skeleton and particle proxies, with no mesh surfaces. Such a model still draws its resolved effects, following the projectile pose and authored scale through the normal moving-proxy lifecycle. The two mass-driver rounds use this form and obtain their gold colour and trail from their authored effect art, rather than a laser tint or a substitute colour. Missing models, unresolved effects and invalid transforms remain rejected. | Stock projectile model structure and referenced effect art; retail capture shows gold rounds in flight. Existing proxy rules BP-40 and BP-64. |
| MD-07 | A legacy root emitter with motion inheritance enabled adds the moving emitter's world velocity, multiplied by its authored inheritance scale, to each particle's movement. A legacy kite faces the camera and points along that total movement projected into the camera plane. Its tail multiplier is one plus the authored kite length multiplied by projected speed divided by the greater of the reference speed and projected speed; a zero denominator gives a multiplier of one. A stationary particle draws an ordinary quad. The gold effect authors this inheritance flag and kite length 15, so the moving shell has a gold streak. | Debug build's particle movement, legacy property loading and kite setup; authored effect and retail gold streaks. |

The obtainable census carriers are `Kedalbe_Battleship`, `Vengeance_Frigate`
and `Skirmish_Underworld_Star_Base_1` through `_5`. The census aggregates the
hardpoints' projectile names into `object_projectiles`; that does not create
an additional ship weapon. Their mass-driver weapons resolve without unit
special cases. This does not implement their unrelated abilities or station
weapons.

The moving-effect reference speed currently retains the greatest sampled
emitter translation speed over that instance's life. Its exact retail reset
policy is unverified beyond these constant-speed rounds. This affects tail
length after a moving emitter slows down, rather than authoritative combat.

For MD-07, the kite's UV center lies on the unstretched diagonal between its
across corners, at the particle's position. Only the backward corner
stretches. This keeps the glow at the head and blends the trail into it;
using the stretched diagonal for the texture center separates the two.
The debug build's quad UV assignment establishes this anchor.

No nearby space projectile tag requires a new flight path: neither round
authors ballistic, gravity, area blast, ion stun or delayed damage tags.
Empty ground, lifetime and shield-absorption particles stay empty. The
land turret and tank mass drivers use a separate damage type and are outside
space combat. Hardpoint minimum range and turrets retain the documented
limits of space-weapon-fire P-04; these mass-driver batteries author neither.

Combat adds load-time classification. The existing partitioned targeting
and projectile phases service the new weapons, with ordered damage commit.
Presentation admits their effect-only models and applies moving-emitter
inheritance and legacy kite geometry in the existing particle worker path.
Existing enum values remain unchanged, preserving M2 identity and replay
pins; the new enum value is appended.
