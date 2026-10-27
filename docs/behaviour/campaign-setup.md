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
| CGS-05 | Campaign load resolves each `Starting_Forces` and `Special_Case_Production` location and unit type by name. An unresolved name is reported (log line and error popup; an assert in the debug build) and the record is kept with a null type. The starting-force placement pass skips a record with a null unit type (assert only), and a null location type never matches a planet. A record naming a type that resolves to no object is therefore skipped with a diagnostic and the campaign starts. A type that exists resolves whatever galactic tags it carries. | debug build; campaign data fixup, starting-force placement; WGS-70 |
| CGS-06 | Campaign load resolves each `Trade_Routes` name. A name that resolves to no route is reported (log line and error popup; an assert in the debug build) and is not added to the campaign's routes; loading continues. | debug build; campaign data fixup |
| CGS-07 | A name reference (`Locations`, `Home_Location`, `Required_Special_Structures`, `Required_Orbiting_Units` and the like) is resolved to an object type at load. A name that resolves to no object is reported (log line and error popup; an assert in the debug build) and the reference stays null; an explicit `None` is a null reference without a report. A null reference never matches an object, but the production-eligibility check ignores it: a null entry in a structure or orbiting-unit requirement is skipped (the orbit check asserts in the debug build and goes on), so the type stays buildable on its other prerequisites. In a structure OR chain the rule is positional: a null entry that leads the chain is skipped without consuming the chain, so the entry after it is tested and its requirement stands, while a null alternative inside a chain led by a known type satisfies the chain. The tech-tree builder asserts on a null requirement. | debug build; name reference fixup; production requirement lists |
| CGS-08 | Creating a company needs its transport type, and a squadron or company its member types. A composite whose transport or member names resolve to no type (or to a composite that is itself uncreatable) cannot be created: a starting force naming it is skipped and the type takes no part in the census or in production offers. The debug build's own result for such a record is **unverified** (the creation helpers return null for a missing transport); this keeps the visible behaviour minimal. | debug build; delegated creation; WGS-80 |
| CGS-09 | Delegated creation tests the type's dummy behaviours in order: ground base (asserts), starbase, ground special structure, space special structure, ground company, squadron, starship. A type with none of them is logged as an unknown type and the assert fires in the debug build; creation returns null, the starting-force pass then goes on with the next record. | debug build; delegated creation dispatcher; WGS-80, CGS-03 |
| CGS-10 | Placing a starbase below level two clears the planet's space special structures after the base update (the clear call). Structures placed earlier by starting forces therefore disappear, so a starting-force list that names orbital structures before a level-one base ends with none of them. Ground structures stay. The political control of the remaining structures is recomputed. | debug build; delegated creation of a starbase; WGS-180 |
| CGS-11 | A planet type that omits `Planet_Black_Market_Modifier`, `Planet_Tech_Availability_Rating` or `Planet_Slice_Difficulty_Rating` keeps the value of a freshly constructed type. The debug build's constructor default was **not** read; zero is assumed (the effect is the black-market price of an offer and the theft ratings of that planet) and the omission is reported. Every stock planet authors these tags; some mod planets do not. | unverified; planet type data |
| CGS-12 | A defeated hero's respawn is scheduled with the authored absence when it is positive, otherwise with `Default_Hero_Respawn_Time`. The debug build hands that default to the planet's creation schedule without a sign test, so a mod value of -1 schedules an already elapsed creation: the hero returns at the next service of the schedule. The exact service tick is **unverified**; the same frame is used. | debug build; hero respawn scheduling |
| CGS-13 | Overlaying a derived object type onto its base returns false, without a report, when the base type name resolves to nothing; the derived type keeps only its own authored tags and nothing is inherited. The galactic type table cannot resolve such a chain through the catalog, so it leaves the type out and lists the unresolved base as an issue; a campaign record that names the type is then handled as an unknown name (CGS-05). | debug build; object type overlay |
| CGS-14 | **EAWR recovery policy, not a retail claim** (RGC-150..154: the debug build's behaviour for an undefined `Company_Units` / `Squadron_Units` member is undefined). Member marking skips a roster slot whose name resolves to no game object, keeps marking every other slot, and reports one note per slot (owner type, roster tag, slot index, name). The owner type keeps its authored roster and its own flags, and the member-flag timeline stays available for every type. | EAWR recovery decision (owner, 2026-10-22); docs/research/remake-gc-retail-rules.md RGC-154 |
| CGS-15 | Both raid category masks start at zero. An omitted or empty `Raid_Force_Free_Object_Category_Mask` or `Raid_Force_Limited_Object_Category_Mask` retains that zero mask; whitespace or separators alone select no categories. A zero free mask exempts no objects from the limited-category requirement and count. Faction, hero-count and transport-payload gates remain independent. | debug build; game-constant initialization, mask parsing and raid admission; WGM-37 |
| CGS-16 | Preserve the full signed `Force_Strength` magnitude. Income uses the positive like/opposing products without a clamp. For nondegrading political gains the target faction receives time multiplier 1 minus strength times `MaxInfluenceTransitionAlignmentBonus` on like alignment, or 1 plus strength times `MaxInfluenceTransitionAlignmentPenalty` on opposing alignment. Factions marked neutral receive neither; the Neutral alignment enum on a nonneutral faction still participates in equality/opposition, as does a neutral-alignment planet. Raw source magnitude is retained; CGS-17 owns aggregate consumption and source lifetime. | debug build; RGC-16..20; WGEP-75; WGM-72/111 |
| CGS-17 | A changed nondegrading target replaces or removes the retained alignment time source; degrading updates leave it installed. Fresh complete-placement initialization does not install a source. The retained source shares signed category -1 with abilities; each category takes its minimum and categories multiply in ascending signed order. After category combination, real planet control applies a hard-coded minimum aggregate time multiplier of 0.01. Raw zero or negative alignment sources therefore progress with positive time, never instantly or in reverse. Same-player equality writes no amount. Current control is not range-clamped; target control remains capped, and checked arithmetic rejects overflow. | debug build; RGC-104..109; RGC-130..134; WGM-72/197 |
| CGS-18 | Additional_Population_Capacity retains the full signed 32-bit source integer. The player-wide sum has no per-object or final floor. Purchase admission compares population with max(capacity minus live and queued usage, zero), before payment or queue insertion. Negative capacity admits zero-population purchases; uncapped AI bypasses the population gate. | debug build; RGC-140..144 |
| CGS-19 | `Planet_Credit_Value` is signed. Preserve negative values through actual planet income and the player's fiscal sum without flooring the income. A negative fiscal total debits the account without the positive-credit difficulty multiplier; the resulting account balance is floored at zero. Checked arithmetic still rejects overflow. | debug build; signed planet income and fiscal payment; WGEP-03..06/16 |
| CGS-20 | Direct special-structure creation appends and retains every authored record, including duplicates beyond the planet's ground or space capacity and at zero capacity. The capacity comparison follows insertion; its debug-only assert does not undo the object, skip later records or refuse campaign setup. Both domains use the same insertion helper with no owner capacity filter. WGS-70 sets the target faction before each starting record, so structures keep that record's supplied faction even when earlier records have another owner. Ground accessibility and ordinary purchase admission remain separate gates; direct and additional-result creation do not raise the authored capacities. This rule is derived from the debug build. | debug build; starting pass, delegated creation and both structure-list insertion paths; WGS-70/80; WGEP-84 |
| CGS-21 | Production reduction target selection reads original types independently of creation reachability or purchase eligibility. Known types without an authored build cost remain time-reduction targets; price reduction still requires positive base cost. An explicit `Applicable_Unit_Types` reference that resolves to no type asserts in the retail debug build; we skip it with a warning, and remaining explicit and category targets still apply. An empty selection supplies no effect and does not refuse campaign setup. Omitted scalar parameters retain the constructor defaults in WGEP-93; malformed consumed parameters still refuse in the remake. | debug build; shared target selection, price/time application and reduction constructors; WGEP-73/93 |
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
provider facts refuse startup. Starting originals with political protection are
placed, including authored forces, story-created types and multiplayer heroes.
Protection stays inert while its source ledger, transfer, removal and checkpoint
lifecycle remain pending (legacy EAWR-2359); startup never sets a sticky protection
flag. The multiplayer hero pass requires authored production behavior at home,
checks owned original identities and uses the shared typed placement (WGS-14).
None of the three single-player Equal Footing definitions starts Tarkin.
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

## Multiplayer AI project policy

The following rules extend the owner's shared-seat policy (WGS-P01 in the
[session walk](walks/gc-session-save.md#owner-policy-human-seats-and-whole-team-battle-roles));
they are **not retail parity**. The private research receipt `out/mp4481/retail-rule.md`
records the XML and debug-build sources: fresh players start without a native
controller, the galactic assignment pass consumes campaign `AI_Player_Control`
pairs, and the six stock MP campaigns omit those pairs. Tactical lobby selection
and a solo staging branch read `Basic_AI`; those are separate retail paths.

| Rule | Project policy |
|---|---|
| GCMP-AI-01 | In actual multiplayer setup mode, each active playable AI faction selects its own authored `Basic_AI` player type. It does not borrow a solo campaign's assignments or use the campaign's menu metadata to determine session mode. Single-player selection retains its authored campaign-pair path. |
| GCMP-AI-02 | Resolve the one globally agreed difficulty adjustment through each selected player's authored `Difficulty_Adjustments` mapping. Faction references, controller definitions and adjustment mappings remain mod data and participate in content agreement. Selection is canonical on every peer. |
| GCMP-AI-03 | Human and inactive players acquire no controller through this path, and Neutral remains excluded from native goal ownership. Nonplayable ambient factions retain their existing authored controller assignment or absence; missing `Basic_AI` does not manufacture one. |
| GCMP-AI-04 | Refuse a required playable faction without `Basic_AI`, an unresolved controller type, or a missing or ambiguous selected difficulty mapping through `EAWR-GCAI-RUNTIME-0001` with the missing input named. No assumed stock player type or Normal fallback is supplied. |

The current lobby agrees one global difficulty, not a per-seat difficulty.
Per-seat difficulty would require a separate agreed-input extension.

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
