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

The initial walk's WGCH rules were **missing in ours for GC** at its research
base. Later additions state their own comparison base. Existing tactical
abilities, production, visibility and hero types are integration points, not a
complete galactic action implementation. Tables name the proposed module for
each group.

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

## Frame schedule clarification

The [galactic frame schedule](../galactic-frame-service-schedule.md) narrows
U-01 with GFS-01/05/07/10/19: commands precede object work, corruption precedes
control inside the planet, object ability managers run at their own object turn,
and scheduled respawn follows ordinary objects before conflict/fiscal. There is
no global hero sweep. GFC-25/26 and GFSC-17 record hero-deadline initialization,
forced/retry/erase paths and corruption start/end/reset admission. WGM-80,
WGCH-43..47 and WGCH-51 retain their arithmetic and placement ownership.
Source-ability callbacks, interrupted transitions, placement ties and restore
still require GFS-U01/04/05/08 and this walk's U-04/U-10 probes.

Open-row follow-up: [GFS-23/GFC-31/GFD-28](../galactic-frame-service-schedule.md)
narrows U-01/U-04 alongside WGCH-11..13/51/61. Fresh transition state is
inactive with zero frames; completion commits/clears it before notifications,
effects and later control. GFSC-23/25 retains ownership interruption, exact
callback admission and restoration, and GFSC-17 retains hero-placement ties.
No runtime completion, source cleanup or placement equivalence is claimed.

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
| WGCH-38 | Black-market absence-time indexing subtracts **1** from the buyer's displayed tech level (WGEP-81), rather than the item's tech and accepts resulting indices **0..4**. Empty/invalid arrays return **0**, otherwise sample the per-index range. Stock Tyber/Urai/Silri ranges are fixed **90/30/60 s** respectively. Selection is prepared before execution; execution rechecks actor existence and current affordability. | debug build GCH-E08; XML data |

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

Initial research counts: **60 rule IDs; missing 60, same 0, differs 0** against
the original walk's GC comparison. Later evidence additions below have their
own comparison; these historical counts are not today's aggregate.
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
| U-01 | Capture-needed: WGCH-61 and GFS-23/GFC-31 settle outer/local corruption order and fresh/reset state; source-specific WGUI-101 callbacks are synchronous. Registration/listener qualification is GFSC-17/23/25. | GFD-28 and GFSC-17/23/25 own due respawn/corruption/action ties, source callbacks, pause/child suspension and restore; no independent scheduling capture. |
| U-02 | Capture-needed: WGCH-72 sources current-owner execution, debit-before-unit-recheck and the absent repeat of mission/required-planet/tutorial/opposing-owner preparation predicates. WGCH-49 sources preparation semantics. | Prepare a choice, then change actor owner, target owner, credits, unit count, required planet and usage count separately. Record whether ordinary UI still submits, payload, debit, unit consumption and transition. Include tutorial restriction and saved prepared payload; service-map C-04. |
| U-03 | Capture-needed: WGCH-62 sources container deduplication/count and weight-only ties. | Stage equal random weights, repeated payload types in one versus several transports, too few candidates and a candidate in transit without a planet. Record consumed container IDs/count; repeat ownership change after preparation and checkpoint RNG continuation. |
| U-04 | Capture-needed: GFC-C-01/02 adopts sourced fresh/reset state, truncation and zero-removal handling; WGCH-75 narrows completion sound precedence. Pause/tactical suspension and interrupted ownership outcomes remain unqualified. | Service-map C-01/02: end-1/end/end+1, nested pause, retained child, owner change and ruins; record committed type, reward/source cleanup, story/sound and first fiscal consumer. Negative duration remains a debug-invalid input, not an invented clamp. |
| U-05 | Capture-needed: WGCH-63 sources first-hero recursion, outer allied/detail/cost gates and vanished-siphon rejection. | Fog on: put allied, invisible or zero-cost hero first and a valid enemy hero later in one transport; reverse them and compare options. Remove or replace the active siphon after preparation; record payment, candidate identity and current siphon recipient. |
| U-06 | Capture-needed: WGCH-73/77 settles human menu preparation's **0 s** absence, tutorial submission gate and the separate direct-AI structure/accounting route; escalation power is WGCH-28. | Sabotage two identical ground then space specials successively; compare displayed prepared price, actual payment/accounting, history and actor default absence. Repeat tutorial-blocked click and direct AI action, with fog on for revealed/hidden one-shot gates. Existing sabotage owner (legacy EAWR-1142). |
| U-07 | Settled: Not applicable to stock FoC: mod-only flags; no stock trigger; out of scope per the FoC-only target. | Closed by coordinator scope decision; no mod fixture or stock runtime capture requested. |
| U-08 | Capture-needed: WGCH-64 establishes advance-tech as prepared list size exactly one. | Compare a one-option menu with first and last choices of a multi-option menu; record prepared advance flag, actor tech/unlocks, payment and acceptance, including stale ownership. |
| U-09 | Capture-needed: WGEP-80 settles ordinary owned-offer disablement and direct repeated-offer payment; WGEP-81/WGCH-71 settles tech operands. WGCH-74 sources purchase-helper current-owner accounting. Zero-price AI selection and ordinary stale-owner menu reachability remain qualification questions. | Compare UI-disabled owned offer with explicit repeated command; change buyer owner and planet allegiance/corruption after preparation, and stage an all-zero versus mixed-price AI offer set. Record recipient, debit, unlock and absence; service-map C-04. Existing economy owner (legacy EAWR-1130). |
| U-10 | Capture-needed: WGM-132..135 sources primary placement and authored company/squadron companions; WGCH-76 sources no-parent death scheduling and independent leader-victory checks. Equal-distance ties, retry/restoration and actual deletion callers remain unqualified. | Service-map C-03/05: due and ordinary-death/action-absence records, equal-distance insertion variants, capacity/blockade, source loss, secondary-only placement, backing-structure loss and no owned planets; compare persistent identity tree and checkpoint continuation. Existing return/placement owners (legacy EAWR-1116, EAWR-2486). |
| U-11 | Capture-needed: WGCH-65 sources final-blow admission, current-type value, maximum-within-category/sum-across-category and allied payout. | Use stock corruption upgrade battlefield bounty in tactical handoff, overlapping same/different categories and allied killers. Record victim current/original type, final-blow player, each recipient percentage, credits and fly text; repeat disabled collection and rounded sub-one payout. |
| U-12 | Capture-needed: WGCH-66..70 source prepared hack price/identity, ruins cleanup and distinct protection/Lua/ability removal; WGUI-101 sources termination qualification. | Prepare hack then remove/change weapon, hacker owner or target owner; verify missing weapon fails before payment and missing weapon owner after it, and record ruins/orbit/starbase/protection cleanup. Compare ordinary ability despawn, source death and finite consumed probe with distinct cleanup/respawn outcomes. |

Capture requests are follow-up evidence work, not a claim that a retail session
was run or that M2 space behavior must change for this walk.

## Unverified sweep: sourced clarifications

The [consolidated sweep](gc-unverified-sweep.md) records whole-item dispositions and residual questions. A clarification below closes only the stated branch; it does not turn an entire mixed question into verified behavior. Opaque GUV evidence remains in private research storage.

| ID | Rule | Source |
|---|---|---|
| WGCH-61 | The inspected outer scheduler executes synchronized commands before ordinary object service, then the separately admitted scheduled-planet-creation pass, conflict work and fiscal processing (WGM-89/94). A scheduled hero creation is not an extra player-service phase. These reads do not establish the relative registration order of ordinary special abilities, corruption expiry and every hero-action callback; U-01 remains open for those branches. | debug build GUV-01/08 |

## Wave 6: candidate and action boundaries

| ID | Rule | Source |
| --- | --- | --- |
| WGCH-62 | Prerequisite consumption gathers matching owned candidates with a current planetary system, so an object without a planet while in transit does not enter this list. A contained candidate collapses to its immediate transport and deduplicates that container identity. Each selected container consumes **one** requested count regardless of repeated payload types or member count; consumption stops when the requested count reaches zero or the candidate heap empties. The heap compares only the floating random weight, without an object/type secondary key. Equal-weight order is not an authored stable-ID policy and remains a capture check. | debug build G6-C01; WGCH-07 |
| WGCH-63 | Neutralization recursively breaks down fleets and transport contents, but returns after the **first** hero found within a transport; outer filtering then rejects allied, hidden-detail or nonpositive-cost candidates. A first rejected hero can therefore prevent a later hero in that same transport becoming an option. A candidate's immediate parent transport supplies company type, otherwise use the hero's original type. Prepared execution rechecks actor, planet, affordability and current siphon activity; a vanished siphon fails before payment. Active siphon identity replacement is not a prepared-identity equality guard in the inspected branch. | debug build G6-C02; WGCH-35/36 |
| WGCH-64 | Human technology-theft submission requires an active menu and valid option index. It copies the prepared choice, actor, location and absence timer, and sets advance-tech exactly when the **whole prepared option list has size 1**, independently of the clicked option's index. It queues the request, emits the tutorial slice action and closes the menu; execution remains WGCH-34. | debug build G6-C03; WGCH-32/34/54 |
| WGCH-65 | Tactical bounty value uses the victim's current type and the active single-player/multiplayer override. A nonnegative override wins, including zero; a negative override takes the **maximum** applicable category default and clamps the result nonnegative. Battlefield percentages take the greatest modifier within each category and **sum** category winners, subject to enabled ledger caps. Payment requires a final-blow player different from and nonallied to the victim owner. Every player allied to that killer can receive its own positive battlefield percentage for the victim faction: nearest-integer payout has a minimum **1**, then adds credits and collected bounty, with local fly text. This is distinct from WAR-60's galactic corruption bounty condition. Ability insertion/removal uses independent source/category identities and allied recipients; do not multiply overlapping category percentages. | debug build G6-C04; WAR-60 |
| WGCH-66 | Human hack submission reads the current authored price and queues hacker/planet IDs plus that prepared price. Execution resolves those IDs. The helper takes the **first exact current type** matching `Hack_Super_Weapon_Required_Type` from stored orbit/breakdown traversal, without an owner filter in that search, then rechecks affordability. It pays before resolving the weapon owner; a missing owner fails after payment without a refund in this path. Success disables building that current weapon type for its owner, destroys the hacker without respawn, and performs ruins cleanup. It also destroys orbit fleets, clears starbase and forces Neutral; those orbital operations are hack-specific additions to WGCH-67. | debug build G6-C05; WGCH-57 |
| WGCH-67 | Shared destruction-to-ruins destroys **all** landed transports irrespective of faction and clears ground special structures. It terminates credit siphoning and clears political modifiers regardless of the destroy flag. With that flag it also zeros land capacity, sets the destroyed flag and notifies destroyed-planet story state. It aborts an active corruption transition, sets no corruption, refreshes visuals and dispatches old-corruption cleanup; in galactic mode it terminates planet abilities and cancels their service. That shared helper does not itself destroy orbit fleets or clear starbase. In hacking, a preceding corruption-removal transition is therefore immediately aborted by this cleanup rather than completing as a later timer. | debug build G6-C06; WGCH-58; WGP-08 |
| WGCH-68 | Political-protection removal tests appropriateness and planet state, removes that source identity from the boolean modifier list and deregisters its target. An external source's planet-owner-change handler removes for the old source-owner relation or missing old owner, and applies when the new owner is the source owner; the planet-as-source branch returns false. Source destruction and generic target termination remain separate lifecycle routes, not an unconditional global protection reset. | debug build G6-C06; WGCH-59 |
| WGCH-69 | Lua `Despawn` on a dead/unresolved wrapper warns and returns no value. For teams it visits members in reverse, stops scoring listeners for the member and its linked parent, and destroys them with the destruction flag; ordinary objects follow the corresponding listener/destruction path. This is the Lua interface, not a claim that all special abilities use that route. | debug build G6-C06; WGUI-101 |
| WGCH-70 | Generic ability activation validates its actor/type, enabled service and ability, then invokes the ability. When successful activation requests actor consumption, the manager first schedules hero respawn using that ability's absence-time getter and no explicit target, then destroys the hero; it returns the activation result and reports consumption. This route is separate from Lua `Despawn` and does not imply successful generic target cleanup for a consumed probe (WGUI-101). | debug build G6-C07; WGM-78..80; WGUI-101 |
| WGCH-71 | The inspected black-market absence getter indexes by **buyer displayed tech minus 1**; item technology lookup is separate and reads ordinary/tutorial item data. This corrects WGCH-38's earlier item-tech operand. Stock fixed ranges remain unchanged. | debug build G6-E04; WGEP-81; WGCH-38 |

## S5 preparation: execution and lifecycle consumers

Research dated 2026-10-09. The
[corruption/hero service map](../gc-corruption-frame-schedule.md) adopts the
galactic frame schedule and supplies action, pause, interruption, placement and
presentation qualification recipes. Opaque S5C evidence records are private
debug-build reads; no retail capture is claimed. Admission, cost, absence,
transition time and fiscal time must remain separate inputs.

