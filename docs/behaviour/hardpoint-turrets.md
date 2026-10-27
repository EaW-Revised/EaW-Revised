# Hardpoint turrets

Ordinary `Is_Turret` hardpoints use authoritative mechanical angles. Manual guns
share their authored geometry and stepping helpers, while keeping their separate
[assignment and player clock](manual-cannon.md). Object-level `TURRET` behavior
retains its separate exact-angle and idle-timer rules.

The rules below are sourced from the debug build. Weapon-before-turret ordering
is [FS-12 / FC-36](frame-service-schedule.md). Rendering reads committed angles;
it never supplies an aim or firing decision to the simulation.

| Rule | Behavior |
| --- | --- |
| HPT-01 | Load `Is_Turret`, `Turret_Bone_Name`, optional `Barrel_Bone_Name`, `Turret_Rotate_Speed`, `Turret_Rotate_Extent_Degrees`, `Turret_Elevate_Extent_Degrees`, `Turret_Rest_Angle` and `Turret_Rotation_Offset` for ordinary hardpoints independently of `Requires_Manual_Target_Assignment`. `Model_To_Attach` selects an attached turret's skeleton; otherwise names resolve in the owner skeleton. A required missing yaw binding prevents firing and mechanical service. Missing optional pitch binding supplies no pitch transform on the yaw bone. |
| HPT-02 | Candidate pointability compares absolute wrapped yaw in attachment coordinates, including the coordinate quarter-turn, against the authored yaw extent. Current angles, pitch alignment and firing cone are separate. Weapon work reads the current pose before this turn's mechanical adjustment. |
| HPT-03 | A shot requires the dot product of the yaw bone's current forward axis and the direction from that bone to the target position to reach the cosine of the full `Fire_Cone_Width`. Exact desired-angle equality is unnecessary. Barrel pitch does not rotate the yaw bone's firing axis. Existing range, target, projectile and muzzle gates still apply. |
| HPT-04 | A pointable directed target wins. Otherwise an opportunity reference, when present, replaces it, and the last-fired reference, when present, replaces that. No new nearest-target scan is introduced. Without a reference, ordinary desire is the authored rotation offset without prewrapping; manual desire retains its previous value. With a target, desire wraps yaw and pitch; an extent strictly between zero and 180 clamps that component and then adds its rotation offset. Zero, negative and 180-or-larger extents do not take that limited branch. |
| HPT-05 | Each admitted mechanical turn steps pitch once by the authored speed, steps yaw linearly once and then steps its wrapped residual once, and wraps the result. The rate is per logical turn, with no elapsed-seconds multiplier. Destruction freezes the turret; weapon recharge does not stop its mechanical work. |
| HPT-06 | Muzzle positions rotate through optional barrel pitch and turret yaw. The yaw firing-axis test remains independent of the fire bone's bind rotation and barrel pitch. Mechanical angles and the last-fired reference are authoritative checkpoint, digest and replay state. |
| HPT-07 | Publish ordinary hardpoint poses separately from commandable manual guns. Attached skeletons retain independent palettes; inline hardpoints compose into one parent palette that preserves the current clip or idle sample. Bind names once during composition, and upload the composed parent palette once per hull. |
| HPT-08 | Mechanical work is one bounded step per live turret slot in the existing partitioned combat phase, with immutable world inputs, disjoint staging and ordered commit. Mechanical service performs no profile copies, allocations, model searches or new world scans. An internal deterministic step counter supports the synthetic cost contract; it is excluded from state. |
| HPT-09 | At combat-table load, individually normalized attachment, yaw and optional barrel axes with maximum absolute pairwise dot at most 2^-12 retain their existing bytes. A slightly sheared frame above that tolerance, up to 1/256 inclusive, receives checked Q24 modified Gram-Schmidt in fixed X, Y, Z order: retain X, remove preceding-axis projections from Y then Z, and normalize each corrected axis. This preserves handedness. Emit one informational `HPT-09` note per corrected hardpoint. Greater shear refuses with the named safety reason; pivot, component, length and angle checks remain in force. This is a project admission policy, not a claimed retail tolerance. Raw authored bone/fire directions remain unchanged. |
| HPT-10 | Ordinary hardpoint extents retain their authored Q24 values without the manual-assignment-only 720-degree admission bound. Manual assignment and independent geometry checks retain their contracts. |

