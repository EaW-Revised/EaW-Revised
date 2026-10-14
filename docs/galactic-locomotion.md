# Galactic fleet locomotion

The engine-free locomotion service consumes accepted planet paths and explicit,
replayable movement inputs. It owns departure, per-frame travel and orbital arrival.
The [movement walk](behaviour/walks/gc-movement-control.md) supplies the rules.

`MoveFleet` is galactic opcode **5** (coordinator-reserved). Its canonical body is
fleet ID (`u64`), path count (`u32`), ordered planet IDs (`u64` each), destination-slot
presence (`u8`), and the optional slot (`u8`). Counts are bounded to 4096; IDs must be
nonzero and distinct, except for the two-entry source/source retreat fallback
(WGM-99), which records a canceled departure. A one-planet path can be recorded but
cannot depart. At execution
the owner, mode gates, source orbit and every graph edge are revalidated. An enroute
fleet cannot be redirected (WGM-07/84/88).

`MovementSetup` explicitly supplies original-type speed, capacity and build cost;
live container contents; faction hyperspace/corruption factors; model world-bounds
half-extents; and a complete observer-specific arrival-access/conflict table. No
authored omission becomes a guessed default. The effective mode modifier is explicit; the campaign adapter reads the authored
`Object_Max_Speed_Multiplier_Galactic` for initialized GC (WGM-97), then the owning
mode service supplies any debug/global factors. Ordinary strategic-speed commands
change pacing only. The adapter requires authored speed/capacity/cost/faction values
when consumed and reports provenance for omissions; it does not guess their defaults.

Departure is a pure calculation followed by an ordered identity commit (WGM-24..27,
87/92). Positive direct non-container holds pack non-hyperspace squadrons by descending
cost, then hyperspace squadrons by ascending speed. A packing cut-off through equal
keys refuses at the WGM-U03 boundary. Whole equal-key groups are represented as
canonical identity sets, without reordering live fleet members. Unpacked ineligible
squadrons become a new orbital remnant; zero holds bypass packing and remnant creation.
The minimum eligible unpacked craft speed is multiplied by faction and mode factors.
Route and both-endpoint corruption bonuses select their maximum, starting at one.

The `galactic-locomotion` phase reads immutable gathered fleet rows and writes disjoint
staging slots. Ordered commit follows before fiscal processing (WGM-89). A changed
desired facing uses one service without displacement. Travel then advances once per
logical frame and snaps only when remaining distance is strictly less than twice
velocity magnitude (WGM-28..30). Planet model bounds offset hop endpoints independently
of graph cost. Inaccessible final arrivals bounce toward the previous planet;
inaccessible intermediate hops continue without linking orbit (WGM-31).

Arrival applies the supplied conflict predicate and the fleet-wide combat-skipping
predicate. Intermediate corruption suppression does not retest endpoint corruption;
final arrivals receive no such suppression (WGM-32..34/38). A conflicting arrival
gathers attacker/planet-owner fleets matching both final destination and current hop,
without a proximity radius (WGM-36). Default slots try the previous slot, then 1/2,
then zero; only equal-owner occupants block a preference (WGM-35, GLM-01). Explicit
nonzero slots select the first occupied slot's fleet for equal-owner merge; explicit
zero never selects that automatic merge target (WGM-11/35).

Events **17..21** are movement begin, finished, canceled, bounced and potential
conflict. Reject reasons **11..13** are invalid path, cannot depart and movement
blocked. These numbers are coordinator-reserved. Potential conflict is an admission
seam: it does not fabricate a tactical conflict or resolve a battle. Published transit
positions and movement notifications expose the piracy/interdiction integration
boundary; no WGM-U05 mid-lane rule is invented.

Observer path policy (WGM-18..23), selection/bribe payment (WGM-84/85), AI bribe decisions,
raid/hero/instant landing, political influence, visibility and battle admission remain
with their owning services. They must supply accepted dispatch and arrival inputs;
this service does not infer those values from viewer state or silently charge credits.

GC replay extension **4** stores the movement inputs and initial fleet motion.
Conditional state section **7** hashes those inputs and current motion. Both are
absent when movement is disabled, preserving existing corpus bytes and hashes.
Snapshot extension 4 contains the same data. Immutable input storage is shared between
snapshots. Checkpoint galaxy section 5 appends the framed movement state after the
existing fleet and membership frames, retaining path/hop, speeds, packed identities,
position, facing and producing command. The unreleased v1 formats remain version 1.
Checkpoint reconstruction compares these state bytes before continuing the campaign.
