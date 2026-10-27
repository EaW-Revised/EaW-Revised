# Tactical enum, object-query and object-Lua rules

Bounded read-only debug-build walk, 2026-10-22, answering Q15..17 of the
[Remake walk queue](../research/remake-walk-queue.md). These are static
consumer facts and proposed discriminating cases, not new retail captures.
Private receipts retain the inspected locations and raw evidence.

Existing [AI rules](foc-tactical-ai.md), [object-script survey](space-object-scripts.md)
and [Lua model](lua-script-model.md) remain authoritative for their already
answered parts. Timing uses [FS-29, FC-57 and FD-47](frame-service-schedule.md);
this walk establishes no new frame order, callback cadence or first-service time.

TaskForce name resolution follows [PL-11 and PL-20](foc-tactical-ai.md):
category lists take precedence; named lists keep authored order and remove
duplicates. The remake resolves names through loaded AI types, then the immutable
registered-type catalog, so registration outside the battle closure remains
visible without creating production, combat or craft metadata. Truly undefined
names retain the existing diagnostic and unresolved-row policy. Selection still
requires its existing free-store metadata or production offers; registration
alone creates neither an eligible live object nor a production candidate.

## Q15 — authored goal-category names and failure boundaries

| Rule | Observed behavior | Evidence |
|---|---|---|
| RGC-110 | `AIGoalCategoryType` is a dynamic enum loaded from `Data/XML/Enum/AIGoalCategoryType.xml`, not a fixed list of stock category names. Each entry supplies its name and, when nonempty, a decimal value; an empty value uses the next positional ordinal. Lookup trims surrounding whitespace and compares uppercased names. The typed lookup reports failure for an unknown name. Its numeric bridge also reports failure, but does not establish a usable fallback value on that path. A consumer must check success; neither zero nor a particular invalid ordinal is proved for an unknown equation constant. Failure to create the converter is diagnosed and contributes to the conversion system's failed initialization result. | debug build HI-E01..04; AI-04 |
| RGC-111 | A fresh goal-function entry requires both a resolved goal and a resolved equation. A missing equation produces a diagnostic and failed verification; creation deletes that entry and returns no entry. The containing function-set reader diagnoses the failed entry, continues to later siblings and returns aggregate failure. This alone does not prove whether its outer catalog is retained. A different branch applies when an equation exists but candidate scoring returns failure: scoring starts desire at zero, then replaces it with the positive value obtained by widening single-precision `1e18`. Do not describe that branch as automatic goal omission or zero desire. | debug build HI-E05..08 |

Authored input is the mounted enum file plus an equation containing
`AIGoalCategoryType[Name]`, and a goal-function record referencing that equation.
There is no object-type inheritance in this enum lookup; mounted data supplies
the entries. RGC-110's empty-entry default is positional: an explicit value on
one row does not reset the next row's positional counter.

The equation parser is outside the open debug executable. **Unresolved:** when
the numeric converter returns false for an unknown name, does the equation
loader omit the equation, retain a failing equation, or reject a wider load?
RGC-111 distinguishes the downstream branches without deciding which one this
parser selects. Static assertion continuation is not a release-run result.

Decided source boundary: `src/script/foc/tactical_ai_loading.cpp`,
`converters` follows authored names rather than a built-in name list. Keep
unknown-name failure distinct from a valid numeric zero. Equation/catalog
admission after that failure remains a narrowed follow-up; the current host's
policy is not additional retail evidence.

Discriminating case: author `ModCategory` with value 37, an empty-valued sibling,
and equations referencing a whitespace/case variant of the valid name, an
undefined name, and a valid zero-valued name. Reference each from its own
goal-function entry, followed by a known-good sibling. Observe equation
registration, goal-function registration, function-set return status and the
outer admission separately; for a retained failing equation inspect candidate
desire. This case is specified, not executed by this walk.

## Q16 — object-query filter forms and results

| Rule | Observed behavior | Evidence |
|---|---|---|
| RGC-112 | `Find_All_Objects_Of_Type` processes filter arguments in their supplied order. Type-name, category, player + name and name + player forms are accepted. A player alone and a type handle are also meaningful filters. String recognition tries properties, then categories, then an object-type name, subject to whether that filter slot is still available. Recognized filters combine: exact owner, exact original type, overlapping property bits and overlapping category bits. The player filter compares ownership, not alliance. An unrecognized argument reports a script error and returns no table; zero arguments and an effectively unfiltered request also error. | debug build HI-E09; existing player-first implementation/contracts |
| RGC-113 | A valid query allocates a Lua table and visits the active mode's object-manager list. Matching objects are appended in that traversal order, with no additional name, distance or ID sort. No matches returns an empty table. Type/category/property tests read the object's original type, rather than promoting a craft to its carrier or using an unrelated parent-mode object. The inspected collection loop adds no fog, owner-human, decoration or pending-deletion exclusion. The membership/lifetime rules of the manager's list still apply. | debug build HI-E09 |

