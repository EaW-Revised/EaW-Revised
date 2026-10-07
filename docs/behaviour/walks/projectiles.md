# Walk: projectile flight and hit resolution in space

This walk follows a projectile from creation to removal, including its launch pose,
flight category, collision, terminal effects and visibility. Sources are **debug build**
(opaque evidence EWP-01 through EWP-16; the evidence map and raw observations stay
private), **XML**, the existing sourced rules linked below, and explicitly
**unverified** questions. The code comparison is against the main revision
`6957fa608db84adee459ea2377f99d6b0849a8fb`, not the pending geometry or expiry fixes.
No new retail capture was made for this walk.
Tracking: projectile flight and hit resolution (legacy EAWR-1764).

The flight-defence follow-up implements WPJ-09/10/17 against immutable copied
source and target inputs in `src/sim/tactical/combat_internal.hpp` and
`src/sim/tactical/projectiles.cpp`. Passive space sources feed that interface
through partitioned world preparation in `session_world.cpp`. Source lookup
uses `SpaceIndex` and restores registration order; it adds no per-projectile
scan over all entities. Active shield slots and user-input jammer slots now publish through the same
partitioned world preparation. Jamming stamps are derived for the current frame
using WHE-32. Static source creation order is implicit; a conditional PDEF ledger
preserves later jammer activation order transactionally.
WPJ-42 has an explicit fresh-instance helper with caller-supplied random draws;
callback probability admission remains disabled. The helper does not interpret
`Projectile_Redirect_Chance_Modifier`, dispose of the old projectile, or cancel
its damage. Those are caller responsibilities, pending sourced callback policy.

The [weapons walk](weapons.md) owns whether, when and at whom a weapon fires.
It supplies a muzzle, facing, scattered aim, target/hardpoint identity, damage and
travel allowance. [Rocket flight](../rocket-flight.md) owns the established spline
construction contract. [Area damage](area-damage.md) owns blast recipients and
falloff; [capital combat](capital-combat.md) owns damage, shield depletion and death;
[heroes](heroes.md) owns ability activation and static-bomb countdowns;
[particles](particles.md) and [audio](audio.md) own effect playback after admission.
Those completed walks are interfaces here, not repeated investigations.

Projectiles are independently serviced objects. The [frame-order walk](frame-order.md)
WFO-12/17/18/26/29 establishes service registration order, behaviour services and
deletion. An object's projectile service performs contact and damage during that
object's turn. A projectile created during an ongoing manager traversal waits for a
later traversal; another manager that has not begun is a separate case. The remake
instead reads immutable world inputs in a partitioned projectile phase and applies
its staged hits in projectile-ID order. This is the documented DP-01/DG-30f project
choice, not a claim that retail globally batches every shot after every weapon.

## Ordered rules and current consumers

In the tables, **same** includes an explicitly scoped interface already implemented;
**differs** means a present consumer produces different behaviour; **missing** means
the specified consumer is absent. Pending changes do not count as implemented.
An unverified interior is stated alongside its established interface.

### Creation and launch inputs

| ID | Rule, inputs and branches | Source | Remake comparison |
|---|---|---|---|
| WPJ-01 | Creation initializes speed from `Max_Speed`; the firing caller supplies origin, facing, fire-at point, owner, source, damage and maximum travel. Initial travel, path distance, bomb fall rate, creation frame and muzzle deadline are 0 (code); targets/path/delayed sound start absent. An unset fire-at point uses (-1e18,-1e18,-1e18), a construction sentinel (code). Projectile speed is in world units per logical frame, not units per second. At 30 frames/s, proton torpedoes at 5 travel 150 units/s; ordinary Diamond Boron at 7.5 travels 225. | debug build EWP-01; XML; WWP-26/29/48/54 | **same**, `src/units/unit_combat.cpp` shot profiles and `src/sim/tactical/projectiles.cpp` `launch_projectile`, for the populated supported launch interface. |
| WPJ-02 | A muzzle is a current world firing-bone position. Hardpoint weapons use `Fire_Bone_A`/`Fire_Bone_B`, including the `Randomize_Between_Fire_Bones` interface; object weapons use their current firing bone. The projectile does not choose a hull centre or a weapon midpoint as its spawn point. | WWP-22/47; debug build EWW-03/09 | **differs**, G10: hardpoint muzzles are loaded, but the `unit_combat.cpp` object-weapon profile leaves fire positions at the unit origin; `combat_fire.cpp` uses those positions. Animated attachment differences are G4. |
| WPJ-03 | An explicitly aimed target hardpoint uses the translation of its current parent-model `Attachment_Bone` world transform. It does **not** replace that point with the centre of `Collision_Mesh`. Invalid index, missing hardpoint, untargetable hardpoint or unavailable attachment transform has a diagnostic/fallback to parent position in the inspected target-point path. A separate general hardpoint-position query prefers attachment bone, falling back to fire bone when no attachment index exists. | debug build EWP-02/03; WWP-19/50 | **differs**, G4: `unit_combat.cpp` places the loaded attachment bind position and `combat_aim.cpp` transforms it by the unit pose; it has no current animated bone-pose input. Static attachment aiming is implemented. |
| WPJ-04 | Without an aimed hardpoint, the target-point interface may select a target bone/eligible aim point; its fallback is target position plus `Ranged_Target_Z_Adjust` in world Z. A shooter's adjusted position also supplies the nearest-hardpoint reference. The adjustment is an aim offset, not movement height or a relocation of the collision mesh. | WWP-16/19/50/72; debug build EWW-16 | **implemented** for `SpaceUnit` and `SpaceStructure`: `unit_combat.cpp` binds the authored adjustment; `combat_aim.cpp`/`combat_fire.cpp` apply launch fallback and nearest-hardpoint reference without moving collision geometry. |
| WPJ-05 | Launch lead/scatter remains the weapons interface: hardpoint scatter uses pre-lead planar distance, category `Fire_Inaccuracy_Distance`, twice authored inaccuracy in space and three synchronized draws in [-r,r]. Object weapons use `Targeting_Fire_Inaccuracy` or fixed-radius override. The resulting spatial aim, including Z and BARRAGE's target offset, enters projectile flight. Hardpoint targeting does not disable scatter. | DG-24; WWP-23/25/51/52; WAD-38 | **same** for the current stock launch/scatter interface, `combat_aim.cpp`, `combat_algorithms.hpp` and `session_step_commands.cpp`. |
| WPJ-06 | A MISSILE retains its target and aimed hardpoint. It stores scattered aim minus led aim in the target's local rotation frame. Other ordinary shots retain their origin, facing and fire-at point; ROCKET construction clears missile tracking. | MS-02/03; WWP-29; RFL-01 | **same**, `launch_projectile`, launch offsets supplied by `session_step_combat.cpp`. |

