# Authored campaign setup boundary

The campaign planner consumes the validated galactic data and original-type table,
the selected definition, explicit player/faction and type/identity bindings, seats
and a session seed. Its owned, immutable plan retains authored locations (including
nonplanet scene objects), campaign-scoped planets/routes, starting-force requests,
required initial placements and faction homes. The graph adapter assigns planet
and route identities; scene-only locations do not become conquerable planets.
The validated loader's WGS-01a behavior gate separates Planet-typed art records
from planetary locations. Name/identity associations come from `GalaxyModel::names`,
and an art record erroneously listed as a campaign planet explicitly refuses.

Setup applies the shared original-type defaults after inheritance: an omitted
`Can_Hyperspace_Without_Activating_Ability` resolves to true ([WGM-100](walks/gc-movement-control.md)),
and an omitted `Build_Requires_Initial_Placement` resolves to false ([WGS-72](walks/gc-session-save.md)).
Explicit and inherited values take precedence, including false movement traits and
true placement requirements. Only referenced starting-force and special-placement
types need fleet traits; provider-expanded bound members resolve on demand from
retained original facts. These defaults admit stock starting-force types such as
`Corellian_Corvette`, `R_Ground_Barracks`, `Rebel_Infantry_Squad` and
`Imperial_Stormtrooper_Squad` through the live trait boundary. Unknown or unbound
provider identities still refuse. A false placement requirement skips marking the
special placement; it does not suppress ordinary starting-force creation.
Placement remains the owning subsystem's typed service regardless of movement traits.

Implemented setup traversal comes from the [session walk](walks/gc-session-save.md):
WGS-01/03/05/08/09/70. All neutral location requests precede placement. For each
planet in authored location order, required special placements are marked once
per original type before starting-force records are visited in their stored order.
Missing/inactive faction owners are skipped. An admitted force request carries
that player's target allegiance of 1.0 before delegated creation. A successful
live creation for a human owner reaches the guard-fleet hook. The inspected debug
hook does nothing; the stock provider preserves each created fleet. Retail
qualification remains WGS-U08. Explicit test services may exercise that hook. Every planet's
political initialization follows the complete force pass; credit-cap updates and
home assignment are subsequent, separate passes. These are startup passes, not
serial additions to the simulation tick.

The placement and political services are typed, explicit boundaries. Startup
refuses a missing service or an unimplemented nested placement branch rather
than constructing one ship per force record or choosing the last force owner as
the planet owner. Returned orbital objects must use the shared allocation
frontier and original-type bindings. Guard unification preserves persistent
member identities/types/owners/locations and unrelated fleets. Unrepresented
authoritative objects cannot consume identities: their owning subsystem must
first supply a persistable model. The full initial political hook retains the
existing political-control model before credit caps and homes. Its stock calculator
consumes complete typed placement presence, excludes trade routes, and sets matching
current/target amount and player (CGS-02; WGM-C04, WGM-66..70). Startup refuses
incomplete state and prior conquest or death journals. The owner-only hook remains
available for explicit subsystem tests; it does not represent stock political
initialization. Startup calculation does not award conquest rewards. Ordinary
control service subsequently applies eligible trade influence through its existing phase.

| ID | Startup rule | Source |
|---|---|---|
| CGS-01 | Home lookup visits stored faction records in order and returns the first case-insensitive match, or no home. | debug build; WGS-09 |
| CGS-02 | Initial political calculation excludes trade contributions and assigns the calculated owner to both current and target allegiance. | debug build; WGS-09; WGM-C04, WGM-66..70 |
| CGS-03 | Delegated creation dispatches separately to base, structure, company, squadron and ship paths; direct ground-base dispatch asserts. Nested objects share the supplied owner and allocation frontier. | debug build; WGS-80/82/92/93; WGEP-84 |

Private debug-build checks CGS-01/02 corroborate two WGS-09 details. The home
getter scans stored faction records in order and returns the first case-insensitive
match, or no home. Initial political calculation delegates to the control
calculator without trade-route contributions, sets current and target allegiance
from that result, and updates the planet's current owner. CGS-03 confirms the
delegated placement dispatcher has distinct base, structure, company, squadron
and ship paths. The stock starting-force provider implements the inspected nested
creation branches of WGS-80/82 and WGEP-84. It creates an ordered transport and
payload for a company, then lands it or allocates its outer fleet after those
objects. Squadron and ship paths allocate the outer fleet first. Base updates
consume no entity ID; special structures retain their own production/census IDs.
Ground specials set base level from their list count, capped at five, and special
control is retained in ground then space order. Inaccessible ground structures
create nothing. Full special lists, direct ground-base dispatch, unknown branches,
missing original/runtime bindings and recursive additional-creation cycles refuse.
An additional result uses the same supplied owner and shared allocation frontier,
including when a primary base update returns no live object.
The authored `Create_Additional_Object` spelling is established by WGEP-84 and
engine-tag evidence ET-T053. It has no stock XML use, so the stock tag-application
registry has no row by design; synthetic authoring and placement tests cover it.

