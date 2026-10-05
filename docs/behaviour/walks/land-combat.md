# Walk 25: land tactical combat

## Scope and evidence

This walk prepares the land battle simulation. It covers companies, squad members,
infantry, ground vehicles, heroes, structures and air support, from placement through
movement, targeting, capture and battle completion. It records the land branches of
shared tactical systems rather than reproducing their internals. The rule table is a
verified inventory, with unresolved branches listed explicitly below; it is not a claim
that every presentation, AI or campaign deployment detail has been established.

Sources are the **debug build**, **effective XML data**, and existing sourced walks.
Opaque evidence IDs `LC-E01` to `LC-E63` identify read-only inspections; unsuccessful
lookups are not evidence and are not cited. `LC-D01` to `LC-D11` identify XML snapshots.
Their private map, raw inspections and tag-status audit remain in ignored
`out/research/land-combat/`. No retail capture was made. Code constants and XML values
are distinguished below; authored examples are not universal defaults.

The code comparison is against `b9f9f3a37e53fb28f9d683dde979b3e4542d8cce`.
`src/sim/tactical/` and `src/units/` implement space tactical behavior; the land scene
and `src/presentation/terrain/terrain.cpp` supply presentation, not these simulation
rules. **Missing** means the land adapter or component is absent. **Shared** identifies
the existing space rule contract, without certifying that all space gaps are fixed or
that the land adapter already exists. There is no measured land implementation to
classify as differing. Proposed modules in the table describe where a gap belongs;
they are not claims that a land component already exists there.

Boundaries: [weapons](weapons.md), [area damage](area-damage.md),
[abilities](abilities.md), [heroes](heroes.md), [production](production.md),
[build pads](build-pads.md), [sensors/UI](sensors-ui.md),
[frame order](frame-order.md) and [battle flow](battle-flow.md) own their shared
contracts. [Reinforcements](reinforcements.md) documents the space branch; land
placement and retreat have explicit differences here. [GC battle handoff](gc-movement-control.md)
owns persistent parent/child identities and campaign return. Land AI strategies,
animation fidelity and campaign fleet travel remain outside this walk.

## Entry points and service order

Battle setup selects company members, creates their objects and optional squad
containers, installs land managers and victory conditions, and initializes fog.
Scheduled orders enter the common tactical command path. Placement admission precedes
creation; garrison entrance is an order followed by movement and fading, not immediate
removal on the click. Each object's logical services then apply movement, surface
effects, reveal, targeting and weapons according to the shared frame-order contract.
Capture runs on its four-frame cadence. Land-wide services maintain weather, air
support, retreat and victory before returning through the common mode service.

The table follows those dependencies: setup, movement and surface state, combat,
garrisons, reinforcement/control, support, then end conditions. This grouping does
not establish an arbitrary global ordering between every object service. Exact
simultaneous entry, capture, destruction and reinforcement races need the capture
listed under U-LC-6; the shared frame-order walk remains authoritative.

## Rule list

### Companies and unit kinds

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| LC-01 | Land company creation reads ordered `Company_Units`; space instead reads `Squadron_Units`. With no member list, the direct-object branch attempts one object. A listed company's failed member placements are skipped; successful members can remain, so creation is not an all-or-nothing transaction. | debug build `LC-E09`; XML `LC-D01` | Missing G1: land company expansion beside `src/units/unit_tables_decode.cpp` and tactical creation. |
| LC-02 | Successful members preserve their source company for population and parent bookkeeping. The first created member's `Create_Team` decides whether to form squads or return members directly. Optional population registration occurs per successful member; squad-created notification follows registration. | debug build `LC-E09` | Missing G1: tactical creation and population registry; shared registry interface WR-02/03. |
| LC-03 | Team construction groups the surviving created objects up to `Max_Squad_Size`, using `Create_Team_Type` or the generic team fallback. Each group's container starts at its members' centroid, with its first member's owner/facing, initializes abilities, then receives an initial movement order. | debug build `LC-E45` | Missing G1: land squad container beside `src/sim/tactical/formation.cpp`. |
| LC-04 | Company count and tactical object count differ: the authored stormtrooper company has 18 infantry members and `Max_Squad_Size=9`, giving two full squads when all spawn. Examples of vehicle companies are four AT-STs, three anti-air vehicles and one AT-AT; their data must drive expansion. | XML `LC-D01`; debug build `LC-E09`, `LC-E45` | Missing G1: land company profiles; do not hardcode one entity per company. |
| LC-05 | Standalone ground objects and teams use distinct land targeting services. Teams retain their own movement/ability coordination; individual vehicles use deployment and body/turret facing gates. A structure does not gain movement merely because it has targeting, and a hero's combat uses its authored object behaviors. | debug build `LC-E27`, `LC-E28`, `LC-E55`; heroes walk | Missing G1: land object-kind routing; shared hero and ability contracts. |
| LC-06 | Team maximum speed comes from the team type with the current object instance, rather than an unconditional average of its members. The ordinary speed modifier pipeline still applies. | debug build `LC-E19`, `LC-E26` | Missing G1: squad movement profile beside `src/units/unit_motion.cpp`. |