### Each projectile service: delay, speed, category

| ID | Rule, inputs and branches | Source | Remake comparison |
|---|---|---|---|
| WPJ-07 | Movement occurs only strictly after the muzzle-delay deadline. At equality a hidden delayed projectile becomes visible, resets its lifetime creation frame and starts a stored delayed fire sound. `Projectile_Appearance_Delay_Frames` and the object-weapon bone delay are firing inputs. The equality frame itself does not move it. | debug build EWP-01; WWP-28/54/60; WAD-37/40 | **same** for movement, `step_projectile` deadline gate and viewer delay visibility; sound scheduling remains the audio interface BA-14. |
| WPJ-08 | Before category motion, add `Projectile_Acceleration_Per_Frame` to speed and clamp to at least 0 (code constant). Do this once per eligible logical service, not once per render frame. | debug build EWP-01 | **missing**, G2: `step_projectile` keeps stored speed; covered by the existing non-stock weapon follow-up. |
| WPJ-09 | LASER/DEFAULT and other direct-route categories advance from the current position by current speed along current yaw/pitch. Shield deflection can change facing before this step. They do not continuously lead a moving target. | debug build EWP-04; WWP-62/63; DG-22 | **same** for ordinary direct flight, `launch_projectile` and `step_projectile`; shield deflection is G6. |
| WPJ-10 | MISSILE validates that its aimed hardpoint still exists, clearing only a missing index. A destroyed hardpoint retained on the parent remains an aim point. With a target under sensor jamming, or no retained target, it uses the direct route for that service; otherwise it uses missile guidance. | debug build EWP-01/05; MS-05/07 | **same** flight gate: `step_projectile` consumes current-frame nonenemy recipient stamps from `prepare_projectile_defences`, retaining the target lock while pursuit is suspended. |
| WPJ-11 | Guidance aims at the hardpoint's current attachment position, else the height-adjusted target position. Add the stored local scatter rotated into world space **only when all three components are nonzero**. An intercept calculation occurs but its returned point is not used; the missile pursues the current aim point. | debug build EWP-05; MS-03 | **differs**, G4: `homing_point` matches the scatter gate and pursuit, and height-adjusted fallback; G4 remains because hardpoint attachment uses a bind pose. |
| WPJ-12 | Turn toward that point by the shortest wrapped angular difference, clamped independently in yaw and pitch by `Max_Rate_Of_Turn` degrees per logical frame. Move current speed along the new facing. No extra space-motion multiplier is applied to projectile turn rate. Stock proton torpedoes author speed 5 and turn 3; there is no missile arrival-sphere shortcut in this route. | MS-01/04; debug build EWP-05 | **same**, `turned`, `facing_toward`, `direction` and `step_projectile`. Circling alone is not proof of a turn-rate bug. |
| WPJ-13 | Target detachment clears the retained missile target; subsequent direct flight keeps the last facing. A removed target is not an instruction to delete or detonate the missile. | MS-05; DG-30g; debug build EWP-06 | **same**, `step_projectile` clears `locked` when the target is absent. |
| WPJ-14 | After the no-target missile service, attempt reacquisition and clear the aimed-hardpoint index for any replacement. `Projectile_Max_Scan_Range<=0` returns none. A positive range queries hostile players' collidables, maximum 2147483647 (code), unsorted, excluding jammed candidates. Best squared distance starts at 0 and replacement requires strictly smaller distance, so even a coincident candidate cannot win. Stock torpedoes author scan range 0. Do not invent nearest-enemy reacquisition. | MS-05; debug build EWP-06 | **same** visible result: `step_projectile` never reacquires. |
| WPJ-15 | ROCKET/MPTL_ROCKET owns a retained spatial spline toward the original fire-at point. Initial straight length is min(`Projectile_Rocket_Straight_Distance`, half planar launch-to-aim separation), along launch facing. `Projectile_Rocket_Curve_Distance`, `Projectile_Rocket_Curve_Offset`, maximum flight distance and target-radius flag feed RFL-01 through RFL-05. Current target motion does not rebuild this path. The established spline, random-offset and integration constants remain in the rocket note. | RFL-01–05; debug build EWP-07; XML | **differs**, G5: `rocket.cpp` implements only zero-offset, target-terminating space routes; `valid_flight` rejects nonzero offsets and post-aim extension profiles. |
| WPJ-16 | If there is no retained path and a valid fire-at point exists, construct one for the current mode. A still-missing path returns without generic advancement. With a path, add current speed to path distance, evaluate the next point and update facing through the ordinary turn limiter. The next position is the spline point; turn-limited facing does not replace it with a missile step. Failed lookup forces terminal advancement at the **current** pose without a new segment collision test or a snap to the aim. | debug build EWP-07; RFL-06 | **same** supported position/endpoint interface, `prepare_rocket_path`, `rocket_point`, `step_projectile`; unsupported construction is G5 and exact presentation-facing parity is U9. |
| WPJ-17 | Defence scans registered sources in registration order, skipping allies. For direct/missile motion, an active missile shield, passive shield or jammer strictly enclosing the current projectile position overrides desired facing with the direction away from the source's height-adjusted position; exact coincidence substitutes world +X. The first applicable source wins; ordinary turn limits still apply. For rockets, trigger a detour when next position is inside/on radius and current position is outside/on radius. Radius comes from `Passive_Missile_Shield_Radius`, overridden by missile-shield ability data, then jamming ability data when jamming applies. Space repathing preserves remaining authored flight allowance and clears tracking; its 30-unit margin is RFL-07's code constant. This flight service is separate from the AI decision to activate defence. | debug build EWP-07, EAD-01/02/03; RFL-07; WTA-34 | **same** sourced flight and active/passive producers: `session_world.cpp` copies existing ability slots and preserves creation/activation registration order; inactive active-only and destroyed sources are absent. Callback admission remains disabled. |
| WPJ-42 | An external projectile-redirection request creates a new object of the same projectile type at the old position, owned/sourced by the redirecting unit. With a replacement target, face its extrapolated height-adjusted point; without one, reverse yaw by 180 degrees, add uniform caller-supplied yaw spread, and on a 50% draw add uniform pitch in [-60,30] degrees (code). Reset travel, speed from `Max_Speed`, allowance from `Projectile_Max_Flight_Distance`; preserve instance damage/damage type but mark redirected damage internally. For rocket/returning-path categories the helper supplies a fire-at point 300 units ahead in XY and also writes 300 to the old projectile allowance (code). The caller owns probability, old-projectile disposal and damage cancellation (U8); this is separate from radius deflection WPJ-17. | debug build EWP-07; damage/ability interface | **missing**, G6: no equivalent flight clone/redirection consumer in `projectiles.cpp`; callback activation remains the owning ability/damage subsystem. |
| WPJ-18 | STATIC_BOMB returns from the bomb-path projectile service without moving, collision-testing or generic expiry. It requires the separate BOMB behaviour; that countdown/proximity/death path owns explosion and removal. A projectile lifetime field is not its fuse. | debug build EWP-08; heroes WHE-29/61/62 | **same interface**, spawned ability/countdown service in `session_abilities.cpp`; the seismic-charge model/sound issue stays with that owner. |
| WPJ-19 | Nonstatic bomb categories move along facing at current speed, then subtract current falling speed from Z and increase falling speed by `Projectile_Bomb_Fall_Accel_Rate`, before generic contact/expiry. The retained bomb fall rate is a separate state from ordinary projectile acceleration. Grenades and returning saber categories dispatch to their own paths; their interiors belong to land/hero walks. | debug build EWP-01/08 | **missing**, G2: ordinary falling-bomb flight is absent from the space projectile consumer; no stock space bomber torpedo uses this category. |

