# Galactic conquest: corruption, heroes and special actions

## Scope and evidence

This FoC galactic-conquest walk covers planet corruption choices, mission
admission, corruption transitions and removal, hero neutralization, galactic
sabotage, technology theft, black-market action selection, credit-siphon
deployment, spy action invocation, and scheduled hero creation. Campaign scripts
are considered only at the galactic action and mission interfaces. Tactical
weapon abilities, land mission objectives and campaign plot progression are
outside this walk.

Sources are **debug build**, read-only research, and **XML data**, the effective
FoC data in `gameobjectfiles.xml` order. Research was performed on 2026-10-02.
Opaque GCH evidence IDs identify private records in ignored research output;
original symbols, addresses and raw reads are not part of this document. No
retail capture is claimed. An XML declaration establishes authored data, not
an untraced runtime effect. Questions marked **unverified** remain questions.

Every WGCH rule is **missing in ours for GC**. Existing tactical abilities,
production, visibility and hero types are integration points, not a galactic
implementation. Tables name the proposed module for each group.

## Existing rules and subsystem boundaries

Read the Wave 1 walks first. These interfaces reuse their rule IDs rather than
defining the same behavior twice.

| Existing note | Comparison and boundary |
|---|---|
| [Economy/production](gc-economy-production.md), WGEP-04/12/13/14 | **Same** corruption-income, siphon expiry, piracy collection and post-fiscal black-market-income rules; deployment and corruption transitions are **missing there**. |
| Economy/production, WGEP-29/32/55/58 | **Same** corruption production factors and black-market offer price/debit/unlock rules; this walk adds action selection and buyer absence timing. |
| [Movement/control](gc-movement-control.md), WGM-14/21/27/33/34/37/83/84/85 | **Same** hero-arrival orders, corruption traversal, raid eligibility and bribe charges; corruption choice names do not replace those fleet rules. |
| Movement/control, WGM-65/77 | **Same** planet-service placement and ownership-driven default corruption; this walk supplies the transition consumer and completion effects. |
| Movement/control, WGM-78/79/80/81 | **Same** hero defeat/scheduling and separate corruption-mission return; actual due-record placement is **missing there** and added below. |
| [Galactic AI](gc-ai.md), WGA-36/45 | **Same** hero freestore policy and saboteur/removal plan interfaces; ability-local option selection belongs here. |
| [Space heroes](heroes.md), WHE-01/38 | **Same** hero classification and tactical skirmish respawn exclusion; galactic action costs and scheduled creation are **missing there**. |
| [Galactic sensors](gc-ui-sensors.md), WGUI-06..32 (parallel Wave 2 walk) | **Same** mask/predicate and spy catalog interfaces: WGUI-19..23 owns defaults, flag prerequisites, source-keyed effects and expiry; WGUI-24 owns transfer influence; WGUI-25..32 owns stock spies/probes and fleet awareness. This walk owns invocation and actor consequences. |

## Service order and actors

There is no one galactic hero sweep. Commands use persistent actor/target IDs;
contained heroes can be reached through a fleet and transport. Arrival abilities
are handed off by WGM-14/34/83. Local humans prepare a choice dialog, while AI
actors select an option within the handler. A later command executes the prepared
choice; its validation and mutation order matters.

Planet service handles corruption before political control and allegiance
(WGM-65). Corruption completion updates state, story/presentation, then
planet-specific upgrades, one-time money, tactical rewards, build unlocks,
special-ability notifications and revolt, in that order. Scheduled creation is
a separate planet service over stored records. Fiscal collection is WGEP-01..14;
it is not an action cooldown. The relative ordering of scheduled creation and
every ordinary special-ability service remains U-01.

## Rules in local evaluation order

### Invocation and corruption admission

Integration: **new galactic action commands and persistent actor/planet state**,
Lua bindings, AI engine, validated ability data and save/replay.

| ID | Rule and conditions | Source |
|---|---|---|
| WGCH-01 | Corruption command availability first respects the tutorial's restriction for the destination, then checks names in `Activated_Corrupt_Planet_Ability_Names` in authored order until an appropriate ability is found. The list is a routing input; an authored ability absent from it is not proved available through this command route. | debug build GCH-E04; XML data |
| WGCH-02 | The contained-ability lookup rejects a fleet whose flattened member count exceeds **1**. Otherwise it descends through fleet members, then transport contents, and finally the object's own special-ability manager. A transport can carry several candidate members; the fleet gate is separate. | debug build GCH-E05 |
| WGCH-48 | Corruption eligibility requires an actor and planetary target; reject an allied object owner, neutral object owner, destroyed planet, active transition or existing corruption. Require `Planet_Retains_Residual_Influence` and at least one nonempty corruption slot. This uses object ownership, unlike WGCH-55's target allegiance. | debug build GCH-E19 |
| WGCH-03 | A corruption actor with no prepared choices fails. A local human opens the corruption dialog; an AI filters choices by credit, mission, planet and unit requirements, admits only positive ability-local scores, then samples the weighted choice. If AI admits no option, return false without performing an action. | debug build GCH-E08 |
| WGCH-04 | Enumerate nonempty planet corruption slots **1, 2, 3**, in that order, without suppressing unaffordable choices. Each carries type, briefing, credits, required unit alternatives/count, required mission type/count and required planets. Omit required planet types absent from the active galaxy. Requirement semantics are WGCH-49. | debug build GCH-E08/E19; XML data |
| WGCH-49 | Unit requirements pool all alternative types globally for the actor's owner and compare their aggregate count with the required count. Deduplicate a transport container; consumable units without a current planet do not count, while nonconsumable units may count through a parent container. No alternatives or count below **1** passes. Mission requirements compare the player's selected-type usage counter with the required count; none/below-one passes. Planet requirements are **any owned instance of any listed type**, not all planets; an empty list passes. | debug build GCH-E19 |
| WGCH-50 | AI corruption score uses `n`, the player's previous uses of that type: intimidation **2/(n+1)**, piracy **1/(n+1)+1**, kidnapping **0.75**, racketeering **1.5**, corrupt militia **1/(n+1)+0.5**, bribery **0.25**, black market **2.5**, bonded citizens **0.75**; unrecognized type **0**. Scores are code constants, separate from XML income. | debug build GCH-E19 |
| WGCH-05 | Execution resolves the actor ID again and requires its owner, destination planet state and planetary service. Reject an active corruption transition or an already corrupted planet. Compare current credits with the prepared required credits, then debit a positive cost **before** rechecking required units. A failed unit recheck returns without a refund in this routine. Do not silently change this ordering to all-or-nothing admission. | debug build GCH-E08 |
| WGCH-06 | A positive required-unit count with a resolved required type invokes prerequisite consumption. If `Destroy_When_Used_As_Corruption_Prereq` is false, the consumer leaves those units alive. A named consumed hero is scheduled using the ordinary default-time path and removed; the reader expects one named instance. | debug build GCH-E08; XML data |
| WGCH-07 | Nonnamed consumable prerequisites are collected globally for the actor's owner. Collapse transport payloads to their transport, avoid duplicate container IDs, and require a containing planetary system. Each candidate receives a synchronized random weight from **0..4**, independent of distance; consume highest-weight candidates until count is exhausted, destroying their payload/breakdown and container. Tie order is U-03. | debug build GCH-E08 |
| WGCH-08 | A tactical corruption mission requires a nonempty authored map and a land or space mission mode. Only a non-AI, single-player campaign actor takes this path; AI and multiplayer campaign use direct corruption. A debug fast-corruption option also bypasses the tactical path. Mission queuing/transition installs a success-dependent corruption request with **5 s** duration. Mission objectives remain the named script's interface. | debug build GCH-E08; XML data |
| WGCH-09 | Without a tactical mission, begin the selected corruption with **5 s** duration. On success increment the player's usage count for that corruption type, notify hero arrival, and queue authored local success speech/hologram when permitted. Both successfully queued mission and direct paths schedule/remove the acting hero through the default-time path. Mission queue failure returns false after any earlier debit/consumption. | debug build GCH-E08 |
| WGCH-10 | Tactical completion uses WGM-81: successful invader result begins the stored transition and emits corruption completion hooks; failure emits its own story failure. It does not enter normal conquest, survivor or base-return reconciliation. | same movement/control WGM-81 |

