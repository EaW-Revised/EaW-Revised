# Walk: projectile blast-area damage and special weapon hardpoints

## Applicability and evidence

Walk 16, tactical space, audited 2026-10-02 against remake commit `65bcfc03`.
This is an implementation contract and gap inventory; it changes no game code or roster gate.
Implementation and remaining evidence are tracked together (legacy EAWR-1076).
Sources are **debug build**, **XML**, existing **behaviour notes**, or explicitly **unverified**.
Opaque EAD evidence IDs identify read-only debug-build observations; their private map and raw
notes stay in ignored research output. No new retail recording was made for this walk.

The scope is projectile damage to recipients besides the direct hit, the direct-hit inputs
needed to avoid double damage, and `HARD_POINT_WEAPON_SPECIAL` firing. It includes effect-only
projectiles created by abilities or destruction, but does not own their creation/activation.
The [weapons walk](weapons.md) owns flight, collision, ordinary targeting and burst timing;
[capital combat](capital-combat.md) owns shields, armor, hull/hardpoint damage and death;
[abilities](abilities.md) and [heroes](heroes.md) own activation, duration, recharge and autofire;
[hazards](hazards.md) owns map-object relationships and destruction. Mass-driver rules remain
outside this walk. XML names are public data identifiers, not debug-build implementation names.

## Frame entry points and handoffs

Weapon hardpoints run with their parent each logical frame (30 frames/s). SPECIAL enters the
same service as other weapon hardpoints; it has no independent area-damage timer. The firing
attempt reads ability charge state, selects a projectile and returns shot consumption to the
ability owner. Projectile service subsequently tests collision before advancing position and
checking expiry. A direct object hit first invokes ordinary damage, then sound, then area damage
unless the damage-cancellation output requests suppression, then other detonation services.
An expiry without collision invokes area damage at the new projectile position and destroys
the projectile. An explicit explosion invokes the same area recipient service at its current
position. Area recipients are visited synchronously in that call, not in a recurring radius
scan on every flight frame (debug build EAD-01–08, EAD-15, EAD-21).

Frame ordering between different objects/behaviours belongs to the [frame-order walk](frame-order.md).
The order below is local to each entry point: first the trigger, then recipient evaluation,
and separately the SPECIAL firing attempt. Do not run impact and expiry blast twice for one
terminal flight step. Pending delayed damage is an interface to the damage scheduler.

## Rules in evaluation order

Verdicts compare this snapshot's executable path, including its data plumbing. **Same interface**
means an existing shared service already supplies the listed contract, not that area damage is
implemented. Gap groups G1–G9 below identify missing work. Counts include interface rules.

### Trigger and direct-hit boundary

| ID | Rule, branches and constants | Evidence; remake verdict |
|---|---|---|
| WAD-01 | Enable area damage only when both `Projectile_Blast_Area_Damage` D and `Projectile_Blast_Area_Range` R are strictly positive. Code defaults: D=0, R=0, `Projectile_Blast_Area_Dropoff` false, `Projectile_Blast_Area_Dropoff_Tiers` N=5, `Projectile_Blast_Area_Max_Victims` 5000, no immune faction, `Max_Secs_For_AE_Delayed_Damage` 0.8 s, `Projectile_Damage_Delay_Secs` 0, `Projectile_Damages_Random_Hard_Points` false. Variants inherit before overriding. | debug build EAD-01–04, EAD-16/22; XML; **missing**, G1 |
| WAD-02 | On an object hit, use the projectile instance's direct damage. If it is **exactly zero**, and D>0 and R>0, substitute D for that direct hit before ordinary damage routing. A positive instance damage, including a hardpoint/ability override, takes precedence. The fallback is not multiplied by falloff. | debug build EAD-02; **differs**, G2 |
| WAD-03 | Run ordinary direct damage first, obtaining the final routed collision-mesh name and the damage-cancellation result. Emit the detonation sound, then call area damage at the contact with that object/mesh as exclusions only when D>0, R>0 and cancellation is false. Cancellation comes from a successful take-damage ability; **shield absorption alone does not cancel the blast**. Ability dispatch is an interface. | debug build EAD-02/05/10/20; **missing**, G1 |
| WAD-04 | Collision is resolved before expiry. With no hit, travel-limit expiry, lifetime expiry and authored `Explode_When_Reached_Target_Radius` termination blast at the updated position. ROCKET/MPTL_ROCKET target-radius termination uses distance along its path; DEFAULT uses squared distance travelled past its fire-at point. A rocket that cannot obtain its next path point forces terminal advancement at its current position. Diamond Boron is ROCKET, not MISSILE; preserve the rocket-flight interface rather than assigning homing semantics by its name. | debug build EAD-03/15; XML; **differs**, G4 |
| WAD-05 | Explicit explosion, including an ability-driven detonation, calls area damage at the projectile's current position with no direct object or mesh exclusion when D>0 and R>0. The caller owns arming, proximity detection and subsequent deletion: these are not extra blast-recipient conditions. | debug build EAD-04 and area-entry caller lookup; **missing**, G1 |
| WAD-06 | A terminal hit/expiry destroys the projectile after its effects. Projectile shutdown releases path presentation and adds no second blast. Generic deletion is not itself an instruction to explode. Destruction/ability services may explicitly request explosion or create a separate blast projectile; their trigger remains with their owning walk. | debug build EAD-03/21; **same interface**, projectile removal in `session_step.cpp` |
| WAD-07 | Impact selects the armor-reduced sound when the direct-hit armor result <=0.75 (code), falling back to `Projectile_SFXEvent_Detonate`. Expiry/explicit explosion requests `Projectile_Lifetime_Detonation_Particle`; its sound runs only if that effect type exists and creation succeeds. The area-damage call is independent of particle existence or successful rendering. | debug build EAD-02–05; **missing**, G9 |

### Area recipients and damage delivery