### Segment collision and hit handoffs

| ID | Rule, inputs and branches | Source | Remake comparison |
|---|---|---|---|
| WPJ-20 | Generic flight tests the full current-to-next segment before advancement/expiry. It checks ground and objects; when both hit, smaller contact fraction wins, with object winning equality. Space has no ordinary terrain ground hit; the ground route is an interface to land. | debug build EWP-09; WWP-65/70 | **same** space behaviour, `step_projectile`; the space route does not add an artificial ground plane for BARRAGE. |
| WPJ-21 | Object collision includes ordinary geometry and attached subobjects; shield geometry is additionally admitted only when `Projectile_Does_Shield_Damage` is true. The inspected query mask is 5 without shield damage and 7 with it (code); exact custom mask mapping stays U2. Search players in sorted order and require the shooter's owner to count them as enemies. The first player/object yielding an eligible contact wins, not the globally nearest object. | debug build EWP-10/11; DG-30a–g | **same** ordinary loaded contact/order interface, `step_projectile`, `CollectionTrees::ray_collect`; the persistent-tree lifecycle/order is the existing collection interface. |
| WPJ-22 | The collidable collection first rejects objects whose current world bounds do not meet the segment. A broad-phase miss cannot be rescued by an aimed-hardpoint damage selector later. Model bound recalculation unions non-light subobject bounds placed by reference-pose bones, with a separate union for collision-enabled subobjects; no such collision subobjects gives a [-1,1] box on each axis (code). No-model object world bounds use position through position+(0.1,0.1,0.1) (code); team objects delegate their bounds. Attached-model contact is established by DG-36/38; the remake's conservative combined geometry bounds are a project solution, while runtime attachment invalidation/world refresh remains U2. | DG-30c/36/38; debug build EWP-10/11/12; project collision requirement; inspected remake bounds | **differs**, G3: `session_world.cpp` `collection_member` uses `profile.collision` alone, while exact testing knows `mesh_bounds`. The pending hardpoint fix encloses both. |
| WPJ-23 | Select `Collidable_By_Projectile_Living` or `_Dead` from the **candidate object's** living/dead state, not the projectile's. Ordinary hostile collision does not add a fog or targetability gate. The remote-bomb/infection fallback can query additional players after no enemy contact, retaining original-target restrictions for same-owner objects; it is not ordinary missile friendly fire. Its detailed special-behaviour rejection is U3. | debug build EWP-10/11; DG-30b | **same** for live M2 candidates, `step_projectile` and collidable membership; collidable dead objects and special fallback consumers are G2 compatibility scope. |
| WPJ-24 | When the object has an explicit collision box, its ordinary collision entry tests that transformed box. Otherwise it delegates to model geometry. The hierarchy checks collision enablement/force-test flags: with collision-enabled subobjects it visits authored subobject order, retaining their collision result; force-all can also request subobject tests. With no collision-enabled subobjects and no force-all, it tests transformed model bounds instead. A mesh leaf first rejects **animation-hidden** geometry, even for force-all, then requires collision enablement/force-all and overlapping query/mesh masks. It checks world bounds, transforms the segment into current mesh space and uses its mesh box tree, or tests all authored faces when no tree exists. Ordinary render visibility is not the animation-hidden predicate. Exact triangle ties remain U2. | debug build EWP-12; DG-36; retail S-43–45 | **differs**, G4: `collision.cpp` and `step_projectile` use loaded static triangles and destroyed-source/shield filters without animated leaf visibility/current bone transforms. The remake's 1/32-unit grid remains a project policy. |
| WPJ-25 | `Model_To_Attach` geometry is placed at its parent's `Attachment_Bone`; `Collision_Mesh` names the damage selector, not a replacement point or permission to hit only that mesh. Destroying a hardpoint removes its attached model from contact geometry. Hull geometry remains hittable independently. | DG-36/38; WCC-62; recording S-45/49 | **same** for static loaded attachments and destroyed-source filtering in `unit_combat.cpp`/`projectiles.cpp`; animated transforms are G4 and collection rejection is G3. |
| WPJ-26 | Shield collision requires shield-damaging projectile, positive shields, no depletion effect and no ion-storm suppression. A torpedo with `Projectile_Does_Shield_Damage=No` bypasses that geometry. Exact retail shield-subobject identification is an existing unverified boundary; the remake recognizes case-insensitive `shield` among stock meshes. | WCC-47; DG-38; hazards BP-19 interface | **same** gates, the mesh-enabled callback in `step_projectile`, `shield_depleted` and `in_ion_storm`. |
| WPJ-27 | If mesh contact fails, a craft with `Collision_Box_Modifier>1` can use its world-box-gated sphere: test start, end and midpoint strictly within modifier times largest transformed box half extent. This is not a closest-distance segment/sphere test. Contact is at step start and routes to hull. Stock fighters/bombers author modifier 2. | DG-37; debug build; XML | **same**, `world_box`, `within_sphere` and the sphere fallback in `step_projectile`. |
| WPJ-28 | A contact supplies object, fraction/contact/normal, collided mesh and original aimed hardpoint to damage. When hitting the original target with an aimed hardpoint, damage routing uses that hardpoint's configured `Collision_Mesh`, even if the physical contact was elsewhere on that object. Other objects use the encountered mesh. Area damage obtains the routed selector and cancellation result. | DG-39; WCC-42; WAD-02/03 | **same**, `aimed_routes`, `step_projectile`, `session_step_combat.cpp`, `damage.cpp` and `blast.cpp`. This routing cannot manufacture a physical hit. |
| WPJ-29 | On object contact, ordinary damage/effect selection runs before detonation sound, blast and status-effect handoffs. Shield absorption selects `Projectile_Absorbed_By_Shields_Particle`; otherwise armor reduction can select `Projectile_Object_Armor_Reduced_Detonation_Particle`, falling back to `Projectile_Object_Detonation_Particle`. Target hit-particle lists/attachment placement remain BP-10/11/17–20/63. Damage cancellation can suppress blast; full shield absorption alone does not. | WAD-03/07; BP-10/11/63; debug build EWP-13 | **same** stock object-hit interface, `battle_effects_impacts.cpp`, `session_step_combat.cpp`, `blast.cpp`. |
| WPJ-30 | Impact sound uses `Projectile_SFXEvent_Detonate_Reduced_By_Armor` when the direct armor result is at most 0.75 (code); an absent reduced event falls back to `Projectile_SFXEvent_Detonate`. It is not selected from the last secondary blast recipient. | WAD-07; debug build EWP-13 | **same** event choice, `battle_audio_prepare.cpp` and `battle_audio_events.cpp` projectile-hit branch; overall sound admission remains audio's interface. |
| WPJ-31 | A ground contact uses `Projectile_Ground_Detonation_Particle` and the ground/effect handoff, then removal suppresses a second lifetime detonation. Space shots do not use this route merely because their target is a low Z point. | WWP-65/67; projectile ground handler interface | **same** space exclusion; detailed ground particle/status ordering is U4/out of scope. |