| ID | Rule | Source |
| --- | --- | --- |
| WGCH-72 | Prepared corruption execution uses the actor's **current owner**, current money and current planet transition/corruption state. It debits positive prepared credits before rechecking the unit requirement. The inspected executor does not repeat the preparation's mission-use or required-planet predicates, opposing-owner eligibility, tutorial command gate or choice enumeration. This bounds execution, not ordinary UI reachability of a stale prepared choice. A failed unit recheck leaves the earlier debit; a failed tactical queue can follow consumption. | debug build S5C-01; WGCH-05..09/49 |
| WGCH-73 | Sabotage menu activation stores the supplied actor ID, absence seconds and planet only when no other dialog is active and its shell is hidden. It emits the open-menu tutorial event and refreshes offers. Submission requires an active sabotage menu and tutorial sabotage enabled; it takes target identity and prepared price from the selected component, queues those with the stored actor/absence/planet and closes the menu. This sender does not debit or destroy the actor. A blocked tutorial submission does not take its close-menu branch. Absence-time production is separate from this stored payload. | debug build S5C-02; WGCH-28..30 |
| WGCH-74 | The black-market purchase helper re-resolves the buyer and reads that object's **current owner** for affordability, debit and unlock recipient. Its item/price/absence remain prepared inputs; the planet is passed to actor removal, not an owner-equality check in this helper. It does not repeat the already-owned offer test. Missing buyer or insufficient money fails before debit. This does not establish that a stale-owner purchase is an ordinary reachable menu interaction; the command wrapper and UI remain distinct boundaries. | debug build S5C-03; WGEP-55/80/81 |
| WGCH-75 | Corruption completion emits increased/decreased story state using the committed type, then its local sound. When the planet owner is local, use that owner's faction event; otherwise, if the corruption-benefiting player is local, use that player's faction event. This is a precedence choice, not two sounds for one completion. Absent faction event starts nothing. Ordered effects then run WGCH-13; direct action success hologram/speech in WGCH-09 is an earlier interface, independent of the completion sound. | debug build S5C-04; WGCH-12/13; WGP-27 |
| WGCH-76 | Hero defeat reports story defeat and health-zero before requesting the ordinary respawn. A tactical child can supply its parent conflict planet; with no parent the supplied planet stays absent and scheduling uses WGM-80's current-system/first-owned fallback. The caller does not require scheduling success before testing faction-leader victories for enemy players in the active or parent galaxy. This hook does not itself establish every galactic deletion caller: neutralization, ordinary death, consumed actions, hack and Lua despawn have separate attribution/scheduling interfaces. | debug build S5C-05; WGM-78..80; WGCH-26/66/69/70 |
| WGCH-77 | The stock special-structure sabotage route requires a corrupted planet. For the local human route it opens the menu with absence time **0 s**, which is passed to scheduling on successful ordinary submission. Eligibility still follows WGM-79; an eligible respawn uses the default-time fallback, while an ordinary unnamed actor has no respawn record. Zero does not establish an instant return. The direct AI route scans ground then space specials, admitting live/non-deletion-pending affordable targets, and samples uniformly over collected entries with synchronized integer randomness. On successful removal it increments sabotage history and posts the negative selected cost to this-frame credit-income accounting; it does not use the prepared-command helper's immediate debit/actor-removal branch. Revealed-planet admission gates its configured one-shot and faction success sound. Generic successful ability consumption is a later action-host interface (WGCH-70). | debug build S5C-06; WGCH-28..31/73; WGP-50 |
| WGCH-78 | Black-market command execution first resolves a planetary target, then calls WGCH-74's buyer helper without rechecking that planet's current corruption/allegiance or the prepared buyer's original owner. AI preparation inserts affordable nonnull offers with **prepared price as weight**, including zero, then attempts sampling when the distribution has any entries; no positive-price filter occurs at this call site. The all-zero distribution's sampled result is not established by that caller alone and remains U-09. | debug build S5C-07; WGCH-37/74; WGEP-80 |
| WGCH-79 | In land-invasion resolution's **no remaining land conflict** branch, surviving invaders and distinct invading/defending players admit the hero evacuation helper with no excluded valid owner. It visits landed transports, skips null/dead or an exact excluded-owner match, and selects a transport when it is stealth **or** reports a noncombat hero. For each selected transport it creates a fleet owned by that transport's owner, unlinks the same transport from the surface, inserts it in the fleet and links the fleet into this planet's orbit. This is relocation with preserved transport/payload identity, not hero death or a respawn. The helper uses forward list indices while unlinking; consecutive matching entries and callbacks need runtime qualification rather than a presumed remove-all policy. Arrival/raid/instant-land admission remains WGM-14/34, and fresh respawn placement remains WGM-132..135. | debug build S5C-08; service-map C-05 |

### Action-to-service and presentation map

| Action | Admission and cost | Timing/outcome | Presentation consumer |
| --- | --- | --- | --- |
| Bribery corruption choice | WGCH-01..07/48..50/72: authored planet slot/prerequisites/credits | Direct/mission increase uses 5 s; completion rewards/effects WGCH-12..16. Paid traversal is separate WGM-84/85, including intermediate-planet cost and owner recipients. | Attached corruption particle/lines WGP-19..21; completion local sound WGCH-75. Bribe price feedback belongs to movement/HUD. |
| Special-structure sabotage | WGCH-27..31/53/73: exact structure identity, rounded cost × factor^history | Remove first, then payment/absence/destruction/history; free/script path remains separate | Sabotage story, faction sound and configured WGP-22 one-shot; no corruption type mutation inferred from that effect. |
| Black market | WGCH-37/38/55/71/74; WGEP-58/80: eligible black-market planet, prepared offer and price | Buy/unlock then actor absence; buyer displayed-tech arrays, no build queue | Local new-options, slice-success and production-complete feedback; existing corruption visuals, no new transition from buying. |
| Superweapon hack | WGCH-57/66/109..112: exact current weapon lookup; human prepared price/current affordability, direct AI payment split | Lock weapon build type, consume hacker without respawn, ruins cleanup, destroy orbit/clear starbase/neutralize | Human helper's faction hack-success sound and configured attached one-shot; both routes refresh the model (WGCH-111). |
| Neutralize / paid bounty hunter | WGCH-20..26/52/63: capability/visible candidate and prepared cost/two absence times | Target attribution/scoring/schedule/destruction or active siphon termination; then debit/hooks and actor absence | Local action feedback and target/scoring notifications. Tactical corruption bounty WGCH-65 is a different final-blow payout; it is not this paid action. |
| Theft / siphon / spy | WGCH-32..42/54..56/64: opposing-target, technology or source-effect gates | Theft seeded success/failure; siphon fiscal duration; spy visibility duration and consumption | Action sounds/arrival/source visibility notifications; no automatic WGP-22 explosion. |
| Planet destruction / protection | WGCH-58/59/67/68: contested-space/leader or source-target protection gates | Ruins and victory interface / source-keyed protection apply/remove | Destruction target effect, model refresh / effect notifications; control and source cleanup remain owning interfaces. |

### S5 gap comparison

Against `d7180d985ebf9b85c421ef3dc9d6a09234652296`, the added action/lifecycle
consumers are **missing** in `src/sim/galactic/session.cpp` and `commands.cpp`;
existing movement/observer corruption booleans, income arithmetic and
`src/campaign/setup.cpp` starting-hero providers are partial interfaces, not
these services. The hero death/return owner and due placement owner remain
distinct (legacy EAWR-1116, EAWR-2486). Existing implementation tickets retain the
admission, transition, economy, neutralization, deployment and special-action
work (legacy EAWR-1140, EAWR-1141, EAWR-1130, EAWR-1142, EAWR-1143, EAWR-1144).

The new service map's qualification cases share frame-service ownership;
they do not claim an observed callback/restore order or add serial hero sweeps.
New rule counts: **WGCH-72..79: 8 missing, 0 same, 0 differs** at the stated
comparison base. There are **79 distinct WGCH rule IDs** across all research
waves. The twelve sweep items remain **1 settled, 11 capture-needed**; this
research narrows their static branches without claiming their runtime probes.

Tracking: S5 corruption/heroes service-map qualification (legacy EAWR-2501), under the full-vanilla S5 epic (legacy EAWR-1968). Prepared-action qualification (legacy EAWR-2508) owns U-02/U-03/U-05/U-06/U-08/U-09; lifecycle qualification (legacy EAWR-2509) owns U-01/U-04/U-10/U-11/U-12. Existing implementation ownership above is retained. Sizes are respectively **M, 2/4/8 h** and **M, 3/6/12 h**, optimistic/likely/pessimistic agent-hours.

## Action-boundary completion

Research dated 2026-10-10, against
`a98b33b8915066cf36cc4c562c59a042a2752318`. The following rows supplement the
earlier admission, formulas and outcomes; they do not repeat those rules as
new discoveries. H1145 evidence IDs identify private read-only debug-build
records and archived effective XML/Lua inputs. No retail run is claimed.
The baseline contains **79 distinct rows**; IDs 80..90 are absent at that base.
This addition uses its reserved **91..120** range and leaves 121..130 unused.

Commands, local menus, direct AI actions and Lua plan results are distinct
interfaces. In particular, a menu-opening handler's false result is not a
failed purchase, a plan's success is not necessarily successful deployment,
and an actor absence timer is not the fiscal lifetime of its deployed effect.
The [galactic frame schedule](../galactic-frame-service-schedule.md) and
[corruption/hero service map](../gc-corruption-frame-schedule.md) continue to
own service order, pause, callbacks and restoration.

### Neutralization and escalating structure sabotage

| ID | Rule and conditions | Source |
| --- | --- | --- |
| WGCH-91 | Prepared neutralization command execution first resolves the stored planet and requires planetary behavior. An absent or nonplanetary target stops before the action helper. This is distinct from resolving the victim hero or active siphon inside WGCH-24/63. | debug build H1145-E01 |
| WGCH-92 | Neutralization execution reads the actor's **current owner** for affordability and payment and calculates price from the **prepared modifier ledger**, not a fresh planet/history/protection enumeration. It tests the prepared validity flag and re-resolves the victim ID, but does not repeat hostility, visibility, capability or victim-location predicates. Current victim type/owner, or current siphon type/recipient, supply execution attribution and feedback. This bounds explicit command execution; ordinary stale-menu reachability remains U-05. | debug build H1145-E02; WGCH-22..26/63 |
| WGCH-93 | Neutralization prepares actor and target absence samples for **each resolvable enumerated option with valid type/company metadata** before the AI valid/affordable filter. Thus disabled or unaffordable options can still take their authored random samples. Nonpositive endpoints use the default instead of drawing; actor and target samples are separate. The human menu-opening branch and a remote non-AI branch return false without executing; AI returns the chosen action's result or false when no admissible option exists. | debug build H1145-E03/E20; WGCH-21/24 |
| WGCH-94 | Immediately before successful neutralization mutation, the actor's final-blow attribution is cleared. Ordinary victim notification is sent from its immediate transport when present, otherwise from the victim object, with the neutralizer as event data. Stock `Galactic_Neutralized_Event` increments a table keyed by **killer's current player ID and supplied type**; `Get_Neutralized_Count_For_Type` returns zero for an absent player/type entry. Siphon targeting explicitly supplies the prepared company type. The price history reader uses company type and attacking player, independently of victim respawn ownership. | debug build H1145-E02/E04; Lua data H1145-D02; WGCH-23/26 |
| WGCH-95 | Shared action hero removal destroys the hero and then its **immediate parent transport**, when that parent is a transport. It does not schedule respawn itself or explicitly destroy an enclosing fleet. Neutralization target/actor removal, ordinary sabotage, theft and hacking use this helper; the generic auto-capture branches can call direct actor destruction instead. Companion cleanup inside transport destruction and callback ordering remain lifecycle qualification, not a claim that every companion receives a respawn record. | debug build H1145-E05; WGCH-25/26/30/34/66 |
| WGCH-96 | Prepared sabotage resolves planet and structure IDs, then invokes the ordinary helper with the stored actor ID, price and absence. The helper reads the actor's current owner for affordability, attribution and payment. It does **not** recalculate escalation, require current corruption, repeat opposing-planet admission, or require the structure owner to match the prepared owner. Exact membership in this planet's ground/space special lists remains the execution identity check. | debug build H1145-E06; WGCH-29/73 |
| WGCH-97 | Sabotage menu enumeration is ground specials followed by space specials, compacting away types with cost below **1**. In sabotage-tutorial mode the menu requests `Tutorial_Sabotage_Cost_Credits` and uses that price **directly**, without the ordinary escalation calculator. Otherwise it requests ordinary cost and recalculates WGCH-28. The ordinary calculator finds the first actor ability matching `Activated_Sabotage_Ability_Names` in authored list order. The clicked component stores target identity and this displayed price for WGCH-73. | debug build H1145-E07; XML data H1145-D01 |
| WGCH-98 | Sabotage menu affordability compares displayed price with integer conversion of **current local credits + 0.5**. Execution compares the prepared price with the actor owner's actual current floating credits. At a fractional balance, an enabled menu button therefore does not guarantee helper acceptance. An affordable escalated ordinary price above the unscaled cost is displayed red; insufficient funds gray/disable the offer. | debug build H1145-E07/E06 |
| WGCH-99 | When sabotage finds the exact structure, it attempts removal and then reevaluates production for that layer **even if removal returned false**. Ground membership suppresses the later space search. Only successful removal enters story/payment/actor-removal/effect/history mutation. Missing membership enters neither reevaluation nor success mutation. The explicit free/script helper still requires nonnegative supplied price, but skips affordability and actor lookup/payment. | debug build H1145-E06; WGCH-29/30 |

### Technology theft, siphon deployment and spy invocation

| ID | Rule and conditions | Source |
| --- | --- | --- |
| WGCH-100 | Prepared theft requires a resolved planetary command target, resolved actor and nonnull prepared technology. Affordability, debit and unlock recipient use the actor's **current owner**. Price, chance, advance flag and absence remain prepared values. Execution does not re-enumerate sliceable types, repeat actor-faction/lock/tech filters or opposing-playable-planet admission, or reject an already-unlocked type at this boundary. U-08 retains ordinary stale-menu qualification. | debug build H1145-E08; WGCH-34/54/64 |
| WGCH-101 | WGCH-32's empty-list tech increment/retry is a branch inside the application handler, **after** its appropriateness gate. That gate itself requires a nonempty sliceable list. With unchanged inputs, an initially empty list is rejected before reaching increment/retry; this branch is not evidence that a normal stock click grants free tech advancement. Where reached, the branch increments player tech before recursive application and contains no rollback on a false recursive result. | debug build H1145-E09; WGCH-32/55 |
| WGCH-102 | Theft's ordinary success and all three failure outcomes invoke actor scheduling with the parent-check bypass and then shared hero removal, even when the result is false. Generic auto-capture invokes that scheduling/removal path with **0 s**, whose fallback remains WGM-80. A local human menu-opening branch returns false while leaving the actor present. AI returns the selected theft result; no affordable choice returns false before theft/removal. Do not equate all false results with the same actor lifecycle. | debug build H1145-E08/E09/E10; WGCH-34..36/95 |
| WGCH-103 | Siphon application repeats the opposing-control check after appropriateness, then checks planet data, generic minor-siphon prevention and an existing active siphon. These refusals occur before percentage sampling. For ground-activated application, generic auto-capture also precedes sampling; otherwise it emits hero-land story state before requesting deployment. There is no credit affordability test or action debit in this handler. Actor construction cost and later fiscal transfers are separate. | debug build H1145-E11; WGCH-39/55; WGEP-12 |
| WGCH-104 | The planetary siphon creation helper additionally requires a **nonnull source with an immediate parent transport**, that transport's original company type, planet data and no active siphon. Missing transport/company returns false before installing the record. The invoked ability registers its target after the creation attempt even when the helper returns false, and emits its effects only on success. An arbitrary uncontained source is not sufficient merely because it passed opposing-planet admission. | debug build H1145-E11/E12 |
| WGCH-105 | Siphon creation stores the source's current player ID, sampled percentage, **current galactic frame + authored cycles × current fiscal-cycle duration in frames**, accumulated siphoned credits **0**, discovery flag **false**, source current type and transport original company type. These copied identities outlive actor removal and support later discovery/neutralization. Strategic mode supplies the fiscal duration; the helper asserts for other modes rather than establishing a tactical deployment policy. Expiry/payment arithmetic stays WGEP-12. | debug build H1145-E12; WGCH-20/40 |
| WGCH-106 | Siphon ability removal first repeats its appropriateness gate and requires planet behavior. It terminates the active siphon only when the stored recipient player equals the **source's current owner**. After a valid planetary lookup it deregisters the target even on recipient mismatch and returns whether the owners matched. It compares player identity, not source object/company identity; source ownership changes and replacement effects therefore need lifecycle qualification. | debug build H1145-E13; WGCH-39 |
| WGCH-107 | Successful siphon deployment returns true to the generic action host; `Causes_Despawn` then controls WGCH-70's scheduling/consumption, rather than the fiscal duration controlling hero return. Stock Han and generic smugglers author `Causes_Despawn = Yes`; the generic actor's lack of a named-hero respawn record is WGM-79. Failed deployment returns false and does not authorize that success-only consumption hook. | debug build H1145-E11; XML data H1145-D01; WGCH-40/70 |
| WGCH-108 | Spy invocation validates appropriateness, then affects the actor's **current owner**, requiring a valid player and planetary target data. It attempts one source-keyed visibility modifier using the authored mask and `Duration_In_Secs`. Successful insertion refreshes target visibility for every extant player, emits hero-land story state only for ground activation, and emits effects. Target registration follows the insertion attempt even when insertion fails. This handler does not debit credits, sample success chance or destroy the actor; success-only consumption is WGCH-70. Mask/expiry/cleanup remain WGUI-19..24/101. | debug build H1145-E14; WGCH-41/56 |