Company and squadron purchase completion (WGEP-41) uses the same nested allocator
as startup. Production retains resolved ordered membership, the transport and
fleet bindings, initial runtime predicates and directed enemy declarations in
its version 2 payload. Completion reads current landed/orbital presence and the
live shared frontier. Newly commissioned contents and original-type metadata
are live state; the tick-zero setup stays unchanged so checkpoints can reconstruct
the queue and repeat the same allocations. Population and owned counts consume
the resulting original identities through the existing census reduction.

The placement context owns movement contents, landed presence, base/structure
state and original census metadata. Every new identity is represented in existing
persistent subsystem models. Initial placement marking uses its production offers;
missing offers refuse. Fresh contexts cannot contain prior forces, queues or
control journals, and competing placement/census/production/guard services refuse.
Runtime ability predicates, movement geometry/access and production admission are
explicit inputs to this provider; their absence cannot become permissive defaults.
WGS-U08 retains the retail placement/save qualification beyond these debug branches.
WGS-U01 is already settled by WGS-69; WGS-U02 custom/menu
admission remains open and no custom option is admitted by this PR.

The coordinator-reserved identity domain `eawr-campaign-setup-v1` combines
validated content identity, selected definition, player/faction/activity bindings
and explicit original-type bindings. Seed and seats use the existing galactic
setup encoding. Created roster/control/identity state uses existing replay and
checkpoint sections, with no new wire numbers or changes to existing corpus pins.
Home/cap receipts belong to the startup context; this PR does not claim to persist
new dynamic home, economy, control or scene state through a checkpoint.

`CampaignRoot` owns the created `GameSession` and setup context. Child battle
entry/return uses that same game and never reruns setup, recreates root players
or reseeds the root (WGS-03/68). Explicit final root teardown releases the owned
game, setup context and cap receipts. The game owns its Lua hosts; UI/audio and
dialog cleanup remain the mode interface of WGS-67/68.

This is the first authored setup change (legacy EAWR-1149). The complete WGS-08 pass chain still
needs credits/tech assignment, multiplayer hero guards, plots/AI, object linking
and influence attachment in follow-up PRs. The live stock factory now prepares
fresh movement, original census, control, production, economic and observer
inputs from authored data and sourced constructor state. `PreparedLiveCampaign`
keeps the initialized root opaque to presentation, publishes its immutable
startup snapshot and transfers that same owner into the live thread. The viewer
uses this path for installed campaigns. The installed-data test creates all three
Equal Footing roots, verifies authored force owners and original identities,
technology targets 2..5, exact tick-zero checkpoints and continuation equality
at 1/2/4/8 workers. The opaque transfer retains the prepared snapshot itself.
Nested production completion remains the named `campaign_production_nested_completion`
seam (legacy EAWR-2349): retained company/squadron offers are unavailable through
the same shared admission used by commands and HUD queries. Unknown required
provider facts refuse startup. Campaigns containing an original type with political
protection refuse through `campaign_political_protection_sources` (legacy EAWR-2359);
its source-keyed transfer/removal lifecycle is required before that ability can
be retained, and startup never sets a sticky protection flag. None of the three
single-player Equal Footing definitions starts Tarkin.
Nested payloads with economic profiles bind their retained original actor identities
to the shared income and production-price provider (WGEP-10/31/73/74). Native actor
writes carry their owner and location through detach, reattach, landing and transit;
loss removes the source. The forecast and payment use the same gathered sources,
and checkpoints retain the actor lifecycle. Political protection remains a separate
startup boundary for multiplayer families that author it.
Dormant developer assets also reference `Large_Campaign_01_Set`,
`P3_Clusters_AI_Set`, `P4_Test_Faceoffs_Space_AI_Only_Set`, `P5_Clusters_AI_Set`,
`P6_Victory_Conditions_Set`, `P7_Hero_Campaign`, `Sandbox_Feature_Demo_Gameplay` and
`Sandbox_P2_Demo`. Installed EAW/FoC campaign registries omit their files;
the P3/P5 hero entries and Feature Demo definition are additionally commented out.
They require a mod to register their files and activate any commented definitions
before they can reach live startup. In particular, the dormant
`Large_Campaign_01_Rebel` authors Mon Mothma without Tarkin; registering it requires
the nested fiscal lifecycle rather than relying on the protection refusal.
Synthetic typed services exercise
ordered startup, three human-faction selections, ordinary/story/tutorial/
multiplayer definitions, refusals, root/child lifetime, checkpoint composition
and 1/2/4/8-worker replay equality. `galactic_starting_forces_contracts` additionally
creates a root with the stock allocator and explicit synthetic presence, checks
its ownership/frontier, and restores its nested state at tick zero under all four
worker counts before comparing continuation.

**CGS-04** is the live launch admission policy: an active human seat requires an
explicit binding to a faction with authored `Is_Playable=true`. A non-playable
binding such as Pirates refuses before planning or simulation-thread startup;
AI and neutral bindings retain their existing roles. Explicit synthetic root
services keep their separate setup contract.