### Advancement, all terminal routes and removal

| ID | Rule, inputs and branches | Source | Remake comparison |
|---|---|---|---|
| WPJ-32 | Generic advance sets the next position and adds current speed to accumulated travel, even when flight curves. The final collision segment is not clipped to remaining allowance. A contact on that segment can precede distance expiry by up to one speed step. | debug build EWP-14; DG-33; WWP-68/70 | **same**, `step_projectile`. |
| WPJ-33 | With nonzero maximum travel, expire when accumulated travel >= allowance, or a route forces expiry. With maximum travel exactly 0, use elapsed logical frames / logical fps > `Projectile_Max_Lifetime`; equality survives. A GRENADE with positive lifetime also selects that lifetime branch. Ordinary positive-range missiles do not gain a second lifetime cap. Delayed appearance can reset the creation frame under WPJ-07. | debug build EWP-14; RFL-08 | **same** ordinary space routes, `FlightState::age_frames` and `step_projectile`; GRENADE remains the land interface. |
| WPJ-34 | `Explode_When_Reached_Target_Radius` is category-specific. ROCKET/MPTL_ROCKET (and the returning-saber category) expire when path distance >= retained rocket maximum flight distance. DEFAULT expires when squared spatial displacement from origin is strictly greater than squared origin-to-fire-at distance. Equality survives for DEFAULT. The inspected branch does not give MISSILE a generic arrival-radius test. | debug build EWP-14; RFL-05/08 | **same** supported rocket/default categories, `step_projectile`; do not add a proximity fuse to cure circling without evidence. |
| WPJ-35 | Forced path exhaustion, distance, lifetime and target-radius expiry share the no-contact terminal route: request `Projectile_Lifetime_Detonation_Particle` at the terminal pose. A named effect must resolve and be created successfully before `Projectile_SFXEvent_Detonate` is started. Blast/status effects run independently of particle existence, visibility or creation success. | debug build EWP-14; WAD-04/07 | **differs**, G7/G8: `step_projectile`/`blast.cpp` provide terminal gameplay, but ordinary expiry has no viewer particle or sound event on this base. Pending expiry work supplies particles; sound remains separate. |
| WPJ-36 | A hit suppresses WPJ-35 even if its damage was absorbed/cancelled or it shares a frame with expiry. After effects, release the retained rocket path and request projectile destruction. Generic destruction/cleanup adds no extra blast or lifetime particle. | debug build EWP-14; WAD-06 | **same**, `ProjectileStep` hit/expired exclusivity and session projectile removal. |
| WPJ-37 | An explicit explosion uses current position, the lifetime particle and the same particle-success-gated ordinary detonation sound, then blast and configured status handoffs. Explicit explosion itself does not perform ordinary segment contact; the caller owns removal/arming. Its status call ordering differs slightly from the advance path, so share outcomes rather than assuming identical callbacks. | debug build EWP-15; WAD-05/06 | **differs**, G8: `explosion_requested` supplies current-position terminal gameplay; ordinary explicit/expiry sound admission is absent. Static-bomb ability explosions remain heroes/audio interfaces. |