| ID | Rule, branches and constants | Evidence; remake verdict |
|---|---|---|
| WAD-08 | Resolve the projectile owner, then visit the current player list by index. Ordinarily query only players the owner counts as enemies. Own units, allies and non-hostile neutral objects are excluded by this relationship test. Do not replace it with different-owner or different-faction tests. | debug build EAD-01; WHZ-51 interface; **missing**, G1 |
| WAD-09 | If `Projectile_Blast_Area_Immune_Faction` resolves to a faction, query **every** player, then exclude each recipient whose owner faction equals that faction. This can admit the shooter, allies or neutrals of other factions; immunity is faction-wide, not owner-only. Direct collision remains WWP-66 and is not widened by this area tag. | debug build EAD-01; **implemented**, `src/sim/tactical/blast.cpp` faction query and immunity |
| WAD-10 | For each player query, start with XML R and multiply it by the current scatter-radius mode multiplier of the object that fired the projectile, if that object still resolves. If the source no longer exists, use unmodified R. This is a current source-state lookup at detonation, not a launch snapshot; D is not multiplied by this radius factor. Modifier production belongs to abilities. | debug build EAD-01; **missing**, G1 |
| WAD-11 | Query that player's collidable spatial tree with no required behaviour, max returned candidates 2147483647 (code), and no distance sorting. Collect a box spanning centre XY +/-R and Z +/-1e18 (code). Exclude objects in limbo. This is a spatial query, not a scan of every ship or every craft each flight frame. | debug build EAD-01/09; **missing**, G1 |
| WAD-12 | Admit a collected object when its **planar centre distance squared <=R²**. If its centre is outside, admit it when its transformed collision bounds overlap the 3D sphere of radius R; base-shield objects use their special hard extents for that fallback. Consequently a large hull can qualify with its centre outside R, and the centre-distance branch does not reject altitude separation. Bounds service details are a collision interface. | debug build EAD-09; **missing**, G1 |
| WAD-13 | Copy the player's query result before delivering damage. Visit it in tree-collection order, not nearest-first and not by entity ID. The area service adds no fog, targeting-category, fighter/capital, death-clone or explicit alive check. Damage routing can independently reject damage; absence of a local filter does not guarantee that every collected object can lose health. Exact tree ordering across retail sessions is U-02. | debug build EAD-01/09; **missing**, G1 |
| WAD-14 | Skip the directly hit object when it has zero configured destroyable hardpoints. Otherwise retain it for damage to other hardpoints. Require `Collidable_By_Projectile_Living` on the recipient, even on this area path, then apply WAD-09 immunity. Destroyable count includes destroyed hardpoints; it does not mean surviving health. | debug build EAD-01/26; **missing**, G3 |
| WAD-15 | Start recipient distance d as planar centre distance. Only when dropoff is true and effective R>0, test the segment from blast centre to object centre against that recipient's collision geometry. Starting intersection gives d=0. A contact gives its planar distance; cap that refined distance to the original centre distance. No contact keeps centre distance. This tests the recipient, not intervening ships/obstacles; it is not line-of-sight occlusion of a blast. | debug build EAD-01; **missing**, G1 |
| WAD-16 | Without dropoff, multiplier F=1. With dropoff and R>0, use **F=clamp(1-floor(d*N/R)/(N+1),0,1)**. The same object-level F applies to all its hardpoint shares. This is a stepped function, not linear decay. For Diamond Boron (R=200, N=5), distance bands at 0/40/80/120/160/200 have factors 1, 5/6, 4/6, 3/6, 2/6, 1/6; an admitted large object outside R can reach later bands or zero. | debug build EAD-01; XML; **missing**, G1 |
| WAD-17 | For a recipient with no destroyable hardpoints, area delay is 0 when the chosen/refined d<1 (code); otherwise `(d/R)*Max_Secs_For_AE_Delayed_Damage`. XML sets that maximum to 0 on all stock positive-area projectile definitions after inheritance. R and delay still remain data inputs; the code default maximum is 0.8 s. | debug build EAD-01/16; XML census; **missing**, G1 |
| WAD-18 | With no configured destroyable hardpoints, deliver **D*F once** to the object's ordinary damage service, marked as area damage, with no contact, normal or collision-mesh selector. Fighters are individual collidable recipients; the blast service does not substitute a squadron container or divide its damage by craft count. Squad/team forwarding remains the damage/squadron interface. | debug build EAD-01/05; **missing**, G3 |
| WAD-19 | With at least one configured destroyable hardpoint, inspect hardpoints in authored order. Include each `Is_Destroyable` hardpoint whose current world position is at **3D squared distance strictly <R²**. Do not require `Is_Targetable` or current health>0. Already destroyed destroyable hardpoints therefore participate in the share count even though ordinary damage routing discards damage to them. | debug build EAD-01/26; **missing**, G3 |
| WAD-20 | On the directly hit object, exclude each destroyable hardpoint whose `Collision_Mesh` case-insensitively matches the final mesh name returned by direct damage. Use that returned route, not necessarily the original collision triangle: an aimed-hardpoint override may have changed it. On expiry/explicit explosion there is no such exclusion. | debug build EAD-01/02/10; **missing**, G3 |
| WAD-21 | For K selected hardpoints, deliver **D*F/K to each**, passing that hardpoint's `Collision_Mesh` and the area flag. Its delay is `(3D hardpoint distance/R)*Max_Secs_For_AE_Delayed_Damage`, with no d<1 shortcut. F remains the object's factor, not a freshly computed per-hardpoint factor. Build the selected list before applying its damage. | debug build EAD-01; **missing**, G3 |
| WAD-22 | If the recipient has destroyable hardpoints but none qualify, deliver no hull fallback. Do not distribute the full D to every hardpoint, divide by every authored hardpoint, or reassign destroyed shares to living ones. The direct object can receive its direct hit plus a separate D*F budget distributed to its other qualifying hardpoints. | debug build EAD-01; **missing**, G3 |
| WAD-23 | Deliver immediate secondary damage through the **same ordinary damage service** as a projectile direct hit, retaining source projectile, damage-type override and shield/energy/hitpoint flags, but set the area flag. That flag prevents the projectile's original aimed-hardpoint route from overriding WAD-21's secondary selector and is supplied to take-damage abilities. It does not bypass shields or armor. | debug build EAD-05/10/20; **missing**, G3 |
| WAD-24 | Shields, energy and hitpoints consume each secondary delivery through WCC-22/46–48 and DG-05–12. The Krayt damage projectile sets shield damage No and hitpoint damage Yes; Diamond Boron sets both Yes. A shield-damaging blast can drain a shield through several hardpoint shares; a shield-bypassing blast retains its bypass. Shield absorption of the primary impact is not the WAD-03 cancellation flag. | debug build EAD-05/10; XML; **same interface**, `damage.cpp::apply_hit` |
| WAD-25 | Shooter cause-damage, recipient take-damage/combat-defense and diminishing-firepower rules still run per immediate secondary damage call. WAD-21's D*F is a **pre-routing** budget, not a promise of that much total lost health. Repeated hardpoint deliveries can encounter state changes from earlier deliveries, including shield-generator destruction and the last-hit frame. Do not combine deliveries into one hull hit. | debug build EAD-10; WCC-22/44 and DG-05; **same interface**, `apply_hit` / ordered damage commit |
| WAD-26 | A positive `Projectile_Damage_Delay_Secs` overrides the area distance delay with a synchronized uniform float draw in `[0.25*tag, tag]` per delivery (0.25 is code). Nonpositive final delay routes immediately. Positive delay stores the raw amount, internal damage kind, collision selector, source owner and projectile XML damage type; remaining frames are `max(1,trunc(delay*logical FPS))`. The service decrements the counter and delivers on zero with no source object or area flag, retaining the owner/type/selector. Projectile damage flags, source modes and original aimed hardpoint are not retained. Ordinary target defense, armor and take-damage modes still apply; projectile diminishing/energy gates do not. | debug build EAD-05, targeted delayed-record read, EFO-04/WFO-14; implemented |
| WAD-27 | If `Projectile_Damages_Random_Hard_Points` is true, each shared projectile-delivery call draws one synchronized random index across all authored hardpoints, then scans forward with wrap for the first alive, destroyable hardpoint. Targetability is not checked; selection is not uniform over eligible hardpoints. When found, its collision mesh replaces the selected mesh while the original split-share amount is retained, before immediate/delayed delivery. Each share reruns selection and can reach an otherwise direct-excluded mesh. Neither flagship damage projectile nor Diamond Boron sets this flag. | debug build EAD-05; targeted chooser read 2026-10-02; XML; **missing**, G8 |
| WAD-28 | Secondary area calls return before impact-particle, recipient hit-particle and flinch creation. Do not spawn the main detonation particle or sound once per victim/share. Ordinary damage side effects, such as shield flashing and hardpoint death effects, still belong to damage/presentation services. Particle/sound inputs are enumerated below. | debug build EAD-05; **missing**, G9 |
| WAD-29 | Count a victim after the direct-object, living-collision and immune-faction gates, before delivery. Even a recipient with no qualifying hardpoints consumes a victim slot. Reset this counter for each queried player; after each accepted recipient, if count >=`Projectile_Blast_Area_Max_Victims`, return from the **entire** blast. Default 5000 is code; no active stock definition overrides it. This is not a query cap, a nearest-N rule or a cap on hardpoint deliveries. Zero/negative values still reach the comparison after the first accepted victim. | debug build EAD-01/16; XML; **missing**, G1 |
| WAD-30 | A weapon's `Fire_Category_Restrictions` filters which direct target it can fire at; it is not a blast-victim category mask. The Krayt SPECIAL guns prohibit Fighter/Bomber/Transport **targets**, while nearby collidable craft can receive the damage blast. Area shields and hardpoint-distance checks still decide whether any health changes. | debug build EAD-01/06; XML; **missing**, G3 |