### Passability, formations and crowding

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| LC-07 | Land occupancy refreshes dirty passability, obtains the movement-class mask unless an explicit mask is supplied, and tests a rectangle based on the object's or type's soft footprint at the candidate cells. Center-only passability is insufficient. | debug build `LC-E18`, `LC-E29` | Missing G2: land occupancy adapter beside `src/sim/tactical/pathfind.cpp`. |
| LC-08 | Movement masks are authored. Infantry permits clear, shield and infantry-only cells; wheeled/tracked/walker permits clear and shield; hover additionally permits water; large walker additionally permits rebel-wall cells. Flying permits the wider steep/water/wall/obstacle/infantry-only set. No-shield variants omit shield. | XML `LC-D02`; debug build `LC-E18` | Missing G2: movement-class profile and terrain-cell mask, not space tracking layers. |
| LC-09 | Formation destination placement queries oriented formation extents with `BetweenFormationSpacing` (XML 5) and `Destination_Collision_Query_Extension` (XML 60). Infantry, bikes and giant vehicles have different collision-filter branches. It tests point and footprint occupancy and, where required, connected passability zones. | debug build `LC-E24`; XML `LC-D07` | Missing G2: land destination placement beside `formation.cpp` and `pathfind.cpp`. |
| LC-10 | The checked giant-vehicle destination branch tests a direct cell route, then a land detour; it rejects a detour longer than 1.8 times the direct distance (code constant). This is a branch-specific acceptance rule, not a universal path-length limit. | debug build `LC-E24` | Missing G2: giant-vehicle destination admission. |
| LC-11 | Path search first permits a zero-minimum-width route. For a requested positive width it repairs segments narrower than requested width minus 0.1 (code tolerance); failed width-constrained repair rejects the route. Smoothing/postprocessing is restored after this repair. | debug build `LC-E35` | Missing G2: width-aware land routes; shared space movement does not prove bridge traversal. |
| LC-12 | The land search selects rectangle search when configured, otherwise cell search when enabled; with neither search enabled it fails. Start/end cells and optional trivial-path handling are setup inputs. | debug build `LC-E47` | Missing G2: land path-search adapter. |
| LC-13 | Company landing layout groups members by authored squad size, rotates offsets by facing and derives spacing from footprints. Team layout uses `FormationSpacing × 20` and a 2.5 footprint-spacing factor (code constants). All member positions must pass placement admission. | debug build `LC-E08` | Missing G2: company-footprint layout; creation's partial-success rule LC-01 is a separate stage. |
| LC-14 | Maximum speed includes instance override, mode and unit speed, crush-fleeing multiplier, disabled engines, retreat pursuit factor, movement modifiers, base-shield speed and on-fire modifiers. Movement modifier output is applied as `1 + total`; terrain feeds that pipeline. XML fleeing multiplier is 2 and base-shield speed multiplier is 0.5. | debug build `LC-E17`, `LC-E26`; XML `LC-D07` | Missing G2: land speed context beside `src/units/unit_motion.cpp`; shared ability modifier arithmetic. |
| LC-15 | Width checks, occupancy, destination collision filtering and formation placement establish mechanisms for chokepoints. Exact bridge queues, crowd separation, reroute timing and persistent stalls are not settled by these inspections. | debug build `LC-E18`, `LC-E24`, `LC-E35`; unverified U-LC-3 | Missing G2: implement verified inputs; capture crowd behavior before specifying avoidance. |

### Terrain modifiers, cover and hazards

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| LC-16 | On a changed logical surface, remove the previous surface's source-tagged modifiers before adding the new surface's nonzero modifiers. `SurfaceFX_Name` selects authored surface settings; effects must not become permanent after leaving the surface. | debug build `LC-E25`; XML `LC-D03`, `LC-D04` | Missing G3: land surface-state component feeding combat and movement modifiers. |
| LC-17 | Good-ground settings supply `Defense_Mod` and `Damage_Mod`. The infantry profile authors 0.5 defense and 1.0 damage. The checked service omits good/high-ground effects for objects converted to the enemy. These are logical surface effects, not automatic bonuses from an arbitrary height difference. | debug build `LC-E25`; XML `LC-D03` | Missing G3: surface modifier admission and ownership context. |
| LC-18 | High-ground settings supply `FOW_Reveal_Range_Mod` and `Fire_Range_Mod`; the infantry profile authors 0.25 each. Reveal and firing consume those modifiers through their ordinary pipelines. | debug build `LC-E25`, `LC-E59`; XML `LC-D03`; WWP-20 | Missing G3: surface-to-fog/range bridge; shared weapon range. |
| LC-19 | The checked speed branch consumes `Speed_Mod` on slow/water surfaces (infantry XML -0.5). Mud also authors -0.5, but this branch does not apply mud speed. XML alone is insufficient to claim mud slows; another consumer or retail behavior remains U-LC-1. | debug build `LC-E25`; XML `LC-D03` | Missing G3: verified slow/water modifiers; mud explicitly unverified. |
| LC-20 | Positive `Terrain_Damage` on damage/lava surfaces schedules a hit at current frame plus truncated `Terrain_Damage_Delay × logical FPS`. When due, it routes ordinary terrain damage and advances the old deadline by that interval, once per service rather than catching up in a loop. Infantry XML authors 10 damage every 1 second. | debug build `LC-E25`; XML `LC-D03` | Missing G3: deterministic terrain-hazard deadline and damage event. |
| LC-21 | Land combat damage additionally multiplies by `Surface_Type_Cover_Damage_Shield` when the victim's closest terrain vertex material is forest-floor, shrubbery or tall-grass and its factor differs from 1. Infantry examples author 0.5 or 0.25. This material-cover factor is separate from good-ground defense. | debug build `LC-E31`; XML `LC-D04` | Missing G3: terrain-material cover lookup feeding `src/sim/tactical/damage.cpp`. |
| LC-22 | Shared source damage modifiers and victim defense modifiers combine as `(1 + source total) × (1 - victim total)` before the land material-cover factor; differing-owner AI damage modifiers can also apply. Shields, armor, damage categories and hardpoint routing remain the shared damage contract. | debug build `LC-E23`, `LC-E31`; weapons and area-damage walks | Shared with weapons/area-damage rules; land cover input missing in `damage.cpp`. |

