# Galactic AI perception provider contract

This component implements the campaign side of the shared world query boundary.
It consumes completed, immutable campaign snapshots; it does not install the
opponent lifecycle, issue fleet actions, or run a route refresh. The sourced
rules are [WGA-06/07/18/62/65/79/80/81](walks/gc-ai.md) and
[FT-05/08/12/15](foc-tactical-ai.md). Remaining integration work stays open under
the planet perception and reach follow-ups (legacy EAWR-1121) and (legacy EAWR-2494).
New retail output qualifications are WGA-111/112/114; decoded execution-route
consumption is also specified by WGA-134..136. Force and connectivity queries
are qualified by WGA-181..194; the owned native component is WGA-195.

## Path connectivity coefficients

Read-only debug-build research qualifies the graph calculation separately from
execution route costs. These rules describe the four internal/external
connectivity perceptions; they do not substitute neighbour counts or force
strength for a path coefficient.

- **WGA-490:** A directed edge requires a positive current allegiance allied to
  the evaluating player; an internal edge also requires that at its destination.
  Every player has both graphs, including human and neutral players. Self distance is zero. Distances use the
  single-precision three-dimensional position calculation, promoted to binary64.
  Round each copied coordinate to binary32 before subtraction, then round each
  vector arithmetic operation; large adjacent coordinates may become co-located.
  A direct trade route divides distance by the single-precision global
  `TradeRouteMovementFactor`. Other edges exist only within
  `MaximumFleetMovementDistance`, including equality. The graph retains only
  vertices incident to an accepted non-self edge, preserving object-query order.
  Internal costs mirror the earlier matrix triangle. Named route speed and
  execution threat do not enter these weights. Retail queries trade existence
  between actual planet objects; an input route with an endpoint outside the
  snapshot therefore contributes no edge and produces a diagnostic naming it.
- **WGA-491:** Binary64 shortest-path relaxation preserves the earlier path on
  equal cost. Positive direct edges start with the source as predecessor; zero
  edges have no predecessor. Count intermediate vertices, excluding endpoints,
  on each selected shortest path. Internal graphs count unordered endpoint
  pairs; external graphs count ordered pairs. Divide each unsigned count by the
  largest count in that graph; a zero maximum gives zero for every retained
  vertex. A vertex filtered from the graph has no coefficient entry. Zero edges
  can leave an incomplete predecessor chain after relaxation. Retail's walk of
  that NIL chain remains unqualified and needs a capture/research follow-up.
  The explicit project boundary skips that endpoint pair, contributes no partial
  counts, emits a diagnostic and preserves the other pairs and publications.
- **WGA-492:** `FriendlyInternalConnectivity` and
  `FriendlyExternalConnectivity` read the evaluating player's corresponding
  graph. A missing vertex returns successful zero. `EnemyInternalConnectivity`
  starts at zero and takes the maximum across all nonallied players' internal
  graphs. Missing owning publications stay unavailable. The retail enemy
  external wrapper forwards an output without initializing it; its equation
  caller's seed is qualified separately by WGA-494.
- **WGA-493:** Retail initialization and post-load reconstruction build the two
  coefficient graphs for each player. This implementation rebuilds derived
  tables from completed control, geometry, route and diplomacy inputs at fresh
  binding and restore. It adds no native archive fields. Loaded equations request
  the producer through their compiled lookup tokens; equal complete inputs reuse
  the prior publication. Raw authored movement constants are rounded to binary32
  for this producer, independently of existing fixed-point force consumers.
- **WGA-494:** Each native lookup expression initializes its own binary64 output
  to positive zero before calling the engine evaluator. Other expression
  operands and the outer equation's output do not seed that leaf. A successful
  callback pushes its output; a failed callback still fails the equation.
  `EnemyExternalConnectivity` therefore starts at zero and takes the maximum
  external coefficient over all nonallied players, including human and neutral
  players. Missing vertex entries contribute nothing and can give successful
  zero; missing owning publications remain unavailable. This qualifies equation
  lookups, without claiming that an arbitrary direct native caller seeds zero.