### SPECIAL firing and ability interfaces

| ID | Rule, branches and constants | Evidence; remake verdict |
|---|---|---|
| WAD-31 | `HARD_POINT_WEAPON_SPECIAL` is a weapon type for ordinary hardpoint service, alongside laser/missile/torpedo/ion/mass driver. It is distinct from `HARD_POINT_ENABLE_SPECIAL_ABILITY`. A SPECIAL with no selected projectile may legitimately fail its firing attempt without creating a shot; do not invent a default projectile for the Kedalbe leech hardpoint. | debug build EAD-06/07/11/17; XML; **missing**, G5 |
| WAD-32 | SPECIAL uses WWP-01–12/13/15–35 unchanged for setup, weapon enable, health/disable/stun/limbo gates, recharge, target and cone/range tests, aimed hardpoint selection, projectile launch and burst timing. It does not require activation merely because its enum says SPECIAL. This is a shared-service interface, not a second targeting implementation. | debug build EAD-06–08; **missing**, G5 (classification prevents entry) |
| WAD-33 | Krayt and Peacebringer both use the two Krayt SPECIAL gun records below. Both opportunity flags are authored False: without an assigned target they do not opportunity-fire; with a target they use the WWP-08 ordered-target/formation gate and do not switch to another opportunity target after a failed attempt. Ordinary targeted shots need no BLAST activation. Krayt's XML has SELF_DESTRUCT, not BLAST. Ability **autofire** and ordered weapon firing are separate interfaces. | XML; debug build EAD-08; **missing**, G5 |
| WAD-34 | Charging BLAST causes every hardpoint firing attempt to fail. When charged, only SPECIAL attempts can proceed. Read the parent's ability state at the attempt; a charged state alone supplies no target and does not bypass cooldown, fog, category, range or cone gates. Initiation, 5 s charge and 90 s ability recharge are WHE-35/36 interfaces. | debug build EAD-06; XML; **missing**, G6 |
| WAD-35 | Start with `Fire_Projectile_Type`; when charged and `Blast_Ability_Fire_Projectile_Type` resolves, replace it with that type; then apply the ordinary target/hardpoint-specific unit ability projectile override. The Krayt damage gun has a charged override; its ion gun has none and keeps its ordinary ion projectile. | debug build EAD-06; **missing**, G6 |
| WAD-36 | On successful charged projectile creation, consume one shared charged-shot count and multiply **instance direct damage** by BLAST's `Damage_Multiplier` if that ability resolves. Charge state is true while its shared counter is negative; each shot advances it toward zero, so the last shot immediately ends the charged-fire gate. There is no per-gun already-fired test here. XML Peacebringer multiplier=5: direct ion 600 becomes 3000; charged damage 600 becomes 3000. **Area damage is still the selected type's D=400, R=250**, not 2000 and not instance damage. Presentation cleanup is handed back to the ability's next service, U-06. | debug build EAD-01/06/12/24/25; XML; **missing**, G6 |
| WAD-37 | Both Krayt guns: one pulse, min/max recharge 10 s (300 frames at 30 fps), pulse delay 0.2 s, range 2200, cone 40 by 180 degrees. Damage gun delays projectile appearance/movement 30 frames; ion gun has no authored delay. WWP-26–33 still own damage-type precedence, travel allowance and modified cooldown. The damage gun's `Damage_Type` **Damage_Star_Destroyer** overrides its projectile's type on both direct and area hits. | XML; debug build EAD-06; **missing**, G5/G7 |
| WAD-38 | BARRAGE gives Broadside/Marauder an area-target proxy and `Projectile_Types_Override`=Diamond Boron Barrage. Its XML inherits D=150, R=200, dropoff true/N=5, but overrides speed 7.5 to 12 and flight distance 5000 to 3000. Both ships author expiry 10 s, recharge 40 s, fire-rate multiplier 3, fixed inaccuracy override 320 and target-position Z offset -150; normal own-weapon pulse count 5, pulse delay 1 s, recharge 8 s. These are inputs/outputs to ability/weapon walks, not a new blast formula. The proxy is `Dummy_Barrage_Target`, assigned an enemy owner and attacked through ordinary targeting; its creation/removal and ability duration stay with abilities. | XML; debug build EAD-13/14; WWP-45/52 interface; **missing**, G6 |
| WAD-39 | Underworld level-3 main cannon is SPECIAL but `Requires_Manual_Target_Assignment` Yes. It requires a nonnull enemy target, no category restriction, pointability, visibility and planar range plus soft radius/minimum range. Assign a manual target only when no manual target is pending; remember the requesting player and frame. No manual target means no shot. Weapon-state validation failure beyond 300 frames (code) drops it with local negative feedback; a successful attempt clears it. A failed later firing attempt, such as cone alignment, does not by itself enter that timeout branch. | debug build EAD-08/18/23; XML; **missing**, G7 |
| WAD-40 | The manual cannon's weapon recharge is 1 s, distinct from XML `Manual_Hardpoint_Firing_Cooldown_Secs` 120 and the player command-readiness interface. Successful manual firing records that requesting player's last manual-fire frame and `round(120*30)=3600` cooldown frames. Readiness is 1 if the stored cooldown <1 or elapsed frames >=cooldown, otherwise elapsed/cooldown; this is shared player state, not per station. It authors one pulse, pulse delay 0.3 s, range 20000, appearance delay 15 frames, turret yaw/pitch extents 360/30, rotation speed 0.5, firing cone 40/3, and Fighter/Bomber/Transport restrictions. It launches `Proj_Underworld_Station_Main_Cannon`: direct 6000, D=1000, R=300. Ordinary turret service and player readiness must both be preserved; command rejection remains the input consumer's interface, U-08. | XML; debug build EAD-06/08/18/23/27–29; **missing**, G7 |

**Original walk snapshot: 40 rules, same interface 3 (WAD-06/24/25), differs 2
(WAD-02/04), missing 35.** The implementation boundary below supersedes those
historical verdicts for the blast service.
The two differing rules are specific mismatches at the boundary of otherwise missing features;
the original walk snapshot gated all four named ships. Current RG-02 review enables
Broadside/Marauder ordinary weapons while retaining the SPECIAL and optional ability gates.

## Stock data census

### Implemented blast query boundary

The blast service now loads inherited WAD-01 inputs, applies WAD-02 primary fallback, prepares
WAD-08–13/15–17/29 recipients in a partitioned detonation phase, and commits in projectile
creation order, player-table order and existing CO tree-collection order. The last ordering
choice is **U-02 unverified project policy**, preserving the existing tree history without a
distance or entity-ID sort. Copy work is counted before the victim cap. Flying projectiles
perform no blast queries. If a source mode differs from one, workers also prepare the unmodified
radius alternative; commit selects independently for each player group if an earlier hit or
this blast's earlier player group removed the source. Later groups are staged even when one
alternative reaches the cap, because only the selected group may exit the whole blast. Both
queries, including staged groups after that possible cap, count toward the work budget.
WAD-05 has an explicit projectile-service request; generic deletion
does not request one. Existing travel expiry uses its updated position; G4 rocket
flight and target-radius termination are implemented in `projectiles.cpp` under WAD-04;
BARRAGE availability remains independently gated under RG-03.