### Planet transitions, effects and removal

Integration: **new corruption state and transitions**, economy_rules, production,
political-control/movement interfaces, Lua bindings, AI engine and save/replay.

| ID | Rule and conditions | Source |
|---|---|---|
| WGCH-11 | Begin-corruption and begin-removal wrappers reject an already active transition. Begin-corruption rejects the no-corruption type. Successful starts refresh corruption visuals and emit their tutorial interface events. Ownership-triggered **10 s** transitions remain WGM-77; action-triggered **5 s** transitions are WGCH-09. | debug build GCH-E08; same WGM-77 |
| WGCH-51 | Increasing requires current type none and no active transition; removal requires current type nonempty and no active transition. Removal's exactly **0 s** becomes **0.1 s**; increase has no such substitution. Record start=current frame and end=start+**truncate(duration × logical FPS)**. A deadline earlier than start triggers a debug invariant; there is no negative-duration clamp in this setter. Hero scheduling's rounded deadline is a different rule. | debug build GCH-E16/E19 |
| WGCH-12 | Completion requires an active transition and current frame **at least** its end frame. Capture old type, end the transition, refresh visuals, emit increased/decreased story state and applicable local faction sound, then apply effects using new/old types. Ending copies target to current, clears increase/decrease flags and start/end frames, resets target to none and records current frame as last change. | debug build GCH-E08/E19 |
| WGCH-13 | Completion effects run in this order: automatic faction upgrade, one-time credit reward, planet-specific tactical upgrade, build unlock, special-ability corruption-change notifications, revolt. Iterate all players for faction-specific automatic upgrades: add the new type's upgrade, then remove the old type's upgrade when present. This is not only the corrupting player's upgrade list. | debug build GCH-E01/E08; XML data |
| WGCH-14 | A new nonempty corruption type pays its positive authored `Corruption_N_Success_Credit_Bonus` to the corrupting player. Removing corruption pays no reward through this routine. It is a transition-completion reward, distinct from recurring WGEP-13 income. | debug build GCH-E08; XML data |
| WGCH-15 | For a valid corrupting player, add the new type's authored tactical reward upgrade at this planet and remove the old one's. Build unlocks similarly add the new authored reward type only when its build cost is positive, and remove the old reward type on exit. Unlock removal is explicit; permanence must not be inferred from a successful mission. | debug build GCH-E08; XML data |
| WGCH-16 | Notify the planet's special-ability manager of the new and old corruption states after rewards/unlocks. Readers of this notification, recurring income and fleet movement are separate interfaces; income uses WGEP-04/12/13, movement uses WGM-21/27/33. | debug build GCH-E08; same linked rules |
| WGCH-17 | Removing **corrupt-militia** corruption to no corruption triggers revolt. Clear ground and space special structures and set starbase level to **0**. Collect distinct owners from orbital fleets then landed transports; for each, request surface retreat then orbital retreat. Clear political modifiers and process neutral allegiance at **1**. Other corruption removals do not enter this revolt branch. Retreat destination selection is WGM-61..63. | debug build GCH-E08 |
| WGCH-18 | Removal requires the actor's owner to equal the planet's **target allegiance player**, existing corruption and no active transition. Cost is `Planet_Corruption_Removal_Base_Cost × Corruption_Removal_Cost_Multiplier`; negative cost becomes zero. Require sufficient money for a positive cost. Begin removal first and debit only on success, then send arrival/local presentation hooks. | debug build GCH-E08/E18/E19; XML data |
| WGCH-19 | Removal duration is the acting ability's `Corruption_Removal_Time_In_Secs`, independent of planet base level. Constructor defaults are cost multiplier **1**, duration **10 s**, legacy removal-level count **1**. The stock active planet removal base cost is **1500** where authored; named hero overrides are cataloged below. | debug build GCH-E06/E08; XML data |

### Hero neutralization and bounty-hunter actions

Integration: **new galactic action/target records**, economy_rules, hero identity,
scoring history, Lua bindings and save/replay. Tactical kill-income bounties are
an effect interface, distinct from paid galactic neutralization.

| ID | Rule and conditions | Source |
|---|---|---|
| WGCH-20 | The ordinary neutralization target gate accepts a planet only when it contains no hackable superweapon; it is not the common opposing-planet gate. Build candidates from visible orbit fleets, then landed transports, then active credit siphon. A siphon remains a target after its deploying hero disappears. Visibility belongs to the sensors walk; capability checks are WGCH-52. | debug build GCH-E08/E19 |
| WGCH-52 | A generic target needs the actor ability's minor-neutralization flag; a named target needs its major flag. A named actor additionally requires the target type's `Can_Be_Neutralized_By_Major_Heroes`; any other actor uses `Can_Be_Neutralized_By_Minor_Heroes`. An invalid check disables the option rather than changing its price. | debug build GCH-E19; XML data |
| WGCH-21 | Preparation samples separate actor and target respawn times from authored ranges with synchronized randomness; either nonpositive endpoint selects `Default_Hero_Respawn_Time` (**360 s**). The displayed prepared option retains these values for execution. | debug build GCH-E08; XML data |
| WGCH-22 | Let `B` be the target's `Neutralization_Cost`, multiplied by the major modifier for a named hero or minor modifier for a generic hero. Cost combines `B`, `B × base_curve`, `B × (protection_product − 1)`, named-target `B × previous_neutralizations_curve`, and **subtracts** `B × corruption_curve`. Floor the sum at **1 credit**. Base contribution uses orbit starbase or ground base only if the target's owner is allied to the planet controller; otherwise its level input is **0**. | debug build GCH-E08; XML data |
| WGCH-23 | Protection removes the target hero's own source from a copied planet modifier ledger, takes the greatest modifier in each category, then multiplies categories. Previous-neutralization history is keyed by target company type and attacking player. Corruption-curve input is the corrupted boolean, **0 or 1**, not a growing level counter. Stock curves and modifiers are cataloged below. | debug build GCH-E08; XML data |
| WGCH-24 | A local human receives the candidate dialog. AI filters valid and affordable options and samples them with equal weight **1**. Execution resolves the actor and target again, requires an active siphon for a siphon target, validates the prepared option and compares current credits with its cost. Missing actor/target, invalid option or insufficient money fails before ordinary success mutation. | debug build GCH-E08 |
| WGCH-25 | `Planet_Auto_Capture_Minor_Heroes` intercepts a generic neutralizer: schedule the actor through the default path, destroy it, return false, and do not charge the prepared neutralization cost or remove the target. Named actors do not take this generic-hero branch. | debug build GCH-E08; XML data |
| WGCH-26 | Successful normal targeting sets final-blow attribution on the target, schedules it with the prepared target timer while bypassing the ordinary parent check, notifies scoring and destroys the target. Successful siphon targeting instead notifies its company scoring identity and terminates the siphon. Then debit, send arrival/tutorial/presentation hooks, schedule the actor with its prepared timer and destroy it. There is no tactical battle for this action. | debug build GCH-E08 |

### Galactic sabotage