### Crush

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| LC-23 | Crush service runs while moving to a destination and uses model-authored crush volumes. It queries nearby enemy-owned collidable objects and tests the victim's bounds against the crusher's local volumes; friendly units are not admitted by this query. | debug build `LC-E05` | Missing G4: land crush query and model-volume input beside movement/damage. |
| LC-24 | Admit `Is_Squashable`, or `Is_Squashable_By_Supercrusher` with crusher `Is_Supercrusher`. A normal crusher additionally needs the victim's current squash permission; a supercrusher bypasses that permission. A vehicle-sized circle overlap alone is insufficient. | debug build `LC-E05` | Missing G4: crush category and per-instance permission gates. |
| LC-25 | Qualifying overlap turns the victim toward the crusher and routes 1,000,000 damage (code value) of the crusher's `Squash_Damage_Type`. This is an ordinary damage call with its gates, not unconditional object deletion. Exact flee/avoid timing remains U-LC-3. | debug build `LC-E05`; shared damage contract | Missing G4: crush damage event; shared damage resolution. |

### Targeting, fog and weapon arcs

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| LC-26 | Land targeting services try queued special attacks first, then eligible formation targets, opportunistic acquisition and retained automatic-target replacement. Formation orders can distribute hardpoint targets. Individual units and teams have distinct coordination branches. | debug build `LC-E27`, `LC-E28` | Missing G5: land targeting state beside `src/sim/tactical/combat_targeting.cpp`. |
| LC-27 | Target suitability rejects inappropriate collidable/target flags, limbo, dead objects, hero-clash participants, transported targets, fogged targets and disallowed stealth targets. Melee adds movement-layer/hover restrictions. Air bombing has its own active-run admission branch. | debug build `LC-E32`, `LC-E28` | Missing G5: land target eligibility; shared hero clash/stealth interfaces. |
| LC-28 | Authored land priority sets rank object categories. Anti-infantry examples rank land heroes 0.5, infantry 1, turrets 2, vehicles 3 and tactical structures 4; anti-vehicle ranks vehicles 1, land heroes 1.5, turrets 2, infantry 3 and tactical structures 4. Excluded categories and opportunity flags also matter. | XML `LC-D09`; debug build `LC-E32`, `LC-E33` | Missing G5: land priority profile in `src/units/unit_tables_decode.cpp`. |
| LC-29 | Replacement rejects absent/disallowed candidates and accepts a valid candidate when the old target is absent/disallowed. Protected assigned targets remain protected. When low-health classifications differ, favor the target at or below the threshold. Otherwise a strictly better priority wins immediately; if not, differing weapon hittability favors the hittable target, then strictly nearer planar squared distance wins even when priority is equal or worse. Equal distance retains the old target. | debug build `LC-E33`, `LC-E49` | Missing G5: land target replacement comparator. |
| LC-30 | Opportunity scanning waits until its next frame, advances that deadline by one logical second plus synchronized random jitter up to half a second, and traverses players from a synchronized randomized start. Enemy/nonneutral relationships and opportunity permission gate scanning. | debug build `LC-E49` | Missing G5: deterministic land scan cadence/order. |
| LC-31 | Deployment can prohibit firing while undeployed. Body-facing requirements, turret pointability and limited-turn rules can require body turning before the attack state enables weapons. | debug build `LC-E27` | Missing G5: land deployment/facing adapter beside `combat_aim.cpp` and `combat_fire.cpp`. |
| LC-32 | A turret needs its model turret bone; relative yaw and elevation must lie within `Turret_Rotate_Extent_Degrees` and `Turret_Elevate_Extent_Degrees`, or deployed variants. XY-only queries omit elevation. Grenade-style shots use the lob solution for pitch. | debug build `LC-E39`, `LC-E43`; WWP-17 | Missing G5: land turret arc/aim input; shared weapon pointability gate. |
| LC-33 | The ordinary weapon pipeline remains authoritative for range, minimum range, firing bones, cooldown, projectile choice and hit feasibility; land asks that pipeline whether a candidate can be shot. This inspection does not establish terrain ray occlusion or hill/building LOS. | debug build `LC-E60`; WWP-17, WWP-20 and weapons walk; unverified U-LC-2 | Shared with weapons rules in `combat_aim.cpp`/`combat_fire.cpp`; land geometry input unverified. |
| LC-34 | Land splash uses the shared positive-radius/damage, victim-query, distance and armor/shield contract. Infantry and structures do not acquire an extra universal splash immunity just from their kind. No general suppression/morale meter was found in the inspected land targeting/surface/crush services; absence across all other abilities is unverified. | WAD-01 through WAD-30; debug build `LC-E25`, `LC-E27`, `LC-E28`; unverified U-LC-7 | Shared with area-damage rules in `blast.cpp`; research before adding a new morale system. |
| LC-35 | Land reveal selects `Land_FOW_Reveal_Range`, clamps its base to at least 10 (code), applies battlefield/object reveal modifiers and the dense-fog multiplier, then updates the shared grid. | debug build `LC-E59`; sensors/UI walk | Missing G5: land reveal profile beside `fog_cells.cpp`/`visibility.cpp`; shared grid. |
| LC-36 | Reveal service undoes remembered allied revealed cells and records new cells when moved far enough or reevaluation is required; limbo removes its old reveal. The inspected circular grid raster rounds range/cell width plus 0.5 and is not a terrain ray-casting proof. Land fog initialization also supplies terrain Z bounds to presentation. | debug build `LC-E50`, `LC-E51`, `LC-E57` | Shared fog bookkeeping; missing G5 land range/terrain adapter. |