Secondary delivery now implements WAD-14/18–23/30 through the ordinary damage service.
Recipients without destroyable hardpoints receive one hull delivery, excluding the direct
object. Other recipients select destroyable hardpoints strictly inside the spatial radius,
including destroyed and untargetable shares. The final direct-routed mesh is excluded
case-insensitively before dividing the object-level budget. Shares are prepared from immutable
positions in authored order, then committed separately with the area flag, original damage
type/flags and ordinary modifiers. An empty selected list has no hull fallback. Positive
hardpoint distance delays retain the selected collision route, owner, amount and damage type
in hashed queued state (WAD-26).
The direct damage result carries the routed hardpoint and cancellation handoff; shield
absorption does not suppress the blast. Take-damage ability production remains ability-owned.
Secondary calls emit no duplicate impact event.

**U-01 unverified project policy:** reuse the existing transformed collision box, mesh segment
service and its fixed-point collision grid. The centre test is planar and inclusive; the
fallback sphere test uses the transformed box, rather than the larger tree cull box. Model
boxes support starting intersection; mesh-volume interior classification and base-shield
hard-extents profiles remain collision-service interfaces outside currently simulated space
content. Full representable Z replaces the unrepresentable debug-build query expansion.

**U-04 storage and delivery settled by targeted debug-build reads:** positive explicit or
area-distance delay retains the WAD-26 value record after projectile deletion, with truncated
frame counts and source-less ordinary delivery. No projectile flags or area context survive
that handoff. The remake retains its phase ordering in due-frame/creation order before new
impacts; the original uses the per-object WFO-14 service turn. The exact same-frame ordering
runtime witness remains open. G8 still owns random hardpoint selection. The U-05
selector is settled by the targeted debug-build read recorded in WAD-27; its runtime
share/exclusion witness remains open. U-03 and U-06–08 remain named unverified requests
below; this implementation changes none of their ability, flight or station-command policies.
The validator bounds tiers to
0–1024 and the delay maximum to 0–3600 seconds for deterministic arithmetic; signed victim
caps retain the first-victim comparison, including zero and negative values.

The focused blast contracts cover hardpoint dilution, final-route exclusion, shield-damaging
and shield-bypassing shares, capital-only direct targeting with nearby craft, immediate/delayed
hardpoint routes, planar bands, just-below boundaries, transformed bounds,
recipient collision refinement, hostile/allied/neutral/faction cases, source deletion,
post-recipient caps, delay, impact/expiry/explicit explosion, replay round trips and identical
state/snapshot journals and work at 1/2/4/8 workers. A distant fleet contributes no recipient
work to a small detonation.

XML survey covers every direct `Projectile` definition in effective `projectiles.xml`, resolves
`Variant_Of_Existing_Type`, excludes comments, and selects all definitions with a blast-area tag
after inheritance: **66 definitions**. All positive-area definitions have delay maximum 0.
Of these, 65 have positive D and R. No active definition authors a max-victims override.
`Proj_Remote_Bomb` authors dropoff but no
positive area damage/range; its separate `Remote_Bomb_Blast_Effect` carries the blast.
This census is data, not proof that each type is used in space skirmish. Ground-only projectiles,
bombing-run projectiles and destruction/ability payloads are retained to make tag coverage complete;
their flight/activation remains outside this walk.

### Primary space payloads

| Projectile XML identifier | Direct / area / radius | Dropoff | Shield / energy / hitpoint flags | Use |
|---|---|---|---|---|
| Proj_Ship_Diamond_Boron_Missile | 150 / 150 / 200 | true, 5 tiers | Yes / No / Yes | Broadside and Marauder own weapon; ROCKET; speed 7.5 |
| Proj_Ship_Diamond_Boron_Missile_Barrage | 150 / 150 / 200 | inherited true, 5 | inherited Yes / No / Yes | BARRAGE override; ROCKET; speed 12 |
| Proj_Ship_Krayt_Megaweapon_Ion | 600 / 0 / 0 | default false | Yes / Yes / No | Krayt and Peacebringer ordinary/charged ion shot; LASER; no area tags |
| Proj_Ship_Krayt_Megaweapon_Damage | 400 / 300 / 200 | default false | No / No / Yes | Ordinary damage gun; LASER; speed 16 |
| Proj_Ship_Krayt_Special_Megaweapon_Damage | 600 / 400 / 250 | default false | No / No / Yes | Charged damage override; LASER; speed 16 |
| Proj_Underworld_Station_Main_Cannon | 6000 / 1000 / 300 | default false | No / No / Yes | Manual station cannon; LASER; speed 70 |
| Proj_Harmonic_Bomb_Slave_I | 300 / 300 / 600 | true, 3 | Yes / No / Yes | Slave_I harmonic bomb; arming/autofire belongs to WHE-29 |
| Krayt_Self_Destruct_Blast | 0 / 1500 / 700 | default false | Yes / No / Yes | SELF_DESTRUCT payload; command/countdown belongs to abilities |
| Vengeance_Self_Destruct_Blast | 0 / 900 / 500 | default false | Yes / No / Yes | SELF_DESTRUCT payload; outside the four-ship gate |

Ion-gun flags have energy/shield effects through the existing ion damage interface. Its XML
does not opt into blast, ion-stun-on-detonation or engine-disabling-on-drain. The plasma station
batteries' `Proj_Plasma_Space_Turret_Blast` has no blast-area tags despite its name.

### All blast-tagged projectile definitions

The table uses resolved XML values. D/R=0 means the code default where unauthored; dropoff
false means the code default. N is shown only when dropoff is true. An immune faction is shown
only when authored/inherited. Values are game units. Primary rows also appear here so this is
one complete inventory rather than a selected roster sample.