Inputs are live filter values and the original types' resolved properties and
categories; the query has no separate XML inheritance step. Filter state begins
with no owner/type and unrestricted category/property masks. Each text argument
is decoded in order: a property or category name is tried only while that
namespace's current mask is the numeric 64-bit all-ones sentinel. A recognized
sentinel in an unrestricted slot consumes the argument and leaves its mask
unrestricted; a finite mask occupies the slot. Once a finite mask occupies it,
a later sentinel is not decoded in that namespace. It must match another
available filter namespace or report an unknown-filter error. Thus sentinel
before category/property can succeed, while category/property before sentinel
can error. The debug-build decoder confirms this ordered rule. An all-sentinel
request without
an owner/type errors as effectively unfiltered.

Unrestricted masks admit objects whose corresponding bits are zero. A mask
containing only 32 one-bits is an ordinary mask; the spelling of an enum name
does not change this rule. A later player argument replaces the earlier owner
filter. The normal supported forms do not depend on that repeated-player edge.

Implementation boundary: `src/script/foc/plan_bindings.cpp`,
`Find_All_Objects_Of_Type`, recognizes ordered owner, type, property and category
filters. Lua contracts in
`tests/script/foc/foc_ai_contract_bindings_tests.cpp` cover both owner/name
orders, owner-only queries, type handles, intersecting filters, empty results,
and invalid requests, including sentinel-first acceptance, late-sentinel errors
for occupied slots, and zero-bit objects. Player-only queries also admit host-view
objects without AI type metadata; finite property/category filters require that
metadata.
Registered game-object names outside the battle type closure resolve through a
shared immutable name-to-type-ID index, without expanding the AI type table; a
known type with no live instances returns an empty table. Unknown names
previously returned an empty table; they now report a script error under RGC-112.
Returned objects retain the host view's traversal order.
The host's original-type and retail manager-order correspondence still require
separate qualification; these synthetic contracts establish filter behavior.

Discriminating case: create same-type objects for two owners in an interleaved
manager order, plus a different type sharing a category. Compare name,
category, player + name and name + player; the two owner-filtered forms must
return identical ordered lists. A valid absent type instance returns `{}`;
an undefined filter name errors. Combine a valid property and category to
distinguish intersection from union. This is a proposed retail/synthetic case,
not a newly run contract.

## Player name query used by alliance helpers

| Rule | Observed behavior | Evidence |
|---|---|---|
| RGC-126 | A player's `Get_Name` is registered separately from `Get_Faction_Name`. It returns the stored player display name as a Lua string after text conversion. A missing underlying player returns no value. It does not substitute the faction name. | debug build: player-method registration and name getter |

The tactical binding reads `AiPlayer::name`, including an empty stored string.
The current lobby/start setup supplies no player display-name source, so that
field remains empty until real lobby/AI player names are wired into it. This is
an explicit implementation seam, not evidence that retail player names are
empty. `Get_Faction_Name` continues to return the independent faction identifier.

Synthetic contracts use a name different from the faction, an empty name and
an unavailable player. They also exercise the name-to-log-to-alliance path used
by `Shared/Util.lua`'s `isAlliedObject`: retrieving and formatting the name must
allow the following alliance check to run. These are synthetic cases, not a
new retail capture.

## Type cost query used after pad construction

| Rule | Observed behavior | Evidence |
|---|---|---|
| RGC-125 | `Get_Build_Cost` on a type asks for its generic credit cost without a producer and returns that integer as a Lua number. It does not calculate an individual factory's effective production price. The generic cost consumer reads the type's stored build cost; producer-dependent discounts and rounding belong to the separate producer branch. | debug build: type-cost Lua consumer and both generic/producer credit-cost branches; WAS-26 |

The tactical binding reads `AiType::tactical_cost`, the generic tactical cost
already used by the AI's reservation accounting (WAS-26). Production options
retain their own effective prices. The Lua contract checks a positive generic
cost and a zero-cost type. Mounted asteroid and defense plans query the newly
created pad child's type after construction starts, then apply their authored
wallet debit. Hosts without generic tactical cost metadata diagnose that missing
capability; they do not substitute a zero cost. Missing `Create_Generic_Object` in station doctrine scripts is a
separate API gap; this cost/query change does not implement object creation.

## Q17 — per-instance attachment, notifications and teardown

