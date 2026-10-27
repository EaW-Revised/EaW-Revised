# Walk 10: reinforcements, hyperspace arrival and retreat

## Scope and evidence

This walk covers mid-battle space reinforcement admission, population, the local player's
placement flow, and the arriving ship or squadron craft's logical-frame service. It checks
retreat eligibility in skirmish and records the campaign retreat contract as an interface.
**FoC forbids retreat in single-player skirmish, multiplayer skirmish and multiplayer tactical
battles** (WR-42); M2's disabled retreat is faithful and needs no escape implementation.

Sources are **debug build**, **XML data**, existing sourced behaviour notes, and explicitly
**unverified** questions. The debug build was inspected read-only; opaque evidence IDs
WR-E01 to WR-E47 have their private map and raw material in ignored `out/research/`.
The original reinforcement pass made no retail capture; WR-56 adds a bounded
campaign observation. XML values below are the effective FoC data. The later
[campaign retreat frame schedule](../gc-retreat-frame-schedule.md) qualifies
native scheduling with new read-only debug-build evidence EFS-R01..11; its
static cases do not turn video timestamps into native frames.

The code comparison has two baselines. The integration checkout at `5ac16e9a` has no economy
or arrival system; its script `Reinforce` binding is a stub. The useful implementation
comparison is pending simulation economy, build queue and arrivals (legacy EAWR-556),
head `62ab82a3`, and UI purchase UI and reinforcement placement (legacy EAWR-574),
head `d462cc23`. **Same** means those pending changes implement the verified rule for M2,
not that they have merged. Every rule below has a comparison in the final table.

Boundaries: [production](production.md) owns buying, building, pool admission and income;
[movement](movement.md) and [space movement](../space-movement.md) own free-space searches and
tracking-layer collision internals; [sensors/UI](sensors-ui.md) owns fog sampling and picking;
[abilities](abilities.md) owns battlefield modifiers and interdiction; the hazards walk owns
which hazards register on tracking layers. The [victory](../space-victory.md) and
[battle-end](../battle-end.md) notes own victory and the end dialog. GC persistence, fleet
destinations, attrition, autoresolve and galaxy travel are interfaces, not walked here.

## Entry points and order

The render frame rebuilds the open reinforcement pane and updates the placement preview.
Mouse events select a pooled type and enqueue an arrival at a point on the Z=0 plane.
At command execution, the game first rejects a pending victory, then checks population and
placement again, then consumes a pool entry and creates the arriving objects. Placement
checks fog, prevention circles, playable bounds, the moving layer, then the static layer.
Each arriving object's logical-frame movement service advances its arrival counter and
applies the timing hooks below. Population is a query over registered objects, not a
periodic cap enforcement that destroys excess units.

Campaign retreat has a battle-wide service: initial lockout, escape countdown, escape,
optional station destruction, and completion. Its internals are outside M2.
[WFO-02/08/12/17/18/26/31](frame-order.md) now establishes command, grid-decay, object and
queue placement. Incoming equal-player ties and exact population-unregistration callbacks
remain frame-order UFO-01/05; U-4 below distinguishes that residual race from the settled schedule.

## Rule list

### Population and offered units

| ID | Rule | Source |
|---|---|---|
| WR-01 | Space cap is the player's faction's `Space_Tactical_Unit_Cap`: Rebel 25, Empire 20, Underworld 25. Invalid player yields 0. The same faction lookup serves campaign space. Captured reinforcement points and station `Additional_Population_Capacity` do not enter the space cap query. | debug build WR-E32; XML data; WPR-04 |
| WR-02 | Population includes only registered tactical population objects, using their stored share. A positive share is registered with its owner when the unit is instantiated; the squadron container is not another whole squadron charge. Buying and waiting in the pool do not reserve population. | debug build WR-E24, WR-E31, WR-E33; WPR-32, WPR-41 |
| WR-03 | A created object matching its source type or original type gets the full source population value. In space, a nonnegative `Space_Override_Population_Value` replaces `Population_Value`, including an explicit zero; an absent or negative override retains the base value. The same space getter supplies initial fleet admission and ordinary reinforcement admission/accounting; the galactic usage query retains its base value (WGEP-49/69). A type matching `Ignore_For_Reoptimization` gets zero. Ordinary homogeneous squadrons give each craft the source squadron value divided by its authored `Squadron_Units` count. The general member branch also divides by the non-ignored entries visited up to the matching member; mixed-type and ignored-member data therefore need the additional verification U-6. Positive shares alone enter the registry. | debug build WR-E33 and space-mode getter/initial-selector qualification; XML data; WPR-41 |
| WR-04 | Sum shares, take the floor, and round up only when the fractional part is strictly greater than `Allow_Reinforcement_Percentage_Normalized` (0). A partly surviving registered squadron still consumes its remaining fraction, rounded by this rule. Removal unregisters its share (WPR-41). | debug build WR-E31; XML data; WPR-41 |
| WR-05 | Admission requires `int(Population_Value + 0.5) <= cap - current count`; a type-less room query uses 1. The XML population lookup here is integer-valued. The pane tests room, the drop tests it again, and the authoritative command tests it again. A failed command leaves the pool unchanged. | debug build WR-E04, WR-E16, WR-E22 |
| WR-06 | Skirmish starting companies and station-launched craft do not enter this arrival registry (WPR-03, PU-22). Completed normal units join their owner's pool; upgrades use production's separate path. Space units have no automatic deployment merely because their build completes. | WPR-03, WPR-22, WPR-32; PU-22, PU-30; debug build WR-E24 |

### Reinforcement UI: render updates and input

| ID | Rule | Source |
|---|---|---|
| WR-07 | Opening the pane requires tactical mode, permission to reinforce, a visible tactical command bar, no pending victory and no retreat underway. Opening plays `Reinforcements_Selection_SFXEvent`, updates its contents and clears the reinforcement-button flash; pressing again closes it. An already open pane is hidden once victory becomes pending. | debug build WR-E08, WR-E23 |
| WR-08 | Skirmish offers the local player's completed pool, counting duplicates by type-definition identity before traversing the group keys into slots. Within one loaded definition set, completion order does not reorder types or choose which groups fit the slot limit. The remake traverses stable `TypeId` keys; exact stock order across loads remains unverified (U-2). Campaign obtains persistent reserve objects instead (WR-48). | debug build WR-E08, EUS-14; PU-66 |
| WR-09 | Each group shows `Icon_Name`, falling back to `i_button_temporary.tga`, and `xN` for multiple copies; one copy has no count label. A type with positive population exceeding remaining room is disabled. Campaign-restricted groups are also disabled and grey (128,128,128,255); enabled groups are white (255,255,255,255). Unused slots are cleared. | debug build WR-E08; PU-66 |
| WR-10 | Four slots per row, 20 slots in the authored shell. Rows past the first are clamped to 0..4 from `(filled - 1) / 4`; pane anchor is half camera width left and half camera height up minus 100 shell units. The close control's translation changes by 46 shell units per omitted row. Art/font interpretation remains the UI subsystem's interface. | debug build WR-E08; UI data; PU-67 |
| WR-11 | Starting a drag requires no active drag, a selected type and a matching entry still in the skirmish pool. It cancels previous placement, optionally plays `Reinforcements_Pick_Landing_Zone_SFXEvent`, and begins placement for that type. | debug build WR-E09 |
| WR-12 | Placement makes one visual clone per squadron craft, or one clone for a ship. Clones use the type's space model, `Scale_Factor`, `Idle_Anim_00_Rate_Mod` and `Loop_Idle_Anim_00`, with a random idle frame if one exists. Clone particle emitters are hidden. These are visual previews, not simulated units. | debug build WR-E38 |
| WR-13 | Each preview update tests the selected point through the space placement predicate. Ship clones sit at the cursor's XY and their `Layer_Z_Adjust`; craft previews add their `Squadron_Offsets` rotated by the player's reinforcement facing. Preview formation offsets do not describe actual craft creation positions (WR-31). | debug build WR-E41 |
| WR-14 | All space preview models are tinted with the predicate's good/bad `ReinforcementOverlayGoodColor` / `ReinforcementOverlayBadColor`, their RGB multiplied by 1.5 and light-scale alpha set to 1. Data colours are green/red at half intensity and authored alpha 128; this space model path does not use that alpha as translucency. | debug build WR-E41; XML data |
| WR-15 | On release, the pointer ray is intersected with Z=0, placement is checked first and room second; success invokes the arrival request callback. The placement-cancel callback runs even after an invalid drop. The authoritative command rechecks state, so a green preview is not a reservation. | debug build WR-E04, WR-E10, WR-E16 |
| WR-16 | A submitted arrival plays the type's `SFXEvent_Fleet_Move`, else the faction's `Reinforcements_Enroute_SFXEvent`, then ends placement without cancellation. A failed ordinary space drop ends placement with cancellation set and a selected type, requesting faction `Reinforcements_Cancelled_SFXEvent`; explicit cancellation does the same. A local authoritative cap failure plays `SFXEvent_Tactical_Unit_Cap_Reached`. Opening/selection/drop sounds are distinct from the frame-35 arrival sound. | debug build WR-E10, WR-E16; reviewed space drop, callback registration and placement-end cross-check; debug-build fleet-move tag-table lookup; XML data |
| WR-17 | The reinforcement overlay is the fog plane's red mode, including fogged space and the unplayable ring, with prevention circles supplied to its blocked-point query. The live viewer follows pane/drag state according to FW-22/23 and reads `SpaceReinforceFeedbackOnlyWhileDragging`; its provider shares the authoritative fog-point, prevention and bounds predicates over immutable published state. It is not the complete ship-footprint or arrival-lane verdict. | FW-22, FW-23; debug build WR-E01, WR-E02 |
| WR-18 | No 90-frame cooldown is checked in the traced space UI, command or instantiation path. `Skirmish_Reinforcement_Delay_Frames` (90) seeds the battlefield delay modifier; the progress icon compares transport elapsed frames with that modified delay. `Reinforcement_Time_Multiplier` changes that delay by rounded base-times-multiplier minus base. This transport contract is an interface, not a space purchase cooldown. | debug build WR-E03, WR-E09, WR-E16, WR-E24, WR-E39, WR-E40; XML data |