### Garrison and transport

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| LC-37 | The inspected category compatibility test requires intersecting `Garrison_Category` masks when both masks are nonzero; if either is unspecified it permits the pair. This is one gate, not proof that all object kinds can enter every host. | debug build `LC-E11` | Missing G6: garrison compatibility beside tactical command admission. |
| LC-38 | Entrance begins only from the idle entrance state, records the target, subscribes to movement/deletion completion and asks the host to prepare entrance. Movement service rechecks slot availability. Within host entrance distance after subtracting entrant extent, it begins fading; fade progress at or below 0.1 completes entry. | debug build `LC-E38`, `LC-E52` | Missing G6: garrison movement/fade state and host callbacks. |
| LC-39 | Committing entry requires remaining slots at least the entrant's `Garrison_Value`, finishes its formation, teleports/stops it, transfers containment, and handles carried remote bombs. It adds the unit to the host with the host's weapon-enablement policy, then sends story/local sound hooks. | debug build `LC-E10` | Missing G6: containment and slot accounting; shared story hooks. |
| LC-40 | Structures leave occupants' weapons enabled; transport hosts disable them. Shared weapon range uses `Garrisoned_Max_Attack_Distance_Multiplier` (XML 1.5). Thus bunker fire and transport storage cannot share an unconditional firing disable. | debug build `LC-E36`, `LC-E44`; XML `LC-D07`; WWP-20 | Missing G6: host policy; shared garrison weapon-range contract. |
| LC-41 | Exit waits for a stationary host with an admitted locomotor state and an available exit position. Host destruction, slot upgrades, gun assignment, full order eligibility and emergency unload behavior remain U-LC-4. Transport's own inspected service adds no independent unload timing rule. | debug build `LC-E52`, `LC-E61`; unverified U-LC-4 | Missing G6: exit state and placement adapter. |

### Reinforcement admission and population

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| LC-42 | Land reinforcement admission rejects a missing type and uses the player's reinforcement facing to test the complete company layout. Human and AI call paths pass different placement flags; those flag exemptions remain U-LC-5 rather than being assumed equal. | debug build `LC-E01`, `LC-E08` | Missing G7: land placement branch beside `session_step_commands.cpp`. |
| LC-43 | Member placement requires passability, permitted fog, no disallowed footprint overlap and enabled reinforcement coverage. Members without a land model can use their configured unique ground container for the layout (`Unique_Ground_Unit` interface). Every member must succeed at admission even though later creation can partly fail (LC-01). | debug build `LC-E08` | Missing G7: company placement transaction and fallback type. |
| LC-44 | With `Use_Reinforcement_Points=True` (XML), every member begins disabled; allied live modifying objects with positive `Reinforcement_Enable_Radius` enable members at planar squared distance **at most** radius squared. With the flag false, members begin enabled. | debug build `LC-E08`; XML `LC-D07` | Missing G7: allied point coverage query. |
| LC-45 | Enemy modifying objects with positive `Reinforcement_Prevention_Radius` prohibit member positions at planar squared distance **strictly below** radius squared. Dead/deletion-pending objects are ignored. The checked shield-related branch additionally tests base power. Equality is allowed here, unlike bombing prevention LC-63. | debug build `LC-E08` | Missing G7: land prevention query and powered-shield context. |
| LC-46 | In multiplayer tactical land, company cap is the minimum of last-known tactical capacity and the faction curve `Land_Skirmish_Unit_Cap_By_Player_Count` evaluated at controllable player count, then converted to integer. Rebel/Empire curve points are (0,10), (1,10), (8,5); intermediate evaluation belongs to the shared curve decoder. | debug build `LC-E16`, `LC-E37`; XML `LC-D08` | Missing G7: land cap branch beside tactical economy/population. |
| LC-47 | In non-multiplayer land, cap starts at `Max_Ground_Forces_On_Planet` (XML 10). A player other than the defending player is additionally limited by last-known capacity; the defender is not. Invalid players return 0. This branch includes the non-multiplayer case; it must not be generalized into the multiplayer curve rule. | debug build `LC-E16`; XML `LC-D07` | Missing G7: defender-aware land cap query. |
| LC-48 | Last-known capacity is refreshed from the population-capacity query. In tactical mode that query starts at faction base capacity and adds live owned objects' `Additional_Population_Capacity`; multiplayer tactical also includes live allied objects' contributions. Captured land points therefore affect capacity before the separate company-cap clamp. | debug build `LC-E34`, `LC-E40` | Missing G7: capture-to-capacity adapter; shared population registry. |
| LC-49 | Land reinforcement-point XML authors capture transition 10 seconds, capture radius 100, enable radius 250 and land reveal range 275. Base/additional-capacity variants include 100, 1, 2, 3, 4, 5 and 10; faction skirmish variants author 4. These values belong to each point type, not a universal +4 rule. | XML `LC-D05` | Missing G7: land point profiles in `unit_tables_decode.cpp`. |
| LC-50 | Population registration, fractional surviving-member accounting and authoritative recheck are shared contracts; land supplies source-company members and its own cap/placement results. Space hyperspace offsets/timing must not be reused as land transport arrival. | WR-02 through WR-05; debug build `LC-E09`; unverified U-LC-5 | Shared population contract; missing G7 land command/arrival adapter. |
| LC-51 | Landing animation, incoming transport lifecycle, unloading order, transport-death outcome and reinforcement delay consumption are not settled by company creation/placement. The space walk's reinforcement-delay transport interface is relevant, but its arrival sequence is not evidence for land. | WR-18; unverified U-LC-5 | Missing G7: capture-backed land arrival state before implementing its timing. |