Integration: **new action commands and planet sabotage history**, production,
economy_rules, hero scheduling, Lua bindings and save/replay.

| ID | Rule and conditions | Source |
|---|---|---|
| WGCH-27 | Legacy activity construction checks ground-base reduction with positive level, starbase reduction with positive level, cancellation with work in either queue, special-structure destruction with a **corrupted** planet and a positive special-structure count, and income halt on an opposing planet. Each requires its ability flag. Stock saboteur data authors special-structure destruction; other activities are mod/legacy interfaces, not claimed stock actions. | debug build GCH-E07/E08; XML data |
| WGCH-53 | Sabotage's opposing-planet gate also requires a nonempty valid activity list. Constructor defaults disable all five legacy activity flags, use income-halt duration **0** and price escalation factor **2**. Stock saboteur XML enables special-structure destruction; the other flags are not implied by this action's name. | debug build GCH-E15/E19; XML data |
| WGCH-28 | Require actor, target structure, planet and state. Ordinary `Sabotage_Cost_Credits` below **1** rejects. Price is target cost times `Sabotage_Cost_Increase_Factor` raised to the planet's **prior sabotage action count**, then nearest-integer rounding; a negative result rejects. Stock/default factor is **2**. This reader requests ordinary cost, not tutorial cost. | debug build GCH-E08/E10/E15; XML data |
| WGCH-29 | Execution requires nonnegative prepared price and, for an ordinary action, a still-existing actor and sufficient current credits. Locate the **same target identity** in ground specials first, then space specials. Remove it with attacking-player attribution and reevaluate production for that layer. A target absent from those lists or removal failure returns false without the success debit/despawn. | debug build GCH-E08 |
| WGCH-30 | On successful removal, emit galactic sabotage story state, debit the actor, schedule it only when the prepared respawn time is **nonnegative**, destroy it, emit the configured sabotage effect/sound and increment planet sabotage history. The explicit free/script path skips actor lookup, payment and actor removal, attributes no attacking player, but still increments history. | debug build GCH-E08 |
| WGCH-31 | In the legacy activity path, auto-capture of a generic actor schedules/removes it and reports true without applying sabotage. Other admitted activities dispatch ground base, starbase, production (space first, then ground), special structures or income halt. Internal legacy effects beyond stock special-structure sabotage remain U-07. Do not unify its result code with WGCH-25's false result. | debug build GCH-E08 |

### Technology theft and black-market actions

Integration: **new galactic action commands and unlock records**, economy_rules,
production discovery, AI engine, Lua bindings, hero state and save/replay.

| ID | Rule and conditions | Source |
|---|---|---|
| WGCH-54 | Enumerate registered types in registration order. A sliceable type must belong to the **actor's faction**, have tech at most the player's current tech, be initially build-locked, be absent from both the player's unlocked and explicitly locked lists, and allow slicer unlocking. Availability is not the enemy planet's build queue. | debug build GCH-E19; XML data |
| WGCH-55 | The common opposing-planet predicate requires actor and planetary target and rejects an ally of the planet's **target allegiance player**. Slicer and siphon also require its **current allegiance player** to have a playable faction; black market further requires black-market corruption. Slicer eligibility needs a nonempty sliceable list. Ownership, current control and target control must remain distinct inputs. | debug build GCH-E17/E19 |
| WGCH-32 | Preparation enumerates WGCH-54's sliceable types. An empty list at maximum tech fails; otherwise increment tech and retry. Prepare price/chance and current-tech timer. Local humans receive a menu. AI admits affordable choices with weight **1** and sets the advance-tech flag exactly when the prepared list has **one item**; the human acceptance flag remains U-08. | debug build GCH-E08/E19 |
| WGCH-33 | Theft price is `Slice_Cost_Credits × (planet_tech_availability_curve + affinity_price_adjustment) × technology_purchase_multiplier`, floored at **0**. Exact type membership in `Planet_Slice_Affinity_Types` enables affinity adjustments. Success chance is `clamp(1 + applicable_base_curve + planet_difficulty_curve + affinity_chance_adjustment, 0, 1)`; use starbase level for space-built types and ground-base level for ground-built types. | debug build GCH-E08; XML data |
| WGCH-34 | Resolve the slicer ID and technology type and compare current money with prepared price. Auto-capture of a generic slicer despawns it using the default timer and returns false without buying technology. Otherwise draw synchronized float in **0..0.999**, succeeding only when draw is **strictly less** than prepared chance. Success debits and unlocks the type; the explicit advance-tech flag increments tech. | debug build GCH-E08 |
| WGCH-35 | A failed theft draws synchronized integer **1..3**: **1** doubles prepared absence time; **2** keeps absence time and spends nothing; **3** spends prepared price without unlocking. All ordinary success/failure paths despawn the actor with the resulting absence time. No production queue is used. | debug build GCH-E08 |
| WGCH-36 | Slicer respawn-array lookup accepts indices **0..4**, returns **0** with empty arrays or invalid index, otherwise samples between per-index bounds. R2-D2 authors fixed **60/60/60/70/70 s**. The zero result feeds the existing default respawn rule, rather than proving instant return. | debug build GCH-E08; XML data; same WGM-80 |
| WGCH-37 | Preparation initially enumerates every nonnull black-market item in list order; WGEP-58 supplies offer filtering, flags and price, including recording already-owned status without removing that offer. AI admits affordable present items and weights by **prepared price**; local humans receive a menu. Debit/unlock are WGEP-55/58. Acceptance of stale or already-owned offers remains U-09. | debug build GCH-E08/E19; same WGEP-55/58 |
| WGCH-38 | Black-market absence-time indexing subtracts **1** from item tech and accepts resulting indices **0..4**. Empty/invalid arrays return **0**, otherwise sample the per-index range. Stock Tyber/Urai/Silri ranges are fixed **90/30/60 s** respectively. Selection is prepared before execution; execution rechecks actor existence and current affordability. | debug build GCH-E08; XML data |

### Credit siphons, spies and hero replacement

Integration: **new persistent galactic action/hero state**, economy_rules,
production placement, AI engine, Lua bindings, sensors interfaces and save/replay.