### Command execution and allowed arrival point

| ID | Rule | Source |
|---|---|---|
| WR-19 | A pending victory drops an arrival command before any creation. It also prevents offering a new placement through WR-07. | debug build WR-E16 |
| WR-20 | In space, resolve a skirmish command to a unit type, check room, check placement, then instantiate; instantiation checks the player's pool still contains that type. Any failure produces no arrival and removes no reserve. Campaign resolves an object identity instead (WR-48). | debug build WR-E16, WR-E24 |
| WR-21 | Reject an invalid player. For an ordinary request, first reject a point fogged for that player. This uses the fog subsystem's point query, not merely whether any friendly ship lies within a hand-coded distance. | debug build WR-E01; V-11 to V-19 interface |
| WR-22 | Next reject a point whose planar squared distance is strictly less than a positive `Reinforcement_Prevention_Radius` squared for any non-allied registered modifying object. Allied objects never prohibit it; equality at the radius is allowed. There is no neutral-owner bypass or visible-enemy test. A neutral gravity station therefore still blocks with its authored 2200 radius, although ordinary combat cannot destroy it; M2 skirmish stations author 2000. | debug build WR-E02 (relationship recheck); XML data |
| WR-23 | Next require the point inside the space mode's playable XY bounds. This is not a fog-outline or camera clamp test. Equality semantics and the construction of those bounds remain U-3. | debug build WR-E01 |
| WR-24 | A type-less query passes after the preceding tests. A non-squadron type with no `Space_Layer`, a missing tracking system, or a missing chosen tracking layer also passes without a collision test. A squadron with no layer uses the corvette layer. | debug build WR-E01 |
| WR-25 | Construct a linear collision query from the point 200 units back along reinforcement facing to the requested point (`Space_Reinforcement_Collision_Check_Distance` = 200). Use the current logical frame through current frame + 115; 115 is a code constant for the visible arrival, not an XML time. Query the selected tracking layer using the type's hard XY extents. | debug build WR-E01, WR-E36; XML data |
| WR-26 | A layer-less squadron's extents are the maximum over craft of absolute formation-offset X/Y plus that craft's hard X/Y extent, independently on each axis. This is a rectangular formation footprint, not a enclosing circle or the actual free-space search result. Reads `Squadron_Units` and `Squadron_Offsets`. | debug build WR-E01 |
| WR-27 | Any collision in the chosen layer rejects a type with an authored layer. For a layer-less squadron, inspect collision IDs: a resolved object outside the corvette layer rejects; corvette-layer objects and unresolved IDs do not. The collision query's prediction and object registration belong to movement, not this walk. | debug build WR-E01 |
| WR-28 | After the chosen-layer test, query the static-object layer with the same sweep. No static layer means allowed; otherwise any collision rejects. Hazards prohibit arrival when their registered footprint participates in that query or they author a prevention radius. No separate asteroid-damage, nebula, ion-storm or mine flag is read by this placement function. Which hazards register is the hazards walk's interface (U-7). | debug build WR-E01 |
| WR-29 | A privileged placement flag skips fog, prevention circles and playable bounds, but still runs type/layer/collision checks. Ordinary UI and reinforcement commands pass the flag false. Its scripted callers and validation are outside this skirmish walk. | debug build WR-E01, WR-E04, WR-E16 |

Space AI placement uses the same ordinary verdict. Its verified search geometry starts opposite
arrival facing, samples ten angles 36 degrees apart, expands the radius by 500 after a failed
ring and clamps candidate XY to playable bounds. A point inside a prevention circle starts
at radius 1000; otherwise it starts at zero. The remake's bounded, distributed search and its
remaining cadence differences are recorded as SAE-10 in [skirmish AI economy](../skirmish-ai-economy.md).

### Creation and per-object arrival service