| Projectile | D | R | Dropoff / N | Immune faction |
|---|---|---|---|---|
| Proj_Ship_Diamond_Boron_Missile | 150 | 200 | true / 5 | none |
| Proj_Ship_Diamond_Boron_Missile_Barrage | 150 | 200 | true / 5 | none |
| Proj_Plex_Missile | 5.0 | 15.0 | false | none |
| Proj_Gungan_Energy_Grenade | 15 | 20 | false | none |
| Proj_ATST_Blaster_Cannon_Red | 4.0 | 25.0 | false | none |
| AT_ST_Barrage | 8.0 | 35.0 | false | none |
| Proj_Flak_Pod | 15 | 75 | false | none |
| Proj_Veers_AT_AT_Max_Power_Laser_Red | 40 | 60 | false | none |
| Proj_Ground_Proton_Torpedo | 20 | 60 | false | none |
| Proj_Ground_Proton_Torpedo_ZC_Turret | 30 | 60 | false | none |
| Proj_T4B_Missile | 10 | 40 | false | none |
| Proj_Ground_SPMAT_Grenade | 90.0 | 30.0 | false | none |
| Proj_TIE_Bomber_Run_Bomb | 50.0 | 100.0 | false | none |
| TIE_Bomber_Bombing_Run_Bomb | 125.0 | 100.0 | false | none |
| Skipray_Bombing_Run_Bomb | 125.0 | 100.0 | false | none |
| Y_Wing_Bombing_Run_Bomb | 125.0 | 100.0 | false | none |
| Proj_Harmonic_Bomb_Slave_I | 300.0 | 600.0 | true / 3 | none |
| Proj_Construction_Pod_Detonation | 3000.0 | 400.0 | false | Rebel |
| Proj_Construction_Pod_Detonation_Underworld_Immune | 3000.0 | 400.0 | false | Underworld |
| Proj_Speeder_Bomb | 75.0 | 80.0 | false | none |
| Proj_Sticky_Bomb | 400.0 | 90.0 | true / 3 | none |
| Proj_Mara_Jade_Sticky_Bomb | 300.0 | 100.0 | true / 3 | none |
| Proj_Kyle_Katarn_Sticky_Bomb | 300.0 | 100.0 | true / 3 | none |
| Self_Destruct_Area_Blast_Effect | 150.0 | 125.0 | false | none |
| Self_Destruct_Star_Base_Level_1_Empire | 1200.0 | 1200.0 | true / 5 | none |
| Self_Destruct_Star_Base_Level_2_Empire | 1400.0 | 1400.0 | true / 5 | none |
| Self_Destruct_Star_Base_Level_3_Empire | 1600.0 | 1600.0 | true / 5 | none |
| Self_Destruct_Star_Base_Level_4_Empire | 1800.0 | 1800.0 | true / 5 | none |
| Self_Destruct_Star_Base_Level_5_Empire | 2000.0 | 2000.0 | true / 5 | none |
| Self_Destruct_Star_Base_Level_1_Rebel | 1200.0 | 1200.0 | true / 5 | none |
| Self_Destruct_Star_Base_Level_2_Rebel | 1400.0 | 1400.0 | true / 5 | none |
| Self_Destruct_Star_Base_Level_3_Rebel | 1600.0 | 1600.0 | true / 5 | none |
| Self_Destruct_Star_Base_Level_4_Rebel | 1800.0 | 1800.0 | true / 5 | none |
| Self_Destruct_Star_Base_Level_5_Rebel | 2000.0 | 2000.0 | true / 5 | none |
| Proj_Wall_Destruction | 150.0 | 45.0 | false | none |
| Proj_Hand_Pulse_Cannon | 15 | 20 | false | none |
| Proj_Dark_Trooper_Missile | 5.0 | 15.0 | false | none |
| Proj_Exploding_Land_Barrel_Detonation | 200.0 | 80.0 | false | Underworld |
| Proj_Ship_Krayt_Megaweapon_Damage | 300.0 | 200.0 | false | none |
| Proj_Ship_Krayt_Special_Megaweapon_Damage | 400.0 | 250.0 | false | none |
| Proj_Krayt_Bombardment_Ion | 500 | 300 | false | none |
| Proj_Krayt_Bombardment_Damage | 2000.0 | 250.0 | true / 5 | none |
| Proj_Rebel_Bombardment_Ion_Cannon | 200 | 50 | false | none |
| Proj_Empire_Bombardment_Turbolaser | 375.0 | 85.0 | true / 4 | none |
| Proj_Felucia_Planet_Spores | 4 | 75 | false | none |
| Proj_Felucia_Planet_Spikes | 8 | 75 | false | none |
| MZ8_Tank_Self_Destruct_Blast | 300.0 | 200.0 | false | none |
| Krayt_Self_Destruct_Blast | 1500.0 | 700.0 | false | none |
| Vengeance_Self_Destruct_Blast | 900.0 | 500.0 | false | none |
| Dekard_Concussion_Blast | 100.0 | 1200.0 | false | none |
| Proj_Drunk_Missile | 500 | 200 | true / 5 | none |
| Proj_Cluster_Bomb | 150.0 | 150.0 | true / 3 | none |
| Proj_Remote_Bomb | 0 | 0 | true / 3 | none |
| Proj_Urai_Cortosis_Blades | 90.0 | 18.0 | false | none |
| Infection_Projectile | 1 | 10 | false | none |
| Proj_Merc_Concussion_Grenade | 13 | 18 | false | none |
| Proj_MAL_Concussion_Missile | 12 | 23 | false | none |
| Proj_MAL_Carbonite_Missile | 8 | 30 | false | none |
| Proximity_Mine_Blast_Effect | 20.0 | 30.0 | false | none |
| Remote_Bomb_Blast_Effect | 250.0 | 120.0 | true / 3 | none |
| Proj_Tyber_Zann_Blaster_Shotgun | 50.0 | 25.0 | false | none |
| Proj_Underworld_Station_Main_Cannon | 1000.0 | 300.0 | false | none |
| Proj_Ewok_Bomb_Of_Doom | 300 | 25 | false | none |
| Proj_Ewok_From_Handler_Bomb_Of_Doom | 300.0 | 25.0 | false | none |
| Proj_MDU_Rocket_Pod_Rockets | 12 | 15 | false | none |
| Proj_Empire_MDU_Concussion_Grenade | 20 | 20 | false | none |

### Every SPECIAL hardpoint and its users

Seven direct SPECIAL hardpoint definitions occur in `hardpoints_underworld.xml`:

| XML hardpoint | Projectile / role | Key authored values | Direct users |
|---|---|---|---|
| HP_KRAYT_MEGAWEAPON_ION | Proj_Ship_Krayt_Megaweapon_Ion | health 1000, targetable/destroyable Yes; no opportunity fire; WAD-37 | Krayt_Class_Destroyer, The_Peacebringer |
| HP_KRAYT_MEGAWEAPON_DAMAGE | Proj_Ship_Krayt_Megaweapon_Damage; charged override Proj_Ship_Krayt_Special_Megaweapon_Damage | health 1000, targetable/destroyable Yes; no opportunity fire; WAD-37 | Krayt_Class_Destroyer, The_Peacebringer |
| HP_KEDALBE_SHIELD_LEECH_00 | No projectile; shield-leech attachment/durability interface | health 250, targetable/destroyable Yes; no fire/recharge/cone tags | Kedalbe_Battleship; its LEECH_SHIELDS ability belongs to abilities/heroes |
| HP_Dekard_Blast_00 | Dekard_Concussion_Blast | targetable/destroyable No; cone 360/360, range 1200, one pulse, delay 0.2 s, recharge 2 s, Super inaccuracy 30 | No direct effective object HardPoints reference; do not add a skirmish ship based on this unused definition |
| HP_Underworld_L3_Station_Main_Cannon | Proj_Underworld_Station_Main_Cannon | targetable/destroyable Yes, health 900; manual/turret; WAD-39/40 | Underworld_Star_Base_3/4/5 and Skirmish_Underworld_Star_Base_3/4/5 |
| HP_Underworld_Station_L4_Plasma_00 | Proj_Plasma_Space_Turret_Blast | targetable/destroyable Yes, health 450; cone 180/180, range 1900, 5 pulses, delay 0.6 s, recharge 3–4 s | Underworld_Star_Base_4/5 and Skirmish_Underworld_Star_Base_4/5 |
| HP_Underworld_Station_L5_Plasma_01 | Proj_Plasma_Space_Turret_Blast | targetable/destroyable Yes, health 500; cone 180/180, range 1900, 5 pulses, delay 0.6 s, recharge 3–4 s | Underworld_Star_Base_5 and Skirmish_Underworld_Star_Base_5 |

No other direct ship definition references SPECIAL. Variants can inherit the parent list.
Skirmish use is confirmed by the `Skirmish_*` station types and the ship roster; galactic station
counterparts are listed to avoid treating the same hardpoint as a different firing mechanism.
Classification alone does not implement Kedalbe shield-leech activation, station turret aiming
or the player command cooldown. None of those changes is authorized by this docs-only walk.

