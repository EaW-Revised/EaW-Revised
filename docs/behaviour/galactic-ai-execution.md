# Galactic execution component contracts

This component implements copied execution-route production/publication and
galactic freestore admission. It consumes the galactic AI walk's WGA-27..32,
WGA-61/77/78 and WGA-126..140 contracts. It does not install a whole campaign
opponent or supply the missing direct world-action consumers.

## Execution routes

The producer receives retained object-query planet order, player-specific AI
usability, enemy raw space force, nonallied current-starbase power, position and
source layer adjustment. These are required inputs. An ownership-filtered
movement path or an absent force getter interpreted as zero cannot replace them.
Trade routes and the maximum proximity distance come from validated campaign
data. WGA-126..140 owns the directed edge and packed-cost arithmetic, strict
intermediate/source/destination relaxation and reconstruction/decoding rules.

The producer partitions independent source rows. Each intermediate stage copies
its row/column and predecessors before disjoint writes. In the qualified
nonnegative numeric domain the diagonal stays zero, so this yields the same
strict improvements as the ordered reducer. Edge/cell visits are counted,
independent of worker count. Decoding publishes player/source/candidate records
in the perception provider's existing DTO; goal production-site membership is
a separate service.

**Project policy:** invalid finite-domain inputs, nonpositive route speed,
out-of-domain packed edge components, component-sum overflow and invalid
predecessor reconstruction refuse a generation transactionally. The debug
build's assertions and malformed numeric conversions establish no normal-game
recovery contract. This refusal is not a claim of retail clamping or saturation.
Planet inputs beyond the range reader's qualified 1000-index cap also refuse.

The derived matrix uses a 64-bit word with 32-bit threat and distance halves.
For `n` planets and largest entering halves `T` and `D`, a simple retained
path has at most `n-1` edges; joining two paths is bounded by `2*(n-1)*T`
and `2*(n-1)*D`. If both are below `2^32-1`, every relaxation fits exactly.
Graphs above that sufficient bound are checked against their actual finite
row/column maxima at each stage, rather than refused from the coarse bound.
Each half-sum is computed in 64 bits and must remain below `2^32-1`, so it
cannot carry into the other half or collide with the all-ones missing word.
Reconstruction checks each actual prefix sum too. Missing words never enter
addition. This preserves lexicographic threat/distance ordering and strict
ties for every graph in the old domain, without saturating larger graphs.
The WGA-386 missing-route decoded fallback remains distance `6553.5` and
threat `6553500`; it is independent of the internal matrix width. Matrix,
geometry and memo are derived and do not alter checkpoint encoding.

Initialization computes immediately and installs the sourced first deadline.
Ordinary refresh only runs inside admitted execution, tests its independent
mode-frame deadline inclusively and rearms at current frame plus ninety. Query
reads never refresh. A producer that is unavailable publishes that capability
as unavailable; it does not publish fabricated threat-free paths. Failed
generations leave the prior publication and deadline intact. The campaign owner
must restore the deadline, original node ordering and provider state before
resumed consumers; rebuilding an absent finder must preserve that deadline.

## Freestore

WGA-28/61 eligibility is distinct from reservation ownership. Null, foreign,
planet, company/squadron member, ground/space special-structure and ordinary
planet-parented fleet inputs cannot be admitted. Whole units retain persistent
identity and transit state. Unknown type traits are excluded rather than
classified as ordinary actors. Copied nested rosters inherit actual parent
location and transit; repeated presence references represent one actor.

WGA-29 assignment fails for a different potential-plan reservation, waits for
transit or absent containing planet, and assigns otherwise. The ordered owner
consumes successful assignments; the pure predicate does not transfer ownership
or complete a coroutine. The reservation map belongs to native persistence.

WGA-30 transport safety reads actual alliance, surface access, landed transport
owners and the authored ground-force limit. Missing stationary orbiting
transport inputs stay unavailable. Transit bypasses those stationary gates.
Surface summaries and per-object safety prepare on partitioned immutable inputs.
Canonical presence copying/index construction is bounded barrier projection;
it does not progress any world object.

Fresh attachment calls stock definitions, creates the main thread, then maps
FreeStore/PlayerObject and zero stamps. Definitions clear those globals, so the
mapping follows them. Loaded attachment retains its restored thread and maps
the sourced zero stamps without replaying fresh definitions. Service uses
independent mode-time stamps and strict elapsed greater than numeric rate.
Main pumping precedes numeric-rate unit callbacks. Stock's misspelled
UnitServciceRate does not supply UnitServiceRate, so its Lua unit callback stays
skipped. Native transport landing is a distinct, still missing action consumer.

**Qualified seams:** pool counts currently classify whole transport carriers as
ground and other whole eligible actors as space. Potential-plan reservations
remain separate from membership/counts/callbacks; the exact retail reservation
and mixed domain count contract awaits U-05. Direct movement binding refuses
with a named missing capability. Native landing, redistribution actions, paid
production notices and magic/cash actions remain separate integration work;
none returns fabricated success.

## Native persistence

The private execution codec contributes `galactic-execution` version 1 to the
whole-game native envelope. It retains each player's initialization/finder
state, refresh deadline, original query node order, raw maximum-distance word
and canonical object-to-potential-plan reservations. The derived route matrix,
copied world facts and work counters are excluded. Integer and numeric words
are little endian; lists are explicitly framed. Domain/version, ordering,
finite configuration and complete payload consumption are validated before
publication; unsupported versions have no legacy reader.

Restore requires the campaign owner's native reference validator after the
world, child, Lua, return and perception reconstruction. Persistent IDs do not
remap. Known identities awaiting their loss/cancel boundary remain valid even
when absent from the current live roster; unknown player/planet/plan/object
references reject the entire restore instead of dropping reservations.
When reconstructed goal identities are unavailable, restore reports the
missing validator capability. The owner rebuilds the derived route matrix only
after validated publication, retaining original node order and `next_refresh`.
The completed whole-game restore and rebuild consumer remains integration work.

## Proof boundary

Fixture contracts cover packed routes, deadlines, eligibility, reservation and
transit decisions and surface safety on one, two, four and eight workers. The
installed test constructs a real authored Normal campaign through production
campaign setup/root providers. Unmodified stock galactic freestore and library
Lua runs through production library/pool bindings and consumes actual whole-unit
space/ground counts with no manually submitted AI actions. Missing installed
data fails rather than skips. Its graph/count equality proves this first
component, not complete stock movement, purchases, magic defence or the final
AI-vs-AI opponent run. Raw enemy-force/current-starbase qualification, U-05/U-06
observations and composed lifecycle/action/return ownership remain open.