| ID | Rule | Source |
|---|---|---|
| WR-30 | A single ship is created directly at the requested XY, with input Z reset to 0, facing the player's reinforcement direction; there is no free-space search for it. The skirmish direction comes from the first starting spawn marker (WPR-03). Creation's height raise is the existing LZ-01 interface; U-5 retains the unresolved creation-flag interpretation. | debug build WR-E24; WPR-03; PL-08, LZ-01 |
| WR-31 | Create squadron craft in `Squadron_Units` order. Search near the requested point within 2500 units, start angle 0, through the shared free-space finder; its zero-vector failure falls back to the requested point. Set input height 0, create and register each craft, then build the squadron container and start arrival for the team. Actual positions need not equal preview offsets. | debug build WR-E24; PL-02 to PL-08 |
| WR-32 | On successful creation, remove one matching type from the pool and set elevated vulnerability on the ship or each squadron craft. Population is charged during creation, including the hidden part of arrival; an arriving unit is not free population until it lands. | debug build WR-E24 |
| WR-33 | Arrival initialization leaves ordinary movement coordination, saves the exit position, computes the starting point and facing velocity, sets counter 0, marks reinforcement, adds invulnerability, hides the model and attached sound outside playback, suspends tracking and locks movement. Ordinary damage respects invulnerability; Lua `Take_Damage` uses the privileged cheat category and bypasses it, as does vehicle-thief damage. No hypothesis about firing is implied by the movement lock (U-1). | debug build WR-E12, WR-E46, WR-E47; PU-35 to PU-39 |
| WR-34 | If the cinematic controller delays hyperspace, hide the model and silence attached sounds outside playback and return without incrementing the counter. Reinforcement initialization sets 0, so the separate randomized battle-opening delay branch (counter -1, random 1..4 and clamp 0..24) does not run for these arrivals. | debug build WR-E12, WR-E17, WR-E18 |
| WR-35 | Otherwise increment the counter. Frames 1..24 hide/silence outside playback and disable the object's reveal service if present and not already disabled, then return. These early frames do not move along the lane. Disabling only stops later servicing: it releases nothing the service already marked (WR-49). | debug build WR-E17, WR-E18; behaviour-kind lookup in private evidence map |
| WR-36 | Reinforcement frames 25..34 stay hidden and silent outside playback but move along the hyperspace table. Invulnerability remains until frame 35. The normal battle-opening arrival has separate visibility handling; do not apply its 7000-unit visibility fallback to reinforcements. | debug build WR-E17, WR-E18 |
| WR-37 | At frame 35 play the owner's faction `SFXEvent_Arrive_From_Hyperspace` on the object when authored. Show/unsilence allied objects, objects without hide-when-fogged, and visible enemy objects; a fogged enemy remains hidden while fog is enabled. For reinforcements, start an object light fade from black lasting `115 / (logical FPS * 4)` seconds (115/120 at 30 FPS), then remove invulnerability. Fog opacity is a separate presentation factor. | debug build WR-E17, WR-E18; XML data; PU-36, PU-37 |
| WR-38 | Frames 25..149 advance along the initial facing velocity using the code-generated distance table below, identically for ships and craft. At 150, end rather than taking another table step. There is no ship-speed or `Hyperspace_Speed` multiplier in this arrival table. | debug build WR-E17, WR-E18, WR-E25, WR-E35, WR-E43 |
| WR-39 | At frame 120 enable the object's reveal service if present, before that frame's movement step. The ordinary fog-grid cadence still controls when the newly enabled sensor appears in the grid. This is separate from model visibility at 35 and movement release at 150. | debug build WR-E17, WR-E18; V-15 interface |
| WR-40 | At frame 150 end arrival: restore the saved exit position and facing, clear the reinforcement flag/state and movement lock, resume ordinary locomotion, emit the reinforcement-unloaded notification, remove old coordination and start a move to the object's current position. Force a fog reevaluation when it has hide-when-fogged. The ship path requires suspended state to finish; the craft path finishes directly. | debug build WR-E17, WR-E18, WR-E26; PU-39 |
| WR-41 | Elevated vulnerability is an independently timed defense modifier, not an arrival flag: `Space_Elevated_Vulnerability_Factor` -3 for `Space_Elevated_Vulnerability_Duration` 5 s, set at creation. For default data it expires around landing; a longer authored duration must survive landing. Damage routing, other defense terms and battlefield suppression belong to combat/abilities; PU-38 describes the default fourfold damage term. | debug build WR-E24, WR-E37; XML data; PU-38; DG-26 |
| WR-49 | A unit with a reveal service marks its circle in the fog grids of its owner's team when it is created, at the drop point: the reveal behaviour services itself as it is initialised, before arrival initialisation moves the object to its hyperspace start and before frame 1 disables the service. The disable is a counter and never releases the circle, so the grid holds those cells (their value is set at once, and the cell ramp lowers only unheld cells) through frames 1..119. At frame 120 the service is enabled again and follows the V-12 movement rule from the held point, so the circle moves to the flying object once it has left that cell. The reinforcement point query (WR-21) reads the owner's grid value at the point (zero is fogged; a lingering cell is not), so a drop widens the valid area at once, while the ship is still in hyperspace, and the next drop can follow from the new edge. The arriving unit stays hidden from the enemy (WR-37, PU-37). Units without a reveal service (fighters) add nothing. | debug build WR-E17 plus the reveal behaviour's initialisation, service and the grid's reveal and ramp; V-11, V-12, V-15; owner playtest |

### Arrival distance table (WR-38)

Let k be the incremented logical arrival counter and E(t) be the code's easing function
with no ease-in and an ease-out fraction of 1/2:
E(t) = 4t/3 for t <= 1/2; otherwise
E(t) = 2/3 + (8/3)(t - t²/2 - 3/8).
The start is exit minus facing times the sum of these distances; k=150 snaps to exit.
All values here are **code constants**, verified by WR-E35 and WR-E43.

| k | Distance per logical frame |
|---|---|
| 0..24 | 0 |
| 25..34 | 750 |
| 35..44 | 750 - 741.875 * E((k - 34) / 10) |
| 45..119 | 8.125 |
| 120..149 | 8.125 - 8.125 * E((k - 119) / 30) |

### Retreat and campaign interfaces