Evidence: debug build, coefficient graph construction, edge weighting, vertex
filtering and binary64 shortest-path calculation. Synthetic coverage includes
line, directed terminal, trade-shortcut and equal-path diamond graphs, independent
lookup output seeds, human/neutral enemy graphs and unavailable owners, with
independent graphs staged on 1, 2, 4 and 8 workers.

The retail coefficient publication cadence is unqualified (legacy EAWR-4786).
Integration uses a named coefficient-publication seam at the existing
connectivity/execution refresh boundary pending the frame-service programme's
qualification; that boundary is a project policy, not a retail timing claim.

Each changed graph uses Floyd relaxation in O(m^3) time and O(m^2) storage for
m retained planets. At m=250, two graphs require approximately
`2 x players x 15.6 million` relaxation candidates per publication. Independent
player/graph jobs use fixed worker partitions and ordered publication. Fresh
binding and restore build once; subsequent builds occur only at the admitted
execution-matrix refresh seam when complete graph inputs differ. Binding each
tick does not recompute an existing publication. The retail refresh frequency
is part of the unqualified cadence seam above; no additional timer is invented.

The installed headless CLI accepts `--live-perception <equation>` with
`--live <ticks> --live-ai on`. It reports failed native lookups beneath that
exact root equation, including player, target and token path. The observer is
off by default and does not change perception results or native state.

## Inputs and ownership

`GalacticWorld::bind` takes a retained completed snapshot, copied player/type/
planet authoring, copied connectivity records, synchronization frame and the
campaign identity frontier. Player/team identities are explicit; no tactical
deployment type replaces a campaign type. Planet, object and producer
preparation and force contributions use disjoint executor staging and canonical
publication. Ordered
roster indexing resolves shared persistent identities without servicing them.
Immutable producer offers remain owned by the retained snapshot.

There is one target per planet. Fleet containers, their members, recursive
payloads, landed actors and production objects resolve to the containing
planet. An enroute fleet has no planet parent. Query targets retain the shared
engine's AI-target handles; the provider does not convert them into object
handles. Position and source liveness come from the selected manager's view.

`GalacticQueries` implements the shared copied-world and equation facade.
Producer rows preserve the real production admission result, effective price
and build time, channel queue time, base level, required level and logical
rate. **Current purchase admission is not prospective production capability.**
Target-dependent reach, distance, composite transport speed, faction speed and
technology weighting remain execution/selection owner inputs. Their absent
values leave producer reach false; there is no default transport speed.

## Reach contracts

`GalacticConnectivity` carries two separate generations:

- Goal membership: player/source/policy and the production-site planet set.
  Same-planet membership succeeds for a recognized galactic policy; a missing
  relation is unavailable. This is WGA-18's membership service.
- Execution routes: player/source/candidate and the decoded distance, threat,
  prefix threat and explicit empty-route flag. This is FT-08's cost service.
  An unavailable execution generation refuses a supplied-source cost query.
  In a declared complete generation, an absent route is an empty path, so the
  existing binding applies FT-15's decoded fallback and policy thresholds.

`GalacticFindTargetQueries` selects the retained galactic or current manager,
resolves object sources through containment, and returns pure route facts.
The existing binding owns all eleven policy names, probability conversion,
thresholds, sentinel filtering and inclusive three-dimensional radius.
Space bypasses the cost service under FT-12. Land requires a delegated land
provider; an explicitly land current mode with no provider refuses a supplied
source rather than pretending its candidates have a space service.

The action owner must publish execution route records and production-site
relations. WGA-126..140 supplies that owner's packed route selection, force,
initialization, periodic refresh and post-load reconstruction rules. The
provider consumes the retained generation without requesting a refresh.
The current/retained engines also need a shared handle registry or
consumer routing before cross-manager target handles can be used by every
object/TaskForce API. This provider alone does not implement that registry.

## World reader (S3a)

`GalacticWorld` reads a completed tick through an `AiWorldReader`: a structural
index plus the published world view. The index resolves every object identity
(planets, fleets and members, landed forces, production objects, nested
payloads) once, first resolution winning, with its containing planet, container
and enroute membership. It is rebuilt by the partitioned attach prepass only
when the roster-bearing inputs change; a motion-only tick keeps it and moves
positions. The fog bits of the view come from the observer's per-viewer ranges,
not from a whole-table search per unit and viewer (WGUI-18 object-details rule
and WGA-08 no-fog rule unchanged).

