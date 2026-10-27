# Galactic space entry

The live campaign admits an unresolved conflict. At Fight, `StockSpaceEntry`
copies the current galaxy roster, current station level, owner and ledger into
the pure space-entry builder. Maps, types, models and rule content are prepared
before live ownership; this boundary performs no file reads. The resolved
record is the replay and checkpoint contract. Reloading it binds its exact map
and tactical content instead of splitting or placing the fleet again.

## Identity and placement

WGT-121 qualifies authored map objects before allocating child IDs. A placement
whose signed enable-level byte exceeds the supplied station level is discarded
first, even when its type disables branch discarding. Otherwise an explicit
`Is_Branched_Map_Discardable=false` keeps it. For discardable types, keep the
defender's objects; keep the attacker's only with `Loads_When_Faction_Present`
or an active corruption mission. Owners whose faction cannot control planets
also keep their objects, independently of playability, neutrality or hostility.
Marker types and MARKER in the generic `Behavior` list retain their authored
placement role; `SpaceBehavior` alone does not grant this exception. There is
no separate build-pad exception. Defaults are branch-discardable true,
faction-presence retention false, and enable level zero. The GC builder compares
resolved owner IDs after faction binding. `plan_live_campaign_setup` rejects
duplicate faction bindings, so this also matches retail's faction-qualified
multiplayer path within the current campaign seat model.

The debug-build map-load reader (private evidence `object-load`) requests the
generic behavior query. The membership reader (`marker-mode`) selects the
generic bitmap, and the initialization reader (`behavior-init`) fills that
bitmap from `Behavior`. `SpaceBehavior` fills a separate bitmap. These handles
identify the read-only extracts retained with the WGT-121 research.

This is map admission, not attack protection: a retained construction ship keeps
ordinary hostility, collision and damage rules (WSU-13, WCC-25, WHZ-51). Mission
maps can author the retention flags; no planet or type name selects a special
case. Ordinary non-hazard SpaceProps remain presentation scenery. Resolved child
replays and checkpoints retain the admitted units rather than filtering again.
The shared selected-start builder takes an optional admission filter; skirmish
and attract callers retain their existing defaults.

| ID | Rule | Evidence | Implementation |
|---|---|---|---|
| WGT-121 | GC map objects follow level, branch-retention, defender, qualified-attacker, non-planet-controlling-owner and marker admission above. | debug build: map-load owner predicate (`object-load`), generic behavior membership and bitmap initialization readers (`marker-mode`, `behavior-init`), type flag defaults, signed enable-level getter and map-field reader; XML faction/type flags | `src/campaign/conflict_entry.cpp`; immutable facts in `src/skirmish/inputs.cpp`; optional filter in `src/skirmish/start.cpp`; renamed synthetic map and resolved child replay on 1/2/4/8 workers |

GC type IDs and tactical type IDs are separate domains (ADR-017).
`ConflictForce.type` retains the campaign identity; `tactical_type` binds the
loaded tactical table. Persistent ships, reserves and riders retain their
entity IDs. Station, defence, escort, craft and unique passenger ship identities
come from the shared allocator frontier. No previous health or cooldown state
is imported (WGT-24); the damaged/reentry qualification remains WGT-U04.

WGT-07 selects the defender before the invader. Transports consume a separate
count allowance. Other forces use the three population passes; each bucket
takes its last-listed member first. WGT-08's deliberate project choice is
ascending **campaign type ID**, independent of worker count and hash-table
layout. It can select different last-cap-point buckets from FoC.

WGT-07 walks the remaining members of each type bucket forward into the
reinforcement pool after selecting initial positive-cost units from its back
(debug build EGT-01). Transport overflow is appended during the initial source
roster scan; ordinary bucket leftovers are appended in the final forward pass.
That retains transport source order before the type buckets (WGT-07).
GC records that complete pool order separately from the persistent-ID-sorted
reserve manifest. Campaign setup domain 4 carries it through replay and save/load;
an omitted order retains domain 3 bytes and the default ascending-ID pool.

