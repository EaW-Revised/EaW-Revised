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