| ID | Rule and conditions | Source |
|---|---|---|
| WGCH-39 | Siphon application rejects a generic actor when minor-siphon prevention is active and rejects any new siphon when the planet already has one. The activated path's generic auto-capture schedules/removes the actor and returns false. Otherwise sample `Min_Siphon_Percentage..Max_Siphon_Percentage` and pass actor identity, percentage and `Siphon_Duration_In_Cycles` to the planetary siphon service. Income/expiry is WGEP-12. | debug build GCH-E08; XML data |
| WGCH-40 | Stock generic Rebel/Empire smugglers author a fixed **0.5** siphon for **5 fiscal cycles**; Han authors **0.45..0.65**, **2 cycles**, and fixed **300 s** absence. The tutorial smuggler explicitly differs: **0.35..0.75**, **10000 cycles**. These are ability values, not globally hardcoded percentages or hero-death timers. | XML data GCH-D01 |
| WGCH-56 | Spy eligibility requires actor and target. A planetary actor targeting itself passes. Otherwise ground-activated spy uses WGCH-55's opposing-planet predicate; other activation styles reject only when the actor has planetary state and is destroyed. Probe consumption is declared by `Causes_Despawn`; the generic action-host consumption hook is an integration boundary. | debug build GCH-E19; XML data |
| WGCH-41 | Spy application delegates from actor/target to the owner's player visibility effect. Invocation and actor lifetime are separate from revelation masks. The stock probe is initially enabled, ground-activated, has `Causes_Despawn = Yes` and **60 s** visibility duration; passive hero spies use automatic galactic activation, do not despawn and author **−1** duration. Defaults/catalog are the same WGUI-19/25..27; effect masks, source keys and expiry are WGUI-20..24. | debug build GCH-E08; XML data GCH-D01; same WGUI rules |
| WGCH-42 | Galactic stealth application toggles the object's galactic-stealth state on and removal toggles it off. Detection and evade-chance decisions are separate sensor readers (WGUI-28..32); the authored `Evade_Detection_Chance` is not a paid-action success chance. | debug build GCH-E08; XML data |
| WGCH-43 | Scheduling eligibility, containing company/squadron type, owner/final-blow fields and rounded deadline are WGM-78..80. The due-record consumer visits records in stored order and processes a record when frame **at least equals** its spawn frame; earlier records that are not yet due do not block later due records. | debug build GCH-E02; same WGM-78..80 |
| WGCH-44 | An ordinary due record gathers planets currently owned by its recorded player and sorts them by proximity to the original scheduling planet. No owned planet erases the record. Try placement at each candidate in order, stop/erase on success, and retain the due record for a later service if all placements fail. Capturing the originally scheduled planet therefore does not by itself cancel or assign the hero to its new owner. | debug build GCH-E02 |
| WGCH-45 | Structure-backed records re-count owned backing structures and owned contained hero-type instances. If hero count is not below structure count, erase the record. Otherwise find the nearest owned planet with the structure; erase when none exists or when placement succeeds, retain only when a found placement fails. The structure check is repeated at execution, not only at scheduling. | debug build GCH-E02 |
| WGCH-46 | Placement uses the destination planet's production creation/placement service for the stored type and player. Failure returns no object. Success sends the local hero-spawn/sound interface, offers AI-owned results to the galactic freestore, and invokes the human fleet-unification interface. Company payload, orbit/surface capacity and containers follow WGEP-41 and movement identity rules. | debug build GCH-E03; same WGEP-41 |
| WGCH-47 | Forced respawn requires a nonnull supplied type and scans this planet's scheduled list. Every record whose stored type **exactly matches** receives current frame as its spawn frame. It neither creates a new record nor places a hero immediately; the next due service still applies WGCH-44/45. | debug build GCH-E08 |

### Other galactic special-action interfaces

Integration: **new action host**, planet lifecycle, production locks,
political control, session victory and Lua bindings. Cleanup inside the
called planet/session services belongs to their walks.

| ID | Rule and conditions | Source |
|---|---|---|
| WGCH-57 | Hacking requires actor, planet state and a hackable superweapon. A local actor opens its choice menu unless it is AI-controlled and nonhuman; this returns false without the immediate destructive branch. Remote non-AI actors return false. The AI branch locks the superweapon type for its owner, destroys the acting hero without a respawn schedule in this branch, begins **5 s** corruption removal if corrupted, marks/refreshes ruins, destroys orbital fleets, clears the starbase and forces neutral ownership. Human acceptance and superweapon identification remain U-12. | debug build GCH-E11/E20 |
| WGCH-58 | Planet destruction requires its ability gate and planetary services. When the actor reports contested space, any nonstealth nonallied orbital fleet blocks the action. If the active superweapon-kills-leader victory condition applies and the current controller's exact leader type is in landed transport contents, register pending victory with **30 logical frames**. Then request destruction to ruins, refresh the planet and emit effects. Planet cleanup and victory resolution are separate interfaces. | debug build GCH-E20 |
| WGCH-59 | Political protection applies a true boolean modifier keyed by the actor/ability's unique source with **−1** duration, registers the planet as a target and emits effects. Its effect on conquest is WGM-74; source removal/expiry is still U-12. It is not a corruption-removal discount. | debug build GCH-E20; same WGM-74 |
| WGCH-60 | On leaving a planet with piracy corruption, a nonallied, nonstealth fleet with positive piracy value adds that value to the planet's accrued piracy income. Value sums each flattened fleet member's **original type** `Piracy_Value`; it does not deduct the fleet owner's credits here. WGEP-13 owns later fiscal payment/reset. | debug build GCH-E13/E14; XML data; same WGEP-13 |

## Authored faction actions and constants

The inventory distinguishes stock definitions, command routing lists and
campaign availability. A campaign can omit or lock a hero; this table does not
promise every actor is available in every GC map. XML class/tag/object names are
mod-facing data identifiers. Older commented definitions are not active data.

| Faction | Active authored actors/actions | Boundary |
|---|---|---|
| Rebel | R2-D2: slicer; Han and generic smuggler: siphon; generic bounty hunter: minor neutralization; named heroes: removal and passive spy/stealth capabilities | Corruption removal actors include Mon Mothma, Obi-Wan, Han, Kyle, Ackbar, Luke and Yoda; routing-list reachability is distinct from authored abilities. |
| Empire | Probe: system spy; generic smuggler: siphon; generic bounty hunter: minor neutralization; Boba Fett: major/minor neutralization; Mara Jade: minor neutralization; named heroes: removal and passive spying | Palpatine, Vader, Veers, Mara and Thrawn/Admonitor author removal. Tarkin political protection and Death Star planet destruction are separate system interfaces. |
| Consortium | Saboteur/defiler: corruption and structure sabotage; Tyber, Urai and Silri: black market; Bossk and IG-88: major/minor neutralization; passive spy capabilities | IG-88 also authors superweapon hacking. Corruption income, piracy/bribery and production multipliers use the linked Wave 1 rules. |

| Action values | Effective XML values and meaning |
|---|---|
| Removal multiplier / seconds | Mon Mothma **0.90/3**; Palpatine **1/3**; Obi-Wan **1/5**; Vader **1/8**; Luke **1/10**; Yoda **0.95/6**; Thrawn **1.1/10**, Admonitor **1.1/20**. Ackbar, Veers, Han, Mara and Kyle omit these overrides and use WGCH-19 defaults. |
| Neutralization curves | Base level **(0,0), (5,1)**; prior named-target neutralizations **(0,0), (1000,750)**; corruption boolean **(0,0), (1,0.25)**. Curves are interpolation inputs, not per-action constants. |
| Neutralization actor / target absence | Generic bounty hunters: **240..300 / 240..300 s**, major cost modifier **2**, minor **1**, major targets disabled. Boba/Mara/Bossk/IG-88: **120..150 / 240..300 s**, cost modifiers **1/1**; Boba/Bossk/IG-88 enable major targets, Mara disables them. |
| R2-D2 theft curves | Tech availability **(0,1.75), (1,1.5), (2,1.25), (3,1), (4,0.75), (5,0.5)**; price affinity **−0.25**; base/difficulty chance adjustments **0** at authored breakpoints **0..5**; chance affinity **+0.25**. |
| Black market | Tyber price **1**, tech adjustment **−1**, absence **90 s**; Urai price **1.5**, adjustment **0**, absence **30 s**; Silri price **0.75**, adjustment **0**, absence **60 s**. WGEP-58 owns uncommon and tutorial price formulas. |
| Corruption and fiscal/hero constants | Action transition **5 s** (code); ownership transition **10 s** (WGM-77); fiscal period **45 s** (`Fiscal_Cycle_Time_In_Secs`, WGEP-02); ordinary hero fallback **360 s** (`Default_Hero_Respawn_Time`, WGM-80). Do not reuse one clock as another. |

Corruption has eight authored types in the GameConstants income-array order:
intimidation, piracy, kidnapping, racketeering, corrupt militia, bribery, black
market and bonded citizens. The recurring income factors are WGEP-13. Each
planet authors its offered types, credits and prerequisites, rather than one
global price per type. The generic automatic upgrade objects mainly carry
corruption display data; the Consortium racketeering variant additionally
authors `Bounty_On_Faction_Ability`, percentage **1.0**, category **1**, no ally
propagation, and target factions Empire/Rebel/Hutts/Pirates. This declaration
does not settle tactical kill-income aggregation (U-11).