### Hacking and planet destruction

| ID | Rule and conditions | Source |
| --- | --- | --- |
| WGCH-109 | Hack appropriateness requires actor, planetary target and WGCH-66's first exact-current-type orbit match. It does **not** apply the common opposing-planet predicate, check weapon-owner hostility, reject ruins or test credits at this gate. A missing `Hack_Super_Weapon_Required_Type` gives no match. Stock requires `Death_Star_II`; neither a similarly named type nor the original Death Star qualifies merely by being a superweapon. | debug build H1145-E15; XML data H1145-D01 |
| WGCH-110 | The direct AI hack route checks weapon ownership but has **no affordability check and no credit debit**. The human prepared-command helper instead charges its prepared price to the hacker's current owner before checking weapon ownership, as WGCH-66 describes. Stock `Hack_Super_Weapon_Cost` is **50000 credits**; it is not an unavoidable payment for every hack route. Both successful routes lock the matched weapon's current type for its current owner and remove the hacker without explicitly scheduling return. | debug build H1145-E16/E17; XML data H1145-D01 |
| WGCH-111 | The human hack helper explicitly emits faction hack-success sound after the weapon build lock and before hacker removal; it attaches the configured `Hack_Super_Weapon_Particle_Effect` after the planet/orbit/ownership mutations. The direct AI destructive helper contains neither of those explicit emissions. Both request a **5 s** corruption removal when corrupted, immediately followed by WGCH-67 ruins cleanup; both destroy orbit fleets and clear starbase. A forced Neutral ownership transition happens only when the named Neutral faction **and its player** resolve. Those lookup failures do not undo the earlier mutations or make the helper report failure. Common activation effects are a separate host interface. | debug build H1145-E16/E17; WGCH-57/66/67 |
| WGCH-112 | Human hack menu refresh enables acceptance by comparing current **local-player** credits with the current authored hack cost. Prepared execution re-resolves hacker/planet and a current weapon match; it does not carry a prepared weapon ID, repeat the menu's local-player identity or recalculate price. Thus a replacement exact-type weapon can satisfy the later lookup. Missing weapon or insufficient current actor funds fails before debit; missing weapon owner remains WGCH-66's after-debit failure. Ordinary reachability of these stale states remains U-12. | debug build H1145-E17; WGCH-66 |
| WGCH-113 | Planet-destruction appropriateness requires actor, planetary target and planet data and rejects an already destroyed planet. When target allegiance has a valid player ID, reject an actor-owner ally of that **target allegiance** player. Invalid target allegiance does not fail this gate. Current allegiance, planet object owner, corruption state and playable faction are not substituted for that target-allegiance test. The contested-orbit refusal remains WGCH-58's later execution check. | debug build H1145-E18; WGCH-58 |
| WGCH-114 | The faction-leader destruction test is admitted only in the active galactic mode with the attacking player's leader-kill victory condition enabled. It reads **current planet allegiance** to resolve faction/leader, then searches landed transport contents for exact **current type** equality. The inspected loop does not additionally compare each transport/payload owner with the controller or search orbit for the leader. Missing controller, faction or leader skips the pending-victory request while destruction proceeds. The pending request's **30 logical frames** is not a destruction cooldown. | debug build H1145-E19; WGCH-58 |
| WGCH-115 | Planet destruction does not debit action credits, schedule/remove the actor, lock a weapon build type or explicitly destroy orbit/clear starbase/force Neutral in this handler. It requests shared ruins cleanup, refreshes the model, emits authored effects and returns true. Hacking adds its own orbital/ownership operations (WGCH-111); those must not be attributed to every ruins request. Tactical super-laser firing, attack recharge and later session victory resolution remain other interfaces. | debug build H1145-E19; WGCH-58/67 |

### Stock AI/Lua action interfaces

These rows describe the authored plan bodies, not observed execution or a
promise that every campaign enables the plans. Goal/equation/template admission,
hero plan attachment and target scoring remain WGA-36/44/45. WGM-14/34 owns
landing/arrival dispatch, so a plan's landing call does not replace an ability
handler's admission or outcomes.

| ID | Rule and conditions | Source |
| --- | --- | --- |
| WGCH-116 | The Rebel technology plan requests one `Droids_Team`, prevents ordinary goal-system removal, and finds an enemy `Is_Good_Slice_Target` with cutoff **0.1**. No target exits. It blocks on production, movement and `LandUnits`, then marks plan success and exits; production failure exits. This plan body has no explicit `Activate_Ability` call, no theft-price payment and no inspection of WGCH-34's theft result. | Lua data H1145-D03; WGA-44 |
| WGCH-117 | The siphon plan requests one Empire/Rebel smuggler team with hero attachment denied. `Suitable_Smuggler_Target` below **0.5** exits. It sleeps **1 s**, assembles, sleeps **1 s**, blocks on movement, marks success **before** its nonblocking `LandUnits` call, then exits; no remaining units exits. The script success flag is therefore not evidence that the planetary siphon record was installed. The deployment helper's actor/container requirements remain WGCH-103..105. | Lua data H1145-D04 |
| WGCH-118 | The spy plan requests one probe team with hero attachment denied, sleeps **1 s**, assembles, sleeps **1 s**, blocks on movement, invokes nonblocking `LandUnits`, then marks success and exits. Production failure or no remaining units exits. It does not wait for visibility expiry, charge an action price or explicitly call `Activate_Ability`; WGUI's revelation result is separate from plan success. The Underworld default disables this plan (WGA-45). | Lua data H1145-D05; WGCH-41/108 |
| WGCH-119 | The hacking plan requests one `IG88_Team` with hero attachment denied, blocks on production, movement and `LandUnits`, then marks success; production failure exits. It contains no explicit activation call, price/credit comparison or action-result test. This authored AI interface feeds the landing route and does not turn the human **50000-credit** menu into AI plan admission. Goal/equation admission remains the AI walk. | Lua data H1145-D06; WGCH-110 |
| WGCH-120 | The shared corruption/sabotage deployment plan assembles one saboteur, blocks on movement, calls `Activate_Ability` and sleeps **10 s**. It marks success only for its corruption goal when the target reports corrupted; it deliberately leaves sabotage without a success result to discourage repetition at escalating prices. This is the same WGA-45 plan policy, not a refund, a ten-second actor absence or a requirement that the structure-removal helper return false. | Lua data H1145-D07; WGA-45; WGCH-77 |

### Input coverage and comparison

| Input group | Consumer and treatment |
| --- | --- |
| Neutralization routing, major/minor flags, target permissions/cost, three cost curves, category protection and actor/target absence ranges | Existing WGCH-20..26/52/63 formulas plus WGCH-91..95 prepared/current identity and removal boundaries. Stock Lua scoring is the history source; tactical bounty is WGCH-65, not this paid action. |
| Sabotage routing, special-structure flag, escalation factor and ordinary/tutorial structure prices | WGCH-27..31/73/77 plus WGCH-96..99. Stock factor **2** remains authored. Other legacy activity flags remain stock-inapplicable U-07; this addition does not enable them. |
| Theft routing, lock/faction/tech fields, affinity/price/chance curves and per-tech absence arrays | WGCH-32..36/54/55/64 plus WGCH-100..102. Price/chance are prepared; ordinary empty-list admission is clarified. No research-only parse is marked applied. |
| Siphon routing, min/max percentage, fiscal cycles, actor absence, activation and consumption fields | WGCH-39/40/70 plus WGCH-103..107. Deployment metadata and fiscal collection remain distinct. Prevention and auto-capture are application gates. |
| Spy routing, mask flags, duration, activation and consumption | WGCH-41/56/70/108 and WGUI-19..32/101. This walk invokes the sensor service; it does not reimplement visibility/expiry or infer that consumed actors clean all effects. |
| Hack routing, required exact type, cost, hack success/effect fields; planet-destruction activation/target effects | WGCH-109..115; stock IG-88 hack authors ground activation and `Causes_Despawn = No`, while its handler removes the actor itself. Stock planet destruction authors ground activation and target effects. The Death Star II's separate tactical `Super_Laser_Ability` is outside this galactic action boundary. |
| Stock goal/category, force type, perception and Lua calls | WGCH-116..120 and WGA-36/44/45. Action result, plan result and template admission remain separate; no AI sweep or service cadence is added. |

Registry dispositions remain the reader-group inventory above: relevant nested
hero/common fields are `todo` or `land-or-galactic`, and the hack cost/required
type are `land-or-galactic`. This docs-only addition changes no tag status to
`applied`; existing action owners retain application/loader work.

At the stated base, `src/sim/galactic/commands.cpp` and
`include/eawr/sim/galactic/commands.hpp` have no prepared hero-action payloads;
`src/sim/galactic/session.cpp` and
`src/script/authoritative/galactic_bridge.cpp` lack these action handlers and
stock plan-result interfaces. `src/sim/galactic/corruption_model.cpp` has fiscal
siphon arithmetic, and `src/sim/galactic/production.cpp` has production/unlock
inputs; neither implements deployment/actor removal or theft submission.
These are integration boundaries, not a claim that all GC economy is absent.

The **30 added rules are missing 30, same 0, differs 0** in that implementation
scope. They bring this file to **109 distinct IDs**, without inventing 80..90.
Against existing behavior notes, the formulas and AI policies are **same** as
the cited rows; prepared execution/cleanup constraints are **missing there**;
WGCH-101 **narrows** WGCH-32's empty-list retry claim, and WGCH-110/111
**distinguish** direct AI hack cost/presentation from the human helper.

Existing owners retain neutralization/sabotage (legacy EAWR-1142),
theft/deployment/spy invocation (legacy EAWR-1143) and hacking/destruction
(legacy EAWR-1144), under the corruption/hero tracker (legacy EAWR-1145).
No duplicate implementation tickets are created. The highest-impact additions
are prepared/current-owner execution, transport-aware actor removal, source
metadata for siphon deployment, the AI/human hack payment split, and the
separate target/current allegiance operands for planet destruction.

### Qualification remaining

Static boundaries are settled here; the earlier twelve-item sweep remains
**1 settled, 11 capture-needed**. U-05/U-06/U-08/U-12 still need ordinary UI
reachability and observed outcomes for changed owners/types, replacement
siphons/weapons, fractional affordability and tutorial versus ordinary prices.
Add companion-bearing transports and post-action checkpoint continuation to
the existing lifecycle qualification (legacy EAWR-2509); do not presume callback
order or companion respawn from WGCH-95 alone. U-01/U-04/U-10 and pause/restore
qualification remain with the service maps. AI plan completion versus actual
landing/deployment needs a trace that records both, not a screenshot of the
plan's success flag. No new retail capture or cadence measurement is claimed.

## Corruption action implementation readiness

Research dated 2026-10-10 against
`9a34672b97f17381fc2574379b180361ddf5d152`. The admission, prerequisite,
mission, transition, theft/deployment and destruction issues were audited
against this walk and [economy/production](gc-economy-production.md).
Most action boundaries were already sourced; the following rows close the
remaining debug-readable corruption menu, mission-request and reward-provider
gaps. Opaque CA evidence IDs refer to private read-only debug-build records
and effective XML/Lua inputs. No retail capture is claimed. These ten rows
use the next free IDs **121..130**, bringing the walk to **119 distinct IDs**;
the historical 80..90 gap remains untouched.

### Prepared choices and mission request

