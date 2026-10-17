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
| WGS-180 | Starting starbase records are applied in authored order. Each admitted record replaces the planet's starbase type, level and supplied faction owner, even at an equal or lower level. There is no first/highest-level selection or planet-controller faction filter. The debug build diagnoses a non-increasing level, then continues to the unconditional base update. | debug build; starting placement and base initialization; WGS-70/80 |
| WGS-181 | Authored starting companies may leave transports of different owners in the same ordered surface list. Surface linking appends the transport without filtering against a prior transport owner or changing its owner. The trailing debug owner-consistency check is bypassed during startup; its later mismatch diagnostics do not rewrite the list. Identity validity and the shared authored ground-force capacity still apply. Downstream mixed-list readers are qualified by WGS-186..188. | debug build; company placement and final surface linking; WGS-80/93 |
| WGS-186 | A nonzero ground base supplies political controller and ground occupation. Without it, political control selects the first non-delete-pending linked transport, while ground occupation selects the first linked transport. The troop military contribution is zero until a non-delete-pending entry supplies one; an all-deleting list supplies neither troop controller nor military presence. Mixed-owner diagnostics do not replace either result with a consensus or last owner. Default invasion defense instead uses the ground base, then target political allegiance; only the invalid-player sentinel or neutral target falls back to the first linked transport. Our zero target represents that sentinel; unknown nonzero player identities refuse world admission before these readers. Explicit invasion defenders remain authoritative. | debug build; political presence reduction, ground occupation and invasion admission; WGM-66, WGM-C01 |
| WGS-187 | Surface linking appends the arriving transport with its own owner, even when an earlier transport has a different owner. No same-owner or alliance filter exists in the link. Caller-specific safety, invasion, accessibility and capacity gates still apply: automatic AI safety scans every nonnull surface entry for nonalliance, independently of the ground-occupation result. | debug build; surface linking and AI safety; WGA-407/410/413 |
| WGS-188 | Undefended invasion cleanup retains transports owned by the recorded defender and removes other surface owners before linking the admitted invaders in their retained order. This differs from invader battle-victory cleanup, which removes remaining non-invader surface entries. Each scan tests the individual transport owner rather than treating the first owner as the entire list's owner. | debug build; undefended invasion and completed land auto-resolve branches; WGM-53/186 |

Route admission follows the campaign's live endpoint types, with the following
qualified rules. A defined endpoint type outside the selected campaign differs
from an undefined type, which still refuses. Case differences resolve to the same
type; a derived type retains its own identity and does not stand in for its parent.
Skipped routes allocate no graph/name identity and add no movement edge. Their
original definitions remain in the validated content digest. Admission warnings
are carried by the model and setup plan, outside persistent graph state.

| ID | Route endpoint rule | Evidence and qualification |
|---|---|---|
| WGS-182 | With exactly one endpoint present, omit the link and continue campaign admission. Emit `campaign_route_external_endpoint` as a warning naming the campaign, route and absent endpoint. | Debug build: the live endpoint selects the route, reports a data-error dialog and a positive-match-count assertion, then guards insertion on a nonempty match list. Shipping continuation is inferred from the coordinator's report that the affected mod campaigns run in retail; removal of the assertion in a release executable was not independently verified. |
| WGS-183 | With both defined endpoint types absent, no live planet selects the route; omit it without an admission warning. | Debug build: route selection compares each endpoint's exact type identity to the current live object's type before looking up the opposite endpoint. |
| WGS-184 | Endpoint lookup uses exact type identity, not ancestry or position. When multiple live instances match, choose the first in live-object order (the last-created matching instance) and emit `campaign_route_duplicate_endpoint`. | Debug build: object creation follows Locations order; creation inserts each new live object at the front of the object list; lookup appends exact-type matches in that traversal order. Linking asserts that one match exists, then takes the first if execution continues. Shipping continuation has the same inference qualification as WGS-182. Duplicate XML definition winners and higher-level setup name bindings are separate boundaries. |

WGS-185: For a repeated unordered endpoint pair, retain the first route in
campaign-authored route order and ignore later routes with the named warning
`campaign_route_duplicate_pair`. The retained route supplies its original factor
and graph identity; ignored definitions allocate no identity. The debug build's
link insertion scans existing links and returns before inserting an already
linked other planet. This differs from repeated route **names** in the XML
registry, whose definition winner remains separately tracked (legacy EAWR-2380).

The assertions/dialog in WGS-182/184 remain a debug-build fidelity distinction;
this adapter implements the coordinator-directed shipping continuation rather
than turning those debug diagnostics into a campaign refusal. Undefined endpoint
types and invalid speed factors on admitted links continue to refuse. Speed
factors on omitted links do not participate in graph construction.

`campaign_route_duplicate_endpoint` is emitted per ambiguous route endpoint
lookup and names that route and endpoint. Duplicate location entries that no
route references do not produce this route diagnostic.

Full setup deliberately retains its duplicate-planet-name refusal. The inspected
debug setup creates every authored `Locations` entry, then revisits those entries
for starting forces. For each it finds exact-type live planets, asserts that
there is exactly one match and selects the first match if execution continues.
Consequently continuation would place forces repeatedly on the last-created
matching planet, rather than once on each created instance. The effective stock
data has no duplicate locations across its 33 registered campaign definitions.
Shipping continuation for duplicate `Locations` at this setup boundary remains
**unverified**; the route continuation qualification in WGS-184 does not establish
it. The planner refuses before producing aliased location bindings or partial
placement. WGS-184 therefore qualifies graph admission only when duplicate
planet names are authored; accepting full setup needs separate evidence for
creation, repeated placement and home resolution.

The stock `Campaigns_Underworld_GC.xml` authors `Empire_Star_Base_2` followed by
`Underworld_Star_Base_2` on Bothawui and Mandalore in `Sandbox_Origin_Rebel`.
`Sandbox_Origin_Empire` authors `Rebel_Star_Base_2` followed by
`Underworld_Star_Base_2` on Honoghr, Kashyyyk, Korriban, NalHutta, Saleucami
and The_Maw. WGS-180 retains the authored records: the
later Underworld station wins. The implementation applies this rule to every
authored type and planet; data overrides remain the way to change these campaigns.
The Origin definitions also author companies of different factions on the same
planet. WGS-181 preserves these records and their individual owners through
startup, ordinary control and checkpoint restore.

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

## Fidelity list

- **Verified (WGS-181, WGS-186..188):** mixed-list political control, ground
  occupation, default invasion defender, land-conflict capability, surface-link
  admission, AI safety and owner-filtered invasion cleanup were compared with
  the debug build. Controller deletion filtering, target-defender precedence,
  owner-unfiltered landing and defender-preserving undefended cleanup now match
  the inspected branches. AI force/intelligence publication retains individual
  owners (WGA-184/186/303); land-conflict admission retains its first capable
  entry rule (WGM-107). Full tactical loss/listener, retreat and failed-link
  composition remains qualified by WGM-PC-U01; this does not close that capture.
- Level-0 starbase placement remains **unverified** and stays refused at this
  boundary; WGS-180 covers admitted positive-level stations.