### Planet offer catalog

XML data GCH-D01 contains **117** nonempty corruption slots.
These are planet-type offers, not a promise that each planet appears in a given
campaign. An omitted field is not supplied a value by this catalog; the runtime
requirements and rewards follow WGCH-04/49 and WGCH-14/15. Tactical maps are
mod-facing XML identifiers; scripts/objectives remain their separate interface.
Speech, briefing and hologram fields are presentation inputs listed in the
reader inventory.

| Planet / slot | Type / credits | Prerequisites | Tactical mode / map | Completion reward |
|---|---|---|---|---|
| Abregado_Rae / 1 | Bribery / 1500 | 1 uses of Corruption_Kidnapping | — | none authored |
| AetenII / 1 | Piracy / 1000 | 1 × Bossk | Space: `_Space_Planet_AetenII_Piracy.ted` | unlock Underworld_TIE_Defender_Squadron |
| AetenII / 2 | Racketeering / 1000 | 2 × Underworld_Disruptor_Merc; 1 uses of Corruption_Intimidation | — | none authored |
| AlzocIII / 1 | Intimidation / 1000 | 1 × Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue | Land: `_Land_Planet_AlzocIII_Intimidation.ted` | none authored |
| AlzocIII / 2 | Black Market / 1500 | 1 uses of Corruption_Piracy | — | none authored |
| Anaxes / 1 | Intimidation / 1000 | 1 × Bossk | Land: `_Land_Planet_Anaxes_Intimidation.ted` | none authored |
| Anaxes / 2 | Racketeering / 800 | 1 × Destroyer_Droid; 1 uses of Corruption_Intimidation | — | none authored |
| Anaxes / 3 | Black Market / 1000 | 1 uses of Corruption_Piracy | — | none authored |
| Atzerri / 1 | Racketeering / 1000 | 3 × Underworld_Merc; 1 uses of Corruption_Intimidation | — | none authored |
| Atzerri / 2 | Bribery / 1500 | 1 uses of Corruption_Kidnapping | — | none authored |
| Atzerri / 3 | Black Market / 1000 | 1 uses of Corruption_Piracy | — | none authored |
| Bespin / 1 | Piracy / 1000 | 1 × Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue | Space: `_Space_Planet_Bespin_Piracy.ted` | unlock Broadside_Underworld |
| Bespin / 2 | Racketeering / 800 | 1 × Underworld_Mobile_Defense_Unit; 1 uses of Corruption_Intimidation | — | none authored |
| Bespin_Story / 1 | Piracy / 1000 | 1 × Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue | Space: `_Space_Planet_Bespin_Piracy_Story.ted` | unlock Broadside_Underworld |
| Bespin_Story / 2 | Racketeering / 800 | 1 × Underworld_Mobile_Defense_Unit; 1 uses of Corruption_Intimidation | — | none authored |
| Bestine / 1 | Kidnapping / 1000 | 1 × IG-88 | Land: `_Land_Planet_Bestine_Kidnapping.ted` | credits 10000 |
| Bestine / 2 | Black Market / 1500 | 1 uses of Corruption_Piracy | — | none authored |
| Bonadan / 1 | Racketeering / 900 | 2 × Underworld_Merc; 1 uses of Corruption_Intimidation | — | none authored |
| Bonadan / 2 | Corrupt Militia / 1000 | 1 uses of Corruption_Bribery | — | none authored |
| Bonadan / 3 | Black Market / 1000 | 1 uses of Corruption_Piracy | — | none authored |
| Bothawui / 1 | Intimidation / 1000 | 1 × Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue | Land: `_Land_Planet_Bothawui_Intimidation.ted` | none authored |
| Bothawui / 2 | Bonded Citizens / 800 | 1 × Destroyer_Droid; 1 uses of Corruption_Piracy | — | unlock Bothan_Slave_Company |
| Bothawui / 3 | Black Market / 1000 | 1 uses of Corruption_Piracy | — | none authored |
| Byss / 1 | Kidnapping / 1000 | 1 × Silri | Land: `_Land_Planet_Byss_Kidnapping.ted` | credits 9500 |
| Byss / 2 | Black Market / 1500 | 1 uses of Corruption_Piracy | — | none authored |
| Carida / 1 | Kidnapping / 1000 | 1 × Bossk | Land: `_Land_Planet_Carida_Kidnapping.ted` | credits 8000 |
| Carida / 2 | Bribery / 1700 | 1 uses of Corruption_Kidnapping | — | none authored |
| Carida / 3 | Black Market / 2000 | 1 uses of Corruption_Piracy | — | none authored |
| Corellia / 1 | Piracy / 1000 | 1 × Tyber_Zann | Space: `_Space_Planet_Corellia_Piracy.ted` | unlock Corellian_Corvette_Underworld |
| Corellia / 2 | Black Market / 1600 | 1 uses of Corruption_Piracy | — | none authored |
| Corulag / 1 | Piracy / 1000 | 1 × IG-88 | Space: `_Space_Planet_Corulag_Piracy.ted` | unlock Acclamator_Underworld |
| Corulag / 2 | Corrupt Militia / 700 | 4 × Starviper_Fighter; 2 uses of Corruption_Bribery | — | none authored |
| Coruscant / 1 | Bribery / 2000 | 1 uses of Corruption_Kidnapping | — | none authored |
| Coruscant / 2 | Black Market / 2000 | 1 uses of Corruption_Piracy | — | none authored |
| Dathomir / 1 | Bribery / 1000 | 1 uses of Corruption_Kidnapping | — | none authored |
| Dathomir / 2 | Corrupt Militia / 800 | 1 × Destroyer_Droid; 2 uses of Corruption_Bribery | — | none authored |
| Dathomir / 3 | Racketeering / 500 | 1 × Underworld_Mobile_Defense_Unit; 1 uses of Corruption_Intimidation | — | none authored |
| Dagobah / 1 | Racketeering / 100 | 2 × Underworld_Disruptor_Merc; 1 uses of Corruption_Intimidation | — | none authored |
| Dantooine / 1 | Intimidation / 1200 | 1 × Urai_Fen | Land: `_Land_Planet_Dantooine_Intimidation.ted` | none authored |
| Dantooine / 2 | Bribery / 1800 | 1 uses of Corruption_Kidnapping | — | none authored |
| Endor / 1 | Bonded Citizens / 800 | 2 uses of Corruption_Piracy | — | unlock Underworld_Ewok_Handler_Company |
| Eriadu / 1 | Racketeering / 600 | 1 × MAL_Rocket_Vehicle; 1 uses of Corruption_Intimidation | — | none authored |
| Eriadu / 2 | Black Market / 1500 | 1 uses of Corruption_Piracy | — | none authored |
| Felucia / 1 | Racketeering / 1200 | 2 × Underworld_Merc; 1 uses of Corruption_Intimidation | — | none authored |
| Felucia / 2 | Bribery / 1000 | 1 × Underworld_Mobile_Defense_Unit; 1 uses of Corruption_Kidnapping | — | none authored |
| Fondor / 1 | Piracy / 1000 | 1 × Bossk | Space: `_Space_Planet_Fondor_Piracy.ted` | unlock Underworld_A_Wing_Squadron |
| Fondor / 2 | Corrupt Militia / 1000 | 2 × Underworld_Merc; 3 uses of Corruption_Bribery | — | none authored |
| Fresia / 1 | Bribery / 1500 | 1 uses of Corruption_Kidnapping | — | none authored |
| Fresia / 2 | Corrupt Militia / 1000 | 1 × Underworld_Disruptor_Merc; 1 uses of Corruption_Bribery | — | none authored |
| Geonosis / 1 | Corrupt Militia / 800 | 1 × Destroyer_Droid; 1 uses of Corruption_Bribery | — | none authored |
| Geonosis / 2 | Bonded Citizens / 1000 | 3 × Underworld_Merc; 1 uses of Corruption_Piracy | — | unlock Geonosian_Slave_Company |
| Honoghr / 1 | Intimidation / 1000 | 1 × Urai_Fen | Land: `_Land_Planet_Honoghr_Intimidate.ted` | none authored |
| Honoghr / 2 | Racketeering / 300 | 2 × Underworld_Disruptor_Merc; 2 uses of Corruption_Intimidation | — | none authored |
| Honoghr_Story / 1 | Intimidation / 1000 | 1 × Urai_Fen | Land: `_Land_Planet_Honoghr_Intimidate_Story.ted` | none authored |
| Honoghr_Story / 2 | Racketeering / 0 | 2 × Underworld_Disruptor_Merc; 2 uses of Corruption_Intimidation | — | none authored |
| Hoth / 1 | Racketeering / 500 | 2 × Underworld_Merc; 2 uses of Corruption_Intimidation | — | none authored |
| Hypori / 1 | Black Market / 1500 | 1 uses of Corruption_Piracy | — | none authored |
| Ilum / 1 | Bribery / 1200 | 1 uses of Corruption_Kidnapping | — | none authored |
| Jabiim / 1 | Racketeering / 900 | 2 × Underworld_Disruptor_Merc; 2 uses of Corruption_Intimidation | — | none authored |
| Jabiim / 2 | Corrupt Militia / 600 | 2 × Underworld_Merc; 1 uses of Corruption_Bribery | — | none authored |
| Kamino / 1 | Corrupt Militia / 600 | 1 × Crusader_Gunship; 2 uses of Corruption_Bribery | — | none authored |
| Kamino / 2 | Black Market / 1300 | 2 uses of Corruption_Piracy | — | none authored |
| Kamino_Prologue / 1 | Racketeering / 800 | 1 uses of Corruption_Piracy | — | none authored |
| Kashyyyk / 1 | Racketeering / 800 | 2 × Underworld_Disruptor_Merc; 1 uses of Corruption_Intimidation | — | none authored |
| Kashyyyk / 2 | Bonded Citizens / 1000 | 3 × Underworld_Merc; 3 uses of Corruption_Piracy | — | unlock Wookiee_Slave_Company |
| Kessel / 1 | Piracy / 1000 | 1 × IG-88 | Space: `_Space_Planet_Kessel_Piracy.ted` | unlock Marauder_Underworld |
| Kessel / 2 | Corrupt Militia / 550 | 1 × Crusader_Gunship; 1 uses of Corruption_Bribery | — | none authored |
| Korriban / 1 | Racketeering / 1200 | 1 × Underworld_Disruptor_Merc; 2 uses of Corruption_Intimidation | — | none authored |
| Kuat / 1 | Kidnapping / 1000 | 1 × Bossk | Land: `_Land_Planet_Kuat_Kidnapping.ted` | credits 10000 |
| Kuat / 2 | Black Market / 1500 | 1 uses of Corruption_Piracy | — | none authored |
| Manaan / 1 | Racketeering / 800 | 2 × Underworld_Disruptor_Merc; 2 uses of Corruption_Intimidation | — | none authored |
| Mandalore / 1 | Piracy / 1000 | 1 × Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue | Space: `_Space_Planet_Mandalore_Piracy.ted` | unlock Tartan_Underworld |
| Mandalore / 2 | Bribery / 800 | 2 × Skipray_Blastboat; 2 uses of Corruption_Kidnapping | — | none authored |
| Mandalore_Prologue / 1 | Piracy / 1000 | 1 × Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue | Space: `_Space_Planet_Mandalore_Piracy_Prologue.ted` | unlock Tartan_Underworld |
| Mandalore_Prologue / 2 | Bribery / 2000 | 2 uses of Corruption_Kidnapping | — | none authored |
| Mandalore_Story / 1 | Piracy / 1200 | 1 × Tyber_Zann, Tyber_Zann_Passenger, Tyber_Zann_Prologue | Space: `_Space_Planet_Mandalore_Piracy_Story.ted` | unlock Tartan_Underworld |
| Mandalore_Story / 2 | Bribery / 2000 | 2 uses of Corruption_Kidnapping | — | none authored |
| MonCalimari / 1 | Piracy / 1000 | 1 × IG-88 | Space: `_Space_Planet_MonCalamari_Piracy.ted` | unlock Nebulon_B_Underworld |
| MonCalimari / 2 | Bonded Citizens / 800 | 1 × Underworld_Mobile_Defense_Unit; 1 uses of Corruption_Piracy | — | unlock Mon_Calamari_Slave_Company |
| Mustafar / 1 | Corrupt Militia / 500 | 1 × Underworld_Mobile_Defense_Unit; 1 uses of Corruption_Bribery | — | none authored |
| Mustafar / 2 | Bribery / 1200 | 1 uses of Corruption_Kidnapping | — | none authored |
| Muunilinst / 1 | Kidnapping / 1000 | 1 × IG-88 | Land: `_Land_Planet_Muunilinst_Kidnapping.ted` | credits 8500 |
| Muunilinst / 2 | Bribery / 2000 | 1 uses of Corruption_Kidnapping | — | none authored |
| Myrkr / 1 | Racketeering / 500 | 1 × Destroyer_Droid; 2 uses of Corruption_Intimidation | — | none authored |
| Naboo / 1 | Intimidation / 800 | 1 × Silri | Land: `_Land_Planet_Naboo_Intimidation.ted` | none authored |
| Naboo / 2 | Bonded Citizens / 800 | 1 × Destroyer_Droid; 2 uses of Corruption_Piracy | — | unlock Gungan_Slave_Company |
| NalHutta / 1 | Kidnapping / 1000 | 1 × Urai_Fen | Land: `_Land_Planet_NalHutta_Kidnapping.ted` | credits 8000 |
| NalHutta / 2 | Bribery / 1000 | 3 × Starviper_Fighter; 1 uses of Corruption_Kidnapping | — | none authored |
| NalHutta / 3 | Black Market / 1800 | 2 uses of Corruption_Piracy | — | none authored |
| NalHutta_Prologue / 1 | Black Market / 1000 | 1 uses of Corruption_Piracy | — | none authored |
| NalHutta_Prologue / 2 | Bribery / 2000 | 1 uses of Corruption_Kidnapping | — | none authored |
| NalHutta_Story / 1 | Kidnapping / 1000 | 1 × Urai_Fen | Land: `_Land_Planet_NalHutta_Kidnapping.ted` | credits 8000 |
| NalHutta_Story / 2 | Corrupt Militia / 1500 | 1 uses of Corruption_Bribery | — | none authored |
| Polus / 1 | Racketeering / 1200 | 1 uses of Corruption_Intimidation | — | none authored |
| Polus / 2 | Bonded Citizens / 800 | 1 × MAL_Rocket_Vehicle; 3 uses of Corruption_Piracy | — | unlock Pyngani_Slave_Company |
| Polus / 3 | Black Market / 1000 | 1 uses of Corruption_Piracy | — | none authored |
| Ryloth / 1 | Corrupt Militia / 1000 | 1 uses of Corruption_Bribery | — | none authored |
| Ryloth / 2 | Bonded Citizens / 800 | 2 × Underworld_Disruptor_Merc; 1 uses of Corruption_Piracy | — | unlock Twilek_Slave_Company |
| Ryloth_Prologue / 1 | Corrupt Militia / 1000 | 1 uses of Corruption_Bribery | — | none authored |
| Ryloth_Prologue / 2 | Bonded Citizens / 800 | 2 × Underworld_Disruptor_Merc; 1 uses of Corruption_Piracy | — | unlock Twilek_Slave_Company |
| Saleucami / 1 | Intimidation / 1300 | 1 × Urai_Fen | Land: `_Land_Planet_Saleucami_Intimidation.ted` | none authored |
| Saleucami / 2 | Racketeering / 700 | 3 × Underworld_Merc; 1 uses of Corruption_Intimidation | — | none authored |
| Shola / 1 | Racketeering / 500 | 3 × Underworld_Disruptor_Merc; 1 uses of Corruption_Intimidation | — | none authored |
| Sullust / 1 | Bribery / 1500 | 1 uses of Corruption_Kidnapping | — | none authored |
| Sullust / 2 | Bonded Citizens / 800 | 1 × Destroyer_Droid; 1 uses of Corruption_Piracy | — | unlock Sullustan_Slave_Company |
| Taris / 1 | Intimidation / 900 | 1 × Silri | Land: `_Land_Planet_Taris_Intimidation.ted` | none authored |
| Taris / 2 | Bribery / 800 | 4 × Underworld_Merc; 1 uses of Corruption_Kidnapping | — | none authored |
| Tatooine / 1 | Bribery / 500 | 2 × Skipray_Blastboat; 1 uses of Corruption_Kidnapping | — | none authored |
| Tatooine / 2 | Black Market / 900 | 1 uses of Corruption_Piracy | — | none authored |
| Thyferra / 1 | Kidnapping / 1300 | 1 × Tyber_Zann | Land: `_Land_Planet_Thyferra_Kidnapping.ted` | credits 7500 |
| Thyferra / 2 | Racketeering / 500 | 1 × MAL_Rocket_Vehicle; 2 uses of Corruption_Intimidation | — | none authored |
| Utapau / 1 | Kidnapping / 800 | 1 × Silri | Land: `_Land_Planet_Utapau_Kidnapping.ted` | credits 9000 |
| Utapau / 2 | Bribery / 1500 | 1 uses of Corruption_Kidnapping | — | none authored |
| Wayland / 1 | Racketeering / 200 | 1 × MZ8_Pulse_Cannon_Tank; 2 uses of Corruption_Intimidation | — | none authored |
| Wayland / 2 | Black Market / 1200 | 1 uses of Corruption_Piracy | — | none authored |
| Yavin / 1 | Racketeering / 500 | 2 × Underworld_Disruptor_Merc; 2 uses of Corruption_Intimidation | — | none authored |
| Yavin / 2 | Bribery / 200 | 1 × MAL_Rocket_Vehicle; 1 uses of Corruption_Kidnapping | — | none authored |