## Existing behaviour notes: comparison

| Existing contract | Comparison to these rules |
|---|---|
| WWP-67 | **Same** impact damage then sound then area interface. **Missing there**: WAD-02 zero-damage fallback, WAD-03 cancellation result, WAD-08–30 victim/falloff/routing rules. Its old "no M2 projectile" scope applies to the pinned fleet, not the expanded gated roster. |
| WWP-68–70 | **Same** hit before expiry and no second effects on a hit. **Missing there**: rocket endpoint handoff and expiry blast details WAD-04/07. |
| WWP-01–13/15–35; space-weapon-fire W-01–12 | **Same** ordinary SPECIAL fire service, range, cone and cooldown inputs. **Missing there**: SPECIAL census and no-projectile exception WAD-31/33. |
| WWP-14/18/26 | **Same** charging/charged gate and direct multiplier. **Missing there**: override precedence, shared count and fixed XML area budget WAD-34–36. |
| DG-05–12/39; WCC-22/44/46–48 | **Same** shared immediate projectile-damage interface WAD-23–25. **Missing there**: the area flag suppresses original-target aim routing, direct-route exclusion and pre-routing split WAD-20–23; these do not replace ordinary damage rules. |
| WHE-35/36 | **Same** charge/shot-state production interface. **Missing there**: WAD-36 shot consumption and no area multiplier. Charged presentation cleanup is clarified by EAD-24: delegated to the next ability service when count is zero, subject to U-06 frame scheduling. |
| WHE-29 | **Same** harmonic-bomb activation/arming/autofire interface. **Missing there**: the common area damage contract; no new activation inference. |
| WHZ-51/52/53 and hazards' Death_Explosions handoff | **Same** relationship and destruction ownership. **Missing there**: the immune-faction mode broadens area recipients, and shutdown itself does not detonate WAD-05/06/09. |
| Ability walk WAB-01–11/30–34, space-abilities AB-01–07/21 | **Same** ability availability/state and fire-rate interfaces. **Missing there**: concrete BARRAGE proxy/projectile inputs WAD-38; activation internals remain with that walk. |
| No existing rule for WAD-01/07–22/26–30/39–40 beyond those handoffs | **Missing there**; this walk records their contracts and remaining unknowns. |

## Gap list against the remake

The original walk snapshot (before the implementation above) had no blast fields in `include/eawr/units/unit_tables.hpp::Projectile`,
no blast parsing in `src/units/unit_tables_decode.cpp::load_projectile`, and no secondary-recipient
stage in `src/sim/tactical/session_step.cpp`'s projectile commit. Expired projectiles are discarded
without blast delivery. `src/sim/tactical/projectiles.cpp::step_projectile` supplies one direct
hit and travel expiry only. `src/units/unit_combat.cpp::combat_table` maps only MISSILE to homing;
ROCKET gets a straight flight. `hardpoint_types` in `unit_tables_decode.hpp` / `is_weapon` in `unit_tables_decode.cpp` and
`unit_combat.cpp`, `HardpointType` and `unit_durability.cpp::role` omit SPECIAL. The priority-name
list and UI reticle do recognize Weapon_Special; that is not executable weapon support.
`src/units/unit_abilities.cpp::ability_table` and the tactical ability-kind dispatch do not model
BARRAGE/BLAST. `src/presentation/space/projectiles.cpp` does not supply missing simulation inputs.

The rule-table verdicts exhaust all 40 rules. The following groups give implementation size and
ticket ownership; overlapping rules are deliberately listed at their data/interface seams.

| Gap | Rules | Implementation / impact | Size / tracking |
|---|---|---|---|
| G1 Blast data, triggers, spatial query and falloff | 01/03/05/08–13/15–17/29 | Carry data/source metadata; trigger once at impact/expiry/explicit explosion; query collidables with the mixed planar/bounds contract; faction immunity, source radius, tier formula, copied candidates, delay and cap. Unlocks blast semantics on all four named gated ships. | L; blast query/triggers (legacy EAWR-1069) |
| G2 Zero-direct-damage impact fallback | 02 | Substitute XML area D only when direct instance amount is zero and D/R are positive; preserve aimed routing and cancellation outputs for the following blast. Prevents zero-damage blast-only payloads losing their primary contact. | S; impact fallback bug (legacy EAWR-1070) |
| G3 Per-recipient and hardpoint delivery | 14/18–23/30 | Recipient living-collision gate; direct-object/mesh exclusion; strict spatial hardpoint selection including destroyed shares; split D*F; area flag bypasses original aim override; no hull fallback. Preserve ordinary shield/energy/armor/modifier services WAD-24/25. | M; area routing (legacy EAWR-1071) |
| G4 Diamond Boron rocket-flight boundary | 04 | The loader currently turns only MISSILE into homing and treats this ROCKET as straight. Implement the weapons-owned path/endpoint contract before a reviewed Broadside/Marauder gate edit. Record spline construction, shield/jamming response and flight-height details in a weapons follow-up; this walk settles only the endpoint blast interface. | L; rocket endpoint bug/interface (legacy EAWR-1072) |
| G5 SPECIAL classification and ordinary guns | 31–33/37 | Classify SPECIAL across table/combat/durability; use existing targeting/cooldown services; allow no-projectile SPECIAL attachments; load the two Krayt gun records and ordinary station plasma batteries. The ion gun already has a usable shared shield/energy interface. | M; SPECIAL support (legacy EAWR-1073) |
| G6 Charged BLAST and BARRAGE interfaces | 34–36/38 | Charge state, projectile override and shared shot consumption use the existing Peacebringer state evidence ticket (legacy EAWR-946); BARRAGE needs its area-target proxy and weapon override inputs. Ability owners supply activation/durations/autofire, not this walk. | M each; Peacebringer state evidence (legacy EAWR-946), BARRAGE interface (legacy EAWR-1074) |
| G7 Manual cannon and appearance delay | 37/39/40 | Reuse weapon-rule coverage (legacy EAWR-714) for the appearance delay and ordinary turret/manual-target gates; add a specific station manual cannon command/readiness integration. Weapon and player cooldowns are distinct; stations already retained as infrastructure do not prove their cannon works. | M; weapon-rule coverage (legacy EAWR-714), manual cannon (legacy EAWR-1075) |
| G8 Extended delivery inputs | 26/27 | Positive projectile delay overrides distance delay with a synchronized draw; optional random-hardpoint selector passes into the shared wrapper. Not needed by the four ships' ordinary payloads. Settle delayed-state retention before claiming full positive-delay fidelity. | M; combat tag coverage (legacy EAWR-650) |
| G9 Detonation effects and audio | 07/28 | One impact or lifetime effect/sound per detonation; no duplicate per-victim impact particle/flinch; area damage remains independent of effect creation. | S–M; presentation tag coverage (legacy EAWR-653), fire-sound timing interface (legacy EAWR-801) |

The first five implementation priorities are G1, G3, G5, G4 and G6. G2 and G8 matter particularly
for effect-only/destruction payloads; G7 additionally prevents Underworld station infrastructure
from silently acquiring an incorrectly automatic superweapon.