| ID | Rule and conditions | Source |
| --- | --- | --- |
| WGCH-121 | A corruption dialog stores the copied choice list, actor ID, planet and information-only flag only when no other dialog is active and its shell is hidden. After setup it marks the dialog active, notifies story that the planet's dialog opened, then refreshes requirements. The local-human application branch returns false after requesting this menu; it does not execute corruption or consume the actor at that point. Remote non-AI application also returns false without executing. | debug build CA-E01/E04; WGCH-03 |
| WGCH-122 | Active dialog refresh reads credits from the **local player**. For an actionable dialog it re-resolves the stored actor and uses the actor's **current owner** for mission, unit and planet prerequisites; a vanished actor returns before refreshing choices. An information-only dialog uses the local player for those prerequisites. Required-planet display/checking is reached only when fewer than **2** mission/unit requirement groups occupy the display. Failing requirements change the displayed feedback; this read alone does not establish every mouse/keyboard admission path. Stock planet XML has no slot with all three prerequisite groups together. AI still checks all groups under WGCH-03/49. | debug build CA-E02; XML data CA-D01; WGCH-49 |
| WGCH-123 | Human submission requires an active, actionable corruption dialog and an index in the stored choice list. It copies that choice into a command with the stored actor ID and the planet's persistent ID, queues it, closes the dialog, then emits `choose_corruption_option` tutorial state. The sender does not recheck credits, requirements, current tutorial permission or actor existence, debit money or consume units. Prepared-command execution remains WGCH-72; ordinary reachability of stale/disabled choices remains U-02. | debug build CA-E03/E10 |
| WGCH-124 | The representative required-unit type differs from WGCH-49's aggregate owner-scoped eligibility count. An empty alternative list returns no type; one entry returns that type. With several entries, visit authored order and return the first type having **any instance in the active object manager**, without an owner filter in this getter; if none has an instance, return the first entry. Execution passes this single resolved type and the full required count to prerequisite consumption. Do not replace that selection with pooled consumption across all alternatives or a nearest/owned-type choice. Actual shortfall, transport and tie outcomes remain U-03. | debug build CA-E05/E09; WGCH-06/49/62 |
| WGCH-125 | The inspected cost, prerequisite, tactical map/mode/script and reward getters look up corruption **type**, testing authored planet slots **1, then 2, then 3** and taking the first matching slot. Thus duplicated type slots are not independent cost/map/reward records merely because both choices were enumerated. The inspected unmatched-type getters return credit bonus **0**, no tactical/unlock reward, empty map and invalid tactical mode; requesting none takes the debug-invalid branch with those same fallback results. These are lookup fallbacks, not evidence for omitted authored-field defaults. Stock's **117** nonempty slots contain no duplicated type within a planet. | debug build CA-E06..08/E11/E18; XML data CA-D01; WGCH-04/14/15 |
| WGCH-126 | The tactical corruption request carries the target planet ID, authored land/space mode and map, actor's current player ID as attacker, the **planet object's owner faction name** as map defender, and the authored script when nonempty. It supplies no preferred pathfinder, disallows player persistence and user pathfinder selection, sets prebattle cinematic selector **0**, starts faded but not letterboxed, sets post-battle resolution selector **0**, and requests battle-pending presentation. After successful queue admission it requests the queued transition, then stores the invader-success corruption player/type and **5 s** duration. This is mission setup, not an ordinary ownership conquest or immediate corruption completion. | debug build CA-E05/E12/E13; XML data CA-D01; WGCH-08/10 |
| WGCH-127 | The shared tactical queue rejects an invalid location identity, a mode other than land/space, an invalid attacker identity, empty map, empty defender-faction name, an already occupied queued-conflict slot or no active mode. Those checks precede queue reset/install. A nonempty script is copied; an empty script is not itself a queue refusal here. Successful installation marks linked tactical tutorial state and suppresses the normal battle-end dialog. WGCH-72's preceding credit debit and prerequisite consumption are not refunded by this queue. | debug build CA-E12; WGCH-08/09/72 |
| WGCH-128 | Direct corruption increments the type-use counter and emits hero-land/local permitted success hologram/speech only after begin-transition succeeds. A failed direct begin or tactical queue returns false before actor scheduling/removal, retaining any earlier debit/consumption. A successfully queued tactical request reaches the common actor scheduling/removal without running that direct-path counter/presentation block; tactical-result bookkeeping belongs to WGM-81. Both accepted paths schedule with **0 s** and the parent-check bypass, then use shared hero removal WGCH-95. Menu-open false, admission failure false and accepted mission true therefore describe different actor lifecycles. | debug build CA-E04/E05; WGCH-09/72/95; WGM-80/81 |
| WGCH-129 | At ordinary corruption completion, after committing the transition and refreshing visuals, resolve the **current corruption-benefiting player** from the player list. That resolved player is passed to one-time money, planet tactical rewards and build unlock additions/removals; the executor's actor/current owner is not a stored reward-recipient operand in this path. Missing beneficiary skips those player-specific rewards/removals; automatic faction upgrades, ability notifications and revolt remain independent ordered interfaces. Tactical rewards are keyed by player and planet identity, while the build-unlocked list belongs to that player. | debug build CA-E14..17; WGCH-13..17/75 |
| WGCH-130 | The stock removal AI plan assembles a selected named removal team, launches it, prevents ordinary goal-system removal, calls `Activate_Ability`, marks plan success and sleeps **300 s**. It does not inspect the activation result or wait for the corruption state to become none. This authored plan result and sleep are not the removal handler's duration, credit debit, actor absence or proof of transition completion. Arrival/selection and goal admission remain movement/AI interfaces. | Lua data CA-D02; WGCH-18/19; WGA-45 |

### Ready-to-implement checklist and provider seams

The checklist covers sourced implementation boundaries, not runtime
qualification approval. The associated issues retain their existing ownership
and sizes (legacy EAWR-1140, EAWR-1141, EAWR-1143, EAWR-1144). Providers below are proposed
interfaces; they are not original implementation names.

| Work | Rule checklist | Provider seams and proposed feature test |
| --- | --- | --- |
| Corruption admission/prerequisites/missions (legacy EAWR-1140) | WGCH-01..10/48..50/62/72/121..128; WGM-14/34/80/81/83 | Persist copied choices, actor/planet IDs and RNG continuation. Supply owner/allegiance and active-galaxy type queries, global prerequisite enumeration, named-hero scheduling/removal, and a tactical mission queue/result provider. Test stale actor/owner/units/credits, all AI scores/no eligible option, any-owned-planet and alternative-unit admission versus single-type consumption, direct/mission/AI/multiplayer/debug paths, occupied queue after debit/consumption, and default actor absence. Compare checkpoint and 1/2/4/8-worker continuation. U-02/U-03 qualify UI reachability and consumption ties. |
| Transitions/rewards/removal/revolt (legacy EAWR-1141) | WGCH-11..19/51/75/125/129/130; GFS-23/GFC-31/GFD-28 | Adapt validated planet-slot rewards and the current beneficiary to the existing corruption fixture, then connect player/planet upgrade ledgers, build unlocks, ability notification, surface/orbit retreat and political control. Test none-to-type and type-to-none admission (no direct type replacement), exact deadline/reset/zero removal, missing beneficiary, add/remove reward order, target-allegiance cost/begin-before-debit, and militia revolt owner deduplication/surface-before-orbit requests. Service-map cases own interruption/pause/restore; U-01/U-04 remain qualified. Recurring income stays WGEP-04/12/13/115 with the economy owner. |
| Theft/siphon/spy/stealth (legacy EAWR-1143) | WGCH-32..36/39..42/54..56/64/70/95/100..108/116..118; WGUI-19..32/101 | Supply registered-type/lock/tech and prepared price/chance providers, deterministic draws, transport/company source metadata, fiscal-duration deployment and source-keyed visibility insertion, plus shared action consumption/scheduling. Test one-option advancement versus multi-option index, initially empty-list admission, all three theft failures, ownership changes, auto-capture, siphon prevention/duplicate/missing transport/no debit, spy self/ground/automatic admission and success-only consumption. Siphon expiry/payment and sensor masks/cleanup remain owning services; U-08/U-12 retain stale-menu and lifecycle probes. |
| Hack/destruction/protection (legacy EAWR-1144) | WGCH-57..59/66..68/109..115/119 | Supply exact-current-type orbit lookup, prepared menu price/current-owner payment, shared ruins cleanup, build locks, source-keyed protection and session pending victory. Test human payment versus free direct AI, missing/replacement weapon and missing weapon owner after debit, removal immediately aborted by ruins, ordered hack-only orbit/starbase/Neutral mutation, stealth/ally contested fleets, current versus target allegiance, exact landed leader and enabled **30-frame** victory request. Source cleanup and ordinary stale interaction remain U-12; session owns victory resolution and tactical weapon behavior stays outside scope. |

### Comparison and qualification

Against the stated base, the session, command payloads and authoritative Lua
bridge lack live corruption-menu/mission action wiring, representative-type
consumption, and the stock removal-plan result interface. The corruption
fixture already provides transition admission/arithmetic and ordered rewards,
removal and revolt; it accepts caller-supplied type-indexed rewards and a
beneficiary ID rather than resolving WGCH-125/129's live inputs. Its tactical
reward storage is a fixture of one supplied recipient, not the campaign's
player-and-planet ledger. The ten added provider/action rules are therefore
**missing 10, same 0, differs 0** in live integration at this base. Existing
WGCH-51's prohibition on replacing a nonempty type is **same** in the fixture;
no new transition algorithm or recurring-economy claim is introduced.

U-02 is narrowed by WGCH-121..123: capture a menu, separately change actor
owner, local credits, unit count, required planet, use counter and tutorial
permission, then record whether normal clicks/keyboard can submit, payload,
debit and transition. Include an information-only menu and vanished actor.
U-03 is narrowed by WGCH-124: stage alternative A owned only by an enemy,
alternative B owned by the actor, and mixed owned A/B counts; record the
representative type, consumed containers, shortfall and RNG continuation.
Equal-weight/container qualification remains the existing recipe. Duplicate
slot precedence and three simultaneous prerequisite groups are not stock
runtime claims; synthetic loader/provider tests cover those static boundaries.

The earlier U-08 and U-12 labels do not mean human theft advancement, exact
hack identification, ruins cleanup or protection removal are wholly unknown:
WGCH-64/66..68/100..115 already source those branches. Their unresolved parts
are ordinary stale-menu reachability and observed source/lifecycle outcomes.
Keep those capture recipes and service-map ownership instead of inventing a
policy or reopening sourced arithmetic. No new service order, capture result,
applied registry status, implementation ticket or economy work is claimed.


## Implementation readiness: neutralization and structure sabotage

Research dated 2026-10-10 at `73bece6fa91004bd0139fd22bf0b1082f4b444c4`.
GAH evidence IDs identify private read-only debug-build records and stock XML/Lua.
WGCH-20..31/52/63/73/77/91..99 already source admission, prepared prices,
current-owner payment, history, identity checks and ordinary/free/AI separation.
The following additions settle outcome and presentation details without
claiming runtime reachability or a new service cadence.

| ID | Rule and conditions | Source |
|---|---|---|
| WGCH-131 | Neutralization's ordinary execution helper contains **no success-chance random draw**: valid prepared option, resolved actor/victim or active siphon and sufficient current actor-owner money enter success unless generic-hero auto-capture intercepts. Preparation requests the actor's synchronized **floating range sample first, then the target's**, or substitutes the default independently when either endpoint of that range is nonpositive. These are absence times, not a success test; AI's equal-weight option choice is another draw boundary (WGCH-24/93). No separate recharge/cooldown is added by this executor. | debug build GAH-H01/H02; WGCH-21/24..26/93; XML GAH-D02 |
| WGCH-132 | The local human neutralization sender checks selected stored-option validity and **current local floating credits against its full prepared cost**. Invalid/unaffordable selection plays negative feedback and leaves the dialog open; accepted selection queues the stored option/actor/planet and closes it. It does not re-enumerate candidates, debit money or remove actors at submission. The list displays an integer conversion of cost; that displayed integer is not the sender/executor's affordability operand. Current actor-owner execution remains WGCH-92; ordinary stale-menu reachability remains U-05. | debug build GAH-H03; WGCH-24/92 |
| WGCH-133 | When generic-hero auto-capture intercepts neutralization, an available text callback shows `TEXT_NEUTRALIZE_HERO_FAILURE_VIGILANCE` to the local actor owner and `TEXT_NEUTRALIZE_HERO_PREVENTED_VIGILANCE` to the local victim owner, in the respective faction color, with a planet-jump callback. The actor is then scheduled/removed and the helper reports false. Otherwise English success feedback distinguishes a local human attacker from a local human victim; other languages use actor/victim type-specific keys, falling back to `TEXT_GENERIC_NEUTRALIZE_MESSAGE`, and append the planet display name. Success feedback precedes victim mutation. This callback interface does not establish final retail line layout or every observer's notifications. | debug build GAH-H02; WGCH-25/26 |
| WGCH-134 | After successful victim removal or siphon termination and payment, neutralization emits hero-land and `hero_neutralized` tutorial state. It requests the prepared success SFX only for a local actor owner; for a local victim owner it separately requests the victim type's death-start sound (siphon type lookup in the siphon branch), before scheduling/removing the actor. Invalid prepared state, vanished ordinary victim, vanished siphon and insufficient money take local-actor negative-feedback branches rather than a random failed attempt. Siphon lookup after termination remains a lifecycle qualification; no claim of actual audible playback follows from the call alone. | debug build GAH-H02; WGCH-26/92/94/95 |
| WGCH-135 | Successful **ordinary/free prepared structure sabotage** requests the configured attached sabotage effect without the direct AI route's locally-revealed gate. The paid helper additionally requests the actor faction's success sound when authored; the free/script helper skips that sound alongside actor/payment handling. The direct AI helper separately selects attacker/enemy accomplishment text by local actor owner versus current planet allegiance and gates its effect/sound on local revelation (WGCH-77). These are separate presentation interfaces, not one universal victim message or fog gate. Prior-count exponent pricing remains WGCH-28/97; absence is WGCH-30/77, not the stock Lua plan's **10 s** sleep. | debug build GAH-H04/H05; XML/Lua GAH-D02/D03; WGCH-30/77/120 |
| WGCH-136 | The synchronized floating-range helper used for prepared absence consumes **one underlying generator step even when positive endpoints are equal**; that equal-range result is the endpoint. It interpolates between ordered endpoints, with reversed endpoints taking a debug-invalid branch before swapping them. This differs from the integer helper's singleton no-advance rule (WAR-111) and from WGCH-21's nonpositive-endpoint default, which bypasses the floating helper entirely. Actor then target sampling must retain these distinct call/state boundaries; do not turn fixed positive timers into zero-draw defaults. | debug build GAH-H01/H06; WGCH-21/93/131 |

### Ready to implement: neutralization and escalating sabotage

For this existing work (legacy EAWR-1142), use separate prepared-option,
current-identity, scheduling, scoring-history, production and presentation
providers. `src/sim/galactic/commands.cpp` and `session.cpp` at the comparison
base do not implement these prepared neutralization/sabotage actions; the
common hero-return and credit-siphon owners supply their own interfaces.

| Contract | Rules/provider seam | Proposed observable test |
|---|---|---|
| Eligibility and preparation | WGCH-20..24/52/63/91..94/131/132; sensors-visible ordered candidates, two-sided capabilities, copied modifier ledger, history keyed by company/attacker, synchronized absence samples | All major/minor actor/target combinations, hackable superweapon, first container hero invalid versus later valid, missing/changed siphon. Include disabled/unaffordable options consuming authored samples; default endpoints; price floor and target's own protection-source removal. No chance-of-success roll after prepared admission. |
| Ordinary outcome/lifecycle | WGCH-25/26/92/94/95/133/134; current actor-owner account, victim/company scoring and attribution, hero-return service, synchronous removal/notification | Stale actor/victim/planet/siphon, owner/credits changes, generic auto-capture false result, normal versus siphon success, victim-before-payment then actor scheduling, local attacker/victim feedback and sounds. Save prepared options/history and compare 1/2/4/8 continuation. |
| Escalating structure sabotage | WGCH-28..31/73/77/96..99/135; exact ground/space identity, prepared ordinary/tutorial price, prior planet history, layer production reevaluation, separate free/AI paths | Two ordinary successful sabotages give base cost then base cost times authored factor (stock **2**), with nearest-integer rounding; stale/missing target and failed removal do not debit/despawn/increment history, while located failed removal still reevaluates production. Negative absence skips scheduling but still consumes actor on success; free action skips payment/actor and has separate effect/sound behavior. |

U-05/U-06 retain fog-on ordinary-menu, repeated ground/space sabotage,
fractional-credit, actor absence and stale-identity captures. Extend those
recipes with attacker/victim advisor text and audio, revealed/hidden planet
and direct-AI/free controls; U-10/U-12 own removal/siphon lifecycle outcomes.
U-07 remains stock-inapplicable legacy activity data, not a missing stock
failure roll. The corruption/hero service map owns callback ties and restoration.

## Planet destruction: admission, cleanup and persistence readiness

