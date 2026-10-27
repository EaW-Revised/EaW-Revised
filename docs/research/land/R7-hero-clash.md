# R7: land hero clash

This walk defines the hero-clash boundary needed by land targeting, abilities,
movement and presentation. It extends [land combat](../../behaviour/walks/land-combat.md)
(LC-27, LC-69), [land abilities](../../behaviour/walks/land-abilities.md) (WLAB-78)
and [heroes](../../behaviour/walks/heroes.md) (WHE-57). It is research, not an
implementation or a claim that a retail capture has passed.

Evidence is read-only inspection of the debug build and the effective XML.
Opaque evidence IDs below resolve to private inspections in ignored
`out/research/gc-land-r7/`; binary locations and engine identifiers stay there.
Authored examples describe the inspected data, not limits for mods. No rig capture
or compiled build was run.

## Admission rules

| Rule | Retail rule | Evidence summary | Open item closed | Planned decision |
|---|---|---|---|---|
| LR7-01 | Clash pairing follows the data's `First_Hero_Type` and `Second_Hero_Type`, rather than a fixed list of named heroes. The melee initializer adds its own hero-identity gate (LR7-05). The automatic search compares squared position distance inclusively against `Clash_Range` squared. | debug build R7-E01; XML R7-D01 | R7 admission gap; no existing numbered U-item. | L2: load type references and clash data; P5: automatic pairing. |
| LR7-02 | A candidate is refused while jetpacking, dead, a death remnant, pending deletion or already in a clash. An existing attack target must have the configured opposing type; no attack target also passes this predicate. This predicate alone does not prove fog, alliance or containment policy. | debug build R7-E04 | R7 admission gap; remaining policy is U-R7-01. | P5: eligibility; L4/L5: jetpack and movement-state interface. |
| LR7-03 | A team is admitted to the candidate list only when it is nonempty and every member passes LR7-02. For a pair in range, each team expands to all of its members, while a standalone candidate contributes itself. Before a clash is created, every submitted participant must be nonnull and free of another clash. Failed initialization destroys the attempted clash; only successful initialization adds it to the active list. A successful pairing removes that second-side candidate from the current search list and ends that first-side candidate's inner search. | debug build R7-E01, R7-E02, R7-E05 | R7 group-membership gap; no existing numbered U-item. | L2 and team-container foundation: preserve member identities; P5: ordered admission and reservation. |
| LR7-04 | `Clash_Type` selects two separate implementations: `Melee_Duel` and `Action_Driven`. An unsupported kind produces no clash. Their admission and completion rules must not be conflated. | debug build R7-E06 | R7 kind-dispatch gap; no existing numbered U-item. | L2: clash kind and data; P5: distinct lifecycle implementations. |
| LR7-05 | The melee initializer resolves one object for each configured type, requires both to have hero identity, rejects equal owners, and requires the first owner's player to regard the second owner as an enemy. It snapshots each participant's current health before starting movement. | debug build R7-E07 | R7 melee-admission gap; runtime qualification remains U-R7-01. | L2: hero identity; P5: owner relation and initial-health snapshot. |

## Membership and targeting

| Rule | Retail rule | Evidence summary | Open item closed | Planned decision |
|---|---|---|---|---|
| LR7-06 | Engaged state and same-clash identity are different queries. Same-clash succeeds only when both objects are engaged, the active land manager can find the first object's clash, and that clash contains the second object. Two independently engaged heroes are not automatically compatible. | debug build R7-E17, R7-E23 | Resolves the membership boundary behind WLAB-78; no existing numbered U-item. | P5 ability framework and Force powers: carry clash identity, not only a Boolean. |
| LR7-07 | Ordinary land target suitability excludes engaged targets (LC-27). The inspected special-ability target predicate instead admits an engaged target when caster and target share the same clash (WLAB-78, WHE-57); its other enemy, type and model gates still apply. These are targeting rules, not evidence for universal damage immunity. | Existing debug-build rules LC-27, WLAB-78, WHE-57; R7-E17 resolves identity | Resolves R7's targeting interface; outside damage remains U-R7-04. | P5: preserve per-consumer eligibility; land targeting and Force-power adapters. |
| LR7-08 | Melee admission disables the participant's targeting behaviour when present and sets engaged state. A failure applying the second participant's initial locks undoes the first participant's initial locks. Movement lock and later combat locks are separate transitions. | debug build R7-E07, R7-E08, R7-E24 | R7 lock-lifetime gap; no existing numbered U-item. | P5: owned lock lifecycle; L4/L5: movement-order admission. |