| Rule | Observed behavior | Evidence |
|---|---|---|
| RGC-114 | A newly constructed object starts without an attached script. Ordinary initialization applies general and mode-specific behaviors, then attaches the resolved nonempty `Lua_Script`. This branch has no carrier-only, human-only, AI-only or projectile exclusion. An object created through the ordinary factory receives its own attachment decision, including a member or projectile; a parent's script is not copied to its members. The weapon projectile factory reaches this ordinary creation path. Empty/inherited names follow SOS-01 and ML-24, not a script-name allowlist. | debug build HI-E10..13; SOS-01/30 |
| RGC-115 | Fresh attachment registers engine commands, calls `Base_Definitions` only for a fresh script state, creates `main` with no parameter, and then binds `Object` to this object. Creation of `main` does not run it. Attaching over an existing script asserts. If fresh script creation fails, attachment asserts and returns false; the ordinary initializer does not test that result and continues. This proves the continued debug branch, not an uninterrupted release run. Save restoration uses the separate load attachment path, with commands registered but no second fresh definitions/main sequence. Type metadata is separate and does not supply this per-instance `Object`. | debug build HI-E10/11; SOS-30/32/34; L-27 |
| RGC-116 | Creation notification is separate from attachment. The ordinary manager factory emits its creation signal only for non-decoration objects without projectile or particle behavior; it does not exclude a carrier or member merely for that role. The scoring receiver uses the notification to begin listening to the created object. In tactical mode its listener admission also includes carried named/generic heroes when a flagship exposes them. This signal does not itself call a per-object Lua creation callback. A projectile can therefore attach its authored script while receiving no ordinary manager creation notification. | debug build HI-E12/13/14; behavior metadata checked against the creation instructions |
| RGC-117 | The observed tactical destruction callback belongs to the scoring script: `Tactical_Unit_Destroyed_Event` receives the destroyed object and, when resolved, the final-blow player (neutral for an unspecified final blow). Its inspected receiver responds to zero-health notification, with a vehicle-thief-ejection ability exception. This is distinct from object deletion and from the Lua library's `Register_Death_Event`: that library stores one handler per valid object, rejects duplicate registration, and later calls the handler with no arguments when `TestValid` fails, removing the entry first. Neither interface means every projectile removal produces a scoring death callback. | debug build HI-E14/15; stock Lua library |
| RGC-118 | Pending deletion invalidates an existing object wrapper: its listeners and cached wrapper entry are removed and its object reference is cleared. The object's destructor shuts down and releases its attached script. The inspected ordinary deletion path marks pending deletion before detachment/release. No per-object Lua finalizer is called by these inspected attachment/destruction paths. Existing SOS-31/33 and L-04 govern stopped service and state cleanup; hull death, invalid wrapper and Lua-state destruction are separate facts. | debug build HI-E16..18; SOS-33; L-04 |
| RGC-119 | Attachment alone establishes neither tactical superweapon readiness nor firing support. The inspected mounted `FireDeathStar` AI plan performs `Find_First_Object` before testing `Is_Tactical_Superweapon_Ready` and requesting `Fire_Tactical_Superweapon`; `Evaluate_In_Galactic_Context` is another separate dependency. The tactical host currently lacks the first lookup binding, though a galactic host provides it. This is an implementation follow-up, not evidence that the superweapon is supported after attachment. | mounted Lua read in place; current binding registration; AI-20 |

Attachment inputs are each created object's resolved `Lua_Script` and normal
initialization versus load restoration. SOS-01 supplies variant inheritance:
blank or `TBD` authored values do not clear an inherited script. A same-named
module on a carrier, an independently created member and a projectile is
considered against each created object's type. An uncreated payload declaration
has no live attachment. RGC-114 does not assert that every possible exceptional
creation route uses the ordinary initializer.

Decided source boundaries: `src/script/foc/tactical_ai.cpp`, `service_objects`,
must reason about live allocation identity and the created type's attachment,
not just purchased carriers. `src/script/foc/tactical_ai_loading.cpp`,
`required_modules`, follows authored script names; broadening the frozen module
closure alone supplies no lifecycle events or missing APIs. The existing native
consumer suppression is project policy documented by SOS-44/45, not a retail
restriction on authored module names. The current source deliberately retains
the frame/barrier seams already named in the object-script survey.

Discriminating case: give a carrier, an independently created member and a
projectile distinct modules that record `Object` identity in `main`. Give a
variant a blank override of an inherited script, a control no script, and a
further object a missing module. Check independent attachment decisions and
continued-debug failure handling. Observe manager creation/scoring listener
admission separately: a projectile is excluded there. Register a death handler
from a surviving observer, then delete the member and projectile separately;
observe invalidation and the no-argument library callback without inventing a
scoring death for deletion. A zero-health case separately exercises the scoring
callback. Restore a saved object script and check that no second fresh `main`
is created. These are proposed cases; no retail run was made here.

Narrowed boundaries: exact first service and event delivery order remain with
the frame-service programme; exceptional creation routes, pooled-state ownership
and transfer/load fixup retain the survey's qualifications. RGC-119 leaves
superweapon behavior and its missing bindings to the named implementation work.