### Capture and build pads

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| LC-52 | Capture uses the shared four-logical-frame candidate/contest/progress service. Multiplayer tactical bypasses opening capture delay; single non-multiplayer land uses `Land_Capture_Allowed_Countdown_Seconds` (XML 30), rounded as seconds × logical FPS + 0.5. Neutral/no-owner capture bypasses that countdown. | debug build `LC-E21`; XML `LC-D07`; WBP-04, WHZ-40 through WHZ-47 | Shared capture algorithm in `src/sim/tactical/pads.cpp`; missing G8 land opening-delay input. |
| LC-53 | Candidate eligibility, capture radius, neutralization, contest and ownership retention are the shared capture rules; a reinforcement point is not captured merely by a UI highlight. The point's own inspected service maintains highlighting; ownership comes from capture service. | debug build `LC-E03`, `LC-E21`; WBP-04 | Shared with WBP-04; missing G8 land object registration. |
| LC-54 | Land pads use authored faction build menus. Rebel/Empire skirmish data offers anti-vehicle, anti-infantry, anti-air, bacta, repair and sensor choices; Underworld instead includes mass-driver and torpedo turrets with its support choices. A space pad menu is not the land menu. | XML `LC-D06`; WBP-06 | Missing G8: land menu/profile binding beside `session_economy.cpp`. |
| LC-55 | Shared pad occupancy, owner/faction permissions, price and construction apply. Under-construction hull/progress/deadline and completion replacement follow the build-pad walk; land turrets then use ordinary land weapons, targeting and damage. | WBP-09 through WBP-19; weapons walk | Shared with build-pad rules; missing G8 land child-type registration. |
| LC-56 | Land skirmish pad data authors build-pad collision extents 10 by 10 and `Minimum_Time_Before_Pad_Can_Build_Again=15` seconds, capture radius 100 and capture transition 5 seconds. Capture geometry and menus still come from the type; these are not interchangeable with reinforcement-point geometry. | XML `LC-D06` | Missing G8: land pad geometry/cooldown profile. |
| LC-57 | Capture, build-pad construction, production and battlefield abilities remain separate services. Buying a land turret must use their shared authoritative ownership/cost path, then bind a land object; it is not direct presentation spawning. | WBP-04, WBP-09 through WBP-19; production/abilities walks | Shared service contracts; missing G8 land service adapters. |