### Visibility and work structure

| ID | Rule, inputs and branches | Source | Remake comparison |
|---|---|---|---|
| WPJ-38 | A projectile with `HIDE_WHEN_FOGGED` consumes its own object fog query for the local observer. It does not become visible merely because either shooter or target is visible. The hide behaviour skips limbo; while settled, its timer checks at 30 frames (code), but continuing smooth transitions keep servicing. Fog disabled, playback and force-unhide/ability exceptions belong to the visibility interface. Enemy stealth can force its visibility target to zero. | debug build EWP-16; visibility walk interface | **differs**, G9: `battle_effects_projectiles.cpp` admits ordinary shots when shooter **or** target is drawn (BP-09); it has no projectile fog-state input. |
| WPJ-39 | Hide behaviour smooths toward its visibility target; model hides strictly below 0.025 (code), with force reevaluation. `Last_State_Visible_Under_FOW` makes the value immediate. Radar visibility uses >0.75 when revealing (target >=0.5), >0.25 when hiding. These are presentation decisions, not simulation removal or damage gates. | debug build EWP-16 | **differs**, G9: projectile drawing has no matching fog transition state. Initialization uses value one, zero velocity and a 0.25-second damping parameter; fogged initialization sets zero immediately. Ordinary new/delayed projectiles do not force reevaluation (U5, EUS-26). |
| WPJ-40 | Decoration detonation creation requires allowed model visibility to enemy, view-frustum admission, local fog clear and `Is_Particle_Enabled`. With an object supplied, the inspected fog helper queries that object; only the no-object overload uses the point. Non-decoration effect objects enter the synchronized manager and do not use those decoration culls; their ability initialization remains the effect/hero interface. A culled decoration can suppress expiry sound under WPJ-35 without suppressing damage. | debug build EWP-15/16 | **differs**, G8/G9: target/source draw admission and viewer particle creation do not preserve this projectile-object admission contract. Exact generic decoration ownership/culls are particles/audio interfaces. |
| WPJ-41 | Ordinary service work is O(1) motion plus owner-player ray-collection traversal and admitted object geometry/sphere work. Missile guidance adds current target/bone reads and bounded angle work; rocket route lookup reuses a retained path, rebuilding only on construction/redirection. Defence adds a scan over registered defence sources, not every unit. Blast query work happens at detonation through WAD-08–21. No numeric retail frame-time budget was measured. | debug build EWP-01/05/07/09/11; RFL/WAD interfaces | **same** work decomposition for supported flight, `ProjectileScratch`, collection tree and mesh BVHs; `MeshCollisionWork`/candidate counts support deterministic work assertions. DP-01 keeps motion/contact staging partitioned and commits ordered hits. |

## Concrete cases and limits of diagnosis

- **Rebel starbase shield generator.** Levels 1–3 use
  `HP_Rebel_Station_One_ShieldGen`; levels 4–5 use the corresponding fourth-level
  definition. The supplied attachment is `HP01_SHG_BONE`, attached model
  `RB_Station_01_HP01_SHG.alo`, collision selector `HP01_SHG_COLL` for levels 1–3;
  levels 4–5 use `HP04_SHG_BONE`, `RB_Station_04_HP04_SHG.alo`, `HP04_SHG_COLL`.
  Authored generator health is 650 versus 950 respectively. WPJ-03 establishes
  attachment-bone aim; WPJ-22 establishes why an outlying attached mesh can be lost
  before triangle testing. The existing geometry issue and pending fix address this
  concrete broad-phase discrepancy. WPJ-12/34 do not establish a special torpedo fuse.
  Residual hit rate, scatter and time to destroy require paired captures (U6).