Station selection uses the conflict-keyed scene stream, separate from subsequent
keyed child services (WGT-SE-U03 remains an original trace qualification). It consumes
one draw only when multiple authored
station markers exist, including level zero (WGT-09).
The created station keeps the selected marker's complete facing, including roll
and pitch (WGT-09, R-ROT-01..03). Only the station type contributes its height
adjustment; the marker type is not instantiated. Three-axis station poses are
staged through the shared yaw placement builder and restored before form-up.
Missing position or orientation still refuses a required station. Existing
yaw-only station placement retains its original path.

Defender placement uses the first defender marker. Ordinary invader placement uses the closest
attacker marker, retaining the first exact tie (WGT-18). Missing station markers
skip the station without a random draw. Missing force markers skip that side's
initial objects and escorts, preserving their campaign identities. With no
defender source, the authored attacker sequence is preserved and starts at its
first marker. Facing is recorded only for created initial objects (WGT-52/53).
The ground prevention-source branch has no stock producer (WGT-62); its mod
producer still needs a copied ground-source DTO.

WGT-113 loads authored map objects before the selected force scene. The station
precedes structures and the retained, galaxy and all-player planet modifier families; those precede ordinary defender
then invader initial forces. Every-specific-marker expansions consume markers
first; ordinary structures then use remembered positions before free fallback
(WGT-11/12/115). Permanent orbital copies have a distinct recorded role and keep
their source structure ID separately from their tactical ID. Multiple copies can
share that parent. The return commit removes a destroyed parent only once;
temporary additions and escorts keep their separate discard policy.
Planetary weapons share that structure gate, use the first special source or
the origin with zero facing when no source exists, and otherwise copy its full
facing (WGT-13/54). Their owner is the planet owner.
Temporary defences have no campaign parent identity and are removed on return.
The station keeps authored garrison spawning enabled, using the currently
loaded Tech0 reserve list. The downward scan for independent starting/reserve
tiers is qualified (WGT-58/59); higher-tech selection remains a loader and
producer seam because the unit table currently retains only Tech0 lists.

Transport passengers remain distinct contained identities (WGT-21).
WGT-122 keeps ordinary ground constituents in that identity tree without requiring
spatial tactical profiles. The company's inherited `Company_Transport_Unit`
chooses one outer transport for the company; its `Company_Units` determines the
carried constituents. This is a type declaration, not a faction-name lookup:
different companies of one faction can select different transports. Existing
hero and unique-ship substitutions remain WGT-21/WHE-49. Transports and other
spatial objects require space profiles; already-prepared passenger metadata
remains in the closure and supplies hero scripts and modifiers. Ordinary ground
constituents without prepared profiles retain their contained identities alone.
A space-allowed unprepared standalone object continues to refuse entry;
space-disabled objects remain retained in the campaign. Initial transports use WGT-07's separate count
allowance, WGT-114's once-per-transport marker cursor and WGT-45..48's rear
formation. Reserve transports retain the same complete identity trees.
Carrier destruction closes over its ground passengers (WGM-55..57/WAR-87),
and surviving or unspent companies keep their original identities through
retreat and return. Replay and checkpoint rebinding reuse these recorded trees;
they never load land weapons or models for ordinary stowed ground units.

| ID | Rule | Evidence | Implementation |
|---|---|---|---|
| WGT-122 | Ground companies enter through their XML-selected transport; ordinary contained ground identities do not require spatial tactical profiles. One company creates one transport with its authored constituents. Transport losses remove carried constituents; surviving and reserve trees remain available for return. | debug build: company space-transport creator (`space-transport`) reads the authored transport and hero substitutions and creates one outer object; contained-object destruction (`transport-death`) destroys each passenger; WGS-80, WGT-07/21/114 and WGM-55..57; XML `Company_Transport_Unit`, `Company_Units` | `src/campaign/starting_forces.cpp` retains the XML-driven company producer; `src/campaign/space_entry_stock.cpp` scopes passenger content roots; `src/campaign/conflict_entry.cpp` accepts contained identities; renamed synthetic entry/reserve/loss/replay/checkpoint/retreat contracts on 1/2/4/8 workers and installed eight-walker company coverage |