### Bombing runs and orbital bombardment

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| LC-58 | Standalone land support initializes each playable nonneutral/nonrogue faction's `Skirmish_Land_Bomber`. Campaign support instead inspects the invading player's orbit fleets for bomber-class ships and bomber craft produced by ship squadron data at the owner's tech. | debug build `LC-E15`; XML `LC-D08` | Missing G9: land support initialization and GC fleet interface. |
| LC-59 | Campaign bombing counts direct bomber objects and spawned bomber members at every tech level up to the owner's current level. With at least one bomber found, clamp member count N to at least 3 (code), then attacker reduction A is `(floor(N/3) - 1) × Bombing_Run_Reduction_Per_Squadron_Percent`; defender modifier sum is D. Interval frames are `max(truncate(max(1+D-A,0) × base), truncate(minSeconds × FPS))`, where base is truncated maxSeconds × FPS. There is no upper clamp after modifiers. XML min/max are 120 seconds and reduction is 0; positive D can still lengthen the interval. The selected bomber supplies `Land_Bomber_Type`. | debug build `LC-E15`; XML `LC-D07` | Missing G9: support cooldown computation and authored modifiers. |
| LC-60 | An available bombing request records the start frame, consumes availability, constructs a path and creates three bomber entries (code count). Their offsets are (0,0,0), (-70,70,0), (-70,-70,0), rotated to the path; each gets a target marker. | debug build `LC-E07` | Missing G9: synchronized bombing-run state and bomber creation. |
| LC-61 | Bombers advance through approach, bombing and leaving states. Availability is gated by battlefield bombing enablement; skirmish can report availability from that enablement, while the campaign path also checks the due frame. The battlefield cooldown/enable bridge remains an abilities interface, not permission to fire continuously. | debug build `LC-E62`; abilities walk | Missing G9: support manager state and ability bridge. |
| LC-62 | A bombing target in fog is rejected. Any active base shield with positive radius blocks a target at **3D distance strictly less** than its radius; this checked shield loop does not filter by owner. | debug build `LC-E30` | Missing G9: support target validation and shield query. |
| LC-63 | Bombing additionally rejects points within **3D squared distance at most** positive `Bombing_Run_Prevention_Radius` squared of objects not owned by the acting player. This local query is not restricted to enemies and does not add its own dead/power/fog filter. | debug build `LC-E30` | Missing G9: bombing prevention query; preserve its different boundary/relationship rule. |
| LC-64 | Orbital support is land-only. Standalone initialization uses the playable faction's first `Bombardment_Required_Orbital_Ships` type; it does not require an actual campaign fleet. Campaign initialization instead matches required types against the invading player's orbit fleet contents. | debug build `LC-E13`; XML `LC-D08` | Missing G9: orbital eligibility and GC fleet interface. |
| LC-65 | Bombardment rejects fogged targets and targets strictly inside an active positive-radius base shield, using 3D distance and no local ownership filter. This target predicate has no bombing-prevention-circle check. | debug build `LC-E14` | Missing G9: separate orbital target predicate. |
| LC-66 | Bombardment consumes authored faction projectile list, salvo count, shots per salvo, initial delay, shot delay and salvo delay. Due shots use a **strictly earlier** next-fire frame; timing converts seconds to logical frames by truncation. Projectile-list index is clamped to its last entry across salvos. Rebel data authors 2 salvos of 8 shots with initial/shot/salvo delays 1.0/0.2/1.3 seconds; Empire authors 3 salvos of 4 shots with 3.0/0.3/1.2 seconds. | debug build `LC-E58`; XML `LC-D08` | Missing G9: deterministic orbital salvo schedule. |
| LC-67 | The first strike is centered. Later multi-shot placement cycles `Bombardment_Distribution` quadrants, chooses a synchronized angle from quadrant × 90 to (quadrant + 1) × 90 - 1 degrees (code), and a radius between max(projectile blast range,1) and blob size minus that minimum. `Bombardment_Offset` (XML 0,0,600) sets firing offset. | debug build `LC-E58`; XML `LC-D07`, `LC-D08` | Missing G9: synchronized strike scatter; shared projectiles/splash. |
| LC-68 | Orbital standalone initialization uses max interval; campaign uses min interval. Both effective values are 260 seconds. Completion clears execution and schedules the next due time; availability observes battlefield enablement and mode-specific due-time handling. Availability sounds distinguish self, ally and enemy; those are support presentation hooks. | debug build `LC-E13`, `LC-E58`; XML `LC-D07`, `LC-D08` | Missing G9: support cooldown/availability snapshots. |
| LC-69 | Support damage uses ordinary projectiles and splash. Bombing/bombard execution refreshes land combat activity; the land ambient-music threshold uses `Music_Land_Battle_To_Ambient_Peace_Seconds × logical FPS` with an inclusive elapsed comparison (XML 15 seconds). Weather, hero-clash and announcements are serviced before common mode service. | debug build `LC-E02`, `LC-E58`; XML `LC-D10`; weapons/area-damage walks | Shared damage contracts; missing G9 land manager/frame and presentation hooks. |

### Victory, retreat and return

