# Victory I unit-level salvo: muzzle bones, axes and launch facing (legacy EAWR-4986)

Static read, 2026-10-25 (burn task). No builds, no game runs. Sources: the Remake 4.0 chain
(`3229239424` over `1770851727`, read-only), the FoC debug build in GhidrAssist (cited by role),
and our loader and combat code on `origin/main` 74687141a.

## Verdict

**Cause found.** For `ROCKET`, `MPTL_ROCKET` and `LIGHT_SABER` projectiles, retail
the projectile-firing step calls the projectile-facing calculation only for the
can-fire result and then **replaces the projectile facing with the firing bone's X axis**
(the bone matrix's x axis, normalised, as a facing with no position).
The rocket is created with that facing, and the spline's straight leg follows the creation
facing (RFL-12). The Victory's 24 `MuzzleA_nn` bones all point **70° off the bow, outward**.
So retail rockets leave sideways for `min(400, d/2)` units and then curve to the target.
Ours aims the straight leg from the muzzle at the scattered aim point. Our rockets therefore
leave the flank already pointing at the target, with no sideways leg.

This corrects the last sentence of RFL-10 in `docs/behaviour/rocket-flight.md` ("the bone's side
axis is not used as the heading"). That is true of `Calculate_Projectile_Facing`, but its caller
overrides the result for the rocket categories. RFL-12 "Same" is wrong for any rocket fired from
a bone whose X axis does not point at the target.

## Data (Remake chain)

| Item | Value | Source |
|---|---|---|
| Unit | `Victory_Destroyer` -> `Victory_Destroyer_Required_Planets` -> `Victory_Destroyer_Template` -> `Capital_Base` -> `Vessel_Base` -> `SFX_Base` | `Units_Space_Empire_Star_Destroyer_Victory.xml`, `_Default_Base.xml` (3229239424) |
| Model | `Empire_Victory_I_Star_Destroyer_Grey.ALO` (loose, `Data/Art/Models`, 11.3 MB, 191 bones) | template line 7 |
| Projectile | `Proj_Missile_Concussion_Heavy_Trajectory` (variant of `Proj_Missile_Concussion_Heavy`): `Projectile_Category ROCKET`, `Projectile_Rocket_Straight_Distance 400`, `Curve_Distance 800`, `Curve_Offset 5`, `Explode_When_Reached_Target_Radius Yes`; `Max_Rate_Of_Turn 25` / `Projectile_Max_Scan_Range 1500` inert for ROCKET (RFL-11) | `PROJECTILES.XML` (3229239424) |
| Salvo | `Projectile_Fire_Pulse_Count 24`, `Pulse_Delay 0.1 s`, `Recharge 30 s` | template lines 61-63 |
| Scatter | `Targeting_Fire_Inaccuracy All, 20`, `Targeting_Max_Attack_Distance 4250` | template lines 42, 65 |
| `Fires_Forward` | absent on the whole chain (only `Spacecraft_Base` authors it, not an ancestor) -> default **No** (W-09) | `_Default_Base.xml` line 393 is inside `Spacecraft_Base` |
| Cone | `Turret_Rotate_Extent_Degrees 300`, `Turret_Elevate_Extent_Degrees 300` | template lines 59-60 |
| `Muzzle_Bone_Name` | absent -> default prefix `MuzzleA` (WWP-47) | - |

`Victory_Destroyer_CSA_Template` has the same weapon block. The Rep template has 18 pulses and
`Republic_Victory_I_Star_Destroyer_Red.ALO`; not decoded here.

### Muzzle bones (ALO skeleton, world = root-relative, model frame)

All 24 are children of `Root`, at Z = 0. They were read with an ad-hoc chunk reader (0x200
skeleton / 0x202 bone / 0x203 name / 0x205-0x206 data). It uses the same row-major 3x4 layout
and the same "column 0 = X axis" rule as our `unit_support.cpp` `axes()`. The model's bow is
-Y (ion cannon `IC_01` at y = -207, engines at y = +117; our loader turns
unit x = -model y, unit y = model x).

| Bones (fire order) | Side (model X) | Positions (x, y) | X axis (model) | X axis in unit frame (+x ahead, +y left) |
|---|---|---|---|---|
| `_00`..`_03` | + | (72.6, -10.1) .. (76.6, 2.1) | (0.9397, -0.3420, 0) | (0.342, 0.940): 70.0° left of the bow |
| `_04`..`_07` | - | (-72.6, -10.1) .. (-76.6, 2.1) | (-0.9397, -0.3420, 0) | (0.342, -0.940): 70.0° right |
| `_08`..`_11` | + | (85.1, 28.9) .. (88.7, 40.2) | (0.9397, -0.3420, 0) | 70° left |
| `_12`..`_15` | - | (-85.1, 28.9) .. (-89.2, 41.1) | mirror | 70° right |
| `_16`..`_19` | + | (97.6, 68.1) .. (101.5, 80.4) | (0.9397, -0.3420, 0) | 70° left |
| `_20`..`_23` | - | (-97.7, 68.0) .. (-101.6, 80.4) | mirror | 70° right |

These are three batteries per flank along the hull edge, aft of midships. Each axis is the
outward normal turned exactly 20° toward the bow, with no pitch. (Bones `_03`, `_04`, `_08`,
`_12`, `_16`, `_20` come later in the ALO bone order, but the suffix order above is what
WWP-47 cycles.)

## Retail code (debug build, by role)

1. The projectile-firing step of the weapon behaviour. With a firing bone A index set,
   the firing matrix is the model's transform of bone A and the muzzle is its
   translation. A Fire_Bone_B box is sampled only when `FiringBoneIndexB >= 0`. With no bone,
   the firing matrix is the unit matrix at the Z-adjusted position.
2. Same function, category switch: `BOMB` takes the unit facing vector. `CLUSTER_BOMB` takes
   muzzle -> target. **`ROCKET` / `MPTL_ROCKET` / `LIGHT_SABER`**: `ok = Calculate_Projectile_Facing(...)`,
   then the projectile facing = normalize(x axis of the firing matrix). All other
   categories use the `Calculate_Projectile_Facing` result. `Create_Object_Of_Type(..., muzzle,
   projectile_facing)`, then `InitialFacing = Get_Facing_Vector()`, and `FireAtPosition` is the
   scattered aim (`local_240`).
3. `Calculate_Projectile_Facing` is as RFL-10 (muzzle -> intercept -> `Add_Random_Inaccuracy_To_Fire_At_Position`;
   for ROCKET the scattered point replaces the aim). It returns false on a zero aim or a
   `Is_In_Cone_Of_Fire` refusal. For rockets this boolean is all that survives.
4. `Is_In_Cone_Of_Fire` builds an identity matrix at the **bone translation**, rotates it by the
   **unit's** facing (Z, Y, X) and transforms the aim into it. It refuses when |yaw| > `Turret_Rotate_Extent_Degrees`
   or (unless XY-only) |pitch| > `Turret_Elevate_Extent_Degrees` (deployed variants when
   deployed). The bone's rotation is not used. For the Victory, 300 > 180, so **the cone never
   refuses**.

## What retail draws for one 24-missile volley

- 24 rockets, one every 0.1 s (3 ticks), 2.4 s in all. Muzzles cycle `_00`..`_23` (WWP-47): four
  from the model's +X flank, four from -X, and so on, front battery to rear.
- Each rocket is created at its muzzle facing 70° off the bow, outward. The straight leg runs
  `min(400, planar d / 2)` units along that facing (offset factor 0.1). At d >= 800 that is 400
  units, about 376 of them sideways. The arc then bends to the scattered aim (factor 1.0, curve
  1.1). There is no post-aim segment, because `Explode_When_Reached_Target_Radius` is Yes.
- Both flanks fire whatever the target bearing. For a target off one beam, the far-side rockets
  first fly away from it and then cross back. For a target dead ahead, the volley looks like two
  fans that leave the flanks and converge forward. This is the owner's "leave sideways, then
  curve to the front".
- Scatter is per missile, a box of ±(d x 20 / 4250) per axis: ±14 at d = 3000 (RFL-13).

## Fix and volley plot (RFL-15)

Fixed as RFL-15 in `docs/behaviour/rocket-flight.md`: a ROCKET shot now leaves along its firing bone's x axis. The text plot shows one 24-missile volley in our sim before and after the fix (this layout, target dead ahead at 3000; test `tactical_rocket_contracts` case `unit_salvo_launch_axis`):

Top-down, bow to the right, port up; `#` the hull around the muzzles, `p`/`s` port and starboard rockets, `X` the aim at 3000 (x from -150 to 3050, y from -560 to 560).

```text
before
+----------------------------------------------------------------+
|                                                                |
|                                                                |
|                                                                |
|                                                                |
|                                                                |
|                                                                |
| pppppppppppppppppppp                                           |
| ####ppppppppppppppppppppppppppppppppppppppppppppppppppppppppp  |
| ####sssssssssssssssssssssssssssssssssssssssssssssssssssssssssX |
| ssssssssssssssssssss                                           |
|                                                                |
|                                                                |
|                                                                |
|                                                                |
|                                                                |
|                                                                |
|                                                                |
+----------------------------------------------------------------+
after
+----------------------------------------------------------------+
|                                                                |
|  pppppppppppp                                                  |
| pppp        ppppppp                                            |
| ppp              pppppppppp                                    |
| ppp                      ppppppppppp                           |
| ppp                               ppppppppppp                  |
| ppp                                        pppppppppp          |
| #######                                            ppppppppppp |
| #######                                             sssssssssX |
| sss####                                    ssssssssss          |
| sss                               sssssssssss                  |
| sss                      sssssssssss                           |
| sss              sssssssss                                     |
| ssss        sssssss                                            |
|  ssssssssssss                                                  |
|                                                                |
|                                                                |
+----------------------------------------------------------------+
```

The section below describes the code before the fix.

## What ours does with the same inputs

- Loader (`unit_tables_profiles.cpp`, `load_weapon` tail): `Fires_Forward` No, cone 300/300, and
  the muzzle list `MUZZLEA_00`..`_23` (`bone_index` is case-insensitive, so all 24 resolve). Only
  their **positions** reach the combat profile (`unit_combat.cpp`, `muzzles.push_back(placed)`).
  The bone axes are dropped.
- Fire (`combat_fire.cpp`): next muzzle, lead from the muzzle, scatter, cone at the muzzle about
  the unit facing (matches `Is_In_Cone_Of_Fire`), event `{muzzle, scattered}`.
- Creation (`projectiles.cpp`, DG-22): `step = normalize(aim - origin) * speed`.
- Rocket spline (`rocket.cpp` `prepare_rocket_path`): the straight leg follows
  `projectile.step / speed`, which is **muzzle -> aim**.
- Result: same muzzles, timing, scatter and cone, but every rocket's straight leg points at its
  aim point. There is no sideways departure.

## Fix shape (not done; for the dispatcher)

1. Carry each muzzle bone's placed X axis next to its position (`muzzles` -> position + axis), and
   use Fire_Bone_A's axis (`fire_axes`, already loaded) for hardpoint rockets.
2. For `ROCKET` / `MPTL_ROCKET` shots, put the launch direction (bone X axis, or the unit facing
   when no bone) into the shot. Creation should set the rocket's initial facing/step from it, and
   `prepare_rocket_path` should use it for the straight leg. Keep `FireAtPosition` = scattered aim.
3. Scope check before the change: hardpoint rocket weapons (`Fire_Bone_A` with `FiringBoneIndexA`)
   take the same branch, so this changes hardpoint rocket visuals and hashes too. Replay identity
   will move for any scene with rockets. Sim tick and replay format are affected; corpus upgrade
   per the format-bump rule if the hash encoding is involved.
4. Doc: amend RFL-10 (last sentence) and RFL-12 "Ours" to point here.

## STOPPED AT

- Not traced: the exact caller that fills `FiringBoneIndexA` from the `MuzzleA` cycle for an
  object weapon (WWP-47 is taken as given), and `FULL_SALVO` activation (it changes rate/delay, not
  facing).
- Not checked: whether the model's bone-transform lookup includes the unit's world rotation (assumed
  yes, as for Fire_Bone_A hardpoints). The CSA/Rep models were not decoded.
- No retail capture or our-build trace was made (burn rules). One short retail capture of a Victory
  volley at a target dead ahead would confirm the 70° outward departure visually.
- Reader used: `out/burn4986/alo_bones.py` (untracked, worktree-local).