## Melee movement and exchanges

| Rule | Retail rule | Evidence summary | Open item closed | Planned decision |
|---|---|---|---|---|
| LR7-09 | The melee rendezvous is centered on the participants' initial midpoint. Its two destinations differ along the world X axis by `Combat_Distance`, half on each side. The first participant takes the lower-X destination only when initially below the midpoint's X; otherwise it takes the upper-X destination. Each receives a separate movement coordinator, is removed from earlier coordinators, and then gets its movement-lock flag set. | debug build R7-E09 | R7 rendezvous gap; obstructed/uneven terrain remains U-R7-03. | L4/L5: coordinated movement and reservation; L1: terrain integration; P5: rendezvous state. |
| LR7-10 | In the normal moving path, both locomotors must cease reporting movement before the spacing check. Fighting begins directly when the absolute difference between squared separation and `Combat_Distance` squared is at most **0.5** (code constant); otherwise the clash enters position adjustment. This is a squared-distance tolerance, not a half-unit distance band. Missing locomotors assert and fall through to fighting; that exceptional path is not proof of normal arrival. | debug build R7-E09 | R7 arrival gate; obstruction remains U-R7-03. | L4/L5 and P5: explicit moving/adjusting/fighting states. |
| LR7-11 | Adjustment stores starting poses and interpolates both positions toward the rendezvous positions and both facings toward each other. At its recorded end frame it enters fighting. The inspected interpolation writes object positions directly; this does not establish obstacle clearance or terrain projection. | debug build R7-E10 | R7 pose-adjustment gap; terrain and missed-end-frame cases remain U-R7-02/03. | L1/L5: pose/terrain seam; L6: interpolate the authoritative pose; R1: scheduled service guarantees. |
| LR7-12 | Entering fighting faces both heroes toward each other and disables their idle and locomotor behaviours when present, in addition to the initial targeting disable and movement flag. The inspected lock helpers pair these disables with enables on release; they do not directly disable the weapon behaviour. | debug build R7-E11, R7-E24 | R7 combat-lock gap; overlapping external disables remain U-R7-04. | P5: track lock ownership; L5/L6: locomotor and animation handoff. |
| LR7-13 | Combat exchange weights are `First_Hero_Win_Exchange_Chance`, `Second_Hero_Win_Exchange_Chance`, and the remainder for a draw. Before choosing, the previous combat result is removed and the remaining weights are summed for the draw. Thus authored chances are exchange weights, not a whole-duel win probability, and with valid remaining weights, the previous combat result is excluded from the next weighted choice. A pending conversation can instead take a non-damaging exchange when `Play_Conversation_Events` permits it. | debug build R7-E14; XML R7-D01 | R7 outcome-selection gap; malformed weights and conversation runtime remain U-R7-05/06. | P5: exchange state and deterministic random choice; L6: conversation/animation integration. |
| LR7-14 | An exchange selects the authored win/lose, draw or conversation animation for each participant. The paired animation kinds must have equal variant counts; one variant index is drawn for both. Selected animations must exist and have equal frame counts and frame rates. Failure asserts rather than supplying a demonstrated replacement animation. | debug build R7-E12, R7-E14 | R7 animation-pairing gap; invalid assets remain U-R7-05. | L2: validate referenced content; L6 and land animation research: matched playback. |
| LR7-15 | A damaging exchange applies damage once when the first hero's animation completion reaches **0.8** (code constant, inclusive). Completion at the end of the animation also supplies 1.0. The exchange latch prevents repeated damage on later services. Draws and conversations have no exchange loser, hence no damage from this path. Death after the exchange hit ends the clash immediately. | debug build R7-E12 | R7 damage-trigger gap; exact global frame ordering remains U-R7-02. | P5: authoritative animation progress and one-hit latch; L6: display that progress without owning damage. |
| LR7-16 | The exchange hit is rounded to the nearest integer from `(loser's health at clash admission * Damage_Percentage + Damage_Amount) * winner's configured damage multiplier`. It is sent through ordinary miscellaneous-damage handling with the exchange winner as source. The percentage basis is the snapshot of current health at admission, not current health at each exchange or maximum health. The first/second multiplier follows the winner's type. | debug build R7-E07, R7-E13; XML R7-D01 | R7 damage-formula gap; damage-pipeline interactions remain U-R7-04. | P5: retain per-participant snapshots; shared damage pipeline; P6: preserve persistent casualty identity. |
| LR7-17 | In the inspected melee data, `Clash_Range` is 100, `Combat_Distance` is 17.65, `Damage_Amount` is 0, `Damage_Percentage` is 0.15, and the first/second damage multipliers are 2.5/1.0. Exchange weights start at 0.40/0.30, with 0.30 left for a draw before LR7-13's exclusion. These are authored examples, not engine constants or mod bounds. | XML R7-D01; consumers R7-E01, R7-E09, R7-E13, R7-E14 | R7 data-versus-code distinction; no existing numbered U-item. | L2/P5: load parameters; do not bake stock values into simulation. |
| LR7-18 | Melee finish releases locks according to the current state, records a participant or no winner, marks finished, queues the authored winner speech when available, and notifies the manager. Destruction of either participant clears its exchange references and finishes with the other participant as winner. The manager ends that clash's cinematic, removes the active entry, and destroys the clash. Adjustment-state destruction and overlapping locks still require qualification. | debug build R7-E15, R7-E16, R7-E22, R7-E25 | R7 ordinary-completion gap; exceptional cleanup remains U-R7-04. | P5: cleanup and winner state; L6: cinematic/audio teardown; P6: casualty/result handoff. |