Shared derived data (the index, the per-planet force aggregates and neighbourhood
sums, the fog bits) is built before the decision pass and frozen while players
decide; nothing shared is written lazily from inside a decision. Per-viewer and
per-player memos may stay lazy and belong to the player that fills them. The
retail AI performs no such prepass: it reads live objects through per-binding
caches (WGA-65). The reader keeps those caches' semantics, so a warm world and a
cold world bound to the same tick hold the same bits.

The view is double-buffered. The publication before the current one is reused
once no reader holds it; a roster epoch (process-local, never saved, hashed or
compared across processes) says whether only positions can differ. A reader that
still holds an older publication keeps an unchanged copy. The producer catalog of
an unchanged tick keeps its job list: credit-only and queue-clock-only changes
share the previous jobs and prices, and equal reservations keep their record.

## Cache and retained context

Cache identity includes selected player, enemy/human roots, planet, full token
chain and every ordered parameter binding. WGA-65's frame-zero miss, inclusive
expiry, interval zero visible bypass, interval minus-one indefinite lookup,
hidden miss evaluation and gain/loss refresh are explicit. Unknown per-token
intervals are uncached. Visibility signals must come from the qualified owner;
observer presentation masks are not substituted for AI cache visibility.

Equation loading compiles each token chain into a root enum and an interned
identity with retained authored spelling. Perception stores compact keys for
player, enemy, human, planet, token and ordered bindings in a flat open-addressing
index. Token readers borrow the interned metadata; the archive writer resolves
the spellings and sorts by the original complete keys. Intern IDs, table slots
and their insertion order never enter decisions, digests or archives. Numeric
zero bindings share lookup identity while their individual sign bits remain
available for the unchanged WGA-195 bytes.

Bindings enter the retained interner only when an ordinary remembered value is
stored. Uncached, failed and read-only misses borrow their caller's arguments;
economy memo bindings use a separate pool released on frame changes. At ordered
boundaries between equations, excess unreferenced native spellings are reclaimed
and compact IDs are rebuilt from retained records. This changes no native value,
stamp or encoded key. Expired load-time token definitions are also reclaimed,
so dynamic arguments and repeated loads do not add unbounded derived history.

The admitted AI decision pass owns a sequential query scope. Usability prepares
copied access facts in parallel, then reads perception in planet order on the
owner thread. Candidate scoring and single-type means run on that same thread;
bulk force, producer and route preparation retain fixed worker partitions and
ordered commits. Nested barrier queries perform no mutex operation. Parallel
battle-child queries of a retained galactic parent serialize whole equations,
including calls to upstream lazy caches. Derived engine producer, build-budget
and configuration caches are either reached on the owner or behind this parent
query boundary; pure partitioned phases do not initialize them.

WGA-62's raw base levels, authored global base denominators, trade-route
denominators, raw open company slots and structure fraction are distinct.
The store retains player/planet/name story flags, pre-clear reads and
player/uppercase-planet hint keys with first duplicate hint insertion winning.
Trigger names use the shared equation parser's uppercase text identity in
setters, direct checks, equation getters and restore. Retail trigger-name case
identity remains a qualification seam; this component convention is explicit.
State capture sorts full keys canonically, so partitioned query insertion
order does not change component records. Restore rejects duplicate identities
before publishing. `GalacticWorld` also implements `StateContributor`, supplying
GALACTIC_PERCEPTION v1 through the native envelope. It stores complete cache
identity/binding words, value/interval/frame, visibility, last-observed, story
flags and hints. Restore checks canonical bytes and rebound identities before
publishing. The whole-game owner supplies callback wiring, clocks and complete
load/fixup order.

`evaluate_in_galactic_context` requires a campaign child, retained manager,
selected AI player and actual conflict planet. It evaluates immediately through
the parent's equation table and store without pumping parent services or
blanket-refreshing perception (WGA-80/81). The owner updates synchronization
time independently while the parent mode clock remains frozen. Lua dispatch
of this seam belongs to the shared binding owner.