## XML reader and registry cross-check

Research reads `gameconstants.xml`, `gameobjectfiles.xml`, `factions.xml`,
`expansion_factions.xml`, `planets.xml`, `genericherounits.xml`,
`namedherounits.xml`, `minor_heroes_expansion.xml`, hero-specific expansion XML,
`spaceunitssupers.xml`,
`herocompanies.xml`, `upgradeobjects_underworld.xml`, `blackmarketitems.xml`,
and the referenced technology/structure types. `unitabilitytypes.xml` is empty;
it is not the source of these nested galactic special abilities.

Registry status below comes from [statuses.json](../../tag-coverage/statuses.json)
at the walk baseline. Scope deferral does not mean behavior is applied. This
docs-only walk changes no status to applied.

| Reader group / tags | Rules | Registry and implementation gap |
|---|---|---|
| `Activated_Corrupt_Planet_Ability_Names`, `Activated_Remove_Corruption_Ability_Names`, `Activated_Black_Market_Ability_Names`, `Activated_Siphon_Credits_Ability_Names`, `Activated_System_Spy_Ability_Names` | 01/03/18/37/39/41 | GameConstants `land-or-galactic`; GC command routing missing. |
| `Activated_Slice_Ability_Names`, `Activated_Sabotage_Ability_Names` | 27/32 | GameConstants `land-or-galactic` with `SCOPE-GALACTIC` evidence; GC routing missing. |
| `Activated_Neutralize_Hero_Ability_Names` | 20 | GameConstants `todo` (legacy EAWR-653); GC routing missing. |
| `Abilities/Corruption_Ability` name, `Initially_Enabled`, `Activation_Style`, `Causes_Despawn`, `SFXEvent_Activate` | 03..10 | GenericHeroUnit `todo` (legacy EAWR-760); generic galactic action lifecycle missing. |
| `Corruption_N_Type`, `Corruption_N_Credit_Cost`, `Corruption_N_Required_Unit_Type/Count`, `Corruption_N_Required_Mission_Type/Count`, `Corruption_N_Required_Planets`, mission mode/map/script, briefing and success/failure speech/hologram tags | 04..10 | Planet authored slots predominantly `land-or-galactic`; dynamically indexed reader names need loader/registry expansion where no exact row exists. |
| `Destroy_When_Used_As_Corruption_Prereq`, `Corruption_N_Success_Credit_Bonus`, `Corruption_N_Success_Tactical_Upgrade_Type`, `Corruption_N_Success_Unlock_Unit_Type`, `Automatic_Planetary_Corruption_Upgrade_Type` | 06/13..17 | HeroUnit/Planet/Faction `land-or-galactic` where present; absent exact authored rows are not proof of an ignored reader. |
| Removal nested name/style/enabled/despawn/audio; `Corruption_Removal_Cost_Multiplier`, `Corruption_Removal_Time_In_Secs`; `Planet_Corruption_Removal_Base_Cost` | 18/19 | HeroUnit/UniqueUnit common fields `todo` (legacy EAWR-760); cost/time overrides and Planet base `land-or-galactic`. |
| Neutralization `Cost_Mod_By_Base_Level`, `Cost_Mod_By_Previous_Neutralizations`, `General_Minor_Hero_Cost_Mod`, `General_Major_Hero_Cost_Mod`, `Can_Neutralize_Minor_Heroes`, `Can_Neutralize_Major_Heroes`, actor/target respawn ranges | 20..26 | GenericHeroUnit/HeroUnit `todo` (legacy EAWR-760); corruption cost curve `land-or-galactic`. |
| `Neutralization_Cost`, `Can_Be_Neutralized_By_Major_Heroes`, `Can_Be_Neutralized_By_Minor_Heroes`, `Planet_Auto_Capture_Minor_Heroes` | 22/25/34/39 | Class-specific deferrals/todos; targeting and auto-capture application missing for GC. |
| Sabotage nested flags, `Sabotage_Cost_Increase_Factor`, target `Sabotage_Cost_Credits`/`Tutorial_Sabotage_Cost_Credits`, `Sabotage_Particle_Effect` | 27..31 | Ability common fields and GenericHeroUnit `Abilities/Galactic_Sabotage_Ability/Sabotage_Cost_Increase_Factor` are `todo` (legacy EAWR-760); target cost rows scope-deferred; common reader rejects ordinary cost below one. |
| Slicer price/chance affinity, base/difficulty/availability curves, min/max respawn arrays; `Slice_Cost_Credits`, `Planet_Slice_Affinity_Types`, planet tech/difficulty ratings | 32..36 | HeroUnit affinity/base/timer fields `todo` (legacy EAWR-760); difficulty/availability curves `land-or-galactic`; type/planet rows require GC loaders. |
| Black-market `Price_Modifier`, `Tech_Level_Adjustment`, min/max respawn arrays; item faction/tech/cost/uncommon data | 37/38, WGEP-55/58 | HeroUnit common/timer fields `todo` (legacy EAWR-760), price/adjustment and BlackMarketItem `land-or-galactic`; existing economy gap owns purchases. |
| Siphon min/max percentage, duration cycles, min/max actor respawn times and common activation fields | 39/40 | GenericHeroUnit/HeroUnit `todo` (legacy EAWR-760) or `land-or-galactic` by row; deployment missing in addition to linked economy gap. |
| Spy `Duration_In_Secs`, `Causes_Despawn`, `Activation_Style`, `Initially_Enabled`, `See_*`; stealth `Evade_Detection_Chance` | 41/42 | Spy common/duration fields `todo` (legacy EAWR-760), visibility fields and galactic stealth scope-deferred; sensors owns visibility readers. |
| Named/generic hero flags, containing company/squadron types, `Respawn_Tied_To_Structure`, `Respawn_Whole_Team_When_Killed`, `Default_Hero_Respawn_Time`, `SFXEvent_Hero_Respawned` | 43..47, WGM-78..80 | Existing WGM registry inventory applies; no exact active structure-backed example proves stock enablement. Due-record service remains missing. |
| `Corruption_Choice_Income_Percentage`, faction corruption production factors and `Corruption_Hyperspace_Bonus` | Linked WGEP/WGM rules | `land-or-galactic`; reuse the existing economy/movement gaps. |
| `Planet_Retains_Residual_Influence`, `Build_Initially_Locked`, `Can_Be_Unlocked_By_Slicer`, `Piracy_Value` | 48/54/60 | Planet/type scope-deferred; GC eligibility, unlock discovery and departure accrual missing. |
| `Hack_Super_Weapon_Ability`, `Planet_Destruction_Ability`, `Political_Control_Protection_Ability` common nested lifecycle fields; `Activated_Hack_Super_Weapon_Ability_Names` | 57..59 | HeroUnit/SpaceUnit nested fields `todo` (legacy EAWR-760); GameConstants routing `todo` (legacy EAWR-653). Galactic action host and called-service effects missing. |
| Corruption particles, line growth/radius/offset, icon/encyclopedia/hologram/speech fields | Presentation interfaces | Scope-deferred; no simulation outcome is derived from their appearance. |