## Action-driven clashes

| Rule | Retail rule | Evidence summary | Open item closed | Planned decision |
|---|---|---|---|---|
| LR7-19 | Action-driven initialization accepts the submitted participant list only when it represents two distinct player owners, with the first owner's player regarding the second as an enemy. A third owner is rejected. This initializer does not repeat the melee initializer's per-participant hero-identity test. It stores all participants and applies its initial locks. | debug build R7-E18; manager gate R7-E02 | R7 group-clash ownership gap; runtime admission remains U-R7-01. | L2: team/member identities; P5: group-clash state distinct from a melee pair. |
| LR7-20 | While no winner is stored, the action-driven service finds participants without active actions, tries to create an action for each, and services the active actions. Failure to find an action reports a warning; that branch does not by itself finish the clash. Individual action effects are a further boundary, not the melee exchange-damage formula. | debug build R7-E19 | R7 action scheduler boundary; action internals remain U-R7-06. | P5: action records and lifecycle; reuse the land ability, movement and projectile services. |
| LR7-21 | On participant destruction, an action-driven clash removes that participant, tells active actions about the loss and clears actions that report completion. When all remaining participants belong to one owner, a synchronized random survivor is chosen as winner, the active actions are cleared, initial locks are released and the manager is notified. It does not necessarily end on the first casualty in a multi-member clash. | debug build R7-E20, R7-E21 | R7 group-casualty gap; last-survivor/simultaneous loss remains U-R7-04. | P5: group completion; P6: survivors and casualty identity; L6: cleanup. |
| LR7-22 | The inspected XML contains one melee record and two action-driven records with `Clash_Range` zero; the latter contain 7 and 13 action entries. Zero range is not evidence of an unconditional disabled flag: the inspected automatic comparison still admits exactly coincident positions. The two action-driven records share a name; the inspected loader appends constructed records to a list without a visible name-deduplication step, but constructor-level naming effects were not inspected (LR7-27). | XML R7-D01; debug build R7-E01, R7-E29 | R7 stock-versus-supported distinction; loading and automatic starts remain U-R7-05/06. | L2: authored records and parser policy; P5: preserve engine-supported action-driven clashes for mods. |

## Scheduling boundary