Unique ships and eligible flagships host riders; otherwise the transport hosts
them. Temporary unique passenger ships register population without claiming a
persistent fleet parent. Empty transports are retained without a tactical object or reserve
offer. A qualifying transport advances its supplied marker cursor once after all
passengers, wrapping at the end; all-deferred flagship passengers leave it alone
(WGT-114). The flagship uses source-context maximum hull plus shields before
deployment substitution, requires a strictly positive rating and keeps the first
source-roster entry on a positive tie (WGT-119). Squadron source ratings use the
container's own scaled health and shields, independently of its craft (WSQ-60).
Background owner bindings remain in the setup but do not enter its recorded
commandable seats. Child replay extension 8 records the complete
initial carrier trees. The existing Combat_Bonus_Ability consumer sees initial
riders. Stock fleet commander Enhance_Defense_Ability and
Battlefield_Modifier_Ability activation follow WGT-49..51 through the campaign
player ledger.

Commander companies use the same authored nested-company allocator at startup
and production completion. Its transport and payload survive into the prepared
space-entry provider; two stock commanders attach to the initial ship with the
largest positive source maximum hull plus shields. The selection has no test
for an existing hero/officer occupant (WGT-22/119). Riders remain hidden,
noncolliding contained identities, while the carrier publishes them for its
minimal world portrait (WHE-45/47, WU-47/48). Ordinary carrier death destroys
those identities and removes their player effects (WGT-51).

WGT-50 uses the greatest signed contribution in **every** category, including
category zero, then adds category winners. Thus two stock commanders give the
owner one 0.25 defence adjustment and qualifying allies one 1.5 reveal
multiplier. The owner-only defence reaches the owner's ships throughout the
battle, without a radius or source-fleet membership scan; allied players receive
only the effects whose own `Apply_To_All_Allies` permits them. Different
categories still add. Removing one of two equal sources retains the other's
contribution. Renamed synthetic officers cover these distinctions, enabled-state
locks, source death and fresh checkpoint restoration on 1/2/4/8 workers; the
focused installed commander contract covers stock company creation and entry.

A pure map/station/structure prefix is assembled first; upgrade resolution runs
before the full force assembly and reserves modifier IDs before force craft.
Retained upgrades resolve against that pre-force scene in player/object traversal
order. Playable allies qualify using the upgrade owner's faction build list;
previous-holder identity gives no preference (WGT-116). A retained row whose type
has no upgrade object in the scene's tables (a planet's automatic corruption
marker, a mod's galactic-only upgrade) is not instantiated and never refuses the
battle: the original passes a null type over and leaves rows of another game mode
untouched (WGT-14), so entry skips it and the return keeps the row in the
planet's list. Native completed-build
records preserve the holder/source lifetime, with campaign-only relocation on
holder loss. Galaxy upgrades visit invader then defender lists, require affiliation
and a qualified first-level declaration, and deduplicate against all same-owner
pre-force objects and their successor chain (WGT-117). Their native modifier
objects exist at zero pose without a ship holder or galactic parent. Planet lists
visit every supplied player and preserve duplicate entries, without galaxy-level
filters (WGT-118). Nonparticipant modifier accounts have no spending capacity and
are excluded from victory contenders. Resolved objects, roles, attachments and
allocator boundaries use existing scene/replay fields. Commander player ledgers
add two derived caches and the last fog-applied player multipliers to campaign
child checkpoints (state domain 4); restore checks the derived caches against
the live source ledger and derives any pending fog event from the saved applied
multipliers. This preserves a staged source removal saved before the next tick.
Skirmish keeps domain 3 and its
existing content encoding. No new parent save section is introduced.