| ID | FoC rule | Source | Ours / integration |
|---|---|---|---|
| LC-70 | Land skirmish base-component elimination searches **enemy-related** base components that are victory-relevant, alive and not deletion-pending. If none remain, it passes. This differs from the space-starbase ownership filter; allies' land bases do not prevent this condition. | debug build `LC-E22`; WBF-32 | Missing G10: land conditions beside `src/sim/tactical/victory.cpp`. |
| LC-71 | Land HQ elimination searches enemy-related ground structures with `HQ_Win_Condition_Relevant`, excluding dead/deletion-pending objects. Domination requires at least one `Control_Point_Domination_Condition_Relevant` capture point and all such points allied to the tested player. Its predicate has no private hold timer; winner delay belongs to registration. | debug build `LC-E22` | Missing G10: HQ/domination selection and relationship tests. |
| LC-72 | All-enemy-unit elimination, condition installation, destruction-triggered reevaluation, first-winner retention and pending-victory damage gating are shared contracts. A map-selected base/HQ condition does not silently become all-units elimination. | WBF-08, WBF-31, WBF-32, WBF-35, WBF-37, WBF-40 | Shared with battle-flow rules; missing land condition dispatch in `victory.cpp`. |
| LC-73 | Land retreat rejects single-player and multiplayer skirmish, but its inspected eligibility path does **not** contain the space branch's unconditional multiplayer-tactical rejection. Campaign/conflict players, opening lockout, other player's active retreat, playable/tutorial permissions and land retreat-prevention modifiers remain gates. | debug build `LC-E42`; WR-42 comparison | Missing G10: land retreat eligibility; do not copy the broader space exclusion. |
| LC-74 | `Land_Retreat_Allowed_Countdown_Seconds` is an opening lockout (XML 30). Executing retreat rechecks idle/eligibility, collects units and markers, disables/deselects eligible units, orders them to markers and starts the faction's `Land_Retreat_Countdown_Seconds`, truncated to frames. Rebel/Empire XML 10.99 gives 329 frames at 30 FPS. | debug build `LC-E41`, `LC-E54`; XML `LC-D07`, `LC-D08` | Missing G10: land countdown/marker state beside session battle flow. |
| LC-75 | Retreat roster includes owned nondead objects with a locomotor, or nonmobile objects whose type authors `Stay_In_Transport_During_Ground_Battle`. Ordinary immobile structures are not automatically evacuated. | debug build `LC-E55` | Missing G10: land survivor roster and transport-stored eligibility. |
| LC-76 | Retreat service subtracts elapsed logical frames, refreshes markers and rechecks eligibility during countdown, then calls escape, sets escaped state and immunity before completing victory/return. This inspected land service is not a space hyperspace-flight sequence. | debug build `LC-E41` | Missing G10: land escape/completion state; shared battle-end contract. |
| LC-77 | The inspected default land safety predicate accepts every nonnull unit; it supplies no marker-distance safety threshold. The attrition path's unsafe persistent nongroup branch therefore cannot justify inventing proximity-based losses. Effective `Land_Retreat_Attrition_Factor=0`; specialized overrides remain U-LC-8. | debug build `LC-E56`, `LC-E63`; XML `LC-D07` | Missing G10: default land attrition interface; specialized behavior unverified. |
| LC-78 | Winner countdown, results and parent-mode return use the shared battle-flow contract. Company survivor identities, persistent bases/heroes and retreat results must cross the GC handoff/return interface; tactical deletion must not erase persistent identities. Campaign-specific base/rogue victory branches remain U-LC-8. | WBF-37, WBF-41, WBF-42; GC battle-handoff walk; debug build `LC-E22` | Shared return contract; missing G10 land outcome adapter. |

## Gap ownership

The table has 78 rules: 66 **missing** land integrations and 12 **shared** contracts
or shared contracts with a missing land input. There are 0 measured **differs** rows.
Shared is a contract classification, not proof of complete land support. The inventory is tracked as the land combat rule/gap walk (legacy EAWR-1449). Existing land programme work (legacy EAWR-17)
is the umbrella; existing GC return work owns persistence rather than a second land
implementation of campaign return.

| Gap | Size | Rules / deliverable | Ownership |
|---|---|---|---|
| G1 | L | LC-01 through LC-06: company expansion, source identity, squad grouping and land object-kind dispatch. | Land company/squad integration (legacy EAWR-1450). |
| G2 | L | LC-07 through LC-15: movement masks, soft footprints, formation destinations, width repairs and speed context; capture bridge/crowd behavior. | Land movement integration (legacy EAWR-1451). |
| G3 | M | LC-16 through LC-22: reversible terrain modifiers, cover, hazards and evidence for mud/surface transitions. | Land terrain simulation (legacy EAWR-1452). |
| G4 | M | LC-23 through LC-25: crush volumes, category/permission gates and ordinary damage events. | Land crush integration (legacy EAWR-1453). |
| G5 | L | LC-26 through LC-36: targeting cadence/priorities, deployment/arcs and fog inputs; settle LOS and suppression scope. | Land targeting/fog integration (legacy EAWR-1454). |
| G6 | L | LC-37 through LC-41: garrison entrance, containment, slots, firing policy and exit; settle destruction/upgrades. | Land garrison integration (legacy EAWR-1455). |
| G7 | L | LC-42 through LC-51: whole-company landing admission, capture capacity and mode caps; verify transport arrival timeline. | Land reinforcement integration (legacy EAWR-1457). |
| G8 | M | LC-52 through LC-57: land point/pad profiles and shared capture/construction adapters. | Land control/build-pad integration (legacy EAWR-1458). |
| G9 | L | LC-58 through LC-69: bombing/orbital eligibility, geometry, deterministic timings, battlefield enablement and projectiles. | Land support integration (legacy EAWR-1459). |
| G10 | L | LC-70 through LC-78: land win conditions, retreat gates/roster/timing and GC outcome adapter. | Land battle-end integration (legacy EAWR-1460); reuse GC return (legacy EAWR-1116). |

The highest-impact land prerequisites are company identity/grouping (G1), terrain
movement/width (G2), authoritative landing/capacity (G7), target/fog/deployment (G5)
and map-selected victory (G10). This preparation does not change the current space
battle or make a measured space fidelity claim.

## Tag-status audit

