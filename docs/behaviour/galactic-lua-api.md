# Galactic Lua inventory and host foundation

The [galactic architecture](../galactic-architecture.md#7-lua) uses the same
authoritative `ScriptScheduler` as tactical sessions. This foundation inventories
stock calls and supplies the mode boundary; it does not implement the galactic
AI evaluator, story graph, economy or movement adapters.

## Inventory evidence

`plan/inventories/galactic-lua-api.json` is reproducible metadata from the winning
FoC source hashes in the committed Lua manifest. The generator reads the
unpacked effective reference view and checks every selected script against its
pinned hash before analysis. No stock source bytes are emitted or committed.

The roots conservatively include galactic-mode AI plans, top-level AI
plans, galactic freestore scripts and all story and intervention scripts, then
follow literal `require` dependencies. Mixed-mode story and intervention roots
include tactical calls. These are an upper bound on the API surface, not proof
that every call runs in a galactic campaign. WGA-32, WGA-37..47 and WST-53..57
identify the galactic consumers and shared library contracts in the
[AI walk](walks/gc-ai.md) and [story walk](walks/gc-story.md).

Each resolved API has a status and source locations with call counts. Object
receiver expressions are retained as evidence but do not become distinct API
names. `bound` means shared scheduler infrastructure, `script` means a helper
defined in every applicable root's dependency closure, `partial` means a
standard-library call subject to sandbox policy, and `missing` means a
registered engine API awaiting a galactic adapter (legacy EAWR-1991). Unregistered
callables and dynamic calls remain `unresolved`. `forbidden` identifies the
unsynchronized random capability prohibited by the authoritative sandbox; it
is not offered as a missing adapter. Tactical world bindings never
count as galactic bindings. Standard-library policy is specified in
[the Lua sandbox](../lua-sandbox.md).

The generated `galactic_api_inventory.hpp` contains only missing global API
names and members of engine global objects. `register_missing_galactic_api` installs named `EAWR-SCRIPT-0215` faults;
implemented names are explicitly excluded and registered by their adapters.
Missing object methods use `register_missing_galactic_method` with the adapter's
reserved handle kind. Faults stop and roll back the failing Lua transaction;
they cannot silently substitute tactical-world results.
GC-only infrastructure member extensions retain the protected built-in call
and existing members, so missing members such as `ThreadValue.Get` can fault by
name. Existing infrastructure calls/members and tactical namespace protection
cannot be replaced by these extensions.

## Mode guards confirmed against the wiki

The Focumentation cross-check (legacy EAWR-2170) read the registered guards of several calls
the wiki labels galactic-only. `Find_Path` raises a script error outside galactic
mode and from a non-AI viewpoint (planet-list result). The object method
`Get_Planet_Location` is valid only on galactic-mode objects. `Release_Credits_For_Tactical`
is valid only in a land tactical battle, for an AI player with a galactic AI (WGA-54).
`Apply_Markup` (WGA-429) and `Purge_Goals` need an AI player with the matching
perception or planning system. No mode guard was found for `FindPlanet`,
`Get_All_Planets` or `Is_Corrupted`, so the wiki's "GC only" label for those three is
unconfirmed by static reading, and a runtime check is wanted. Evidence: debug build F2170-E07.

## Host boundary and scheduling

`ScriptedGalacticSession` owns a `GalacticSession` and distinct AI and story
scheduler sets. Fresh GC schedulers use a galactic clock profile at one frame
per 1/30 second; instance IDs are unique across both sets so their shared
counter-based random keys cannot alias. The whole-game coordinator remains
responsible for gating the parent when a tactical child is active.

At each recorded session tick the bridge admits player input, translates the
previous service's ordered script commands into typed `GameCommand` input,
steps the galaxy, services AI when the galaxy ran and services attached story
scripts at that barrier. The translator must supply an AI or script seat;
scripts cannot impersonate a human seat. Accepted input and both script hosts
share each system seat's sequence cursor. Unsupported verbs and invalid orders
are reported deterministically and never reach world storage.

`GetCurrentTime` and `.Frame` read the completed galactic frame. AI service ticks
advance only unpaused. Story service ticks advance at recorded session barriers
while mode time stays frozen during pause (ADR-013, WST-12). WST-54 throttles
thread pumps only when both `ServiceRate` and `LastService` are numbers and
elapsed time is at most the rate; it stamps `LastService` after pumping and
detaches a finished attached script. Attached suspended plots still receive Lua
service (WST-16); graph activity is the future story reducer's responsibility.
Adapters prepare typed notifications from the stepped world and consume ordered
reports through `GalacticScriptEngine`, without changing the galaxy directly.
They key events from the supplied scheduler's next service tick, use their own
reserved producers, and leave story thread pumps and stamps to the bridge.
Decisions must derive from supplied world and scheduler state; a future adapter
with additional mutable state needs its own checkpoint and hash contribution.
Pure preparation may use the executor; Lua service uses the existing
`script-instances` partitioned phase and ordered commit.

Stock `Sleep` and `Register_Timer` are Lua helpers using mode `GetCurrentTime`.
The lower-level C++ `BindingContext::start_timer` retains scheduler-service-tick
deadlines; it is not exposed as a galactic elapsed-time API by this foundation.
Future adapters must use the stock mode-time helpers or provide explicit
galactic-frame timer semantics.

## Checked API behaviour (Focumentation cross-check, debug build)

Rules for the adapters of the missing names above; the wiki page "Lua Commands" and
"Lua and XML: EvaluatePerception" were compared and the debug build decides.

- **GA-01 `FindTarget.Reachable_Target` answers an AI target, never a game object.** For an
  object target (a planet, a unit) the answer is an AI target wrapping it, and a caller converts it
  with `Get_Game_Object()`; stock `galacticfreestore.lua` does so for every planet it picks, and the
  story and intervention scripts do the same. A region answers its AI target as well. No candidate
  gives nil. Contrast `FindTarget` with a TaskForce, which answers the game object for an
  object target (FT-01). The wiki is right; our binding (`find_target` in
  `src/script/foc/plan_bindings.cpp`) answers the game object for both forms, so
  `Get_Game_Object()` on the answer would fault as a missing API (L-16) and the freestore's planet
  redistribution (WGA-34, WGA-35) could not run. Two stock callers use the answer directly: the
  tactical freestore tests it with `TestValid` and passes it to `Fire_Special_Weapon` (M2), and the
  force assembly passes it on as a staging target, so AI-target handles must be accepted there.
- **GA-02 `Find_Path(player, from, to)`.** Exactly three arguments. Only in the galactic mode (any
  other mode is a script error), only for a player with an AI (otherwise an error). `from` and
  `to` are each a planet game object or an AI target wrapping one. The answer is a sequence of
  planet game objects, one per hop of the AI path finder; a bad argument gives an error and no value.
- **GA-03 `GetCurrentTime.Galactic_Time()`** answers the galactic mode's frame count times the
  logical frame length in seconds. It faults when there is no galactic mode (a skirmish).
- **GA-04 `Is_Campaign_Game()` and `Get_Game_Mode()`:** see L-51 in `lua-script-model.md`; the
  campaign test is also true inside a tactical battle that has a galactic parent.
- **GA-06 Object calls with a galactic precondition.** `Get_Planet_Location` is valid only on an
  object owned by the galactic mode (any other mode is a script error); it walks up the object's
  containers to the first one that is a planet and answers it, or nothing when none is.
  `Is_Corrupted` answers whether the planet is corrupted. `Force_Test_Space_Conflict` works only
  for fleets and planets (a script error otherwise). `Set_Check_Contested_Space(flag)` takes
  exactly one boolean. `Mark_Parent_Mode_Object_For_Death` is a script error on a dead object. The
  wiki's remaining GC-only notes (`FindPlanet`, `Get_All_Planets`, `Release_Credits_For_Tactical`, the
  effect of `Invade` outside the campaign) were not read.
