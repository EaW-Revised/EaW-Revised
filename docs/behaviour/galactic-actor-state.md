# Live galactic actor facts

The actor record owns combat-skipping ability state and the retained garrison
flag. Consumers read completed immutable records; unavailable facts refuse.
These values are independent of unit strength, observer intelligence and fleet
location. Changes publish through the sparse actor notification boundary;
idle ticks retain the same record publication.

WGM-160..163/169 govern the ability owner. An admitted null/self-target effect
sets one actor's combat-skip scalar, clamps its evade chance to the authored
0..1 interval and records its self target. Removal clears chance, skip and
registration. Successive effects are scalar writes. Fresh automatic abilities
use resolved current constructor authoring; disabled abilities retain fresh
zero state. Specific-handler validation corrects other activation styles to
galactic automatic. Unknown lifecycle callers remain GST-U01/02; arrival,
combat and discovery do not independently clear the owner.

The recursive nonempty/all predicate follows actual fleet, squadron and
transport contents. A transport ignores its own skip flag, and empty
containers do not skip. Deletion-pending actors skip before dispatch. One
ordinary companion defeats fleet-wide stealth. Missing dispatch, missing
payloads and cyclic containers refuse. Movement, control and hostile HUD
admission read the owner; explicit standalone fixture inputs retain their
separate supplied-fact boundary. WGM-165..168 detector actions, observer
minor/major intelligence, raw AI strength and presence remain independent.

GFS-07 control reads immutable derived fleet skip inputs keyed by the actor
and membership publication generation. A dirty partitioned phase rebuilds
them; clean ticks perform no recursive ability scans. These inputs are never
saved as a second owner. Split/merge commits keep nested contents intact and
relocate their parent chain to the actual root fleet.

| Rule | Observable retained-garrison behavior | Evidence |
|---|---|---|
| GAG-01 | Fresh ordinary construction starts false. The constructor's constant initial status has this bit clear. A station, planet defender, landed company or actor at a particular location does not gain it from that classification. | debug build GAG-E01: constructor, constant initial flags and retained getter |
| GAG-02 | The explicit setter writes a Boolean without inspecting type or location. The traced callers mark tactical spawned troops, squadron members and their team, single spawned ships, deployed troops and tactical escort construction. The spawned-unit dispatcher admits land/space and returns failure in galactic mode. Its team constructor accepts the caller's explicit garrison argument. The setter-reference census contains no clear call; no arrival, combat or detection clear is established. The GC producer supplies false only at qualified ordinary construction; foreign/generated provenance needs its lifecycle owner. | debug build GAG-E02: setter-reference census, spawn dispatcher, nested squadron/team creation and starting-force tactical overload |
| GAG-03 | Retained status is read directly, independently of tactical building occupancy and the separate spawn-enabled flag. The world archives the authoritative value once. An absent provenance stays unavailable, rather than inferred false. A trusted lifecycle owner may write its explicit value; death/removal destroys the actor record. | debug build GAG-E01/02; project archive boundary |

The galaxy checkpoint section is version 9, production constructor codec 4,
and canonical snapshot format 3. Live ability scalars, self-registration,
qualified dispatch and optional garrison provenance survive restore before
the next consumer; no original-type activation reconstruction replaces them.
The installed proof requires a stealthed hero fleet and the sourced false
garrison bit on ordinary stock GC actors. A stock true GC spawn is not claimed.
The trusted `set_actor_garrison(true)` writer currently has no production
producer: the GAG-02/03 tactical spawn paths remain unwired to this GC owner.