| Rule | Retail rule | Evidence summary | Open item closed | Planned decision |
|---|---|---|---|---|
| LR7-23 | The inspected automatic initiation routine has a local `frame % 15 == 0` gate (code constant). The active-clash service itself iterates the active list without that gate. These local observations do not establish land phase placement, effective calls per second, first-frame normalization or same-frame order relative to unit movement, abilities and damage. | debug build R7-E01, R7-E03; LC-69 supplies only the existing coarse boundary | U-LC-6 stays open; U-R7-02 belongs to R1. | R1/frame map decides phase and dependency rules before P5 installs a service; no independent cadence implementation is authorized by this row. |

## Further lifecycle boundaries

| Rule | Retail rule | Evidence summary | Open item closed | Planned decision |
|---|---|---|---|---|
| LR7-24 | The melee release helper undoes movement and initial locks for the moving state, and combat, movement and initial locks for fighting/finished states. It has no adjusting-state release branch and asserts there; finish also asserts outside moving/fighting before continuing. This inspection does not prove that destroying a hero during adjustment leaves permanent locks, because the full destruction/teardown chain was not inspected. | debug build R7-E15, R7-E25 | Narrows U-R7-04 to an explicit exceptional-state capture; does not close it. | P5: cover every owned state in cleanup; qualify retail adjustment-state destruction before declaring a fidelity difference. |
| LR7-25 | Action-driven admission uses a shared object-lock helper, sets engaged on every participant and also on its parent when the parent is a team. Coordinatable participants receive individual coordinators aimed at their present positions. Release calls the helper to unlock survivors and clears participant and team-parent engaged flags. The helper's complete disable/restore semantics and last-member parent cleanup remain unverified. | debug build R7-E26 | R7 action-clash membership propagation gap; U-R7-04 retains exceptional cleanup. | L2/team-container foundation: parent/member state; L5: coordinator and lock interface; P5: action-clash lifecycle. |
| LR7-26 | For a free participant, action creation evaluates every authored action against that participant and the clash's participant set; only strictly positive relevance entries are retained, with their chosen targets. Selection is weighted by total relevance. The chosen template is copied and started with source, target and the remaining objects; a failed start destroys that attempt. Successful starts become active records; completed actions receive cleanup and are removed, and finishing the clash cleans all active records. Per-action relevance formulas/effects remain a separate inspection. | debug build R7-E27, R7-E28; XML R7-D01 | R7 action-selection/cleanup gap; U-R7-06 retains effect semantics and retail execution. | P5: cloneable action state and deterministic weighted selection; L2: authored templates; R1: per-action service timing. |
| LR7-27 | The clash database initializer reads the supplied database and constructs/appends a clash record for each node it advances through. Automatic initiation consumes the resulting type list. This supports a data-defined catalogue, not a compiled stock-pair table; constructor defaults, reference resolution and duplicate-name semantics remain unverified. | debug build R7-E29, R7-E01 | R7 catalogue-discovery gap; U-R7-05 retains parser policy. | L2: data-driven catalogue and content identity; P5: enumerate loaded records. |

## Integration decisions

The programme's R7 entry had no dedicated U-row. The rows above therefore say which
R7 gap they resolve rather than claiming closure of unrelated questions. U-LC-6
(same-frame races) remains owned by the land frame-map work; U-LAB-8 (other handler
internals) remains open. This walk does not close either item wholesale.

L1's terrain query and L4/L5's ordinary path and movement services remain reusable
interfaces. P5 adds explicit clash membership and lifecycle state; L6 renders that
state and the authored animations. Pair types, range and action parameters remain
data-driven. The programme still needs an implementation owner for hero clash;
its research entry does not allocate a dedicated implementation ticket.

## Unverified behaviour and exact follow-up captures

No capture below has been run. Use the existing battle-session debugger API for
logical state; retain a short lit video and per-frame positions, membership,
health and command outcomes. Keep fog off except in the explicit fog test.