Read-only **debug build** follow-up and effective stock **Lua/XML data**,
2026-10-10, comparison base `bf87fe4d9029cb83aef281bd5f01b9ec691c270d`.
GPD evidence IDs identify bounded reads held in ignored research storage.
This appendix refines the action work (legacy EAWR-1144) and its later space-map
consumer (legacy EAWR-2491). It contains no retail observation. Within-handler
mutation order below does not add a frame-service schedule; use
[the galactic schedule](../galactic-frame-service-schedule.md) and
[the corruption schedule](../gc-corruption-frame-schedule.md) for service,
pause, child-mode and callback timing.

| ID | Rule and boundary | Source |
|---|---|---|
| WGCH-137 | Stock `Death_Star` and `Death_Star_II` both author the ground-activated `Planet_Destruction_Ability`. The activated-destruction name list contains their authored action name. Stock `Eclipse_Super_Star_Destroyer` has no type inheritance or such ability: it authors a tactical `SUPER_LASER`/`Super_Laser_Ability`, with **10 s** recharge and expiration. Do not grant stock Eclipse a GC destruction action or use its tactical recharge as the Death Star's GC timer. Mod-authored abilities retain their data-driven admission. | XML data GPD-X01 |
| WGCH-138 | The destruction menu predicate walks the actor's parent containers to its containing planet. An actor already contained at another planet is refused; no containing planet or the requested planet permits the named-ability check. The common subobject check refuses a fleet breakdown with more than **one** entry before recursing; it visits transport contents and finally asks the object's ability manager for named target appropriateness. These are UI/container gates, distinct from WGCH-113's target-allegiance gate and WGCH-58's execution-time contested-orbit refusal. | debug build GPD-E07/E20 |
| WGCH-139 | The hack menu predicate first requires a named activated hack ability appropriate for the target, then a resolved required type and an exact **current type** match in attached-fleet breakdowns. This lookup has no weapon-owner hostility or corruption test. Stock `Hack_Super_Weapon_Required_Type` is `Death_Star_II` and `Hack_Super_Weapon_Cost` is **50000 credits**. Corruption in the stock AI desire equation is not a universal human hack gate; payment still follows WGCH-110/112. | debug build GPD-E08; XML data GPD-X01; WGCH-109..112 |
| WGCH-140 | With destruction enabled, the shared ruins helper first sends the destroyed-planet story notification, then terminates siphoning, destroys landed transports, clears ground structures and zeros land special-structure capacity. It clears political-control modifiers, captures the old corruption type and aborts the transition. Only when old corruption is non-none does it set none, refresh corruption visuals and dispatch old-state cleanup. In an active galactic mode it terminates the planet's abilities. It sets the **destroyed flag last**. With destruction disabled it omits story, capacity-zero and flag writes while retaining the other cleanup. This qualifies WGCH-67's inventory: the story callback precedes cleanup and cannot be moved behind the flag write merely because the final state matches. | debug build GPD-E01; WST-80..82; WGCH-67 |
| WGCH-141 | Ruins calls landed-transport destruction with no faction exclusion and hero scheduling enabled. That helper traverses the stored transport list **in reverse**. For each transport it repeatedly finds a contained named hero, requests respawn with no explicit planet, **zero** requested seconds and no admission bypass, then destroys that hero; after those requests it destroys and removes the transport. Hacking's additional orbital-fleet cleanup uses the same reverse-container/named-hero pattern without faction exclusion. Scheduling requests are not guaranteed returns: WGM-79/80 owns admission, company selection, location and the nonpositive-time default. Ordinary destruction owns remaining contained units. The consumed hacker's lack of an explicit schedule remains WGCH-110. | debug build GPD-E02/E27; WGM-79/80 |
| WGCH-142 | Ground clearing repeatedly removes the first stored ground special structure and then refreshes ground base level. Removal detaches that structure's local or radius-based planetary influence, removes its list membership and detachment listener, and destroys it unless already delete-pending. Ruins supplies no final-blow player: the remover's optional health-zero/final-blow branch is not requested here. Shared ruins does not clear space special structures, orbital fleets or starbase, or debit/refund player credits in its own body. Those containers and ordinary destruction/production callbacks retain their owners; hacking adds WGCH-111's orbital/starbase/ownership operations. | debug build GPD-E01/E03/E26; WGCH-115 |
| WGCH-143 | A live destroyed planet's surface-accessibility reader returns **false** regardless of the authored accessible-surface flag. Its base income reader selects `Planet_Destroyed_Credit_Value`, rather than assuming income is zero; blocked-income and other fiscal modifiers still follow WGEP-05. Galactic model selection returns the authored `Destroyed_Galactic_Model_Name` model for a planet with the destroyed flag, and the ordinary galactic model otherwise. The GC destruction action refreshes the model after ruins has set the flag. This closes the static model-selection part of presentation U-03; actual assets and visible playback remain capture qualification. | debug build GPD-E10..12/E17; WGEP-05; WGP-08 |
| WGCH-144 | The planet serializer and its paired loader explicitly retain destroyed state, land/space special-structure capacity, ground/starbase state, orbit and landed identities, the associated map name, current base credits, allegiance/apparent ownership, AI target index, siphon state and corruption type/transition deadlines. Political/income/protection modifiers, scheduled creations and trade links also have save/load paths. Destroyed state is persistent planet data, not inferred from a backdrop or empty roster. The inventory does not prove every callback or post-load continuation correct, and does not require copying the original save format. | debug build GPD-E04/E05/E21; WGT-56 |
| WGCH-145 | A loaded object identified as a planet with destroyed planetary state requests a model refresh during post-load fixup. Galactic post-mode-switch resume separately visits planets and terminates abilities on those marked destroyed, when ability data exist. Neither body deletes the planet merely because the flag is set. Retain the planet identity and the distinct loaded-model and resumed-ability consumers; their complete visible/listener continuation is WGCH-PD-U03. | debug build GPD-E16/E19; WGM-06 |
| WGCH-146 | Ruins and the destroyed-flag setter do not remove the planet, delete trade links or request a graph rebuild. Graph initialization admits loaded planet objects without a destroyed-state filter and connects pairs under WGM-16's distance-or-trade-route rule. The inspected ordinary hop cost, expansion and their allegiance/enemy-force/corruption-bypass/AI-usability consumers have no separate destroyed-state veto. Destruction can still change route admission through removed corruption, remaining starbase/orbit forces, control, story restrictions and visibility: preserving the node/edges does not guarantee every former route remains admissible. Do not turn a ruined surface into removal of the space-travel node. | debug build GPD-E01/E13/E28..30/E34..37; WGM-16..22 |
| WGCH-147 | AI `IsSurfaceAccessible` queries the live planet's surface reader and returns **0** for destroyed state; it does not substitute the old authored accessible flag. Native `LandUnits` creates its block-status object, then returns it immediately when that surface is inaccessible, before deploying members or requesting land-management events. The AI connectivity evaluator instead scans its stored nearby planets and resolves matching type/faction plus optional distance attenuation; it has no additional destroyed-state or surface filter in this body. AI target/plan deletion is not implied by this flag, and retained force knowledge remains the AI perception owner's responsibility. | debug build GPD-E10/E31..33; WGA-06/206 |
| WGCH-148 | Hack-menu Close calls deactivation: mark the menu inactive, clear its hacker/planet identities and hide/disable its shell. This body does not enqueue the prepared hack, debit funds, consume the actor or mutate the planet. The prepared executor separately resolves its stored actor/planet IDs and invokes the paid helper. The inspected successful hack and GC destruction bodies perform their mutation sequence without a hacking-progress timer or cancel-after-success branch. WGCH-111's **5 s** is corruption removal immediately aborted by ruins, not a five-second weapon transfer, temporary lock or cancellable hacking duration. A menu close is not evidence of withdrawing a previously submitted command. | debug build GPD-E09/E23..25; WGCH-66/110/111 |
| WGCH-149 | Stock Death Star desire multiplies **50** by first-attack admission, enemy ground power **strictly greater than 1500**, and human-controlled target. Its plan requires `Super`, assembles/moves its force, marks success before `LandUnits`, then prevents goal-system removal and disables target-contrast checks; it does not directly invoke planet destruction or test its returned result. Stock hack desire multiplies target corruption and enemy `Death_Star_II` presence by a weight: **0.1** when normalized controlled planets are below **0.5**, plus **10** for enemy leader-team ground presence and **10** for its space presence. Its authored plan remains WGCH-119. Raid and unrestricted land-grab equations separately multiply by `IsSurfaceAccessible`. These are goal-specific authored desires/plan calls, not universal destroyed-planet deletion, human admission or a guarantee of action execution. | Lua/XML data GPD-X03; WGCH-119; WGA-36/44/45 |
| WGCH-150 | Destroyed ownership continues through WGM-67/76: orbital presence may control an inaccessible surface, destroyed state suppresses residual influence and capture-bonus awards, and ordinary planet income uses WGCH-143/WGEP-05. Production remains a downstream provider: ground facility/base requirement loss uses WGEP-35's backward revalidation, hacking's starbase-zero request uses WGEP-53's space revalidation, and control loss can bulk-stop both queues under WGEP-72. Do not synthesize an action-wide full refund or zero every player account. WGEP-117/118 already distinguish manual refunds from an observed automatic human producer-loss cancellation without refund; destruction-specific complete listener/AI accounting remains WGCH-PD-U01. | debug build GPD-E14/E38; WGCH-142/143; WGM-67/76; WGEP-35/53/72/117/118 |

### Current implementation boundaries

At the comparison base, `src/sim/galactic/hero_actions.cpp` supplies pure hack,
destruction and protection policy helpers and `tests/galactic/hero_actions_tests.cpp`
has synthetic action contracts. The original blanket **missing** action labels
are historical. The current ruins helper marks transports absent without
WGCH-141's explicit named-hero scheduling requests, and records its story effect
after clearing ground state and setting the destroyed flag. It also emits
corruption visual/cleanup effects when old corruption is none. These are
**different policy/effect boundaries** under WGCH-140/141, not proof that a live
campaign already delivers those effects in that order.

`src/sim/galactic/control.cpp`, `income_encoding.cpp` and `victory.cpp` retain
destroyed metadata in their own saved state. They do not replace WGCH-144's
complete action outcome and continuation providers. Live command admission,
ordinary contained-object destruction, hero-return scheduling, structure influence,
production callbacks, synchronous story delivery and ownership/victory remain
explicit integration seams. No new action code or test execution is claimed here.

For WGCH-147, `src/script/foc/galactic_ai_perception.cpp` currently projects
`IsSurfaceAccessible` from production's surface-accessible input. The action
integration must update that provider from authoritative ruined state; a saved
control flag alone is insufficient. WGM-16's graph and the existing AI perception
owners remain the shared providers, rather than a new destruction-only pathfinder
or blanket AI-target deletion rule.

The tag registry still separates parsing from these consumers. At the comparison
base, `SpaceUnit` destruction-ability name/activation/effect fields and
`GameConstants` activated-hack names are **todo**; activated-destruction names,
hack required type/cost and planet tactical destroyability are **land-or-galactic**.
The galactic shard marks destroyed income, galactic model and destroyed space-map
fields **todo**. Apply destroyed income through the fiscal owner, destroyed model
through presentation, and tactical destroyability only through WGT-100's firing
gate. WGT-57's inspected map path does not consume `Destroyed_Space_Tactical_Map`;
its parsed presence is not permission to choose that map. `SpaceProp`
`Death_By_TSW_Replacements` is **todo** and belongs to the later child-scene
producer. This research PR changes no application status: the implementation
owners update the exact class/tag rows when they wire those consumers.

### Ready to implement: hacking and planet destruction

These are proposed feature tests for the existing action work (legacy EAWR-1144).

| Contract | Rule IDs / provider seam | Proposed observable test |
|---|---|---|
| Actor/menu/target admission | WGCH-57/58/109/113/137..139; named ability lists, containment, exact current types, target allegiance and contested fleet facts | Wrong containing planet; one versus two fleet-breakdown entries; Death Star versus Death Star II versus stock Eclipse; allied/invalid target allegiance; stealth, allied and visible hostile orbit controls. |
| Human, AI and cancel | WGCH-66/110..112/148; prepared price, current actor account, current weapon/owner, build lock and menu state | Cancel before submit leaves all gameplay state unchanged; human debit versus free direct AI; missing weapon before debit versus missing owner after debit; replacement exact-type weapon; no fabricated hack-progress timer or post-success unlock. |
| Shared versus hack-only cleanup | WGCH-140..142; synchronous story, named-hero scheduler, ordinary destruction, influence/production/corruption/ability providers | Observe story while destroyed flag is still false and rosters remain; reverse named-hero requests and admitted/refused scheduling; corruption-none versus active transition; preserve shared space structures/orbit/starbase while hack adds its own removals. |
| Economy, ownership and victory | WGCH-58/114/115/143; authored destroyed income, current/target allegiance, session pending victory and fiscal/control consumers | No action-created credit award/refund; nonzero destroyed income, blocked income and siphon cleanup; exact landed leader and enabled **30-frame** pending request; no leader match in orbit or by original company type. |
| Travel and AI view | WGCH-146/147/149/150; retained graph node/edges, live surface state, perception/force facts, authored desires and ordinary route gates | Destroy the middle of a three-planet path: graph topology remains while loss of corruption bypass can change admissibility. An AI surface query becomes zero and landing emits no deployment; retained connectivity and space conflict remain possible under their normal gates. Observe plan result independently from actual action success. |
| Persistent continuation | WGCH-144/145; authoritative destroyed state, map cache, roster IDs, action effects and source-owned services | Save after success and before the next fiscal/child-entry consumer; fresh-owner reload matches an uninterrupted control, preserves ruined appearance and income, and emits no second destruction, payment, scheduling or victory request. Compare 1/2/4/8 workers. |

### Remaining runtime qualifications

Static rules above narrow U-12; they do not claim that every stale menu state is
ordinarily reachable or that a requested hero schedule succeeds. Use the existing
Lua-debugger/capture interfaces and matched uninterrupted controls for these rows.

| ID | Qualified unknown | Capture recipe |
|---|---|---|
| WGCH-PD-U01 | Actual hero/contained-unit deaths, admitted respawns, production cancellation/refunds and ability-source cleanup following ruins. | Stage mixed factions, named/generic heroes, structures with local/radius influence, orbit and ground production queues, a siphon and active corruption. Compare GC destruction, human hack and direct AI hack. Record scheduler requests separately from admitted return records, ordinary death callbacks, queue/account changes and final rosters. U-12 still owns ordinary stale-menu reachability. |
| WGCH-PD-U02 | Observed post-destruction route choice, AI goal retention and visibility-dependent knowledge. | Use a three-planet route through the target plus a long trade-route connection; retain one moving fleet and one AI plan. Destroy only the middle planet. Record planet IDs, graph edges, path queries, control, force observations and surface accessibility before/after the next existing scheduled consumers. Include fog-on AI and save/reload controls; do not infer topology from a drawn lane. |
| WGCH-PD-U03 | Full restored outcome/listener continuation rather than a serialized-field inventory. | Save just before submit, after ruins, after hack orbital cleanup and before fiscal payment or a later space child. Reload into fresh services; compare account/lock, destroyed state, map cache, hero-return records, source cleanup, income, story and pending victory with uninterrupted controls. Record duplicate callbacks and first eligible consumers using the established schedules. |
| WGCH-PD-U04 | Ordinary UI/callback presentation and story mutation under reentrant rewards or duplicate planet names. | With fog on, compare Cancel, accepted and stale human menus and direct AI action. Add a destroyed-planet story callback that records the flag/rosters and a dependent planet-filtered event; compare single, multiple, duplicate and initially-empty lists before/after destruction and reload. Record requested particles/sounds separately from visible/audible output and actual story trigger/disabled state. |