| ID | Interface rule | Source |
|---|---|---|
| WR-42 | Reject space retreat for an invalid player, multiplayer tactical battle, single-player skirmish or multiplayer skirmish. The space retreat service does not advance the campaign retreat flow in multiplayer tactical mode. M2's `CANRETREAT=0` is faithful, independent of its AI buying-context choice. Skirmish therefore has no successful retreat countdown, retreat losses or escaped-fleet result. | debug build WR-E05, WR-E06, WR-E13; SK-43 |
| WR-43 | Campaign eligibility passes conflict-participant identity, initial lockout, exclusive retreating-player ownership, playable faction and tutorial permission, then player-level prevention. Enabled prevention modifiers return their authored failure reason; an owned tactical superweapon also forbids retreat. Initial lockout reads `Space_Retreat_Allowed_Countdown_Seconds` (30 s). Modifier suppliers and galaxy conflict construction remain interfaces. | debug build WR-E06, WR-E13, WR-E27; XML data |
| WR-44 | An allowed campaign request enqueues a retreat event; execution rechecks eligibility and idle state, builds eligible units and starts `int(Space_Retreat_Countdown_Seconds * logical FPS)` (Rebel/Empire 10.99 s, Underworld 4.99 s: 329/149 frames at 30 FPS). It stops/faces and deselects eligible units and disables their targeting/weapons during countdown. Faction `Space_Retreat_Off_Map_Dest_Pos` is (12000,12000,0). Local type `SFXEvent_Retreat_Start` takes precedence over faction begin sound; enemy begin sound is separate. Countdown UI is an interface. | debug build WR-E14, WR-E28; XML data |
| WR-45 | The campaign roster requires owned locomotion, hyperspace capability (the squadron source type for a team), standing engines where engine hardpoints exist, no permanent engine disable/death for a single ship, and locomotion that is not suspended (WR-54). At expiry it removes units with all engines destroyed or engines currently disabled. Eligibility is rechecked during countdown: a newly forbidden retreat cancels it. Player cancellation is allowed only for that player's countdown and never for automatic forced retreat or after escape begins. | debug build WR-E05, WR-E07, WR-E15, WR-E29; WR-54 |
| WR-46 | Countdown expiry transitions to escaping, chooses the retreat/station-destruction cinematic, positions eligible units, makes them immune to damage, disables all units' targeting/weapons and clears movement coordination. WR-50..52 qualify flight completion and the following station wait. Forced-retreat triggers remain a GC interface. | debug build WR-E05; WR-50..52; XML `Space_Station_Destruction_Forces_Retreat` False |
| WR-47 | Completed campaign retreat tests victory for the enemy conflict participant using the capable-enemies-retreated condition, then destroys remaining owned objects except roster members, fleet containers and contents of a retreating flagship. Preserved units are not ordinary destroyed-unit losses. Galaxy persistence, attrition/protection and post-battle loss statistics receive this outcome; their internal counting remains U-8. | debug build WR-E20, WR-E21 |
| WR-48 | Campaign reserves are persistent object identities, not the skirmish's newly built type entries. UI and command resolve the persistent identity; instantiation consumes the reserve identity, preserves a parent-mode identity on created units and reports reinforcement to the story system. Fleet members use the same free-space placement interface. Parent-mode creation, initial deployment, garrison reserves and post-battle survival are GC boundaries. | debug build WR-E08, WR-E10, WR-E16, WR-E24 |
| WR-50 | Escape flight completes when cinematic playback is absent or its current cinematic frame is at least **90**. It does not test each ship against an off-map destination. If playback still exists at flight completion, enter the station self-destruction wait; otherwise mark escaped, report enemies-retreated victory and destroy the unescaped remainder in that order (WR-47). | debug build retreat qualification |
| WR-51 | While the cinematic frame is below **30**, each roster member moves **8.125** along the cinematic departure direction per flight-service call. From frame 30, a roster prefix uses faction `Space_Retreat_Flight_Move_Increment`; later members retain 8.125. Movement precedes wait reduction and at most one prefix advance; reload `Space_Retreat_Unit_Increment_Wait_Frames` without carrying overshoot. Displacement is once per service call, not scaled by elapsed frames. WR-57..59 qualify native cadence and each member's first accelerated frame. | debug build retreat qualification; EFS-R04/06; faction XML interfaces |
| WR-52 | The station wait completes when cinematic playback stops. While playback continues, do nothing before cinematic frame **181**. At or after 181, find the retreating player's stations, create each living station's authored `Retreat_Self_Destruct_Explosion` when present and apply lethal damage to the station. That service call still reports incomplete; absence of a station does not by itself finish a playing cinematic. Only after the wait completes mark escaped and perform the WR-47 victory and remainder cleanup. The retreat cinematic lasts **5 seconds** without a station and **15 seconds** with one. | debug build retreat qualification; XML `Retreat_Self_Destruct_Explosion` |
| WR-53 | Campaign space retreat reads the retreating player's enabled retreat-prevention entries and rejects a request when an entry is true, returning that source's failure reason. Countdown servicing rechecks the same eligibility before reducing the countdown; a newly forbidden retreat takes cancellation instead of progressing. This check belongs to countdown, not the escaping or station-wait branches. An entry enabled earlier in this frame's object/movement service is visible to the later retreat turn; that cancels a due countdown before escape. The ability's own admission path still determines when it writes the entry. | debug build retreat qualification; EFS-R03/04; WR-43, WR-45; FD-R01 |
| WR-54 | The additional locomotion exclusion in WR-45 is **suspension**, not a generic zero-speed or stationary-unit check. Shared ship, fighter and team locomotion read the object's locomotion suspension flag; the base locomotion interface reports false. Structures without locomotion are absent from the candidate scan. A squadron team passes the hyperspace test according to its squadron source type; individual craft's type flags do not replace that test. This settles the former virtual-predicate boundary without declaring every stationary ship or every fighter ineligible. | debug build retreat qualification: roster, locomotion interface dispatch and suspension reader |
| WR-55 | The public Lua player `Retreat()` call checks eligibility, then submits a logical request intended for a nonlocal player. That request path rejects the local player. The Lua call does not propagate the request's failure status and can return true without enqueueing retreat. A true Lua result alone therefore does not qualify a local player's countdown or escape; use the local retreat UI and observe the transition. | debug build retreat qualification; bounded debug-build observation |
| WR-56 | A bounded campaign observation with two ships and an owned station shows the local retreat confirmation followed by the visible countdown, the ship departure cinematic, the station cinematic and explosion effect, then the battle-loss result. The station model remains visible during that cinematic; visible model presence is not a living-station test. This observation qualifies that presentation sequence, not individual native departure frames or the survivor ledger. | debug-build campaign capture; WR-50..52 |
| WR-57 | Native retreat service runs once per admitted tactical mode service, after both object managers/deletion tails, timeline, collision, movement coordination and setup support, and before victory countdown/income. It reads elapsed gameplay frames since its last service. Multiplayer tactical skips progression; editor skips this call and pause refuses logical admission. No independent retreat modulo gate or catch-up movement loop exists on this ordinary path. | debug build EFS-R03/04/08; FS-R02; FC-R01 |
| WR-58 | The cinematic frame is elapsed active-mode gameplay frames since initialization, sampled in the logical space prefix before objects and retreat. It is not advanced by rendering or a separate wall-clock animation timer. At countdown expiry the prefix has already run, so initialization supplies frame 0 without a flight movement step; the next admitted prefix supplies frame 1. Active standalone playback forces nominal 30 logical FPS regardless of ordinary speed/fast forward. At 30 logical FPS, 5/15-second retreat durations are 150/450 mode frames; the station-only shot lasts 300. | debug build EFS-R01/02/04/05/08/09; FS-R01/04; FC-R02/03 |
| WR-59 | With consecutive native frames and stock `Space_Retreat_Unit_Increment_Wait_Frames` 1, final roster member n (zero-based) first accelerates at cinematic frame **30+n**, provided that frame is below 90. Its engines brighten from **20+n**, also below 90. Frame 90 completes group flight without another step regardless of any ship's position. First acceleration is not a fixed screen-disappearance, model-hide or campaign-survivor frame. A positive modded wait w gives frame 30 for member 0 and 31+(n-1)w for n>=1 because the initial wait is zero. | debug build EFS-R04/06; FC-R04/05; FSC-R02/05; WR-50 |
| WR-60 | Cinematic staging uses `ATTACKER ENTRY POSITION` for either retreating side, places modeled members near it in descending maximum-tactical-health order, and then orders departure by increasing distance to a point 10000 units along the opposite marker yaw. Equal distance retains staging order. Departure direction comes from the marker's rotated local negative X. This cinematic flight is separate from countdown's faction off-map facing point. | debug build EFS-R05/06/09; FS-R07 |
| WR-61 | The debug build constructs the ship and station camera keys directly in these paths; no retreat camera XML or ship animation supplies their native frame gates. Ship camera/target start at 0 from attacker-entry geometry. The shot cuts to the attached station at 150 with camera offset (0,-2000,2000), holds through 210, then begins an eased Z-axis orbit strictly after 210 with a 1000-gameplay-frame period and ease-in/out 0.5. The station-only path starts attached at 0, holds through 60 and rotates after 60. Attachment can use cached state after removal. Ship flight uses WR-51's direct positional increments; the station explosion uses `Retreat_Self_Destruct_Explosion`. | debug build EFS-R05/09/11; FS-R08/09; WR-52 |
| WR-62 | The consumed faction `Space_Retreat_Flight_Move_Increment` defaults to **250**; its wait defaults to **2**. Rebel/Underworld author increment 500 and wait 1. Empire authors ignored `Space_Retreat_Flight_Increment` instead, so retains increment 250 with authored wait 1. Do not alias the two spellings. | debug build EFS-R10; effective XML; existing tag-registry ignored spelling |
| WR-63 | At cinematic frame 181, a living station has already received that frame's ordinary object service before retreat creates its explosion and applies lethal state. This call remains incomplete; playback ending in a later logical prefix permits the same frame's retreat suffix to mark escaped and perform WR-47 before the following victory countdown. No second object-service pass or early completion on an absent station follows. | debug build EFS-R01/03/04/07; FS-R06; FD-R02/04; FSC-R03 |

| ID | Rule | Source |
|---|---|---|
| WR-74 | A player cancellation request queues a cancellation event only for its own countdown and when the automatic-forced flag is false. Execution rechecks player identity and that flag. Player-command execution accepts only countdown; an internal cancellation accepts any non-escaped active phase under the same identity/automatic guard. Accepted cancellation stops the roster's departure-facing operation, issues a degenerate movement request and re-enables weapon hardpoints plus available targeting and weapon behaviors for those roster objects. It clears retreating-player identity, escape countdown, accelerated prefix, jump-sound progress, inter-unit wait, engine-brightening progress and the roster, then returns idle. It does **not** reset initial lockout or last-service frame, reconstruct previous orders, or reselect the units in this inspected path. For a local player-command cancellation, only roster member 0 supplies the optional type `SFXEvent_Retreat_Cancel`; otherwise use faction `Space_Retreat_Cancel_SFXEvent`. The helper does not search later members for a type cancel sound. | debug build EVR-01; WR-45; FC-R01 |
| WR-75 | Detaching a roster object removes that reference while retreat is active. Removing the last member requests **internal** cancellation; WR-74's phase and automatic-forced guards still decide whether it succeeds. Countdown prevention also uses that internal path. Request acceptance, event execution and internal cancellation are therefore separate decisions; a cancellation input queued during countdown cannot cancel flight if execution occurs after escape has begun. Runtime exceptional deletion/restore remains U-8 and UFS-R03. | debug build EVR-01; WR-53; FD-R01/03 |
| WR-76 | After marking escaped, the completion helper sends tutorial event **`retreat_complete`**, removes all active-mode victory waits with queued evaluation disabled, then visits player slots in stored list order. It tests only the other attacker/defender participant who regards the retreating player as an enemy, using the capable-enemies-retreated condition. That predicate requires a tactical mode with a retained parent, completed retreat and a different player; it also checks that the **retreated player** regards the contender as an enemy, then copies the installed condition and asserts its delay is **0**. The helper accumulates success and does not stop its slot loop at the first success; the monitor's existing first-pending gate keeps later tests from replacing a result. Remaining-object cleanup follows this helper, even when a test does not accept a new result. | debug build EVR-04; WR-47; WGS-30/32; WGM-51 |
| WR-77 | Escaped-state cleanup traverses the retreating player's stored owned-object list from first to last, rechecking its count. Preserve objects in the active retreat roster, fleet containers, and objects whose ancestor-container chain reaches a flagship which itself is in that roster. Merely being inside another surviving carrier is not this flagship exception. Remaining objects receive direct destruction, rather than the attrition pass's miscellaneous damage request. Roster membership is tested against an active retreat and exact object reference/identity; the helper is not a separate type-based survivor list. Final galactic reconciliation remains WGM-55..60, and live loss-column qualification remains U-8. | debug build EVR-04; WR-47; WGT-36/43 |
| WR-78 | Story reward **`FORCE_RETREAT`** resolves a supplied faction and its existing player, requires an active-mode retreat service and no retreat already underway, then submits the ordinary **non-UI** start request. That request refuses the local player and rechecks ordinary eligibility, initial lockout, idle state and the automatic-forced guard before enqueueing. This reward does not set the automatic-forced flag or bypass those gates; its story diagnostic is not a success receipt for countdown or escape. Missing faction/player/service submits no request. The station-destruction option's distinct installation branch is WGT-76. | debug build EVR-05; WR-43/55; Lua/XML story interface |
| WR-85 | `Hyperspace` starts false when omitted from a type. For a nonempty `Squadron_Units` list, the post-load pass replaces the squadron's capability with each resolved craft's capability in authored order: **the last craft wins**, even over an explicit or inherited squadron `Hyperspace` value. Variant composition replacement happens before this aggregation. WR-45/54 then reads that resolved squadron source value, rather than testing any/all/first live craft. An escaped squadron retains its living craft through the completion sweep; those craft are not independent unescaped losses. Carrier-spawned temporary squadrons have no persistent fleet identity to return and do not become bought squadrons on retreat. The stock bought X-wing squadron has no authored flag, but its craft supply true. | debug build GRET-05..09; effective XML; WR-45/47/54; WGT-24/37 |
| WR-86 | Faction retreat countdown content starts at **15.99 seconds**. An effective `Space_Retreat_Countdown_Seconds` value overrides that constructor default; omission does not refuse battle admission. An accepted retreat uses trunc(seconds times logical FPS), as in WR-44 and FC-R01. | debug build; WR-44; FC-R01 |
| WR-87 | A non-playable faction may participate in battle. Its tactical retreat request is rejected by the faction eligibility gate; `Is_Playable` and an authored countdown are not tactical admission requirements. This evidence qualifies the tactical retreat coordinator, not the separate autoresolve withdrawal interface. | debug build; WR-42..44 |