| Item | What remains unknown | Capture that would settle it |
|---|---|---|
| U-R7-01 | Retail qualification of the verified enemy-owner admission, plus fog, containment and multiple-instance boundaries. | Stage one configured pair just inside/on/outside its authored range, then repeat allied, enemy, fogged, garrisoned, jetpacking and already attacking a different type. Add two instances on each side and record which instance pairs and why each rejected one remains free. |
| U-R7-02 | Global phase placement, first eligible frame and same-frame cancellation/damage ordering. | Land frame-map work first supplies the schedule and dependency rule. Then issue range entry, attack, damage and deletion at the same logical boundary; record object services and clash state before/after the scheduled manager turn. U-LC-6 remains open. |
| U-R7-03 | Rendezvous failure, crowded or uneven terrain, placement correction, and a participant that cannot use a coordinator. | Stage the melee pair on flat ground, opposite sides of a ridge, across a wall and with both rendezvous slots occupied. Record coordinator destinations, collision/path status, Z, movement lock and the moving/adjusting/fighting transition. Repeat with a test carrier that lacks the expected locomotor. |
| U-R7-04 | Damage from outside the clash, invulnerability/armor/shield interaction, overlapping disables, destruction during adjustment, simultaneous destruction and cancellation/teardown. | At moving, adjusting and fighting stages apply one known direct hit, splash hit and ability hit from a third unit; repeat with authored protection and preexisting disable sources. Destroy either/both participants at each stage, then test survivor move/attack/ability orders and inspect lock counts, membership, manager entries and cinematic/audio teardown. For an action-driven group, kill one member, then the last enemy, then all members at one boundary. |
| U-R7-05 | Constructor-level duplicate-name effects, invalid weights/unknown types, missing or mismatched animation pairs and rounding ties. The list loader was read; record-parser defaults were not. | Use a separate test mod with two same-named records that have distinct ranges, then reverse their order; test a missing type, unsupported kind, omitted field, zero/negative remaining exchange weights, missing variant and unequal animation lengths. Record loaded record identity, selected outcomes, assertions and ability to resume orders. Use a half-integer computed exchange hit to determine rounding ties. |
| U-R7-06 | Retail visibility and full execution of action-driven clashes; per-action relevance formulas, move/attack/ability effects and conversation sequencing are not yet certified by a runtime observation. | Give a test action-driven record a positive range and distinct name, stage its configured pair/group, and record each chosen action with health/distance inputs, locomotion, projectile creation, ability activation/cooldown and membership until completion. Separately enable conversations on the melee pair and step speech start/end, exchange choice and damage. Repeat without altering the stock zero-range records to distinguish authored accessibility from engine support. |
| U-R7-07 | Save/load, replay continuation, MP equality and campaign persistence while a clash is active. | Save and reload in moving, adjusting, before/after the damage latch and mid-action; compare participant IDs, initial-health snapshots, previous exchange, animation progress, locks, action state and outcome. Replay the same commands at 1/2/4/8 workers and reverse partition order in the remake; retain paired retail observations for the saved clash and GC survivor return. |

## Data handoff and scope limits

The loader work needs `Clash_Type`, `First_Hero_Type`, `Second_Hero_Type`,
`Clash_Range`, `Combat_Distance`, `Damage_Amount`, `Damage_Percentage`, the two
`*_Hero_Damage_Multiplier` and `*_Hero_Win_Exchange_Chance` fields, authored
win/lose/draw/conversation animation types, winner speech and
`Play_Conversation_Events`. The inspected data also author `Involved_Hero_Types`
and `Clash_Actions`; this walk does not equate the former with the automatic
search, which explicitly uses the first/second pair.

Action data include `Action_Object`, `Action_Target`, `Action_Relevance_Type`,
`Action_Health_Percentage_Curve`, `Action_Distance_From_Target_Curve`,
`Ability_Name`, `Ability_Cooldown_Time_In_Secs`, authored movement/jetpack and
animation fields, and attack projectile, shot-count, accuracy, firing-bone and
sound fields. Authorship alone is not proof of each consumer's semantics. No tag
registry status is changed by this documentation-only walk.

For the future implementation, clash identity, participants, initial-health snapshots,
state, lock ownership, previous combat result and damage latch belong to simulation
and replay/checkpoint state; animation rendering belongs to L6. Partitioned work must
read immutable inputs and commit competing admissions and effects in a stable order,
with keyed random draws and 1/2/4/8 equality. This is a project architecture decision,
not a claim that the original implementation uses that execution model. The existing
land pathfinding ticket (legacy EAWR-1451) needs a movement/lock interface, not a second
clash-specific pathfinder. Terrain physics/LOS and the rest of U-LAB-8 remain outside
this walk.