## Strategic planet destruction: human control and request binding

Read-only **debug build** and effective stock **XML/Lua data**, 2026-10-10,
comparison base `9938fb8275ddccd7599660f38230791cb39b5c91`.
GDB evidence IDs identify private bounded reads. This appendix closes the static
binding gap in H070/GHUD-S33; it claims no retail recording. The sourced human
route uses the galaxy's drag special-action slot. Existing tactical fire controls
and stock scripted activation have separate entry contracts.

| ID | Rule and boundary | Source |
|---|---|---|
| WGCH-151 | Each galaxy planet display clones **two** special-action slots from the `g_special_ability` component template. Stock XML declares it a `TextButton` with `Drag_And_Drop = True`. In the ordinary drag preview, an admitted destruction action shows `TEXT_TOOLTIP_ABILITY_DESTROY_PLANET_NAME` in the **first** slot, stores its selected actor and action kind there, and hides the second slot. No admitted action hides both; the preview-suppressed branch also clears their action/actor parameters. This establishes a galaxy drop target, not a destruction-specific tactical fire button or a requirement that scripts be the sole trigger. | debug build GDB-E02/E03/E04; XML data GDB-X01 |
| WGCH-152 | The shared top-action selector first honors the target's all-abilities tutorial restriction. It chooses the supplied actor with the **lowest original-type** `Ranking_In_Category`, retaining the first on equal rank, then rejects a story-provided special space-map override for that target/actor. Stock Death Star authors rank **0**; the selector's initial comparison value **999** does not exclude a first candidate with a larger authored rank. It checks destruction **before** theft, black market, sabotage, spy, neutralization, siphon, corruption, removal and hacking. It does not search later supplied actors for a destruction ability after the top actor fails that predicate. | debug build GDB-E01/E12; XML data GDB-X01; WGCH-138 |
| WGCH-153 | For a galaxy fleet drag, the preview supplies the fleet's breakdown; for the landed-force widget it supplies the source planet's landed transports. More than **one** supplied entry suppresses the selected action unless it is destruction. A separately admitted fleet raid can replace the selected slot afterward. This outer destruction exception does not remove WGCH-138's inner refusal of a multi-entry fleet container. Missing actor/ability, wrong containing planet, destroyed target and allied target allegiance remain their own predicates; neither the contested-orbit execution check nor the leader-victory condition is read by this slot selector. | debug build GDB-E01/E02/E11; WGCH-113/138 |
| WGCH-154 | The galaxy's left-button release path dispatches a dragged source onto the planet's special-action slot. A source that is itself a special-action slot is ignored by that dispatcher. For a fleet source at the **same planet**, a non-raid action invokes ground-deployment preparation using the slot's actor and target planet. A different-planet source takes the ordinary movement/destination-actor interface instead; WGCH-138 normally prevents destruction from being previewed for an actor contained at that different planet. A landed-force source hands off to launch/movement. The inspected destruction branch has no target-confirmation dialog or immediate ruins mutation. These branch reads do not establish which stale combinations ordinary retail UI can reach. | debug build GDB-E04/E09; WGUI-55; WGCH-138 |
| WGCH-155 | Ground-deployment preparation requires actor/container and target. For a nontransport it accepts an appropriate ground-activated ability. For a transport it scans contained objects in stored order and stops at the **first** object with such an ability; a story-provided special land-map override for that object rejects this submission rather than continuing to a later contained actor. Acceptance queues the **original supplied container/object identity**, not the found contained actor identity, plus the target planet identity. A rejected preparation queues nothing and returns false. | debug build GDB-E05/E06 |
| WGCH-156 | The queued request is generic ground deployment with **actor/container ID and planet ID**, not a named destruction ability, prepared price, victory decision or tactical countdown. Execution resolves both IDs afresh; unresolved IDs cause no deployment. A transport is searched again in stored order for a contained actor with an appropriate ground-activated ability; a nontransport reaches the deployment callback after its debug validity check. The callback receives the live actor and planet. The request body does not perform cleanup itself or publish the destruction effect's returned success to the HUD. Stale container membership is therefore a live execution question, not a pinned prepared inner-actor pointer. | debug build GDB-E06/E07 |
| WGCH-157 | The deployment callback enters the actor's generic ability service. A cancelled service refuses this route; otherwise it requests activation of the **ground-activated group** against the planet, rather than looking up only the destruction name selected by the UI. Generic enabled-ability/appropriateness, activation and successful actor-consumption contracts remain WGCH-70. An appropriate named ability in `Activated_Destroy_Planet_Ability_Names` admits the destruction preview, while authored `Activation_Style` controls the invoked group. Preserve both data inputs for mods; neither faction name nor a fixed stock weapon list substitutes for them. | debug build GDB-E07; XML data GDB-X01; WGCH-70/137/138 |
| WGCH-158 | Destruction execution's ability gate and planet services precede contested-orbit checks. When the actor's contested-space flag is true, a **nonstealth nonallied** orbital fleet returns false before pending victory or ruins; a local actor owner receives negative-feedback SFX. An unscouted nonstealth hostile orbital fleet still refuses an executed destruction attempt and requests generic negative-feedback audio for the local actor owner; this feedback is not gated by sensor knowledge and does not grant fleet details. The preview remains observer-filtered. Stealth and allied fleets do not take that refusal. The slot selector contains no such orbit check. Likewise the leader-kill victory condition does **not** admit/disable destruction: only its pending-victory request requires the active galactic condition, current controller's leader and exact current-type match in landed contents. Destruction can succeed without any pending victory. | debug build GDB-E01/E08; WGCH-58/113/114 |
| WGCH-159 | The focused organization panel has an additional **shared hero ability drop slot**. Its preview requires one dragged organization entry whose **original type** is named or generic hero, then uses WGCH-152's selector. Its receive handler invokes the same ground-deployment preparation only when the dropped object's identity equals its stored actor. This is a generic authored-hero route, not evidence that stock Death Star uses that panel: stock Death Star has no named/generic-hero declaration. Stock strategic admission is already sourced through WGCH-151's galaxy slot. | debug build GDB-E10; XML data GDB-X01 |
| WGCH-160 | Stock campaign Lua separately assembles a Death Star fleet and calls `Activate_Ability("Death_Star")`, bracketing that call with the weapon's `Set_Check_Contested_Space(false)` and restoration to true. Later scripted attacks wait for fleet movement before repeating that bracket. The authored script's **45 s** attack delay is not a generic GC destruction recharge. The stock galactic AI plan remains WGCH-149's `LandUnits` route, while stock space AI explicitly calls `Fire_Tactical_Superweapon` after `Is_Tactical_Superweapon_Ready`. These authored entry points do not convert the tactical countdown into the strategic human control or establish successful runtime execution. | Lua data GDB-X02; WGCH-137/149 |

### Result publication and implementation readiness

The result remains **same** as WGCH-58/114/115/140..150: session owns pending
victory, the planet retains its identity and travel node, shared ruins performs
ground cleanup, and presentation reads authoritative destroyed state for the
model. A galaxy request must not remove the graph node or apply hacking's extra
orbit/starbase/Neutral mutations. Stock effects are authored through
`SFXEvent_Target_Affected`, `Target_Particle_Effect` and
`Target_Particle_Bone_Name`; requested effects do not prove their visible playback.

The binding rules are **missing** from our live input/action integration at the
comparison base. `src/sim/galactic/hero_actions.cpp` remains a pure policy helper;
the copied host-admission/refusal provider work inspected in its draft
(legacy EAWR-2771) supplies neither the slot selector, queued deployment nor live result
publication. Its refusal facts must preserve **preview admission versus execution
refusal**, without exposing a hidden blocking fleet. Existing target/container
and cleanup rules are **same**; WGCH-158 qualifies the inventory's former broad
"contested-space gates" description. Existing policy/effect differences remain
the preceding readiness appendix's WGCH-140/141 findings.

The `SpaceUnit` destruction ability name/style/effect rows remain **todo**, and
`GameConstants/Activated_Destroy_Planet_Ability_Names` remains
**land-or-galactic**. This docs-only change applies no tags. Implementation must
read the authored names, style, original-type ranking and effects through their
validated owners, and update each exact class/tag application row when wired.

| Work | Ready-to-implement checklist: rules and provider seams | Proposed observable feature test |
|---|---|---|
| Strategic destruction HUD (legacy EAWR-2311) | WGCH-151..155/158/159; GHUD-S31 drag adapter, observer-filtered actor/planet facts, original-type ranking, tutorial/story admission, named-ability/container predicates and live command submit. Separate preview admission from execution refusal; result publication comes from the action/session owners. | Drag eligible stock DS/DSII onto the first planet action slot; stock Eclipse offers no strategic action. Reverse equal ranks, include a lower-ranked ineligible actor, one/multiple breakdown entries, wrong planet, allied target allegiance, tutorial and special-map overrides. Add allied/stealth/nonstealth hostile orbit controls and assert that orbit/victory are not additional selector predicates. GPU checks exercise the slot and feedback after the authoritative synthetic command result; GHUD-C11 retains retail layout/reachability. |
| Action request and destruction interface (legacy EAWR-1144) | WGCH-155..158/160 plus WGCH-58/70/113..115/140..150; stable container/planet IDs, live ground-ability dispatch, contested-space flag, synchronous story/ruins, named-hero scheduling, production/ownership and session pending-victory providers. Hacking/protection retain their existing distinct contracts. | Submit, then remove actor/planet or change transport contents before execute; confirm live identities and no pinned inner actor. Test first eligible contained actor and special-map rejection, cancelled/enabled service, data-authored ground abilities, contested flag true/false and local-owner refusal feedback. Assert no mutation on orbit refusal; enabled exact landed leader queues **30 frames**, disabled/no leader still permits ruins. Retain planet/edges and shared-versus-hack cleanup; save/reload and 1/2/4/8 continuation follow the action/session feature tests. |

### Runtime qualifications for the binding

| ID | Qualified unknown | Capture recipe |
|---|---|---|
| WGCH-DB-U01 | Ordinary human slot visibility, layout, text, input-profile behavior and contested refusal presentation. Static binding is sourced; retail interaction has not been recorded. | Extend GHUD-C11 with fog-on DS and DSII galaxy fleet drags at the same planet, mixed fleet ranks, count-only versus known contents, destroyed/allied targets, tutorial/story map controls and stock Eclipse. Record actual `g_special_ability` clones, drag/release and any confirmation that appears; qualify the authored `Offset`/`Text_Offset` coordinate mapping and `Right_Justified` text anchor against the planet at multiple viewport sizes; qualify the authored `Offset`/`Text_Offset` coordinate mapping and `Right_Justified` text anchor against the planet at multiple viewport sizes; compare ally, stealth hostile and nonstealth hostile orbit, local-owner SFX and victory condition on/off. Distinguish a slot absent due to target/container gates from an offered action refused after submit; do not infer control absence from a single idle frame. |
| WGCH-DB-U02 | Ordinary stale/repeated deployment reachability and successful scripted/AI activation with full galaxy result/restore feedback. | Use the existing Lua-debugger/capture interface to record queued actor/container and planet IDs, swap/remove the first eligible contained actor before execute, repeat a request and compare cancel-before-release. Trace the stock scripted contested-flag bracket and AI landing separately from human drop and tactical fire. Record actual activation/refusal, destroyed state/model, synchronous story, pending victory and graph identity; compare uninterrupted and restored controls. Reuse WGCH-PD-U01..04 for downstream lifecycle/accounting rather than declaring complete victory or presentation parity from a returned helper result. |
## Live corruption loop: readiness for the full conquest play-through