These fresh read-only debug-build rows refine U-8's cancellation, cleanup and
scripted-request boundaries; raw reads and the opaque EVR evidence map stay in
ignored research storage. Cancellation restoration was missing from the earlier
rules. Completion and cleanup agree with WR-47 and now expose their wait and
identity contracts. No runtime loss-column, cinematic restore or independent
automatic-forced space producer is qualified here.

These qualifications establish the completion predicates, native tactical phase,
timer domain and per-member acceleration schedule. The schedule's FD-R01..04
and FSC-R01..07 provide acceptance cases. Exact rendered disappearance
and the full presented camera/effect sequence retain UFS-R01/02 there; neither prior video FPS nor
the accelerated-prefix index establishes a model-hide frame.

## EAWR deployment-facing extension

WR-X01 is an owner-requested EAWR feature beyond FoC. While a space reinforcement is being dragged, wheel up adds 15 degrees of counter-clockwise source-plane yaw per detent and wheel down subtracts 15 degrees. The preview models and squadron offsets rotate immediately. Wheel presses and releases belong to placement during the drag; after it ends they reach the normal camera zoom path.

The choice belongs to the local player for that battle and persists through successful drops, failed drops, cancellation and closing the pane until another drag wheel gesture changes it. Starting another battle clears it. Before any wheel gesture, placement and arrival retain the player's FoC reinforcement facing exactly. Every rotated drop carries the explicit normalized Q24 yaw in its command and replay (opcode 26); the authoritative WR-25 sweep, craft heading and arrival flight direction all use that value. No persistent simulation preference is needed to reproduce a replay because each drop records its own facing.

## Existing notes and code comparison

The comparison is against the pending purchasing heads above. **Missing** includes a
partly implemented rule with a missing branch; details say which part. GC interfaces are
excluded from M2 gap totals. Integration has only WR-42 and the fog presentation interface;
the arrival implementations below depend on simulation economy, build queue and arrivals/purchase UI and reinforcement placement merging.

| Rule | Existing behaviour notes | Ours: pending implementation or boundary | Status / gap |
|---|---|---|---|
| WR-01 | same: WPR-04, PU-21 | `src/skirmish/economy.cpp` `economy_rules` | same |
| WR-02 | same: WPR-32/41, PU-21 | `src/sim/tactical/session_economy.cpp` `execute_economy`, population shares | same |
| WR-03 | differs: WPR-41/PU-21 omit ignored/mixed member branches | `population_share` covers M2 homogeneous rosters; general branches absent | missing G-5 |
| WR-04 | same: WPR-41, PU-21 | `src/sim/tactical/economy.cpp` `population_count` hardcodes fraction threshold 0 | differs G-5 |
| WR-05 | same: PU-21/33 | `execute_economy`, `layout_pool`, placement input | same |
| WR-06 | same: WPR-03/22, PU-22/30 | ledger pool and production service; free initial/launch objects have no shares | same |
| WR-07 | missing there except pane toggle in PU-66 | `EawrProductionPanel::_gui_input` toggles; no full permission/victory guard | missing G-4 |
| WR-08 | same: PU-66 explicitly leaves internal order unverified | `src/presentation/ui/production.cpp` `layout_pool`, first-completion order | same for grouping; U-2 |
| WR-09 | same: PU-66 | `layout_pool`, `production_view.cpp` | same for skirmish |
| WR-10 | same: PU-67 | `pool_rows`, `EawrProductionPanel` | same |
| WR-11 | same: PU-68 | `EawrProductionPanel::_gui_input`, viewer's placement selection | same except sound under WR-16 |
| WR-12 | missing there: PU-G15 only names preview | no preview clone creation | missing G-3 |
| WR-13 | missing there | no cursor-following clone pose | missing G-3 |
| WR-14 | missing there | no placement-validity clone tint | missing G-3 |
| WR-15 | differs: PU-68 replaces drag-release with a subsequent world click | UI input uses click placement; command still validates | differs G-3 |
| WR-16 | missing there except PU-68's pick sound | no full reinforcement-selection/drop/cap audio | missing G-6 |
| WR-17 | same: FW-22/23 | `LiveFogView`, purchase UI and reinforcement placement overlay/provider hook | same, subject to G-1's exact validity |
| WR-18 | differs: PU-G8 leaves possible space delay unverified | no 90-frame space delay | same |
| WR-19 | missing there | `execute_economy` has no pending-victory guard | missing G-4 |
| WR-20 | same: WPR-32, PU-33 | `execute_economy` checks room/placement/pool | same |
| WR-21 | same: PU-31 | `TacticalSession::Impl::placement_valid`, fog point query | same; tests tracked by production data-to-rules validation (legacy EAWR-751) |
| WR-22 | same: PU-31 | `placement_valid`, non-allied strict planar circles | same |
| WR-23 | same: PU-31, PU-G21 | bounds checked, original bounds construction unverified | same predicate; U-3 |
| WR-24 | missing there | `placement_valid` still tests static circles for no-layer single ships | differs G-1 |
| WR-25 | same: PU-32 distinguishes project endpoint from original sweep | `placement_valid` uses circle at endpoint only | differs G-1 |
| WR-26 | same: PU-32 describes extents | enclosing-circle formation footprint | differs G-1 |
| WR-27 | missing there: PU-32 notes corvette exception only | layer-less squadrons skip every mobile layer blocker | differs G-1 |
| WR-28 | same: PU-32 | endpoint static-circle test replaces static sweep | differs G-1 |
| WR-29 | missing there | ordinary command only; no privileged scripted route | interface, outside M2 |
| WR-30 | same: PL-08; PU-34 and PU-G26 retain height uncertainty | `execute_economy` single-ship branch and `layer_z_adjust` | same placement; U-5 |
| WR-31 | same: PL-08/current PU-34; WPR-32's older formation-placement gap superseded | `find_free_space` used per craft, point fallback | same |
| WR-32 | same: WPR-32, PU-34/38 | `execute_economy`, ledger/shares/arrival record | same |
| WR-33 | same: PU-35/37/39 | `ArrivalState`, movement phase and hidden-instance mask; scripted damage faithfully bypasses invulnerability | same; arrival script-damage immunity review is not a gap |
| WR-34 | missing there | no cinematic-delay input | missing G-10; no default M2 trigger known |
| WR-35 | missing there | an arrival below frame 120 keeps the circle marked at creation at the drop point (WR-49) | same |
| WR-36 | same: PU-35 to PU-37 | `arrival_position`, visible frame 35 | same |
| WR-37 | same: PU-36/69 and PU-G14 | model appears abruptly; faction arrival sound absent | missing G-6/G-7 |
| WR-38 | same: PU-35 | `arrival_step`, `arrival_tail`, movement phase | same |
| WR-39 | missing there | the frame-120 enable moves the held circle by the V-12 rule | same |
| WR-49 | PU-31 reads fog only | the creation-frame circle is held at the drop point through frame 119; later drops validate against it | same; chained drops tracked as a drop-area growth bug (legacy EAWR-1875) |
| WR-40 | same: PU-39; unloaded/fog hooks missing there | arrival record removed at 150; final motion reaches exit; no unloaded notification or forced fog refresh | missing G-9 |
| WR-41 | same default: PU-38; PU-G19 records longer duration bug | `arrival_hit_defense` loses modifier when arrival record ends | differs G-8 |
| WR-42 | same: SK-43; victory/end notes explicitly exclude retreat | `src/script/foc/ai_perception.cpp` `CANRETREAT=0`; no retreat order | same |
| WR-43..48 | missing there; campaign excluded in existing notes | later GC interfaces; no M2 tickets | interface |