- **Red/green ship turbolasers.** `Proj_Ship_Turbolaser_Red` and
  `Proj_Ship_Turbolaser_Green` author LASER, speed 25 (750 units/s at 30 frames/s),
  turn 0, authored flight distance 2200 and shield damage enabled. Object impacts
  request `Large_Damage_Space`, shield absorption `Projectile_Shield_Absorb_Large`,
  sound `SFX_Small_Damage_Detonation`; the lifetime particle slot is empty. Their
  straight scatter/segment contact uses WPJ-05/09/20–30, including the generator's
  attachment candidate and aimed-damage handoffs. Weapon range still supplies the
  effective travel allowance.
- **Proton torpedoes.** `MISSILE`, speed 5, turn 3 degrees/frame, scan range 0,
  no shield damage, maximum authored flight distance 2200. Their lifetime particle
  is empty. A travel-limit miss can therefore vanish without an expiry effect in
  retail too. Weapon-supplied travel allowance takes precedence over the projectile's
  authored maximum. Do not assume Diamond Boron's expiry look applies to torpedoes.
- **Diamond Boron, ordinary and BARRAGE.** Both are `ROCKET`, curve distance 500,
  offset 0, straight distance 500, target-radius expiry enabled, model
  `W_concussion_missile.alo`, scale 2. Ordinary speed/range is 7.5/5000; BARRAGE
  inherits and overrides to 12/3000. Authored turn 30 and scan range 2000 do not
  reclassify it as a tracking MISSILE. Object, ground, lifetime and shield-absorb
  particle slots name `DB_Missile_Explosion`; ordinary sound is
  `SFX_Concussion_Missile_Detonation`, reduced-armor sound
  `SFX_Small_Damage_Detonation`. Blast is 150 damage/radius 200 with five dropoff
  tiers, supplied to WAD-16. A path-end miss uses the lifetime slot, not an invented
  generic impact. Fog can hide its model independently of terminal gameplay.
- **Bombers versus Hutt containers.** `Orbital_Resource_Container` is a
  `SpaceStructure`, collidable by living projectiles, health 20, shield 0,
  `Ranged_Target_Z_Adjust=30`, and `NotOpportunityTarget`. Its small variant
  inherits the adjustment. The earlier weapons note's "only Nebulon-B 35" inventory
  was limited to that roster, and must not be applied to this prop. Missing the
  30-unit aim offset is established; whether it alone explains a given miss is U7.
  Opportunity target selection remains the targeting walk's interface.
- **STATIC_BOMB.** A seismic charge's separate bomb behaviour owns its fuse/death
  explosion. Its projectile service returning without movement is intentional;
  neither a missing ordinary expiry event nor a guessed missile route explains
  the separate bomb-model presentation issue. `Proj_Harmonic_Bomb_Slave_I` authors
  speed 0 and an empty `Projectile_SFXEvent_Detonate`; its
  `Harmonic_Bomb_Explosion_Slave_I` death/effect sound belongs to the bomb/death
  route, not G8's ordinary expiry sound.

## Existing-note reconciliation

| Existing rules | Verdict for this scope |
|---|---|
| WWP-19/22/23/25/29/47/50/52/72 | **Same launch interfaces**, with the container's authored 30-unit adjustment added to the XML inventory. Attachment-bone aim is now explicit; no collision-centre substitution. |
| WWP-60/61 | **Same**, clarified strict delay equality, creation-frame reset and zero-speed clamp. The older gap table's "no delay" row is stale for currently supported manual/SPECIAL weapons. Acceleration remains absent. |
| WWP-62–66; MS-01–06 | **Same ordinary route/contact rules**. Older "all other M2 projectiles are lasers" inventory is stale after Broadside/Marauder/hero roster expansion. Rocket and static-bomb interfaces are distinct. |
| WWP-67; WAD-02/03/05–07 | **Same damage/blast interfaces**; "no M2 area projectile" in the older walk is stale. Particle-success gating applies to no-contact expiry/explicit sound, not to blast delivery. |
| WWP-68–70; DG-33; RFL-08 | **Same**, clarified >= travel, strict > lifetime/default displacement, category-specific target-radius and failed rocket lookup at current pose. No generic MISSILE arrival radius. |
| DG-30a–g/36/37/38/39; WCC-42/47/62 | **Same ordinary contact/damage interfaces**, with DG-36's "hidden or not" limited to render visibility: animation-hidden mesh leaves are rejected. Living/dead permission is the candidate's state. Conservative combined attachment bounds are G3; mesh selection and aimed damage routing cannot bypass it. Exact shield-index/triangle-tie boundaries remain unverified. |
| RFL-01–07 | **Same retained-path/defence interfaces**; nonzero-offset/extension/redirection parity is still outside the implemented zero-offset subset. |
| BP-09 | **Differs** as a retail statement: shooter-or-target visibility is a remake approximation. The projectile object's local fog state and smoothing drive the inspected retail hide service. |
| WSU-02–05 | **Same shared visibility interface**, except WSU-04's "at or below" is too broad at exact equality: the inspected hide predicate is strictly below 0.025. This walk records that boundary without changing the completed visibility walk. |
| BP-10/11/17–20/63; BA impact rules | **Same hit-effect/sound handoffs**; ordinary expiry-particle coverage and expiry sound admission are missing on the audited base. |
| WFO-12/17/18/26/29; DP-01; DG-30f | **Same schedule and project-choice statements**; no new claim of retail globally phased projectile damage. |

## Gaps, ownership and priority