The old `Corrupt_Planet_Ability` example in the saboteur XML is commented out;
its handler is a legacy/mod interface, not a second stock corruption action.
Likewise the command list's Tani slicer and several old sabotage names do not
establish active ability definitions. No ignored-tag classification is changed
from that observation alone.

## Gaps, ownership and verification

Counts: **60 rule IDs; missing 60, same 0, differs 0** against our GC code.
Shared-note comparisons above are separate from implementation verdicts.
Added IDs appear beside their consumers in evaluation order; numerical order
records this walk's evidence additions.

| Gap group | Rules / integration | Ticket ownership and size |
|---|---|---|
| Corruption admission, prerequisites and mission actions | 01..10/48..50; new action host, AI engine, Lua bindings, save/replay | New gap (legacy EAWR-1140); **L, 24/48/80 h** |
| Corruption transitions, rewards, removal and revolt | 11..19/51; new planet state, economy_rules, production, movement/control, Lua | New gap (legacy EAWR-1141); **L, 20/40/72 h**; recurring economy uses the existing economy gap (legacy EAWR-1130) |
| Hero neutralization and sabotage | 20..31/52/53; action/identity/scoring, economy_rules, production, save/replay | New gap (legacy EAWR-1142); **L, 20/40/64 h** |
| Theft and special-unit deployment | 32..36/39..42/54..56; unlocks, action host, Lua, save/replay | New gap (legacy EAWR-1143); **L, 16/32/56 h**; spy visibility belongs to sensors |
| Black-market selection and timer details | 37/38/60; economy_rules, AI engine, save/replay | Reuse corruption/black-market economy (legacy EAWR-1130); incremental **S, 4/8/16 h** |
| Other galactic special-action interfaces | 57..59; new action host, planet lifecycle, production, political control, session victory | New gap (legacy EAWR-1144); **M, 12/24/40 h**; control uses the movement/control gap (legacy EAWR-1114), human acceptance and cleanup bounded by U-12 |
| Hero timer consumption and placement | 43..47; production, new persistent hero records, AI engine, Lua, save/replay | Reuse tactical return/hero scheduling (legacy EAWR-1116); incremental **M, 8/16/28 h** |