Rule-status totals (WR-01..42 and WR-49, excluding WR-29): **24 same, 8 differs, 10 missing**.
WR-08's ordering, WR-23's bounds and WR-30's height remain explicitly unverified despite
matching the established predicates. The remaining seven rules are interfaces: WR-29 and
WR-43..48. These counts concern rules; gap tickets group related rules.

## Gaps and ticket ownership

Tracking issue: reinforcement and retreat rule walk (legacy EAWR-919).

No implementation changes are part of this walk. Existing issues were checked before filing.
Sizes are implementation estimates: S (bounded change), M (several services), L (new system).

| Gap | Rules | Work and observable acceptance | Size | Work |
|---|---|---|---|---|
| G-1 | WR-24..28 | Use rectangular hard extents, a 200-unit facing sweep over 115 frames, correct layer bypasses and squadron filters. Pin a blocker behind the point, moving into the lane, static obstruction, corvette exception and no-layer ship bypass. | M | arrival-lane sweep and layer filters (legacy EAWR-911) |
| G-2 | WR-35, WR-39, WR-49 | Hold the creation circle at the drop point while the arrival service is disabled and move it from frame 120, separately from model frame 35. Pin fog before/after 120 and a second drop beyond the old edge. | S | frame-120 arrival sensors (legacy EAWR-912); daisy-chained drops (legacy EAWR-1875) |
| G-3 | WR-12..15 | Create/pose/tint space placement clones with hidden emitters and implement drag-release placement. Pin preview geometry and validity separately from actual craft free-space positions; capture a lit clip of valid and invalid placement. | M | reinforcement preview and drag release (legacy EAWR-913) |
| G-4 | WR-07, WR-19 | Gate pane/placement on reinforcement permission, command-bar visibility and pending victory; authoritative arrival must refuse after pending victory without changing pool/population. Pin both preselected and new requests. | S | pending-victory reinforcement gate (legacy EAWR-914) |
| G-5 | WR-03, WR-04 | Read the rounding threshold; support share accounting beyond M2 homogeneous small squadrons and verify ignored/mixed roster branches before implementing them. | S | data-driven economy constants (legacy EAWR-753); U-6 |
| G-6 | WR-16, WR-37 | Wire pane selection, landing-zone selection, drop acknowledgement/cap failure and spatial frame-35 arrival audio. The existing production-sound issue already owns arrival audio; use this rule list to bound its reinforcement hooks. | S | production and arrival sounds (legacy EAWR-725); broader battle audio foundation (legacy EAWR-443) |
| G-7 | WR-37 | Apply independent black-to-normal arrival light fade from frame 35 for 115/120 s at 30 FPS, multiplied with fog/death presentation. Preserve headless hashes and compare a lit arrival clip. | S | frame-35 arrival light fade (legacy EAWR-915) |
| G-8 | WR-41 | Retain timed vulnerability independently after landing and pin a modded duration above 150 frames. Direct debug-build verification corrected the former script-immunity claim: script/cheat damage bypasses immunity in FoC, pinned before 35 and at 35. | S | combat-constant application (legacy EAWR-844) for timed vulnerability; arrival script-damage immunity review (legacy EAWR-916) is not a gap (debug build WR-E46/47) |
| G-9 | WR-40 | Emit unloaded completion for plans and force the existing fog-refresh interface at arrival end. Pin one completion per surviving arrival; a destroyed arrival emits none. AI reinforcement planning remains purchasing-capable skirmish AI setup (legacy EAWR-603), live AI economy queries (legacy EAWR-786). | S | arrival completion and fog reevaluation (legacy EAWR-917) |
| G-10 | WR-34 | Provide a cinematic-delay gate that holds arrival counter and visibility. Needs a sourced M2 trigger or a focused capture before adding a live-game dependency. | S | cinematic arrival-delay trigger research (legacy EAWR-918) |

The five highest M2 impacts are G-1 (unsafe/incorrect placement), G-2 (early sensors), G-4
(arrivals after battle decision), G-3 (the placement interaction), and G-6/G-7 (audible and
visible arrival). Station purchasing and reinforcements (legacy EAWR-530)/simulation economy, build queue and arrivals/purchase UI and reinforcement placement remain the main delivery path; this walk does not duplicate
production's buying, build-queue and reinforcement-button flash (legacy EAWR-726)
or AI-buying tickets. Production data-to-rules validation (legacy EAWR-751) already owns fog-admission test coverage.

## XML/tag registry audit

At the audited integration head, the following relevant rows are `todo`; these are not
claims that the pending purchasing PRs have passed the registry gate. Implementers must
update the applying code location and rule IDs in their implementation PRs.

| Tags / object class | Registry target | Rule / boundary |
|---|---|---|
| `Space_Tactical_Unit_Cap` (Faction), `Population_Value` (Container, SpaceUnit, Squadron, UniqueUnit) | todo: economy tag support (legacy EAWR-654) | WR-01..05 |
| `Allow_Reinforcement_Percentage_Normalized` (GameConstants) | todo: combat tag support (legacy EAWR-650); hardcoded implementation tracked by data-driven economy constants (legacy EAWR-753) | WR-04 |
| `Reinforcement_Prevention_Radius` (SecondaryStructure, SpaceUnit, SpecialStructure, StarBase) | todo: combat tag support (legacy EAWR-650) | WR-22 |
| `Space_Reinforcement_Collision_Check_Distance` (GameConstants) | todo: movement tag support (legacy EAWR-649) | WR-25 |
| `ReinforcementOverlayGoodColor`, `ReinforcementOverlayBadColor` (GameConstants) | todo: presentation tag support (legacy EAWR-653) | WR-14; read in space preview, not just land |
| `Reinforcements_Selection_SFXEvent`, `Reinforcements_Enroute_SFXEvent`, `Reinforcements_Cancelled_SFXEvent` (Faction) | partial: local space skirmish gestures; land and campaign callers remain outside this path | WR-07, WR-16, BA-84 |
| `Reinforcements_Pick_Landing_Zone_SFXEvent` (Faction) | todo: fighter tag support (legacy EAWR-651) | WR-11 |
| `SFXEvent_Tactical_Unit_Cap_Reached` (Faction) | todo: economy tag support (legacy EAWR-654) | WR-16 |
| `SFXEvent_Fleet_Move` (space types) | authored type event; debug-build tag-table lookup verifies this XML spelling, with faction fallback only when absent | WR-16 |
| `SFXEvent_Arrive_From_Hyperspace` (Faction) | incorrectly marked land-or-galactic at audit; corrected to todo production and arrival sounds (legacy EAWR-725) by this walk | WR-37 directly reads it during skirmish arrival |
| `Space_Elevated_Vulnerability_Duration`, `Space_Elevated_Vulnerability_Factor` (GameConstants) | todo combat-constant application (legacy EAWR-844), parsed but not applied on integration | WR-41 |
| `Squadron_Offsets` (Container), `Space_Layer` (Container, SpaceStructure, Projectile) | todo: fighter tag support (legacy EAWR-651) / movement tag coverage (legacy EAWR-649) respectively | WR-13, WR-24..27; Container population/formations are later interfaces, not the M2 Squadron row |
| `Icon_Name`, `Space_Model_Name`, `Scale_Factor`, `Layer_Z_Adjust`, `Space_Layer` (arrivable space types), `Squadron_Units` / `Squadron_Offsets` (Squadron) | applied in existing UI, movement and start readers; does not imply arrival-preview application | WR-09, WR-12/13, WR-24..31; G-3 still needs clones |
| `Idle_Anim_00_Rate_Mod`, `Loop_Idle_Anim_00`, `Ignore_For_Reoptimization` | no todo/deferred arrival-type row: idle-rate authored on props, idle-loop on other classes, ignored-member tag authored on GroundCompany | WR-03, WR-12 describe generic type getters and defaults; mods require a per-class registry update when authored |

