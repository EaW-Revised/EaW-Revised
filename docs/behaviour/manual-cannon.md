# Manual cannon targeting and appearance delay

This implements the space hardpoint interface of [the area damage walk](walks/area-damage.md)
WAD-39/40 and the positive projectile delay in WAD-37. Evidence is the debug build and
authored XML. The station roster gate remains closed; commands and published snapshots
provide the integration hooks. A dedicated HUD control and animated hardpoint mesh
bones remain outside this change.

| Rule | Contract |
|---|---|
| MC-01 | A manual request names a live enemy and one manual hardpoint with a selected projectile. Admission requires visibility, unrestricted category, pointability and inclusive planar minimum/maximum range from the fire midpoint. The existing collision policy supplies zero soft radius; a new radius is not guessed. |
| MC-02 | Admission refuses replacement while that gun retains a manual target. Accepted requests retain the requesting player and logical frame without replacing the unit's ordinary order. No retained target means no automatic manual-gun shot. |
| MC-03 | Ineligible weapon state waits through request frame plus 300. Strictly beyond that boundary it clears the target and publishes feedback addressed to its requester. A removed target clears immediately without timeout. Later firing failures from range, fog or cone alone keep waiting and do not generate this feedback. |
| MC-04 | A successful attempt clears its manual target. Completing its one-pulse burst records the requesting player's last firing frame and nearest whole `Manual_Hardpoint_Firing_Cooldown_Secs * 30` frames. The ordinary weapon countdown remains separate. |
| MC-05 | Readiness is one when stored cooldown is below one or elapsed frames reach it; otherwise it is elapsed divided by stored cooldown. The clock is shared by all that player's manual weapons. The command consumer refuses a new request while readiness is below one; existing pending guns do not acquire an invented global firing lock. |
| MC-06 | Turret admission tests its authored yaw extent in the owner's attachment-bone coordinates. It does not require its current firing cone or pitch alignment. Service aims at the retained target, clamps limited extents and applies rotation offsets, then advances pitch once and yaw through both linear and wrapped residual steps at the authored per-frame speed. Without a target it retains its last desired angles. Child fire positions and axes follow barrel pitch and turret yaw before ordinary cone/muzzle checks. |
| MC-07 | Positive `Projectile_Appearance_Delay_Frames` keeps the launched projectile at its muzzle without path age, motion or collision through the expiry frame. Equality reveals it; the following logical frame starts movement. Published completed ticks are one greater than the executed frame, so presentation hides through completed tick equal to expiry and reveals at expiry plus one. Explicit forced explosions retain the existing precedence. |
| MC-08 | Manual cooldown and usable mechanical bone frames bind separately from ordinary projectile/recharge data. The bounded combat profile retains minimum range with stock default zero; the mod-only `Fire_Min_Range_Distance` tag remains gated pending its registry route. Missing required turret geometry refuses manual admission. Positive appearance delay applies independently to all loaded space hardpoint weapons, including a 30-frame Krayt damage gun; absent/zero delay preserves old identity and projectile bytes. |

WAD-40's selected cannon retains direct damage 6000, blast damage 1000 and radius 300
through the ordinary projectile and blast services. Its 120-second player cooldown is
3600 frames; ordinary recharge remains one second. Its authored speed 0.5 advances
pitch by at most 0.5 degree per frame and may advance yaw by one degree because both
yaw adjustment steps execute. Appearance delays 15 and 30 use the same service.

The supported manual interface covers one-pulse space hardpoints. Other manual burst
forms, deployment/limbo/weapon-mode gates not represented by current combat state,
and non-space objects remain gated. Timeout feedback is an authoritative event hook,
not a newly invented sound. Projectile sound scheduling and visual mesh articulation
require their presentation consumers; the snapshot supplies manual angles and clocks.

Manual state extends canonical session and snapshot bytes only when manual weapons or
player clocks exist. Projectile delay extends only positive-delay projectiles. See
[the replay format](../replay-format.md) for the reserved additive fields.