Rule-row counts: **same 25**, **differs 12**, **missing 5** (42 rows). Rows with
several gaps count once; sourced interface rows are explicitly scoped above.

| Gap | Rules | Concrete work / acceptance | Size and scope | Existing tracking |
|---|---|---|---|---|
| G1 | WPJ-04/11 | **Implemented** for space units and structures: fallback/nearest-hardpoint references and homing use the authored world-Z adjustment. Container aim is +30; Nebulon-B +35. Model/collision transforms are unchanged. | Resolved | Height-adjusted target-position implementation (legacy EAWR-713). |
| G2 | WPJ-08/19/23 | Add authored per-frame speed acceleration, falling-bomb state and dead-object collision permission for supported custom space categories; distinguish them from stationary BOMB/countdown. Zero clamp and delayed-start contracts. | M; compatibility follow-up | Existing non-stock weapon rules issue (legacy EAWR-714). |
| G3 | WPJ-22 | Conservative collection bounds must include attached contact geometry. Pin laser/missile contact outside parent hull, empty-space misses and destroyed attachment filtering; compare workers. Retail hit-rate capture remains separate. | S; M2 bug | Existing station-generator issue (legacy EAWR-1738); pending hardpoint collision fix (legacy EAWR-1740). |
| G4 | WPJ-03/11/24/25 | Publish current animated attachment-bone poses and mesh animation-hide state for projectile aim/contact instead of bind-only inputs. Cover copied deterministic poses, transform/scale and hidden-leaf rejection. Static station geometry is G3, not evidence of animation error. | L; compatibility follow-up | Animated attachment follow-up (legacy EAWR-1759). |
| G5 | WPJ-15/16 | Implement nonzero spatial curve offsets, post-aim extension/trimming and construction-failure state from RFL; keep the established stock route. Test seeded offset draws, endpoints and work bounds. | L; compatibility follow-up | Broader rocket-route follow-up (legacy EAWR-1760); existing restricted route issue is closed (legacy EAWR-1072). |
| G6 | WPJ-09/10/17/42 | Integrate active/passive missile-shield, sensor-jamming and callback projectile-redirection flight interfaces; pin ally exclusion, radius entry, direct/missile deflection, redirected ownership/damage and rocket allowance. Do not enable guessed callback probabilities. | L; M2 defence integration | Flight-defence follow-up (legacy EAWR-1761); separate AI activation stubs remain the existing issue (legacy EAWR-785). |
| G7 | WPJ-35 | Deliver final no-contact pose/reason and authored lifetime particle exactly once for ordinary space expiry. Collision suppresses it; canonical replay state must retain the agreed presentation boundary. | S; M2 bug | Existing Diamond Boron issue (legacy EAWR-1730), pending expiry-particle fix (legacy EAWR-1741). |
| G8 | WPJ-35/37/40 | Preserve authored expiry/explicit detonation sound and its effect-resolution/creation admission gate. Diamond Boron has an event; a torpedo with an empty lifetime slot emits none through this route. Pin missing/culled/created particles and hit/expiry exclusivity. | M; M2 bug | Expiry-sound follow-up (legacy EAWR-1762); separate STATIC_BOMB issue remains with heroes/audio (legacy EAWR-1724). |
| G9 | WPJ-38/39/40 | Replace source-or-target visibility for projectile models/expiry decorations with the projectile object's observer fog input and hide transition contract. Verify visible endpoints with fogged intermediate flight and revealed flight after source removal. | M; M2 bug | Projectile-fog follow-up (legacy EAWR-1763); Diamond Boron symptom is also in the existing owner issue (legacy EAWR-1730). |
| G10 | WPJ-02 | Consume the object weapon's current firing bone at the launch interface. Its target-point and `Targeting_Max_Attack_Distance` travel allowance discrepancies remain WWP-47/48/50's existing weapons work; do not silently classify the unit-origin fallback as muzzle parity. | M; M2 launch integration, weapons owner | Existing object-weapon muzzle/aim/travel issue (legacy EAWR-710). |

Top five current M2 concerns are G3 station attachment collection, G1 container
aim height, G7 missing terminal particles, G9 projectile fog admission and G8
terminal sound. Pending fixes narrow G3/G7; they do not establish retail hit-rate
or appearance acceptance and do not close G1/G8/G9.

## XML coverage remaining

The audited `docs/tag-coverage/statuses.json` marks these direct flight/aim inputs
**todo**: `Projectile_Acceleration_Per_Frame`, `Projectile_Bomb_Fall_Accel_Rate`, `Projectile_Max_Scan_Range`,
`Projectile_Redirect_Chance_Modifier`, `Ranged_Target_Z_Adjust` (including
`SpaceStructure`/`SpaceUnit`), and `Targeting_Fire_Inaccuracy_Fixed_Radius`
(`SpaceBuildable`/`SpaceUnit`). The scan-range todo is not evidence that a new
retargeting algorithm is wanted (WPJ-14). Redirect probability is a defence
interface whose exact consumer is U8.

Damage-routing interfaces also read `Projectile_Damages_Random_Hard_Points` and
`Projectile_Damage_Delay_Secs`, both todo, owned by WAD-26/27 and the damage walk.
The former selects a random destroyable hardpoint before ordinary routing; the
latter replaces the caller's delay with a synchronized draw from one quarter of
the authored positive delay through the full delay. `Passive_Missile_Shield_Radius`
(`SpaceBuildable`) now feeds passive flight defence. The space redirect-ability
angle row remains an owning-callback input. No callback probability is inferred
from either tag's mere presence.