The following nearby tags are explicitly outside the traced M2 space rules:

- `Use_Reinforcement_Points` (True), `Reinforcement_Enabling_Radius`,
  `Reinforcement_Region_Blob_Name` and point-ownership countdown sounds: land placement
  interfaces. The enabling-point switch's two direct readers are land company placement
  (WR-E44/45); the space placement and cap functions read none of them. Existing todo rows
  combat tag coverage (legacy EAWR-650)/presentation tag coverage (legacy EAWR-653) are not space arrival implementation requirements.
- `Skirmish_Reinforcement_Delay_Frames` (90), `Reinforcement_Time_Multiplier`,
  `Reinforcement_Deploy_Time_Multiplier` and transport progress: WR-18's separate transport
  interface. Existing rows todo: combat tag support (legacy EAWR-650)/space ability rule walk (legacy EAWR-760) remain; do not apply 90 to space arrivals.
- `Reinforcements_Shadow_Blob_Material_Name`: land preview blob (WR-E38); space uses model
  clones. `Reinforcements_Ready_SFXEvent` and
  `Reinforcements_Requesting_SFXEvent`: other pane/transport notification paths, not proved
  by this walk; keep U-9 and their todo: presentation tag support (legacy EAWR-653) rows.
- `Disable_Reinforcement_Vulnerability`: battlefield modifier supplied by abilities (legacy EAWR-760).
  WR-E39 verifies that it adds the disable state; whether each space damage route consumes
  it is not established here. No M2 starting unit supplies it.
- `Hyperspace`, `Disallows_Hyperspace_Retreat`, retreat-prevention/protection abilities and
  the `Space_Retreat_*` tags: WR-43..47's campaign interface. Registry todo/deferred entries
  here do not justify adding skirmish retreat. `Space_Retreat_Attrition_Factor` is 0;
  `RetreatAutoResolveLoserAttrition` 0.65 and `RetreatAutoResolveWinnerAttrition` 0.50 are
  autoresolve data, not an arrival/retreat countdown calculation.
  The exact campaign `todo` set is `Space_Retreat_Allowed_Countdown_Seconds`,
  `Space_Retreat_Attrition_Factor`, `Space_Retreat_Countdown_Seconds`,
  `Space_Retreat_Flight_Move_Increment`, `Space_Retreat_Off_Map_Dest_Pos`,
  `Space_Retreat_Pursue_Max_Speed_Mod_Factor`, `Space_Retreat_Unit_Increment_Wait_Frames`
  and `Space_Retreat_Units_Damaged_Mod_Factor` movement tag coverage (legacy EAWR-649), plus
  `Space_Retreat_Begin_SFXEvent`, `Space_Retreat_Cancel_SFXEvent`,
  `Space_Retreat_Countdown_Color_RGBA`, `Space_Retreat_Countdown_Text_ID`,
  `Space_Retreat_Enemy_Begin_SFXEvent`, `Space_Retreat_Not_Allowed_SFXEvent` and
  `Space_Retreat_Not_Allowed_Reason_1_SFXEvent` through
  `Space_Retreat_Not_Allowed_Reason_3_SFXEvent` presentation tag coverage (legacy EAWR-653).
- `Hyperspace_Speed`, `Hyperspace_Speed_Factor`: galaxy/story travel interfaces; not read
  by WR-38. `Additional_Population_Capacity` is not read by WR-01's space cap query.

No subsystem-applied tag in this docs-only is newly marked `applied`. The only registry
change corrects the verified skirmish use of the arrival-sound tag.

## Settled questions from the unverified sweep

Question IDs are retained; these boundaries no longer require a new source read. Opaque evidence IDs identify ignored research receipts. Runtime acceptance and explicitly remaining clauses stay below.

| ID | Sourced disposition | Evidence |
|---|---|---|
| U-5 | The first creation flag adds nonzero Layer_Z_Adjust to the supplied position before retaining creation position and initialization. The second flag marks a death clone. Ordinary space reinforcement supplies zero input Z and true/false, so single ships and each created company craft receive their own authored layer adjustment. | EUS-39 |

## Unverified and requested captures

| ID | Unknown / fidelity deviation | Focused evidence to settle it |
|---|---|---|
| U-1 | Whether an arriving ship/craft can fire or accept attack/ability orders before 150. The movement lock is proven; simulation economy, build queue and arrivals's blanket fire/order suppression is a project choice, not settled by it. **Sweep:** Still unverified: Movement lock and object-service order are sourced, but incoming fire, attack commands and individual ability admission have separate gates. A lock is not proof of blanket fire/order suppression before local counter 150. | Debug-build or retail arrival next to a hostile target, orders at 34/35/119/120/149/150, with fog stated; record first accepted order and first shot. Retained sweep boundary: EUS-04. |
| U-2 | Stable visible ordering of grouped pool types. Internal key traversal is proven, first-completion order is not. **Sweep:** Completed pool entries are counted in a map keyed by type-definition identity, then displayed in that key traversal. Within one loaded definition set, changing completion order does not reorder grouped types. Exact stock order across loads remains a runtime observation, not an alphabetical or completion-order contract. Still unverified: Exact stock display ordering and allocation tie behavior across loads require a capture; the static completion-order independence is settled. The remake first-completion grouping differs; pool slot-order fix (legacy EAWR-1808). | Build two types in both completion orders and capture the pool; repeat a new session. Retained sweep boundary: EUS-14. |
| U-3 | Original playable-bound construction and inclusive edge semantics. Simulation economy, build queue and arrivals uses declared extents. **Sweep:** Still unverified: Playable-bound setter/getter retain a box; their virtual producer and exact point-containment edge equality remain unresolved. Declared extents are not proof of inclusive original edges. | Read the bounds supplier; capture cursor validation immediately inside/outside an asymmetric map edge with fog off. Retained sweep boundary: EUS-37. |
| U-4 | **Partly settled:** grid decay before arrivals; local-120 reveal enabling can feed the later same-object reveal service that frame; command creation charges population before object/deletion service **Sweep:** Ordinary manager detach removes tactical-population registration before command-bar/audio detach; destruction that is merely queued has not yet reached that endpoint. Already-sourced decay/reveal/creation order remains unchanged. Still unverified: Competing arrivals, exceptional deletion and every population-unregistration caller still require route-specific traces; do not infer release at lethal contact. | [WFO-02/08/12/17/18/26/31](frame-order.md), debug build. Competing same-player events and exact share-unregistration callbacks remain UFO-01/05 there; step a full-cap death plus two requests through the existing debugger harness. Retained sweep boundary: EUS-31. |
| U-6 | The general ignored/mixed squadron share branch differs from the simple equal-share description; crafted XML can visit multiple roster entries before finding a match. **Sweep:** Still unverified: The mixed/ignored roster share branch is not settled by grouped UI counts or station spawner selection. Its full roster reader and custom ignore-list fixture remain necessary. | Confirm with targeted disassembly and a mixed/ignored roster population observation before implementing the unusual branch. Homogeneous M2 rosters are settled. Retained sweep boundary: EUS-37. |
| U-7 | Hazard registration: which specific asteroid/nebula/storm/mine footprints enter the static sweep. No direct hazard flag is read in arrival validation. **Sweep:** Still unverified: Dense-grid initialization and arrival tests are separate interfaces. A map-load dense snapshot does not enumerate all asteroid/nebula/storm/mine objects admitted to the reinforcement-modifying static sweep. | Hazards walk supplies registration evidence; request arrival previews inside and beside each hazard only where a remaining mismatch needs capture. Retained sweep boundary: EUS-10. |
| U-8 | GC-only live survivor/attrition ledger and battle statistics, cancellation feedback and exceptional deletion/restore, and any independent automatic-forced space producer. WR-74..78 source restoration, detach cancellation, completion, exact cleanup membership and the gated story request; WGT-76 separates optional station victory installation. WR-50..63 retain sourced flight/station gates, native phase, timer domain and acceleration. The serializer inventory WGS-132 does not qualify resumed cinematic/audio parity. | In a campaign battle, record a mixed ordinary-ship/squadron/hero-carrier/flagship roster and source identities before countdown, after player cancellation, after a prevention cancellation, at escape completion, in both result loss columns and after return. Repeat last-member direct removal and a checkpoint during countdown and flight, with an uninterrupted control and engine/jump feedback. Stage station loss and a superweapon outcome separately: record actual request, phase and automatic-forced flag instead of treating victory or sound as proof of a forced countdown. Preserve RNG before/after and destination identity; stock zero attrition does not qualify a nonzero loss roll. Exact screen disappearance and complete presentation remain UFS-R01..03 in the [retreat schedule](../gc-retreat-frame-schedule.md). |
| U-9 | Reinforcement ready/requesting notifications and cancellation callers outside ordinary space placement, hyperspace flash/streak presentation and a live skirmish trigger for cinematic arrival delay. | A lit retail clip from completed build through pane open, cancelled and accepted drag, and landing; trace delay callers only when an M2 trigger is identified. |

