# Tactical station-script queries and creation

Bounded read-only debug-build checks for the station-upgrade path. Evidence is
static consumer evidence; the retail timing witness remains open (legacy EAWR-3802).

| Rule | Behavior | Evidence |
|---|---|---|
| SSU-01 | Type `Get_Base_Level()` returns the authored unsigned `Base_Level` as a Lua number. It does not derive a level from the type name or clamp it to stock values. Extra arguments are ignored. | debug build: type query and field getter |
| SSU-02 | `Find_Player` takes exactly one string. Case-insensitive `local` reads the configured local-player slot, independently of the calling script or the selected player's human/AI flag. An empty roster or invalid local slot yields no player after the debug assertions. Ordinary skirmish setup first selects an entry marked local and not AI; without one it asserts and bypasses that normal lobby construction branch. Continuing after that assertion is not evidence for a supported retail AI-only skirmish. | debug build: binding, slot getter, player reset and skirmish player construction; WST-90..94 |
| SSU-03 | `Create_Generic_Object` takes exactly three arguments: type name or type wrapper, an extractable position, and a player wrapper. It creates through the active mode's ordinary object factory at that position with zero facing and the supplied owner, returning one object wrapper. Invalid arguments or failed creation raise a script error. It does not enter the tactical purchase queue or debit its price. | debug build: creation binding |
| SSU-04 | Type `Get_Tactical_Build_Cost()` returns the integer authored tactical price without player discounts. Multiplayer tactical uses `Tactical_Build_Cost_Multiplayer`. Campaign tactical uses `Tactical_Build_Cost_Campaign` when at least one credit, otherwise the multiplayer price. Extra arguments are ignored. | debug build: type query and mode-sensitive price getter |
| SSU-05 | Created-upgrade attachments retain full-width object wrappers independently of physical allocation, script instance and generation. Existing unsigned 64-bit save fields carry the wrapper unchanged. An object-script handle is a physical object ID below 2^37 or a direct-creation reference (bit 63 set); other values are refused on load and attach. A creation whose owner has no economy account (a defeated or account-less seat) raises a script error at the call instead of failing at the ordered commit, and a loaded direct-created upgrade must hold a physical object ID shared with no live unit or other held object. | project attachment seam |

The remake carries an explicit local view in its tactical script configuration.
Ordinary local starts select their human lobby seat. The headless AI-only
diagnostic explicitly selects its first configured lobby seat as the local view;
it remains AI controlled. This is a project fixture policy, not a claim that
retail's normal skirmish setup admits a lobby without a local non-AI entry.
Missing or invalid selections stay nil; scripts never substitute their owner.
The selected view participates in the native script-configuration identity.

RaW's station library queries local difficulty, then the station's base level,
then schedules direct creation of upgrade types. Those creations are distinct
from the ordinary research goal and its population-room gate (WAS-07/16).
Queue-completion bookkeeping (WPR-22) must not be inferred from a call to the
generic factory. Automatic effects of an existing upgrade object are WPR-51.

The current tactical factory capability is ordinary space upgrade objects with
loaded upgrade rules and a commandable owner. Other types, including station
level-up objects and upgrades requiring a station-backed bounty source, raise an
explicit Lua error. This capability boundary does not
claim support for ship, squadron or land factory creation. Direct objects persist
without a station holder and supply their ordinary existing-object bonuses.
They do not increment queued-build lifetime counts or tech, or remove an earlier
upgrade; those operations belong to WPR-22 queue completion.

FH-40 remains the project command boundary: world effects commit in order on the
next tick. The calling instance can immediately query its own returned reference's
validity, type, owner and position from its staged command. Other instances see
it after commit. The retained reference uses the high bit plus a 43-bit script
instance and 20-bit command sequence; overflow raises an error. This is a project
reference quota, not a retail object limit. The ordered world commit allocates
the ordinary hidden-object ID separately. Both identities and the position are
retained by replay/checkpoint state, independent of worker count.
If ordered factory execution fails after a staged wrapper was returned, the
entire tick fails before publication. Earlier creations in that tick, wallet
changes and the physical ID cursor remain uncommitted. Duplicate references,
unsupported factories and physical ID exhaustion exercise this contract on
1/2/4/8 workers, including a retry of the failed tick.

**SSU-05: created-object attachment seam.** A direct-created upgrade with an
authored `Lua_Script` uses the ordinary attachment lifecycle. Its script instance
and generation use the physical allocation ID; its `Object` wrapper keeps the
full 64-bit creation reference returned to the creator. Due pumps retain that
wrapper explicitly instead of deriving it from the script instance. The generic
script registry accepts nonzero 64-bit object handles independently of instance
identity; duplicate, instance and generation checks remain. Existing save fields
already store these handles as unsigned 64-bit values, so the save format is
unchanged. The production bridge contract checks attachment, wrapper identity,
continued object-turn effects and save/load on 1/2/4/8 workers.