Research dated 2026-10-10 at merged comparison base
`042541d28de3b5fca26d8e7be5d73db4c153ac45`. This appendix audits the ordinary
campaign path for the full conquest sign-off (legacy EAWR-1969), including the
ownership gap in the [capture audit](gc-movement-control.md#planet-capture-readiness-for-the-full-conquest-play-through).
It does not sign off the play-through. Earlier fixture coverage and historical
walk counts do not establish live campaign coverage at this base.

Evidence is read-only **debug build**, effective XML/Lua data and inspection of
our merged code. LC-E01/E02 identify private query records; LC-EN01 identifies
the existing debug-build enum export independently read for this audit;
LC-X01 identifies the effective stock data cross-check. No new retail run,
runtime equivalence, frame-order measurement or applied tag status is claimed.
The [corruption service schedule](../gc-corruption-frame-schedule.md),
GFS-C-01..05/GFC-C-01..07, owns order/cadence; its C-01..06 and the existing
U-02/U-03/U-04/U-11 retain ordinary interaction and lifecycle qualification.

### Audit of required behavior and evidence gaps

The action chain is already **sourced**: routing/contained-hero admission
WGCH-01/02/48; copied choices and prerequisites WGCH-04/49/121..125;
current-owner debit/consumption WGCH-05..07/62/72/124;
mission/direct request and actor removal WGCH-08..10/95/126..128;
transition/removal/revolt WGCH-11..19/51/75/129;
recurring money WGEP-04/12/13/14/115; production WGEP-29/32;
ownership WGM-77/195; visibility WGUI-03/10..16/31/37/40/80;
world effects WGP-19..22/47..49; AI WGCH-03/50/130 and WGA-45.
GHUD-S25..S27 identify the three presentation work boundaries.

The **missing debug-readable evidence** was the default corruption identity,
current beneficiary selection, planet-local automatic upgrade storage,
tactical instantiation and source-keyed bounty lifecycle, plus immediate
initial/override effects versus timed ownership changes. The rows below settle
those boundaries. Bounty arithmetic was already sourced by WGCH-65/WAR-60;
this appendix makes their different mode inputs and rounding explicit.

**Qualified unknowns** remain ordinary stale/disabled-menu submission (U-02),
prerequisite shortfall/ties (U-03), interrupted completion and callback/restore
composition (U-04 and service-map cases), battlefield payout/source cleanup
(U-11), and observed HUD/AI delivery (WGCH-LC-U01..03). Static reads or XML
comments do not turn these into passing captures. Missing implementation is
listed separately below; it is not a claim that the retail rule is unknown.

### Additional sourced boundaries

| ID | Rule and conditions | Evidence |
| --- | --- | --- |
| WGCH-161 | The ordinary completion beneficiary lookup visits the current player-ID table in order and returns the first existing player with an existing faction whose `Benefits_From_Corruption` is true; no match returns no beneficiary. It does not select the planet owner, local player or previously acting hero. WGCH-129 owns the completion call and which rewards skip an absent beneficiary. Multiple benefiting factions therefore need an explicit current-player traversal provider, rather than a fixed Consortium identity. | debug build LC-E01; WGCH-129 |
| WGCH-162 | Automatic corruption upgrades are stored in each player's **planet-specific** list, keyed by the changed planet identity. For every existing player/faction, resolve `Automatic_Planetary_Corruption_Upgrade_Type` for the new type and add it when present, then resolve/remove the old type when present. This is not a globally active upgrade or a list limited to the corruption beneficiary. The stock Empire/Rebel racketeering entry is a generic display upgrade; the stock Underworld entry is the variant with the bounty ability. | debug build LC-E01/E02; XML LC-X01; WGCH-13 |
| WGCH-163 | Land/space starting-force creation reads **every player's** upgrade list for the battle planet. Each entry must resolve, be affiliated with that player's faction and behave as an upgrade in the generic or current tactical mode; admitted entries create an upgrade object owned by that player at the scene origin, with no galactic parent linkage. This reader does not limit traversal to invader/defender, require the player to own the planet or deduplicate entries. Automatic corruption and authored mission tactical rewards both reach this planet-specific interface. | debug build LC-E01 xrefs and decompile; WGCH-15/162; WGT-118 |
| WGCH-164 | `Bounty_On_Faction_Ability` applies only with a valid source object/type and no explicit target. For `Apply_To_All_Allies=false`, use the source object's current owner; otherwise visit current allied players. Insert authored `Bounty_Percentage_Amount` and `Stacking_Category` into the active mode's per-player, per-target-faction bounty ledger, using a unique source identity; removal removes that source/category from the same routed recipient ledgers. Negative authored percentage is reset to **0** during validation; no stock-derived upper bound is established. Stock racketeering authors **1.0**, category **1**, no ally propagation, and Empire/Rebel/Hutts/Pirates targets. Kill-time aggregation/payment remains WGCH-65, independently of whom the ability propagates to. | debug build LC-E01/E02; XML LC-X01; WGCH-65 |
| WGCH-165 | Initial political ownership by a corruption-benefiting faction installs default corruption immediately, refreshes visuals and invokes WGCH-13's ordered effects with the **planet owner** as supplied reward recipient. The campaign starting-corruption override likewise resolves its named planet type, uses the first matching active planet (more than one is debug-invalid), sets default for a positive configured level and none otherwise, refreshes visuals and invokes effects with old type none and the current owner. These are initialization/override interfaces, without the ordinary completion beneficiary lookup or a timed mission. No matching active planet does nothing. | debug build LC-E01/E02; WGM-77; WGCH-13/161 |
| WGCH-166 | Strategic death bounty uses the positive victim bounty value under WAR-60's final-blow/hostility and corruption/NalHutta gates. Its eligible default-corruption system is racketeering (WGCH-168), and payout is nearest-integer bounty directly to the killer's credits and collected tally; this strategic branch does **not** apply the tactical percentage ledger or tactical minimum-one/fly-text branch. Tactical deaths instead read the victim-faction ledger separately for every player allied to the final-blow player, apply WGCH-65's percentage aggregation and minimum **1** after rounding, then local fly text. Mode-specific collection enablement remains the existing death/tutorial interface. | debug build LC-E01; WGCH-65; WAR-60 |
| WGCH-167 | The stock corruption/sabotage AI plan assembles its authored saboteur team, waits for `Move_To(Target)`, calls `Activate_Ability()` **without a name**, then sleeps **10 s**. Only a corruption goal whose target then reports `Is_Corrupted()` is marked successful; the sabotage branch deliberately does not mark success. Ability-local corruption choice still uses WGCH-03/50, and AI uses direct corruption under WGCH-08. The separate removal plan calls the same unnamed activation but marks success without waiting for removal (WGCH-130). Plan success, sleep and transition completion are different interfaces. | Lua LC-X01; WGA-45; WGCH-03/08/50/130 |
| WGCH-168 | The debug-build default corruption classification aliases **racketeering**, not a ninth mission choice or intimidation. WGM-195's nonbenefiting-to-benefiting capture therefore requests racketeering for **10 s** only when not already increasing/corrupted; it aborts on the not-increasing path before checking existing corruption. Benefiting-to-nonbenefiting capture aborts and requests **10 s** removal only if already corrupted; benefiting-to-benefiting does neither. A source-player-to-new-owner siphon match terminates independently. Preserve existing mission corruption on benefiting acquisition when already corrupted; do not replace it with racketeering merely because ownership changed. | debug build LC-EN01/E02; WGM-77/195 |

### Per-type effect and data map

All eight choices share authored slot costs/prerequisites/missions/rewards and
the corruption boolean consumers. Named choices do not imply an additional
unsourced simulation effect. Read the data references; the stock names and
values below are evidence examples, not a runtime whitelist.

| Type | Stock recurring multiplier, WGEP-13 | Additional sourced consumer and live gap |
| --- | --- | --- |
| Intimidation | 0.5 | Travelling-fleet reveal skips intimidated candidate planets, WGUI-31; live transition/type-specific reveal producer missing. Generic automatic upgrade is display data. |
| Piracy | 0.8 | Departure of a nonallied nonstealth fleet accrues positive original-type piracy value, WGCH-60, later collected/reset under WGEP-04; `corruption_fixture::accrue_piracy()` has no live departure caller. |
| Kidnapping | 0.4 | Slot-specific rewards/unlocks and generic display upgrade, WGCH-14/15/125/162; live reward/list producer missing. No additional effect is established here by the type's label. |
| Racketeering/default | 4.5 | Source-keyed battlefield bounty, WGCH-162..164; strategic default-system bounty, WGCH-166/168; live upgrade producer and bounty/death consumers missing. Default applies immediately at benefiting initialization, not through a paid mission. |
| Corrupt militia | 0.5 | Removal to none invokes structure cleanup, distinct-owner surface/orbit retreat and Neutral allegiance, WGCH-17; fixture requests exist, live revolt execution missing. Slot-authored tactical upgrades remain WGCH-15/125. |
| Bribery | 0.5 | Travelling-fleet reveal uses `Bribery_Fleet_Reveal_Range`, WGUI-31. Paid traversal WGM-84/85 is separate from a planet's mission cost; type/status publication is missing. |
| Black market | 0.8 | Planet gate for buyer offers, WGEP-58/WGCH-55; live `execute_black_market()` exists, stock buyer population and corruption transition/actor lifecycle are missing. `Planet_Black_Market_Influences` income reroll is WGEP-14, independent of this choice. |
| Bonded citizens | 1.0 | Authored unlock-unit rewards and removal, WGCH-15/125/129; live player unlock producer missing. It does not imply a universal unit type or permanent unlock. |

For every type, corruption-benefiting factions receive recurring income only
on nonowned corrupted planets, while the owner keeps ordinary income; a siphon
has its own owner deduction, recipient and expiry rules. Production uses the
faction's two-element corrupted/uncorrupted arrays, not a different hardcoded
factor for each mission. Movement uses WGM-21/27/33's corruption predicates;
neither bribery mission costs nor piracy accrual are a generic transit debit.
Tactical mission completion enters WGM-81, not ordinary conquest/capture return.

### Gap table against merged live campaign code

**Missing** means no ordinary live campaign producer/consumer for the named
contract. A fixture helper is named as a reuse point and does not change that
verdict. **Partial** means a live consumer exists but lacks corruption inputs
or downstream effects. These are contract counts, not new implementation issues.

| Required contract and sourced rules | Merged file/function in the live path, or missing | Verdict / implementation seam |
| --- | --- | --- |
| Initial/default/override state, WGCH-165/168 | `src/campaign/planet_economy.cpp::prepare_corruption_economy()` is installed by `src/campaign/live_setup.cpp`; it creates planet rows with corruption none. | Partial: supply benefiting-owned initial racketeering, campaign overrides and immediate effect recipients; retain authored references. |
| Hero routing/contained admission, WGCH-01/02/48 | Missing. `src/sim/galactic/corruption_actions.cpp::prepare()` accepts copied fixture actor facts. | Missing: live enabled-ability lookup, contained actor traversal, object owner/alliance, residual influence and tutorial providers. |
| Choice/menu preparation and refresh, WGCH-04/49/121..125; GHUD-S25 | Missing. `src/sim/galactic/corruption_views.cpp::observed_corruption_menu()` is called only by tests and the viewer fixture. `GalacticSession::Impl::publish()` in `session.cpp` supplies a null corruption menu. | Missing: validated authored slots/first-type getters, live prerequisites, actor/planet identity, briefing/art and information-only menu producer. |
| Human submit, WGCH-123; GHUD-S25 | `apps/viewer/src/galactic/corruption_hud.cpp::choose_corruption()` requires a fixture, records acceptance and closes it; `src/sim/galactic/commands.cpp` has no corruption action payload. | Missing: ordinary input to copied queued action, distinct UI and execution admission. |
| Debit/prerequisite/acting hero mutation, WGCH-05..09/62/72/95/124/128 | Missing. `corruption_actions.cpp::execute()` mutates a standalone fixture; no live caller. | Missing: shared account/death/hero-return/RNG adapters and representative-type consumption. Fixture currently pools alternatives for consumption, whereas WGCH-124 requires a single selected type; do not wire that behavior unchanged. |
| Tactical corruption mission/result, WGCH-08/10/126..128; WGM-81 | Missing. `corruption_actions.cpp::finish_mission()` is a fixture; `src/campaign/tactical_return_live.cpp` handles ordinary battle return. | Missing: authored mission queue/map/script/defender, explicit success/failure result, stored 5 s request and actor lifecycle; no conquest reconciliation. |
| Authoritative transition service, WGCH-11/12/51; GFS-C-02/GFC-C-01 | Missing. `src/sim/galactic/corruption_model.cpp::begin()/complete()` are fixture calls; `GalacticSession::tick()` has no corruption completion service. | Missing: persist one current/target/deadline/last-change owner and service before control under the sourced schedule. |
| Ordered effects/rewards/unlocks, WGCH-13..16/75/125/129/161/162 | Missing. `corruption_model.cpp::complete()` uses supplied beneficiary and fixture sets. | Missing: current player traversal, planet-specific/all-player upgrade lists, shared accounts, story/sound, player unlocks, ability callbacks; do not treat fixture global upgrade sets as planet lists. |
| Fiscal preview/collection/siphon/reroll, WGEP-04/12/13/14/115 | `src/sim/galactic/economy_corruption.cpp::corruption_payments()/reroll_planet_income()` called by `session.cpp` publication/tick; `prepare_corruption_economy()` loads authored percentages/flags/range. | Partial: actual arithmetic and serialization exist; stock transition, siphon deployment/termination and piracy sources do not. Preview must remain nonconsuming. |
| Piracy departure accrual, WGCH-60 | Missing. `corruption_model.cpp::accrue_piracy()` has no live caller in `src/sim/galactic/movement.cpp`. | Missing: original-type amounts, flattening, owner/stealth admission and ordered ledger append at departure. |
| Corrupted production pricing/time, WGEP-29/32 | `src/sim/galactic/production.cpp::prepared_production_offer()` reads economy-corruption state; `live_setup.cpp` provides fresh uncorrupted basis. | Partial: consumer exists; bind authoritative changed state and preserve retained admitted queue prices/durations across cleansing/capture. |
| Black-market availability/purchase, WGEP-55/58/80/81 | `economy_corruption.cpp::GalacticSession::Impl::execute_black_market()` is a live command consumer. `planet_economy.cpp::prepare_black_market_buyer()` has only a test caller. | Partial: populate real buyer/current-owner ability admission, corrupted planet facts, ordinary UI and shared consumed-hero lifecycle. |
| Automatic/reward upgrades into battle, WGCH-162/163 | `src/campaign/space_entry.cpp::space_entry_upgrades()` consumes `SpaceEntryUpgradePlayer::planet`; `space_entry_stock.cpp::StockSpaceEntry::resolve()` never populates that list. | Partial: the stock entry no longer refuses a planet that carries these rows (a retained row with no upgrade object is skipped and kept in the list, WGT-14). Still missing: retain every player's planet upgrade list as its own family, load reachable types, pass it to land/space child providers, including nonparticipants and authored duplicates. |
| Strategic/tactical bounty and tally, WGCH-65/164/166; WAR-60 | Missing. `src/campaign/auto_resolve_live.cpp::outcome_inputs()` supplies lethal closure/record callbacks but no bounty payment; no live source-keyed bounty ability/death consumer was found. | Missing: separate strategic default-system gates from tactical ledgers, final-blow attribution, collection enablement, shared credit adjustment, collected tally and local fly text. |
| Capture/recapture corruption/siphon, WGM-195; WGCH-168 | `src/sim/galactic/control.cpp::GalacticSession::Impl::service_control()` commits ownership without these consumers; production callers do not install `set_control_effects_handler()`. | Missing: old/new benefit classification, abort/10 s request, preservation of increasing/existing corruption and source-equals-new-owner siphon termination. |
| Hero cleansing/revolt, WGCH-17..19/51 | Missing. `corruption_model.cpp::remove()/complete()` calculate fixture debit/removal/revolt requests. | Missing: live target-allegiance/ability admission and actor outcome; apply actual specials/starbase, retreat and political-modifier cleanup in sourced order. |
| Faction intelligence/movement consumers, WGUI-03/10..16/31; WGM-21/27/33 | `src/sim/galactic/observer.cpp::planet_knowledge()` and `observer_service.cpp::GalacticSession::Impl::service_discovery()` read corruption; `movement.cpp` and `graph.cpp::bypass()` read separate planet booleans. | Partial: connect one authoritative transition to observer/movement inputs, typed travelling reveal, source cleanup and discovery persistence. Income permission still has no corruption shortcut. |
| Ordinary AI corruption/removal, WGCH-03/50/130/167; WGA-45 | Missing. `src/script/foc/plan_bindings.cpp` taskforce `Activate_Ability` binding returns without action when no name argument exists; the stock plans use that shape. No corruption-specific live perception/action producer was found. | Missing: current corruption/transition/usage/requirements facts, unnamed galactic ability dispatch, weighted synchronized choice and result separation; stock removal success cannot stand in for cleared corruption. |
| Status widgets, GHUD-S26; WGUI-37/40/80 | Missing. Economy/observer snapshots do not supply an observer-local corruption transition/siphon-recipient/bounty status adapter for these widgets. | Missing: filtered status/content/tooltip facts and authoritative feedback, distinguishing political bounty status from tactical salvage. |
| World effects, GHUD-S27; WGP-19..22/47..49 | `apps/viewer/src/galactic/galactic_scene.cpp::service_corruption()` calls `corruption_visuals.hpp::observed_corruption()` and particle service. The default projection reads only the observer's corrupted boolean. | Partial: live transition/direction/progress and typed adjacency/source facts are missing; fixture particles are not an observed campaign transition. Preserve the sourced separate line-growth clock. |

Contract count: **7 partial, 13 missing, 0 fully covered, 0 live differences**.
The unconnected fixture's pooled-consumption mismatch is recorded inside its
missing live contract, not counted as a changed ordinary campaign behavior.
Economy-corruption canonical state already has encoding/checkpoint coverage;
it is not persistence of the absent action, mission, transition or reward owners.

### Registry and provider requirements

At this base, `GameConstants/Corruption_Choice_Income_Percentage` is **applied**
and faction `Benefits_From_Corruption` is **partial**. The authored faction
automatic-upgrade list, planet slots/rewards/removal base, corruption routing
names and generic-hero corruption ability fields remain **land-or-galactic**;
hero removal common lifecycle fields are **todo**, with sourced time/cost
overrides scope-deferred. `UpgradeObject/Abilities/Bounty_On_Faction_Ability`
name, percentage, category, faction list and ally-propagation rows are **todo**.
Active original-type piracy authoring is `Piracy_Value_Credits`, still
**land-or-galactic** for GroundCompany/SpaceUnit/Squadron. The earlier reader
inventory's shorthand `Piracy_Value` does not authorize reading an invented tag.

Implementation must follow effective campaign/planet/faction/ability/type
references, including inherited variants and arbitrary reachable mod types.
Update exact registry class/tag rows when their live application lands. Pure
display upgrades remain presentation consumers; land mission objectives remain
the authored script/land host; no declaration is marked ignored or applied by
this docs-only audit.

### Ready-to-implement contribution to the sign-off

Existing action, transition, economy, return, AI and presentation owners retain
their work (legacy EAWR-1140, EAWR-1141, EAWR-1130, EAWR-1116, EAWR-1120, EAWR-2003, EAWR-2306, EAWR-1155).
These checklists describe implementation readiness within the full-playthrough
issue, not replacement tickets or approval of its release batch.

| Work | Rules and provider seams | Proposed focused observable test |
| --- | --- | --- |
| Live action/menu/mission | WGCH-01..10/48..50/62/72/95/121..128; live actor/container IDs, slot-first-type getters, current accounts/prerequisites, copied queued payload, synchronized RNG, mission queue and hero-return adapters. | From an ordinary campaign, open/cancel/submit each authored slot, retain unaffordable choices and slot gaps; test enemy-only first unit alternative versus owned second, stale units after debit, occupied mission queue after consumption, hero disappearance and direct/AI/multiplayer versus land/space mission success/failure. |
| Transition/completion/cleansing/capture | WGCH-11..19/51/75/125/129/161/162/165/168; persistent transition owner, current beneficiary traversal, all-player planet lists, player unlocks/accounts, callback/source cleanup, old/new faction and retreat providers. | Fresh benefiting-owned planet starts racketeering; capture clean planet, capture already bribed planet, recapture mid-increase/removal and benefiting-to-benefiting controls. Cleanse corrupt militia and another type; assert no cost on failed start, add-before-remove rewards, owner/beneficiary distinction, source-equals-new-owner siphon cleanup and actual surface/orbit revolt effects. |
| Recurring economy/production/piracy/market | WGEP-04/12/13/14/29/32/55/58/115; authoritative state projection into existing fiscal/production, departure and buyer/lifecycle consumers. | Use all eight types with perturbed authored percentages/factors and renamed referenced types; compare preview versus collection, nonowned versus owned payment, nonsentinel absent siphon recipient, end equality/next frame, nonstealth/stealth/allied piracy, new versus already admitted work, and black-market offers before/after cleansing. |
| Tactical effects/bounties | WGCH-65/162..164/166; player-and-planet upgrade registry, every-player child lists, validated bounty sources, current victim type/faction/final-blow and account/tally/collection providers. | Fight and auto-resolve separate racketeering/default and other-type controls, enable/disable collection, overlap same/different bounty categories, change ally propagation, include a nonparticipant benefiting player and duplicate planet upgrade entries, then cleanse and fight again. Check strategic sub-one rounding versus tactical minimum-one and remove only the relevant source. |
| AI and observed presentation | WGCH-03/50/130/167, WGUI-03/10..16/31/37/40/80, WGP-19..22/47..49; live unnamed activation/perception, observer-local prepared menu/status and authoritative transition/effect publications for GHUD-S25..S27. | Run stock corruption/removal plans through real Lua bindings; test no eligible/affordable AI choice, prior usage weights and success flag before removal completes. Observe E/R/U menu, local/other siphon status, bounty status, transition particles/path and settled lines with fog on; ordinary input must emit the real command, not a fixture acceptance counter. |
| Continuation and deterministic integration | GFS-C-01..05/GFC-C-01..07/C-01..06, U-02/U-03/U-04/U-11; persist payloads, RNG, mission IDs/results, transitions, planet/player lists and source identities through existing session/checkpoint owners. | Save after preparation, debit/consumption, queued mission, end-1 and capture; restore through a fresh host and compare uninterrupted state/events on 1/2/4/8 workers. Rewards, destruction, piracy collection, callbacks, bounty sources and presentation sequences must not repeat. New per-entity service uses copied inputs, disjoint staging and ordered commits under ADR-009. |

### Runtime qualification recipes

| ID | Qualified unknown | Capture recipe |
| --- | --- | --- |
| WGCH-LC-U01 | Ordinary campaign UI reachability and visible/audible S25..S27 composition, including other-faction corruption/removal/status. Static menu, visibility and effect contracts are sourced. | Extend GHUD-C08/C10 with one stock Underworld mission and E/R cleansing controls, fog on: record initial ownership/type, hero/container, all offered slots/costs/requirements, information-only inspection, normal drag/click/cancel/submit, local/other siphon and political bounty tooltips, increasing/completed/removing particles/paths/lines and success/completion audio separately. Repeat at two viewport sizes and after child return; do not infer state or permitted action from an idle icon. |
| WGCH-LC-U02 | Exact initialization/ownership/completion callback composition and fresh-host continuation, including all-player upgrades, recipient changes and interrupted transitions. WGM-195/WGCH-165/168 settle static branch policy. | Extend service-map C-01/02/04: owned default, configured override, clean and already corrupted capture, mid-increase/removal recapture, both benefit-classification directions, same-benefit control and source-equals-new-owner siphon. Record current/target/deadline/last-change, each recipient/list/unlock, account delta, source cleanup, revolt identities and story notices before the next fiscal consumer; pair uninterrupted and saved controls. Existing service-map owner decides frame ties. |
| WGCH-LC-U03 | Observed AI activation/plan outcome and strategic versus tactical bounty/source delivery. Lua success and returned helpers do not prove these live results. | Use existing Lua-debugger/capture APIs to record stock saboteur/removal taskforce destination, unnamed activation, admitted choice, money/usage/actor lifetime, corruption state at plan result and later completion. Fight/auto-resolve matched racketeering and another-type controls, same/different source categories, allied killers and collection disabled; record every final-blow/current-type victim, recipient amount/tally/fly text, then cleanse and repeat after save/load. Reuse U-11 and WAR-U04 instead of asserting per-death attribution from an aggregate treasury change. |


## Live theft provider qualification

The focused debug-build read GAH-C01 compares the authored curve tag kinds and the theft service's curve construction. GAH-C02 follows its common ability constructor. Raw reads and symbol maps remain private research artifacts.

| Rule | Contract | Evidence |
| --- | --- | --- |
| WGCH-169 | Theft's planet-availability, base-level and planet-difficulty curves use the same piecewise-linear evaluator as WGEP-63's political-income curve. Authored pairs replace the previous curve occurrence; distinct points sort by x and interpolate linearly over the bracketing interval. Fractional planet ratings remain fractional inputs. Fewer than two points and values outside the authored domain return zero without extrapolation. Duplicate-x behavior remains WGEP-63/U-03; the live validated provider refuses those inputs rather than choosing a tie rule. | debug build GAH-C01; WGEP-63; WGCH-33 |
| WGCH-170 | The common ability constructor initializes `Initially_Enabled` to true; an explicit authored false remains false. The theft service inherits that constructor and initializes both cost and chance affinity adjustments to zero. An omitted affinity modifier therefore contributes zero; authored values replace the corresponding adjustment. Curve interpolation and numeric representation retain WGCH-169 and the project's fixed-point arithmetic boundary. | debug build GAH-C02; GAH-C01; WGS-88 |


### Live corruption authoring defaults (2026-10-10)

Focused static constructor/tag-default qualification, private evidence GAH-D01
(original type and its byte-member initializer) and GAH-D02 (typed slot parser).
These establish omission inputs, not observed mission or frame execution.

| ID | Rule | Evidence |
| --- | --- | --- |
| WGCH-171 | A fresh original type consumes the unit when used as a corruption prerequisite. An explicit destruction flag of false opts out; named hero status does not change this default. | debug build GAH-D01; WGCH-06/07 |
| WGCH-172 | Each of the three fresh corruption slots starts with no corruption type, no required mission type, zero credit cost, zero required unit count and zero required mission count. Authored replacements remain slot-specific. | debug build GAH-D01/D02; WGCH-04/49 |
| WGCH-173 | Fresh slots have empty required-unit/planet lists and empty briefing/map/script strings, an invalid tactical mode, zero success credit reward and unresolved tactical-upgrade/build-unlock references. Absence of a map/mode therefore cannot fabricate a tactical mission. | debug build GAH-D01; WGCH-08/13 |

### Live ruins adapter: project rules (2026-10-11)

These rows record how the live host applies the sourced ruins rules. They add no
observed behaviour; each cites the sourced rows it implements and marks the
choices that remain unverified.

| ID | Rule | Evidence |
| --- | --- | --- |
| WGCH-176 | The ruined base income is the planet's authored `Planet_Destroyed_Credit_Value`, carried as a per-planet host input and selected by the fiscal owner whenever the control owner's ruined flag is set; it is never a one-time write over the ordinary value. An omitted authored value retains the ordinary base (the constructor default is **unverified**; least visible choice). Blocked-income and modifier arithmetic is unchanged. | project rule; WGCH-143; WGEP-05 |
| WGCH-177 | One trusted ruins request applies WGCH-140's order: story notification, siphon, landed transports in reverse with named-hero requests, ground structures with their planetary influence, land capacity, political modifiers, corruption abort and cleanup, planet-keyed abilities, destroyed flag last. A request for an unknown or already ruined planet applies nothing and journals nothing. A named hero whose container classification is unresolved refuses the whole request **before** any mutation (named seam); partial cleanup is never recorded. The destroy-disabled form omits story, capacity and flag writes. | project rule; WGCH-67/140/141/142 |
| WGCH-178 | Hero requests inside ruins use no explicit planet (WGM-80 then picks the object's current system), zero requested seconds and no admission bypass. A hero requested earlier in the same traversal counts as dead for later parent checks; the carrier and its remaining contents are ordinary destruction and never schedule a return. | project rule; WGCH-141; WGM-79/80 |
| WGCH-179 | Ruins also writes the control and production surface-access flags false, so every existing surface reader (landing, production admission, control, AI perception) observes WGCH-143/147's reader result through its existing provider rather than a second authority. Control residual influence already excludes a ruined planet (WGM-67). Planet model refresh, the destroyed-planet tactical map and other presentation consumers remain presentation seams. | project rule; WGCH-143/147/150 |

### Live hacking: project rules (2026-10-11)

| ID | Rule | Evidence |
| --- | --- | --- |
| WGCH-180 | The host reads the activated hack names, `Hack_Super_Weapon_Required_Type` and `Hack_Super_Weapon_Cost` as authored inputs; a type's hack ability binds only when its authored name is in the activated list, and its service starts enabled unless the authored flag says otherwise (WGCH-170). An absent or unresolved required type never matches (WGCH-109); no other type substitutes. An omitted cost is zero (constructor default **unverified**; least visible, and only reachable when a mod authors a required type without a cost). | project rule; WGCH-109/139/170 |
| WGCH-181 | Execution order after the WGCH-112 pre-debit refusals (missing weapon, insufficient funds): human debit, weapon owner lookup (a missing owner settles refused after the debit, no refund), build lock of the matched CURRENT type for its CURRENT owner, hacker removal with no schedule, the 5 s corruption-removal request when corrupted, shared ruins, orbital fleet destruction with the reverse container/named-hero pattern, starbase clear, forced Neutral. The direct nonhuman route skips the debit and affordability check. The shared ruins and orbit plans are prepared against copied inputs before the debit, so an unresolved named seam refuses before payment (a deliberate host-safety deviation from the sourced order, never observed as partial cleanup). Hacking an already ruined planet repeats the hack-only operations and skips the shared ruins request. Both hack routes (human prepared and direct nonhuman) request that shared ruins with destruction **enabled**, so a hacked planet gets the destroyed flag, the destroyed-planet story notice and its ruined base income (WGCH-140/143). | project rule; WGCH-66/109..112/141; debug build (both hack routes' ruins request) |
| WGCH-182 | The prepared hack payload carries the actor, planet and authored price only; it never carries a weapon. The weapon identity published to the menu is display-only. The acting object must be at the target planet when the menu opens (host rule shared with the other kinds); no opposing-planet, hostility, ruins or credit gate applies to preparation. Faction hack-success sound and the attached particle effect are human-only presentation consumers and stay named seams. | project rule; WGCH-109/111/112 |

### Live strategic destruction: project rules (2026-10-11)

| ID | Rule | Evidence |
| --- | --- | --- |
| WGCH-183 | The host reads `Activated_Destroy_Planet_Ability_Names` as an authored routing list; a type's destruction ability binds only when its authored name is in that list, and its service starts enabled unless the authored flag says otherwise (WGCH-170). The command carries the ORIGINAL supplied object and the planet only; the bearer is resolved afresh at execution (WGCH-155/156), so a transport's contents may change between preparation and drop without a pinned identity going stale. | project rule; WGCH-137/155/156/170 |
| WGCH-184 | Execution order: ability gate and planetary services (WGCH-113/137/138), then the contested-orbit refusal (WGCH-158) when the actor's flag is armed, then the pending leader-kill victory request (WGCH-58/114), then the shared ruins request (WGCH-177). The contested-space flag defaults to armed and is changed only by the journaled authored bracket (WGCH-160). A refusal settles a typed `refused` receipt that carries no fleet identity, type or count. The ruins plan is prepared against copied inputs before any mutation, so an unresolved named seam refuses before the victory request. | project rule; WGCH-58/113/158/160/177 |
| WGCH-185 | Destruction moves no credits, consumes no actor, locks no weapon type, destroys no orbital fleet, clears no starbase and changes no ownership (WGCH-115); it owns no price, so a priced payload is never a destruction capability. A direct execute with no retained preparation is admitted (the queued drop carries no host-issued menu). The target effect, model refresh and sound consumers stay named seams. | project rule; WGCH-115/155/156 |