These are the fidelity list for this walk. The retreat qualification adds a bounded
debug-build campaign probe; its video frames do not establish native tactical service
frames. No implementation, build, simulation test or scenario ID was added. Validation
is docs/link/evidence-ID review, clean-room scanning, `git diff --check` and the
tag-registry checker; implementation tickets carry their future acceptance checks.

## Campaign land reinforcement admission

Read-only **debug build** GLH-E04/07..09/16 and effective **XML data** GLH-X01,
2026-10-10. These rules extend the earlier land-only interfaces; they do not
replace WR-01..41's space arrival or establish land deployment animation time.
The galaxy's transport roster is WGA-239 and initial company creation is
WGT-80..82 in the [GC tactical walk](gc-tactical.md). Population room, location
validity and reserve identity consumption are separate interfaces.

| ID | Rule | Source |
|---|---|---|
| WR-79 | In an ordinary campaign land child, maximum company population is `Max_Ground_Forces_On_Planet` (**10** stock) for the defender; for every other valid player it is **min(last known tactical population capacity, that limit)**. Missing player yields zero. The standalone multiplayer-tactical land branch instead takes min(last known capacity, faction `Land_Skirmish_Unit_Cap` for the controllable player count). Do not apply the standalone cap to every campaign battle or count transports as equivalent to tactical population shares. | debug build GLH-E08; XML GLH-X01 |
| WR-80 | Tactical capacity refresh stores faction `Base_Population_Capacity` plus `Additional_Population_Capacity` of owned live/nondeleting objects; this is not a scan limited to objects named reinforcement point. The inspected additional-allied-player scan is gated by standalone multiplayer-tactical mode, so an allied enabling radius alone is not campaign ownership capacity. Current company count sums registered tactical population shares and rounds up only when the fractional part is **strictly greater** than `Allow_Reinforcement_Percentage_Normalized` (**0** stock). Ordinary room compares rounded requested land `Population_Value` (null type uses 1) with max minus current. The AI open-slot reader adds min(current, weakened count); the inspected weakened-count helper returns **0**, so this build supplies no weakened-company bonus. | debug build GLH-E04/08/16; WR-03/04; XML GLH-X01 |
| WR-81 | Ordinary land placement maps the company's authored constituents into positions with the player's reinforcement facing and validates terrain occupancy, fog, formation/collision and reinforcement modifiers. With `Use_Reinforcement_Points` **True**, every constituent must lie within a live/nondeleting allied positive `Reinforcement_Enabling_Radius`, using planar squared distance **<= radius squared**. A positive enemy `Reinforcement_Prevention_Radius` rejects constituents at **strictly less** than its radius squared; an energy-pool source additionally requires its owner's base-powered modifier. Turning the point switch off removes the enabling requirement, not the prevention check. Geometry uses the unique ground carrier when a model-less constituent names one. | debug build GLH-E03/07; XML GLH-X01 |
| WR-82 | The land reinforcement wrapper rejects missing type, derives facing from the player's active-mode reinforcement direction and passes through its privileged placement flag. An ordinary nonhuman caller additionally enables the mobile-object overlap exemption; ordinary human input does not. The privileged flag independently bypasses the wrapper's reinforcement-modifier and fog checks. Neither path is a blanket terrain/obstacle bypass. Preserve per-constituent validation results rather than validating just a company's center. | debug build GLH-E03/07 |
| WR-83 | AI landing-location selection requires an active land mode, takes the current open slots and consumes its requested type vector **from the back**. Null or over-cap tail entries are discarded and make the overall result false. Enumerate allied positive-radius reinforcement-modifying objects; when both perception regions exist, reject unreachable points and rank by the goal's reachability cost, otherwise rank by planar distance from the target. Visit ascending cost. Try offsets at **half enabling radius**, starting on +X and increasing angle by **pi/4** (code) while angle < 2pi. Each accepted placement appends its position, removes that type and subtracts its land population, then again drops any nonfitting tail types. Return true only when no requested types remain and none were discarded; partial positions may already exist on a false return. | debug build GLH-E09; WGA-239 |
| WR-84 | Stock reinforcement-point data authors enabling radius **250**, capture radius **100**, capture transition **10 s** and an ordinary point capacity **100**; variants override capacity to **1, 2, 3, 4, 5 or 10**. These are data inputs with variant inheritance. Campaign WR-79 still caps ordinary attacker population at 10. `Reinforcement_Region_Blob_Name` is presentation data; the point highlight service does not prove capture progress or population-refresh timing. `Skirmish_Reinforcement_Delay_Frames` **90** is standalone data and does not establish a campaign transport's arrival time. | XML GLH-X01; debug build GLH-E07/08/09; WR-18 |

At comparison base `a653ef312f64f017a4c2998d17e42d682b57af37`, these land
consumers are **missing**: `src/sim/tactical/economy.cpp` implements
space arrival, and `src/campaign/conflict_entry.cpp` refuses land child creation.
The space population ledger, validated authored types, capture services and
deterministic command commits are reuse seams. Six sourced rows do not change
the original space-walk status counts.

Synthetic implementation cases: defender at population 9 may admit a cost-1
request without a captured point increasing its cap, but still needs valid
placement; attacker capacity 4 admits no cost-2 request when current count is
3; enabling-boundary equality is accepted and prevention-boundary equality is
not rejected by that radius test. An AI vector with a nonfitting tail can
produce some positions and return false. These are proposed arithmetic/contract
tests, not observations of a retail battle.

| ID | Qualified unknown | Capture recipe |
|---|---|---|
| U-10 | Live land population refresh versus point ownership transition, competing requests and reserved-but-not-unloaded population/identity release. Static capacity and placement readers do not establish these service ties. | Use the existing debugger harness and frame-service programme: fog off, one captured point, a full/near-full company roster, two same-player requests and point loss during arrival. Record ownership transition, last known capacity, registered shares, pending reserve IDs, command acceptance and unloaded callback at each boundary. Request any missing schedule dependency from that programme rather than tracing it ad hoc. |
| U-11 | Stock land transport deployment/facing, model appearance, audio/cancellation and equal-cost point selection across loads; the space 35/120/150 counters are not land evidence. | Fixed campaign land map with fog off; record one company and one unique-vehicle hero from request through deployment/cancellation. Log selected point, each constituent pose, transport state, first accepted order/unloaded callback and presentation. Repeat tied points and a mid-arrival save/load against an uninterrupted control. |

Nearby tags remain separate consumers: `Reinforcement_Time_Multiplier`,
`Reinforcement_Deploy_Time_Multiplier` and faction
`Garrison_Reinforcement_Delay_Seconds` belong to transport/garrison deployment,
not WR-79's cap equation or WR-83's location search. Radius overlays, countdown
sounds and blobs belong to presentation. This docs-only append applies no tag
and changes no tag-registry status.