Implementation must use immutable copied phase inputs, partitioned recipient preparation and
ordered commits, with deterministic work counters and 1/2/4/8-worker equivalence. Preserve visible
ordering where shield/hardpoint state changes are order-sensitive; any stable replacement for
the original tree order requires a documented project decision and U-02 evidence. A raw all-units
scan per flying projectile is not equivalent to this detonation-only spatial query.
For one detonation, recipient work follows collected candidates plus their inspected hardpoints,
not all objects times all flight frames. Each queried player's candidate copy is made before
its deliveries; the victim cap cannot retroactively limit that already completed query/copy.

## XML and constants ledger

The direct blast reader consumes all six `Projectile_Blast_Area_*` fields listed in WAD-01,
`Max_Secs_For_AE_Delayed_Damage`, recipient `Collidable_By_Projectile_Living`, hardpoint
`Is_Destroyable` / `Collision_Mesh`, and current positions/bounds. Its delivery wrapper consumes
`Projectile_Damage_Delay_Secs` and `Projectile_Damages_Random_Hard_Points`. Impact fallback consumes
`Projectile_Damage`; WWP owns the instance override. No dedicated GameConstants scalar sets R,
D, dropoff, tiers or maximum victims. FPS comes from the frame synchronizer; the numerical defaults,
query ceiling, Z expansion, 1-unit delay shortcut and 0.25 random-delay fraction are code values.

