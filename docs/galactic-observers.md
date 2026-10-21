# Galactic discovery and observer intelligence

The galactic session retains canonical intelligence for every player and planet.
Its human observer set follows the agreed player roster in ascending identity order;
startup reveals each human's own home and player load refreshes that same set on every peer.
Solo games retain an explicit `solo_reference_human` for WGA-127/264 AI policy.
Multiplayer AI reads its own knowledge and perception, retaining the multiplayer
usability override even when its discovery producer has not initialized a planet.

`frame(seat)` resolves the requesting human after agreement and publishes that
player's observations, menus, forecasts, notices and hero returns. These derived
caches are keyed by player and never encoded, hashed or restored over a peer's
chosen seat. Canonical simulation snapshots retain all-player query results.
Presentation reads `GalacticSnapshot::observations()` instead of treating the raw
owner as an intelligence fact. Discovery and knowledge are independent:
intelligence can expose a category without discovering the map location, and a
discovered planet need not expose its owner, income or force contents.

Planet facts mirror the live control state at each control commit, undefended
landing completion and control-battle return. Ownership, target owner, controller,
integer political control and base owners/levels therefore follow captures
instead of retaining startup values. WGUI-100's actual-state branch supports
these live inputs; the existing knowledge predicates still decide what each
observer can see. AI readers of allegiance and target owner also receive these
live values after a capture.

Fidelity gap: when a later control commit has no cap controller, the observer
adapter substitutes the current owner. This fallback is **unverified** against
retail. Successful observer startup cannot contain an unlisted controller,
including zero, because setup validation rejects it; the fallback applies to
later commits rather than changing the startup provider.

## Discovery

WGUI-01..05 and GFD-23/GFSC-13 place discovery before current object movement.
It reads prior committed orbit presence and current allegiance through copied
inputs, stages fleet traits and per-observer counters in partitioned phases,
then commits in player/planet order. Allied allegiance and corruption benefits
can qualify immediately. Nonstealth local orbit presence increments the reveal
counter only while unpaused; pause or loss of qualifying presence resets it.
The old counter must be strictly greater than the authored delay in logical
frames before the orbit branch reveals a location.

Automatic reveal uses exact squared coordinates with a strict range boundary;
zero range disables the distance filter. Trade endpoints have no distance gate.
Locally owned candidates are excluded. Neighbours are derived once at setup;
discovery performs no quadratic geometric query during a tick. A pass does not
invent a repeated topology cascade: WGUI-107 expands only qualifying allied,
orbit-delay or corruption seeds, including already discovered eligible seeds.
Fresh discovery is false and its counter is zero (WGUI-99). Explicit setup
records also support reconstruction of synthetic and checkpoint states.

## Knowledge and apparent memory

WGUI-06..18 and WGUI-98 define independent intelligence categories and queries.
The owned human default is 40959, an owned AI default grants all categories,
and the canonical Rebel/Underworld default is 17; Empire and unknown factions
use zero. Canonical faction definition IDs are supplied after name lookup;
neither enum ordinals nor corruption benefits select the default mask.
Stealth gates, base thresholds and corruption shortcuts remain predicate-specific.
Income, strongest-ship and force-sensitive queries have no corruption shortcut;
the superweapon query is unconditional. Individual-object queries preserve the
separate minor/major hero detection and orbit/ground container branches.

`known_owner` and `known_political_control` exist only when their current query
succeeds. Target ownership selects the default mask; current ownership supplies
the live owner fact. Apparent ownership returns that fact when visible and
otherwise the retained owner. Apparent allegiance returns current integer
control/controller when political control is visible, or when a local orbit
fleet belongs to a non-Empire observer. An Empire observer with local orbit
presence sees current control below one, and otherwise sees zero with no
controller; without these predicates it reads the retained tuple. WGUI-99/100
initialize apparent control and owner to
zero. An ordinary local refresh clears mask/control/owner to 0/0/-1 before
recomputation. A direct recomputation preserves prior bits and caches the
**current actual** owner/control when the respective bit is lost. This is not a
generic last-seen world model, and ordinary refresh can erase that memory.

## Effects and input seams

An `ObserverCommand` on the script seat carries the active mode frame, logical FPS and
lifecycle facts from the control, ability and clock owners. Human seats cannot
fabricate intelligence. WGUI-19/20 normalize spy duration and prerequisite
categories. WGUI-21..23 append source/ability identities, OR overlapping sources,
and remove all records matching a removed identity. Expiry occurs on an explicit
refresh only when final frame is strictly less than the supplied current frame.
An ordinary nonpositive duration expires at frame zero; -1 is permanent;
positive seconds convert to frames with half-up rounding. WGUI-102 permits
linked tactical refresh with child time, including a clock lower than the
retained galactic clock. Player load restores ordered sources and observer
state, then performs ordinary refresh before mode fix-up; exact resync keeps
the restored state unchanged.

The `linked_child_load_clock` producer seam remains for player loads that restore
an active tactical child: the campaign clock owner must supply the active mode's
fix-up frame/FPS instead of the retained galaxy clock. WGUI-102 and
active-mode load-clock integration (legacy EAWR-2242) track
that integration and its save-during-battle case. Galaxy-only load and exact
resync are covered here.

A spy change refreshes every observer at the affected target; a generic modifier
refreshes only its recipient. WGUI-24 planet-self transfer removes the prior
recipient's matching identity and adds the new recipient's effect at the planet,
or across its strictly bounded nearby query when influence is positive. Other
sources do not use that transfer branch. Failed inputs leave source and memory
state unchanged. The source must exist when adding or transferring; cleanup
can remove a source after its destruction.

The observer setup is replay extension 12, payload schema 2, and runtime state section 13.
Checkpoint planet payloads retain that state, and sparse section IDs leave
other services' reserved IDs available. Galactic rules version 2, state encoding 4
and checkpoint galaxy version 36 reject older project data. The multiplayer rules
identity uses the `eawr-gc-rules-v2` domain. Source lists, timers and apparent-control
history remain canonical for every player; view context is never part of those bytes.

WGUI-12/103 ownership-bit acceptance requires both base stealth flags false.
Orbit fallback requires an observer-owned fleet and ground stealth false.
Known ground contents also expose ownership when any nonnull, non-delete-pending
landed transport exists, regardless of its owner or separate death flag.
Automatic ability influence cadence and ordinary movement/death qualification
remain WGUI U-03 producer seams. Stock probe and specialized
spy masks (WGUI-25..27) can be supplied through the validated effect interface;
this slice does not infer missing XML inheritance or install autonomous source
services. AI discovery initialization retains its owning producer seam.

WGUI-101 source-event actions synchronously insert/remove enabled
Galactic-automatic effects. They derive the recipient from the live source's
current owner, validate spy mask/duration and refresh every observer before the
command returns. The action interface supplies influence insertion/removal
events; no unsourced polling cadence is introduced. Owner-destroyed despawning
termination leaves a consumed probe's timed effect attached; ordinary termination
removes its matching effects at registered targets. Ordinary death and
owner-transfer races retain their targeted capture qualification.

`galactic_observer` covers reveal timing, stealth/corruption and range boundaries,
category predicates, overlapping source expiry/removal/transfer, both apparent
refresh paths, replay equality on 1/2/4/8 workers and checkpoint continuation.