Tracking issue (legacy EAWR-1145) links five new phase-3 sub-issues and the reused
economy/return/control owners. None changes Phase 2 tactical code.

Highest-impact M3 gaps: the persistent corruption/action host; ordered
corruption rewards/removal/revolt; hero neutralization and scoring history;
technology theft/deployment; reliable due-hero placement and checkpoint state.
Implementation should stage independent evaluation on immutable inputs, commit
cross-object actions in deterministic order, and preserve 1/2/4/8-worker replay
parity. Persist corruption current/target/end state, type-usage counters,
sabotage counts, prepared option payloads/timers, actor/container identities,
unlock/upgrade ownership, active siphon records, neutralization history, due
spawn records and synchronized random state. The session/save walk owns the
complete checkpoint envelope.

| Unverified | Evidence task or capture that settles it | Ownership |
|---|---|---|
| U-01 | Trace planet service and special service registration order; stage due respawn, corruption end and hero-action command in one frame, including pause/tactical suspension. | GC scheduler / session |
| U-02 | Capture story/tutorial gate interaction and stale prepared corruption choices, especially actor/planet ownership change after the menu; alternative/count and mission/planet semantics are verified by WGCH-49. | Corruption action gap |
| U-03 | Read priority tie handling and capture equal prerequisite weights, repeated payload types, transports in transit and too few consumable candidates. | Corruption action gap |
| U-04 | Capture exact-frame transitions, pause/tactical suspension and interrupted ownership transitions. Setter conversion and zero-removal behavior are verified by WGCH-51; negative duration is a debug-invalid input. | Corruption state gap |
| U-05 | Trace candidate company/container enumeration, allied/stealth filtering and a vanished siphon. Capability flags are verified by WGCH-52; sensors owns revelation. | Neutralization gap |
| U-06 | Trace menu preparation's sabotage absence timer and tutorial entry route; capture two successive identical structures to confirm displayed/debited prices. Power operands are verified by WGCH-28. | Sabotage gap |
| U-07 | Legacy/mod-only sabotage base reduction, queue cancellation, income halt and cleanup expiry; inspect only if a mod fixture enables those flags. | Sabotage extension |
| U-08 | Trace the human menu's last-option advance-tech flag and compare actor tech/unlocks before and after acceptance. Registered-type filtering and AI flag are verified by WGCH-32/54. | Theft gap |
| U-09 | Black-market already-owned acceptance, zero-price AI weighting and prepared actor/planet ownership change; item tech lookup is verified separately by WGCH-38. | Economy gap (legacy EAWR-1130) |
| U-10 | Resolve equal-distance placement ties and company companion handling; capture failed placement retry, removed backing structures and loss of all planets. Forced-respawn matching is verified by WGCH-47. | Return gap (legacy EAWR-1116) |
| U-11 | Trace tactical bounty payment from corruption upgrades: attack ownership/final blow, category precedence and stacking. Piracy accrual is verified by WGCH-60. | Economy/tactical handoff |
| U-12 | Trace human hack acceptance and hackable-superweapon identity, destruction-to-ruins cleanup, protection source removal and generic despawn lifecycle. WGCH-57..59 establish their action boundaries. | Special-action boundary |

Capture requests are follow-up evidence work, not a claim that a retail session
was run or that M2 space behavior must change for this walk.