Nonparticipant ownership uses WGT-123's validated native relationship classes.
Participants, admitted modifier/structure owners and bound neutral scenery are
projected from frozen faction relationships and authoritative live relation
tables. Allied owners share a label; distinct hostile classes receive labels in
ascending owner order after the two combatants. Labels do not encode owner IDs or
claim the original's numeric team values. Each directed ally/enemy/neutral pair
involving a newly bound owner must match the tactical predicate, otherwise the
named `WGT-123 relationship seam` refuses the required modifier/galactic binding.
The already-admitted combatant pair keeps its existing opposing-side contract;
its directed asymmetries remain a separate tactical-model boundary. Unbound map
owners retain the shared phantom, neutral or removal path. A bound map-only NPC
uses a projected label when representable and the existing phantom path
otherwise; a required modifier or galactic-structure owner cannot use that
fallback. Unused galactic factions receive no explicit entry slot, though the
shared map builder can still create background players. Original numeric teams and
faction relationships are separate mechanisms; copying the default team zero
would incorrectly ally tactical enemies in this representation.

## TSW-01 Tactical super weapon start

An object whose effective behaviour list has `TACTICAL_SUPER_WEAPON` (the Death Star) does not keep the pose its
marker or formation gave it. At its first service the debug build finds the nearest enemy-owned object that carries
`DUMMY_TACTICAL_SUPER_WEAPON_TARGET` (the planet backdrop props; nearest by distance from the object, the first on a tie, owner
faction different from the object's). A target owned by the object's own faction is skipped, compared by faction name. A
target whose editor owner index names no faction is not owned by the object's faction, so it counts: the rule as found is
"owner faction different from the object's" and no capture covers an unmapped owner, so the remake reads it literally
(an owner that is no faction differs from every one). With one it stands `TSW_Start_Distance_From_Target` from it, or
`TSW_Attack_Distance_From_Target` when its owner is the defending player (a campaign entry only: a skirmish has no
defender, both sides arrive the same way, so the skirmish start always uses the start distance), on the line from the target through the
object's current position; its height is the target's when `TSW_Start_Pos_Match_Targets_Z` is set (else the line's own),
then raised by `TSW_Z_Adjust_Relative_To_Target_Pos`. With no target it stays put. Either way it then faces the
battlefield centre, keeping only the heading; an object standing exactly on the centre (x and y both zero) has no
direction to face and gets heading zero. Rig capture 2026-10-16 (Coruscant skirmish, script-spawned
`Death_Star_II`): its height was -3100, which is the target prop's authored map height (-4300) plus 1200; the prop's
`Layer_Z_Adjust` (-2500 there) is not added to the target's height. The remake applies the rule when the start is built (skirmish, `place_tactical_super_weapons`)
and after the class form-up (campaign entry, `form_space_entry`). Not modelled: the following move into attack range
(attacker only), the in-range-and-ready state the defender starts in, and the super weapon's attack stages; the
free-space search the debug build runs when a script spawns the object (xy only, not part of a campaign entry).

## Boundaries retained for qualification

- Generic map choice/cache and destroyed-planet backdrop replacement have qualified
  rules (WGT-55..57), but their producers still produce named refusals.
- The mod ground prevention-source producer (WGT-62, no stock producer) remains
  an explicit placement seam. Initial class form-up implements WGT-45..48,
  including the current-pose collision-model bounds, team bounds/offsets,
  balanced mixed fighter/bomber order, screen/lead rings and rear transports
  (EGT-S3-Q01..04). `Ship_Class` is independent of movement layer. A missing
  loader collision box retains the existing unit-box fallback; distinguishing
  a missing model from an existing model without a renderable is a named
  geometry seam, not proof of the native model-less box. Optional
  provider target presence admits the one-shot presentation intro; its coordinates
  never rotate or advance the authoritative formations. The cinematic renderer is
  still a presentation seam.
- Stock automatic commander defence/reveal implement WGT-49..51 through source
  identities and player/category ledgers. Hidden contained riders remain active;
  death removes their contributions. Registration and player qualification share
  the existing partitioned lifecycle phases. Sparse ordered reduction caches
  defence categories and reveal multipliers; ordinary frames discover no recipients.
  A source lifecycle event compares bounded player multipliers with the last
  fog-applied values, then refreshes only affected owners in the existing
  partitioned fog phase (WGT-50). Stationary anchors release their previous
  coverage and mark the changed range; cell service cadence remains unchanged.
  Initially-disabled handlers are skipped, and explicit input type/name locks can
  override the default. The persistent story/corruption lock producer remains a
  named input seam; death spin records never enter the live source roster.
- Garrison content selects each owner's tech (0..5), independently choosing the
  first nonempty starting and reserve tiers downward. Complete tiers replace
  lower lists; missing reserve types contribute zero, and reserve-only types do
  not create rows. Content stores per-owner profiles, so equal station types at
  different owner techs retain distinct selections. Tech metadata is opt-in at
  campaign load and in its content identity.
- Corruption/story-owned planet upgrade producers supply the separate all-player
  list input; entry does not manufacture those records. Tutorial and explicit story map
  overrides, indigenous bonuses, revelation and prebattle cinematics remain
  their owning interfaces. No default producer is inferred here.
- Galaxy-wide planet upgrade declarations are copied and filtered at current
  ownership (WGT-15). Return credits/upgrades use the exact map tables and
  child ledger, then the return lane retains the authoritative planet rows.
- Retained planet upgrades choose the current eligible campaign build-list
  holder before binding the child (WGT-14); absent holders drop the upgrade.

The focused `galactic_space_entry` scenario admits an over-cap movement
conflict, builds its station/weapon/rider scene, checks the allocator and roles,
launches a station garrison and exports/replays both parent and child on 1/2/4/8 workers.
`galactic_space_entry_stock` exercises the authored-map provider against the
installed FoC XML and maps, stages a unique passenger ship, and installs the
campaign AI host. Native retreat snapshot eligibility drives its retreat goal
and withdrawal command (WGT-33, WR-45/46/50..56).
The immutable cinematic seat setting disables playback for the AI (WR-50,
FC-R02). The engine completes its escape at initialization without a playback
command. The proof reaches escape and committed battle end without a viewer,
then compares its hashes and replay bytes on 1/2/4/8 workers.
The prepared campaign metadata is projected to each battle's reachable types:
map objects, deployed forces, reserve pools, riders, applied upgrades and the
transitive campaign production, squadron, spawner, ability and projectile closure.
Projectile and craft/spawner indices are remapped without altering their data.
Restore, return, AI and presentation derive the same subset from the resolved
record; no mutable cache or live filesystem read supplies their content.
Capability gates run on that subset. Seeded rocket offsets and post-aim
extensions use the same RFL-03/05 flight consumer as other tactical entry.
Nonpositive rocket curve distances refuse with every affected reachable
projectile named; oversized collision meshes (DG-36..38) also refuse by name.
Unrelated future/story prototypes stay outside the battle.
The stock proof checks exclusion, transitive craft retention and exact rebinding.
Neither test claims visual or unresolved retail
qualification.

## Explicit fleet pathfinder

WGT-06/40 and GHUD-R43/44 supply the explicit fleet-slot selection. The owning
active human seat sends `SetFleetPathfinder` for an idle orbital fleet and a
direct member, or zero to clear it. The admission validates the whole request
before changing flags; replacement clears the other flags in that fleet.
Eligibility comes from the content provider's space-participation fact or
transport representation. Nested passengers and foreign members cannot be
assigned through this command. Pause admission uses the fleet-management policy.

The flag belongs to the persistent member and survives movement, member transfer,
replay and checkpoints. Preserving it across these operations is the project's
explicit retention policy; GHUD-R44 only captured one refresh that cleared the
visible retail slot. It did not establish a normal movement or save reset.
Merged fleets may retain several selected identities; WGT-06 consumes the first
qualifying flag in source order. An explicit assignment replaces them together.

The space-entry provider copies current flags alongside roster order. Only the
invader uses them: its selected member and every always-initial superweapon
start, with other forces reserved. A missing flag retains WGT-07's population
split. A preferred story type remains a separate input; no untouched fleet gains
an automatic selection. `Preferred_Pathfinder_Types` and land entry remain with
the land provider; this change adds no land-battle rule or viewer gesture.
