# Campaign credits, tech and multiplayer heroes

The [campaign setup boundary](campaign-setup.md) now plans the player passes of
[WGS-11..14](walks/gc-session-save.md), then commits represented startup economy
when the typed services finish. The actual single-player/multiplayer mode comes
from the caller; the campaign's menu metadata does not select a runtime mode.
Standalone tactical startup remains its own skirmish path and receives no GC
credit or tech assignment from this API.

Authored credits and starting tech use the first case-insensitive faction record,
preserving duplicates. Missing records return zero. The tracked debug-build
getter rule WGS-73 establishes the maximum-tech getter's missing-record value
as the code default five. Authored tech receives no displayed-tech adjustment.
Admitted custom options supply the same credits and start/max tech for each
player. Any nonzero faction displayed-tech adjustment subtracts exactly one from
each custom tech request, including negative adjustments and magnitudes other
than one. The faction constructor supplies zero for an absent adjustment
(WGS-76). These are gameplay option values after any staging display conversion;
the planner does not repeat that UI conversion.

Campaign staging and advanced option admission (WGS-75) establish
that selection resets custom options, unsupported definitions hide/disable the
option entry, and the advanced campaign dialog requires support. The planner
therefore refuses custom options when the selected validated definition has
false support; omitted support retains WGP-74's true constructor default. It does not reject a story definition merely because it
is a story. Full menu filtering, tutorial/story visibility and network option UI
remain WGS-U02; this check is the campaign option admission boundary only.

The order is object assignment (including caps/homes and multiplayer heroes),
then the complete credit pass, then the tech pass. The inspected credit setter
stores the supplied value directly, without clipping it to the cap (WGS-74).
WGS-74 establishes that the runtime custom-credit getter supplies a signed 32-bit integer; custom values
outside that domain refuse, while representable negative amounts remain direct assignments.
WGS-77 establishes the assignment order. Tech assignment calls the owning service to set current tech with initial
unlock processing enabled and notification disabled, then set maximum tech.
That service owns the prior-maximum clamp and production/story side effects;
its returned committed current value enters the startup ledger. The adapter
does not clamp current tech against the newly requested maximum in advance.
Missing tech services explicitly refuse root creation. Future fiscal,
production, unlock-list and story updates remain their owning subsystems.

The hero helper runs only in actual multiplayer when custom settings are absent
or heroes are allowed. A missing home creates no replacement. A resolvable home
must have production before its faction's stored hero types are visited. Each
candidate uses a complete query of owned objects by original type, rather than
displayed/current type. A live creation guards later duplicates for that player;
a verified no-live-result does not. Creation remains the typed placement service
and may expand into several persistent objects. The same allocator and roster
checks as starting forces apply; hero creation does not rerun political control
or starting-force human guard unification. WGS-U08 nested production internals
remain explicit owning-subsystem work.

`PlayerEconomySetup` contains one ordered row for every root player: current
credits, cap, current tech and maximum tech. Coordinator-reserved GC replay
extension tag three carries it. Its canonical payload joins the existing
players state, snapshot and checkpoint section only when present; absent economy
keeps legacy bytes unchanged. No new commands, events, state sections, checkpoint
IDs or versions are introduced. The campaign identity additionally includes
actual mode and admitted custom economy/hero inputs under the existing
`eawr-campaign-setup-v1` domain. Replays and checkpoints reconstruct committed
values without rerunning campaign planning or the startup providers.

Contracts cover the three human factions and ordinary/story/tutorial/multiplayer
definitions, first-record values, missing-record defaults, signed nonzero tech
adjustments, hero on/off and original-type guards, duplicate/expanded/no-live
hero creation, replay/checkpoint retention and 1/2/4/8-worker equality. Legacy GC,
graph and fleet fixture bytes must round-trip unchanged, and their existing pins
remain untouched. This work does not complete plot/AI setup, influence/object
linking or the persistable scene/ground/base/production models needed to launch
a full stock campaign.