`Projectile_Lifetime_Detonation_Particle` is **partial**, covering spawned
HARMONIC_BOMB/WEAKEN_ENEMY types rather than ordinary expiry. `Projectile_Max_Lifetime`,
`Projectile_Rocket_Curve_Distance`, `Projectile_Rocket_Curve_Offset`,
`Projectile_Rocket_Straight_Distance` and `Explode_When_Reached_Target_Radius` have
partial/limited implementation notes for the stock rocket endpoint subset; read
those notes rather than treating parsing as unrestricted flight support.

Status/effect tags read by terminal handoffs remain with their owning walks:
`Projectile_Stun_On_Detonation`, `Projectile_Distract_On_Detonation`,
`Projectile_Instant_Heal_On_Detonation`, `Projectile_Convert_Enemy_On_Detonation`,
`Projectile_Cause_Invulnerability_On_Detonation`, `Projectile_Weaken_Enemy_On_Detonation`,
`Projectile_Ion_Stun_On_Detonation` and `Projectile_Modifier`, with their radii,
durations and category filters. Blast tags/`Max_Secs_For_AE_Delayed_Damage` are WAD's
inputs. Their recipient internals are not new projectile-flight gaps.
`Projectile_Ground_Detonation_Particle` and grenade data marked land-only
stay with the land interface unless a custom space profile selects those routes;
the falling-bomb acceleration row is todo, not marked land-only.
No shared registry rows are changed by this docs-only walk.

## Settled questions from the unverified sweep

Question IDs are retained; these boundaries no longer require a new source read. Opaque evidence IDs identify ignored research receipts. Runtime acceptance and explicitly remaining clauses stay below.

| ID | Sourced disposition | Evidence |
|---|---|---|
| U1 | Settled by this walk: positive scan queries exclude jammed candidates, but best squared distance starts at zero and strict replacement rejects coincidence too. | Previously sourced in this walk |
| U5 | The visibility smoother initializes at one with a 0.25-second damping parameter and zero velocity; fogged initialization immediately sets zero, local ownership sets one. Logical service advances the damped value; last-state-memory types snap. Ordinary creation and delayed-projectile admission do not force reevaluation. The traced force caller is cinematic space-arrival exit, and the force flag does not bypass the early timer/convergence return. | EUS-26 |
| WPJ-17-order | Statically declared primary/secondary MISSILE_SHIELD slots register the owner at creation, independently of active state; passive shield registration is separate. Registry insertion is append order and deletion removes the matching entry. Jammer activation registers its owner, while affected recipients only carry the current-frame stamp (EAD-01/02). | EAD-03 |

## Unverified work and captures that settle it

| ID | Open boundary | Evidence needed |
|---|---|---|
| U2 | Shield-index creation, exact triangle ties/custom mask mapping and runtime attachment world-bounds refresh. Hierarchy, animation-hide rejection, leaf world-bound/inverse-transform gates and model collision-bound union are settled by EWP-12. **Sweep:** Still unverified: The shield index getter is resolved, but only returns stored state. It does not establish index construction, triangle tie/mask ordering or dynamic attachment bounds invalidation; EWP-12 remains the settled contact hierarchy. | Remaining triangle/tree leaf and bounds-invalidation reads, then generator captures with stationary/rotated geometry; retain per-shot candidate/mesh/contact traces. Retained sweep boundary: EUS-37. |
| U3 | Remote-bomb/infection special-behaviour rejection. Candidate living/dead collision permission is settled by EWP-11. **Sweep:** Still unverified: The targeted collision entry-point query returned no resolved special-behavior filter body. Existing living/dead permission evidence cannot settle remote-bomb/infection exceptions. | Special behaviour enum and collision caller reads; no ordinary space-friendly-fire inference. Retained sweep boundary: EUS-37. |
| U4 | Complete ground, grenade and returning-saber detonation internals. | Owning land/hero walk; space BARRAGE alone cannot settle them. |
| U6 | Whether the pending generator-bounds fix restores retail hit rate/time to destroy, and whether any torpedo orbit remains. | Paired Empire attack on level-3 Rebel generator: ordered aim/lead/scatter, candidate admission, contacted mesh, hit/miss, path/expiry reason, health/time; repeat after geometry fix. |
| U7 | Whether container +30 fallback closes bomber misses in the reported scene, including owner/targetability and lead/scatter. | Paired bomber attack at fixed container poses/heights with and without aimed hardpoint; record actual fallback aim and contacts. |
| U8 | Full redirect probability and direct/missile defence conditions, including `Projectile_Redirect_Chance_Modifier`. **Sweep:** The traced blaster redirect callback compares a synchronized uniform draw against Redirect_Percentage × Projectile_Redirect_Chance_Modifier. It rejects the listed missile/rocket/grenade/bomb/saber categories before that redirect branch; the block-percentage branch is separate. A successful clone while the recipient is in MOVE animation does not by itself return hit-cancellation success. Still unverified: The callback’s preceding virtual suitability check and the complete direct/missile shield interfaces remain separate; do not treat this probability as a universal flight-defence chance. Existing flight-defence integration (legacy EAWR-1761) owns integration. | Defence call tree and active/passive/jamming captures; use RFL-07 only for the already-established rocket interface. Retained sweep boundary: EUS-36. |
| U9 | Diamond Boron normal/BARRAGE retail fog, terminal-pose effect and audible detonation acceptance. | Capture spawn/path-end/distance/target-radius/direct-hit cases with fog on/off, including absent/cull/success particle routes; compare one blast, one particle and the appropriate sound at the retained pose. |

No unknown branch is a license to enlarge collision spheres, remove scatter,
accelerate torpedo turning or add an arrival fuse. Those changes require sourced
behaviour and paired evidence.