HPT-10 is a project admission correction supported by the binding-path audit:
before ordinary turret binding, the 720-degree extent validator only received
manual-assignment profiles. Ordinary hardpoints now share that profile type,
but do not acquire its manual-assignment extent bound. Preserve their authored
Q24 extents, including 800 and 999 degrees; keep the 720-degree absolute extent
bound for `Requires_Manual_Target_Assignment` and all independent geometry,
speed, rest and offset checks. No wider ordinary extent bound is introduced.
HPT-02 compares only wrapped yaw with the extent. HPT-04 negates and clamps an
extent only after proving it is strictly between zero and 180; larger extents
never enter angle addition, subtraction, negation or mechanical stepping.
This corrects admission and does not claim a new retail angle rule.

| Case | Coverage |
| --- | --- |
| HPT-C01 | Authored rate, simultaneous desire and weapon-before-turn firing. |
| HPT-C02 | Full firing cone admits a shot before exact alignment; muzzle follows the pose. |
| HPT-C03 | An out-of-arc directed target cannot fire. |
| HPT-C04 | Limited versus zero/negative/180-degree extents, offsets, no-reference desire and residual yaw wrapping. |
| HPT-C05 | Opportunity and last-fired references remain available during recharge. |
| HPT-C06 | Missing yaw binding and destroyed hardpoint cannot continue firing or stepping. |
| HPT-C07 | Multiple synthetic capitals: separate ordinary/manual snapshots, checkpoint continuation and equal 1/2/4/8-worker hashes. |
| HPT-C08 | Synthetic 12- and 148-slot mechanical service: exact linear step count and zero transient allocations, including profiles with vector payloads. |
| HPT-C09 | Renamed inherited ordinary hardpoint loads authored geometry/rates and changes content identity when mechanics change. |
| HPT-C10 | Parent/attached name collisions and missing required bindings use only the selected skeleton. |
| HPT-C11 | Slightly sheared loaded frames recover an invented orthonormal articulation frame, with one note; corrected aiming/firing has identical state and snapshot hashes. Frames in the original tolerance band remain byte-identical; above-limit frames refuse. The opt-in installed Remake guard reports the chain's maximum authored turret shear and fails on any skipped combat unit. |
| HPT-C12 | HPT-10: strict ordinary 800/999-degree extents retain authored values, create and step mechanical state, aim/fire without clamping, and preserve half-turn wrapping; manual assignment still refuses them. |

`Attachment_Bone`, `Model_To_Attach` and `Fire_Bone_A/B` retain their existing
placement roles. Object-only XY restrictions, deployed extents and idle delays
are separate interfaces and are not copied onto hardpoints. No separate
hardpoint elevation-speed tag is supplied by the reviewed data or rule: pitch
uses `Turret_Rotate_Speed`.

The HPT-09 admission bound follows a read-only scan of turret-bound attachment
and composed yaw/barrel frames in the installed FoC, Remake 4.0 development
chain and Republic at War chain (2026-10-23). Maximum absolute pairwise dots
after individual normalization were respectively 0 (18 frame references),
0.000911215 (77,945 references) and 0.000000059605 (454 references).
The Remake maximum was Blackguard's `HP_Imperial_II_AA_Turret_DLTL_Green_03`
attachment frame in `Empire_Imperial_SD_Carrier_New.ALO`; 1/256 is about 4.3
times that observed maximum. Victory's first affected hardpoint measured
0.000341020 and Tector's 0.000552401, both above the previous admission
tolerance. Counts include inherited definitions; missing bindings retain
HPT-01's explicit no-fire behavior. The scan evidence is attached to the fix's
validation receipts; no installed assets are included in fixtures.

Current-frame target-pose age and indirect owner/stun/reattachment reset questions
remain frame-service programme seams. Runtime boundary tolerance and initial
nonzero-rest behavior still require focused retail qualification; synthetic
tests establish the deterministic contract above, not those unmeasured cases.