SPECIAL reads `Type`, `Fire_Projectile_Type`, `Blast_Ability_Fire_Projectile_Type`, `Damage_Type`,
`Projectile_Damage`, `Projectile_Appearance_Delay_Frames` and the WWP-01–35 hardpoint tags: `Health`,
`Is_Destroyable`, `Is_Targetable`, `Requires_Manual_Target_Assignment`,
`Allow_Opportunity_Fire_When_Idle`, `Allow_Opportunity_Fire_When_Targeting`,
`Fire_Category_Restrictions`, `Fire_Min_Range_Distance`, `Fire_Range_Distance`, `Fire_Bone_A/B`,
`Randomize_Between_Fire_Bones`, `Fire_Cone_Width/Height`, `Fire_Inaccuracy_Distance`, `Is_Turret`,
`Fire_Min_Recharge_Seconds`, `Fire_Max_Recharge_Seconds`, `Fire_Pulse_Count`, `Fire_Pulse_Delay_Seconds`.
Other mode/disable gates retain their WWP owner. Turret fields are `Turret_Rest_Angle`, `Turret_Rotation_Offset`,
`Turret_Rotate_Speed`, `Turret_Rotate_Extent_Degrees`, `Turret_Elevate_Extent_Degrees`, `Turret_Bone_Name`
and `Barrel_Bone_Name`; manual command readiness reads `Manual_Hardpoint_Firing_Cooldown_Secs`.
Target position, soft radius, travel and flight tags remain the [weapons ledger](weapons.md#xml-tags-this-subsystem-reads).

Ability inputs are `Unit_Abilities_Data/Unit_Ability/Type`, `Projectile_Types_Override`,
`Mod_Multiplier`, `Targeting_Fire_Inaccuracy_Fixed_Radius_Override`, `Target_Position_Z_Offset`,
`Expiration_Seconds`, `Recharge_Seconds`, `Spawned_Object_Type`, and `Abilities/Blast_Ability/`
`Charge_Up_Seconds`, `Damage_Multiplier`, `Charging_Effect`, `Charged_Effect`, `Activation_Style`.
These remain ability-owned; tag presence is not verified availability or AI autofire.
The BARRAGE `Area_Effect_Decal_Distance` 320 is an ability targeting/presentation input,
not the blast radius (200); the detonation service does not read it.

Shared damage reads `Projectile_Does_Shield_Damage`, `Projectile_Does_Energy_Damage`,
`Projectile_Does_Hitpoint_Damage` and the WCC damage ledger. Impact presentation reads
`Projectile_Object_Detonation_Particle`, `Projectile_Object_Armor_Reduced_Detonation_Particle`,
`Projectile_Absorbed_By_Shields_Particle`, recipient `Damage_Hit_Particles` / `Shield_Hit_Particles`,
`Particle_Attach_To_Collision`, `Induced_Flinch_Intensity`, `Projectile_Lifetime_Detonation_Particle`,
`Projectile_SFXEvent_Detonate`, `Projectile_SFXEvent_Detonate_Reduced_By_Armor`, `Fire_SFXEvent`, and
charged `SFXEvent_Activate`. These are shared direct/lifetime presentation inputs; WAD-28 skips
the direct-impact portion for secondary deliveries.

Detonation dispatch additionally hands off `Projectile_Stun_On_Detonation`,
`Projectile_Distract_On_Detonation`, `Projectile_Instant_Heal_On_Detonation`,
`Projectile_Convert_Enemy_On_Detonation`, `Projectile_Cause_Invulnerability_On_Detonation`,
`Projectile_Weaken_Enemy_On_Detonation`, `Projectile_Ion_Stun_On_Detonation` /
`Projectile_Ion_Stun_Radius`, and `Causes_Infection` to their existing owning services.
Combat-modifier detonation state also hands off to its owner; no `Projectile_Modifier` XML tag
was found, so the internal state getter does not establish a public tag of that name.
Do not unify their radius/category filters with blast damage. Ion stun on a direct hit remains
the ion walk interface; the expiry dispatcher does not automatically reproduce that direct-hit
zero-radius ion recipient. Ground contact particle/surface FX tags belong to land collision.

`Allows_Special_Weapon_Use` and `GameConstants/Energy_Beam_*` are **not** read by the projectile
blast service or ordinary SPECIAL shot attempt. They belong to other ability/beam interfaces;
the SPECIAL enum alone does not opt into them. `Death_Explosions` is a destruction handoff, not
an alternative spelling of `Projectile_Blast_Area_Damage`.

### Registry coverage

The relevant [tag registry](../../tag-coverage/statuses.json) rows are listed here without
introducing new application statuses: the blast rows below reflect the implemented
service; other historical rows retain their separate owning work.

| Tag / classes | Status / current tracking | Rule |
|---|---|---|
| `Abilities/Blast_Ability/@Name` / UniqueUnit | todo; tag coverage (legacy EAWR-760) | WAD-31–40 |
| `Abilities/Blast_Ability/Activation_Style` / UniqueUnit | todo; tag coverage (legacy EAWR-760) | WAD-31–40 |
| `Abilities/Blast_Ability/Charge_Up_Seconds` / UniqueUnit | todo; tag coverage (legacy EAWR-760) | WAD-31–40 |
| `Abilities/Blast_Ability/Charged_Effect` / UniqueUnit | todo; tag coverage (legacy EAWR-760) | WAD-31–40 |
| `Abilities/Blast_Ability/Charging_Effect` / UniqueUnit | todo; tag coverage (legacy EAWR-760) | WAD-31–40 |
| `Abilities/Blast_Ability/Damage_Multiplier` / UniqueUnit | todo; tag coverage (legacy EAWR-760) | WAD-31–40 |
| `Abilities/Blast_Ability/SFXEvent_Activate` / UniqueUnit | todo; tag coverage (legacy EAWR-760) | WAD-31–40 |
| `Blast_Ability_Fire_Projectile_Type` / HardPoint | todo; tag coverage (legacy EAWR-650) | WAD-31–40 |
| `Collidable_By_Projectile_Living` / GenericHeroUnit, HeroUnit, Marker, MiscObject, Mobile_Defense_Unit, Projectile, SpaceProp, SpaceStructure, SpaceUnit, StarBase, TransportUnit, UniqueUnit | todo; tag coverage (legacy EAWR-649) | WAD-01–30 |
| `Is_Turret` / HardPoint | todo; tag coverage (legacy EAWR-650) | WAD-31–40 |
| `Manual_Hardpoint_Firing_Cooldown_Secs` / HardPoint | todo; tag coverage (legacy EAWR-650) | WAD-31–40 |
| `Max_Secs_For_AE_Delayed_Damage` / Projectile | todo; tag coverage (legacy EAWR-650) | WAD-01–30 |
| `Projectile_Appearance_Delay_Frames` / HardPoint, HeroUnit, UniqueUnit | todo; tag coverage (legacy EAWR-650) | WAD-31–40 |
| `Projectile_Blast_Area_Damage` / Projectile | applied; `src/sim/tactical/blast.cpp` | WAD-01–30 |
| `Projectile_Blast_Area_Dropoff` / Projectile | applied; `src/sim/tactical/blast.cpp` | WAD-01–30 |
| `Projectile_Blast_Area_Dropoff_Tiers` / Projectile | applied; `src/sim/tactical/blast.cpp` | WAD-01–30 |
| `Projectile_Blast_Area_Immune_Faction` / Projectile | applied; `src/sim/tactical/blast.cpp` | WAD-01–30 |
| `Projectile_Blast_Area_Range` / Projectile | applied; `src/sim/tactical/blast.cpp` | WAD-01–30 |
| `Projectile_Damage_Delay_Secs` / Projectile | todo; tag coverage (legacy EAWR-650) | WAD-01–30 |
| `Projectile_Damages_Random_Hard_Points` / Projectile | todo; tag coverage (legacy EAWR-650) | WAD-01–30 |
| `Projectile_Lifetime_Detonation_Particle` / Projectile | todo; tag coverage (legacy EAWR-653) | WAD-31–40 |
| `Requires_Manual_Target_Assignment` / HardPoint | todo; tag coverage (legacy EAWR-650) | WAD-31–40 |
| `Unit_Abilities_Data/Unit_Ability/Target_Position_Z_Offset` / SpaceUnit | todo; tag coverage (legacy EAWR-760) | WAD-31–40 |
| `Unit_Abilities_Data/Unit_Ability/Targeting_Fire_Inaccuracy_Fixed_Radius_Override` / SpaceUnit | todo; tag coverage (legacy EAWR-760) | WAD-31–40 |

`Projectile_Blast_Area_Max_Victims` has no active stock XML occurrence and therefore no registry
row in this snapshot; its commented occurrences are not application evidence. Add coverage if
the loader supports it for modded data. No selected row is deferred. Already applied/partial
shared weapon/damage tags retain their existing owner rule IDs.

## Settled questions from the unverified sweep

Question IDs are retained; these boundaries no longer require a new source read. Opaque evidence IDs identify ignored research receipts. Runtime acceptance and explicitly remaining clauses stay below.

| ID | Sourced disposition | Evidence |
|---|---|---|
| U-04 | Positive delayed damage stores amount, damage kind, contacted mesh name, owner attribution and projectile damage-type identifier. Delay is trunc(seconds × logical FPS), clamped to at least one; each object service decrements it and delivers at zero before behaviours. The source object and area-routing flag are not retained: delivery supplies no source object and ordinary routing flags. Implemented by source-less delayed delivery; exact original per-object same-frame ordering remains unverified. | EUS-15 |

## Unverified and precise capture requests

Debug-build branches above are verified; the following questions remain separately scoped.
Use the original-game Lua debugger harness with logical-frame health/shield/hardpoint snapshots,
fixed initial positions, fixed environment and fog state (fog off except U-07). Captures are
coordinator-scheduled and do not block publishing this docs-only contract.

| ID | What is unverified | Capture or targeted read that settles it |
|---|---|---|
| U-01 | Retail boundary fidelity for mixed planar object admission, 3D hardpoint radius and collision-refined falloff | One Diamond Boron blast against isolated craft at distances 39.99/40/40.01 and 199.99/200/200.01; repeat with a large ship whose centre is outside R but hull crosses R, and with 0/200/201 height separation. Log every recipient/shield/hardpoint delta and actual detonation contact. Use single shots and disable repair/other fire. |
| U-02 | Exact retail tree/player enumeration and reproducible cap ordering; replacement order decision | A private test payload with max victims 1 and 2, enemies under two owners, deliberately out-of-distance order; repeat identical spawn order, reversed spawn order and different positions. Include a hardpoint ship with no hardpoints within R before a craft to prove victim-slot consumption; record affected IDs. This custom payload is a probe, not stock behaviour data. |
| U-03 | Retail direct-route exclusion and destroyed-hardpoint dilution, especially shield-generator death during a split | Krayt damage shot aimed at one live station hardpoint; place two other hardpoints inside R and one exactly on R. Repeat with an inside hardpoint already destroyed, then a shielded Diamond Boron hit. Record direct route, per-hardpoint HP, shield delta and same-frame death ordering. |
| U-04 | Storage, truncation and source-less delivery settled by the targeted debug-build read and EFO-04; exact per-object same-frame runtime ordering remains unverified | Private payload with distance-delay maximum 0.8, then explicit delay 0.5; compare immediate/queued damage with shields up, delete shooter between launch and detonation and between queue/due frame. Log due frame, source attribution and selected hardpoint. Stock container deaths provide a positive explicit-delay consumer. |
| U-05 | Chooser algorithm settled by WAD-R05; custom-payload per-share runtime witness remains unverified | [Targeted debug-build read](../retail-blast-evidence.md): random start across all hardpoints, then forward wrapping scan for living/destroyable; targetability is not checked. Each application can redirect its original share to a new collision mesh, including the direct-excluded mesh. Still capture a private positive-area payload with `Projectile_Damages_Random_Hard_Points` Yes and mixed station hardpoint states. |
| U-06 | Cross-service timing of charged presentation cleanup and shots when a gun is destroyed after charging **Sweep:** Still unverified: Per-object behavior/hardpoint order is sourced, but charged presentation cleanup and a gun destroyed between charge and fire have independent callbacks. No specific pending-charge cleanup path was resolved. | Peacebringer BLAST against a fixed capital: log activation, charge counter/meshes, both projectile creations, first appearance and hit frames, then destroy one SPECIAL during charge and again after charge completion. Repeat with unequal gun readiness. Verify normal 400+300-area shot versus charged 3000+400-area shot; no inferred per-gun shot latch. Retained sweep boundary: EUS-04. |
| U-07 | BARRAGE retail proxy/flight-height and expiry interaction | Broadside and Marauder, normal then BARRAGE at a fixed point; fog on for proxy targeting gate, fixed XY/Z recipients, trace fire-at points, proxy owner/position, rocket terminal frames and area recipients. Confirm speed/flight changes with identical inherited D/R/falloff. Full spline and jamming rules belong to a weapons follow-up. |
| U-08 | Station manual-command rejection/readiness across multiple retained stations and actual turret alignment | Two Underworld level-3+ stations owned by one player; request the same manual cannon target, then another station before/at/after the 120 s readiness boundary. Log requesting player, per-gun recharge, acceptance/rejection, turret yaw/pitch, 15-frame appearance delay and damage; retain a failing target beyond 300 frames. Command-event ownership gates remain with sensors/UI and player setup. |

RG-02 now enables Broadside and Marauder after review of ordinary Diamond Boron
blast damage and rocket termination. Krayt and Peacebringer retain their separate
SPECIAL weapon gate. Area
damage alone does not authorize BARRAGE/BLAST/autofire availability, a new player input, rocket
flight approximation, or a SPECIAL station weapon. Re-enable through a separate reviewed data
edit after its prerequisites pass.