- **GA-07 `Resume_Hyperspace_In()`** takes no argument and releases the held hyperspace-arrival
  cinematic only while a space battle is the active mode (WST-50); in any other mode it does
  nothing. It is the release for a link to a battle whose pre-battle cinematic option is 2 (WST-50).
- **GA-05 Script throttle:** story sub-plot scripts use the same `ServiceRate`/`LastService` gate as
  every other engine-serviced script (L-29), already stated for story as WST-54.

## State, replay and remaining adapters

The GC clock save identity is separate from tactical saves; default tactical
save bytes and clock validation stay unchanged. The existing scheduler
persistence stores the frozen mode clock alongside its service tick. The bridge
hash includes named AI/story scheduler hashes, pending script commands and seat
routing cursors. Off-thread world hashing derives the same combined digest.
The world's replay records translated commands and reproduces the galaxy without
running scripts; full Lua replay also requires the pinned modules and adapters.

The foundation exposes each host's scheduler save blob and a typed
`GalacticScriptSave` barrier package carrying those blobs, pending orders and
routing cursors. `restore` takes a galaxy restored to the same barrier and
schedulers with matching pinned modules, bindings and config; the caller then
registers the same pure adapters. Full checkpoint section encoding and adapter
state belongs to checkpoint wiring; the script package alone is not a
whole-game checkpoint. World queries, shared
session flags, story graph/rewards, lifecycle attachment and AI plan service
remain explicit inventory gaps. This does not claim stock solo GC or story
campaign acceptance.

Synthetic tests cover typed next-tick routing and world replay, paused clocks,
strict story throttling, missing-API diagnostics, terminal host failures,
GC scheduler save/load, script-barrier restore-and-continue and 1/2/4/8 worker
combined-hash equality. Inventory tests
check pinned bytes, helper scope, dynamic calls and ambiguous dependencies
without requiring installed game data.