The audit is per `(object class, tag)` at the comparison base; a tag applied to space
does not certify its ground classes. This docs-only walk does not change registry
statuses. Full matching rows are retained privately; the land consumers to implement
are grouped below. Related visual/sound tags stay with presentation work.

| Registry status | Land-relevant tags and scope |
|---|---|
| `todo` | `Use_Reinforcement_Points`, some classes of `Reinforcement_Prevention_Radius` and `Additional_Population_Capacity`; `Garrisoned_Max_Attack_Distance_Multiplier`; generic classes of `Is_Squashable`, `Is_Squashable_By_Supercrusher`, `Is_Supercrusher`; `SurfaceFX_Name`; generic turret rotate/elevate extents and speed; battlefield `Enable_Bombing_Runs`/`Enable_Planetary_Bombardment`; `Bombing_Run_Prevention_Radius`, min/max bombing intervals and squadron reduction; min/max bombard intervals, `Bombardment_Required_Orbital_Ships`, projectile, salvo/shot counts and delays, offset, distribution and blob size. Container/hero `FormationSpacing` is also todo; `HQ_Win_Condition_Relevant` currently has a presentation todo row and needs a simulation application. Generic garrison-upgrade admission/apply-to-allies rows remain shared ability work; extra land slots are G6. |
| `deferred` | `Destination_Collision_Query_Extension` and `BetweenFormationSpacing`; G2 must consume their land collision query values. |
| `land-or-galactic` | Ground classes of `Company_Units`, `Create_Team`, `Create_Team_Type`, `Max_Squad_Size`; `Garrison_Category`, value, slots, entrance/exit distances and bones; crush flags and `Squash_Damage_Type`; `SurfaceFX` setting children (`Defense_Mod`, `Damage_Mod`, reveal/fire/speed mods, terrain damage/delay), `Surface_Type_Cover_Damage_Shield`; land reveal/capture/retreat tags; `Max_Ground_Forces_On_Planet`, land cap curve, point capacity, `Land_Bomber_Type`, `Skirmish_Land_Bomber`, deployed turret extents. Their sourced exclusion from the space milestone is not evidence of land implementation. |
| `partial`, `applied` or presentation-only | Company/team rows for space hero/container classes, custom soft footprints, reinforcement prevention and turret extents have other consumers. Ground variants still need the table's land application. Land models, status icons, fog colors, audio, tracks and support overlays are presentation interfaces, not simulated consumers. |

Movement-class XML has a wildcard `todo` row in the generic combat tag backlog
(legacy EAWR-650). Reuse its loader work where applicable; G2 supplies the land
application and evidence. The audit also finds `Max_Ground_Base` and
`Surface_Bombardment_Capable` marked `foc-ignores`: this walk does not read them and
does not convert their names into new behavior. The mod API names above describe
data interfaces, not private debug symbols.

## Unverified and capture plan

| Question | What remains unknown | Evidence that would settle it / owner |
|---|---|---|
| U-LC-1 | Mud's authored speed effect has no consumer in the checked surface branch. Direct special-surface-to-special-surface transitions may preserve hazard timing; the reset path is not a blanket cleanup. | Same infantry across clear/slow/water/mud and lava-to-good-ground, frame-stepped speed/damage measurements; G3. |
| U-LC-2 | Terrain/building occlusion, hill LOS and projectile collision are not proved by the circular fog raster or weapon hit-feasibility call. | Paired units across ridge/wall, fog-enabled/disabled, direct/arc projectiles with logical target/fire events; follow the geometry consumer first; G5. |
| U-LC-3 | Full formation candidate enumeration, bridge queueing, crowd avoidance/stalls, crusher flee timing and immunity transitions. | Infantry/bike/large walker across one-cell and wider bridges, blocked destination, allied/enemy crowd and normal/supercrusher, with frame positions; G2/G4. |
| U-LC-4 | All garrison order gates, host gun distribution, upgrades, destroyed-host survival, forced unload and selection semantics. | Structure versus transport, full slots, incompatible categories, moving unload, upgrades and host destruction; trace containment and weapons; G6. |
| U-LC-5 | AI placement flag exemptions, complete reinforcements command checks, transport delay/loading/unloading, landing obstruction and transport death. | One company landed at enable/prevention/fog boundaries, human versus AI, blocked member, kill transport during each phase, log pool/population/identity changes; G7. |
| U-LC-6 | Same-frame race between capture, unit death, capacity update, reinforcement command and victory registration. | Controlled command sequence at a capture/death boundary with logical frame trace, following the shared frame-order contract; G7/G8/G10. |
| U-LC-7 | No generic suppression/morale meter was found in the inspected services; absence across specialized abilities/story paths is not proven. | Search authored ability consumers and observe infantry under repeated near-miss/explosive fire before defining a new mechanic; G5. |
| U-LC-8 | Campaign-specific base/rogue win conditions, specialized retreat safety overrides and persistent survivor/attrition details beyond the shared return boundary. | Campaign land attacker/defender victory/retreat with stored units and bases; trace the specialized condition and return payload; G10 plus existing GC return. |

No unverified row is an implementation requirement for an invented default. Each gap
ticket must retain these evidence tasks and record the resulting rules before filling
the uncertain branches.