## Qualifications and proof boundary

WGA-111 qualifies authored base income divided by the context target maximum;
WGA-112 qualifies authored land structure slots divided by
`MaximumSpecialStructures`, without clamping. Those optional normalized planet
inputs remain the authoring owner's responsibility. WGA-114 qualifies controlled
count divided by galaxy planet count in its nonempty fixture; this component's
player getter currently supplies only the raw count.

WGA-181..194 supply raw fleet/company/garrison/base metrics, category selection,
normalization, presence, bitfields, neighbourhood weighting and connectivity.
Game aggregation rebuilds unfiltered target contexts; category-bound local
numerators therefore do not change game normalization denominators. Hero
exclusion remains the authored equation subtraction. Presence is independent
of metric and garrisons, and bitfields inspect live contents.

Force metrics/categories/starting rows, structure space/garrison masks, live
stealth and potential indigenous facts are typed copied authoring/service
inputs. Missing applicable inputs refuse a number. Island tokens consume known
true/false potential-path flags; an absent producer row stays unavailable.
Player raw-strength evaluators, threshold-specific visibility, observation age
granularity, mixed contexts and markup weighting remain U-01/GAI-U02 seams.
Unsupported tokens return no numeric value. U-07 still
qualifies the entry/load and retained-context callback boundary. The nominal
perception service does not itself call `bind` or sweep tokens (WGA-79).

The installed component test uses a real initialized stock campaign, its
planets, queues and nested forces, stock equations, unmodified galactic
freestore/library modules and production Lua query bindings. It verifies stock
definitions and equation consumers on 1/2/4/8 workers. An additional explicitly
positioned four-planet live campaign control uses stock types/garrisons and
unmodified equations to evaluate Normal Empire space defence at Kamino and
Coruscant, with Easy/Hard zero, and roundtrips the native component. Its manager is explicitly
the query component shell; stock faction lifecycle/goal-set installation is a
separate owner. The source-separated reach pick is a supporting fixture using
declared route records. The final installed stock reach/action/frozen-child
proof awaits composed lifecycle, execution and campaign owners; this initial
component test does not close the opponent slice.

## Offensive goal readers

The stock offensive desire equations (Conquer_Pirate, Conquer_Opponent, Conquer_To_Reconnect, Raid, Weaken_Planet and the
unrestricted grabs) read `IsCorrupted`, `IsConnectedToCorruption`, `HasCreditSiphon`, `BaseIncomeNBTP`, `AnyCurrentThreats`,
`TargetPoliticalControl` and `WorstIslandFractionOfLargest`. WGA-449..452 and WGA-473..476 qualify these readers; WGA-477 and
WGA-478 record why the human-targeting goals stay at zero in an all-AI game and when the first-attack grace period ends. A
missing owning fact (corruption state, observer row, island sizes, authored constants) stays unavailable and fails the equation
under WGA-418; a successful reader may legitimately return zero.

## Economy and connectivity publications

The economy aggregate memo belongs to its querying player. Ownership observations,
expiry and funding inputs retain WGA-65/293/299 semantics. Immutable retained
budget rows and complete build-head publications are shared between tick headers;
readers do not copy those records into alternating DTO buffers.

Connectivity is an immutable shared publication. Execution matrices are separate
immutable generations per player, indexed by source and candidate identity. A
player refresh replaces only that player's matrix. The deadline refresh accepts
an explicit executor and can run as a standalone prepass; its current invocation
stays at the admitted execution boundary (WGA-138/139, GFC-A07/A16).

Pure production-site results are memoized per player, target and policy over the
complete access input and immutable graph. Equal inputs may reuse a result even
when unrelated observation generations change. Initialization remains partitioned
with ordered publication. The retained lists and stamps still refresh at their
existing boundaries, including unread native rows (GFS-A14, GFC-A14, WGA-329).
Changing that retained cadence to demand time is a separate fidelity change; this
publication optimization does not change native bytes or save/restore behavior.
