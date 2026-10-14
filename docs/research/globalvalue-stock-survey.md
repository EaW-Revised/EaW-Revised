# GlobalValue stock usage and storage decision

The stock value-kind expansion is **not needed for stock**. All 35 active
`GlobalValue.Set` sites pass numbers. The six active `GlobalValue.Get` sites
consume numeric values or handle a missing entry. No stock site stores a string,
boolean, nil, table, function, thread or game object. Shared numeric values are
needed across script instances; reference identity is not.

This is a static survey of all 370 Lua files in the effective FoC `Data/Scripts`
tree, including inherited EaW content, on 2026-10-10. The effective tree resolves
archive overrides before the survey. Every `GlobalValue` token outside comments
and string literals is one of the 41 direct calls below: there are no aliases or
indirect references in this corpus. One additional Set occurrence, in
`story/story_underworld_actiii_m12_space.lua:214`, is commented out and would
write numeric zero. The survey describes call sites, not execution coverage.

## Evidence and interpretation

[L-55](../behaviour/lua-script-model.md) records the debug-build contract and the
Focumentation cross-check: retail permits non-wrapper values and shares the
store across scripts. This survey independently checks the stock-only claim;
it does not narrow that retail contract. The RNG work in the separate range
correction did not expand GlobalValue storage (legacy EAWR-2217).

Paths and lines below are relative to the effective `Data/Scripts` tree. Values
are described in our own words; no stock source is reproduced. Key families:

| Family | Key | Sharing requirement |
|---|---|---|
| Fog | `Allow_AI_Controlled_Fog_Reveal` | Story/scoring writers and land/space AI readers are different scripts. |
| Contact | Player-specific `CONTACT_OCCURED` | Event-library writes can be read by the generic value evaluator in another instance. |
| Opening | Player-specific `OPENING_MOVES_UNDERWAY` | Evaluator writes can be read by the generic value evaluator in another instance. |
| Attacks | Player-specific `num_ai_attacks_left` | Multiple plan instances use the same attack allowance through the taskforce library. |
| Gains | Player-specific `player_gains_before_attacking` | Multiple plan instances share the numeric threshold. |
| Land/Space | Player-specific `recent_land_enemy_controlled` / `recent_space_enemy_controlled` | Multiple plan instances share the previous territory counts. |

The generic evaluator accepts a requested key, so its concrete key depends on
its caller. It does not prove a particular Contact/Opening producer-consumer
pair ran; it establishes the available cross-instance read path. Likewise the
library itself is loaded separately into each plan instance: calls from
`conqueropponentplan`, `deathstaruseplan`, `crushplan` and `raidplan` require one
shared numeric registry rather than one registry per loaded library.

No row below needs object identity or alias preservation. Numbers are copied
values. Missing entries are not stored nil values: the readers explicitly
handle absence where indicated.

## Every active call site

| File:line | Call | Family | Value passed or consumed |
|---|---|---|---|
| `ai/landmode/burnunitsland.lua:69` | Get | Fog | Number compared with 1; missing entry does not match. |
| `ai/spacemode/burnunits.lua:69` | Get | Fog | Number compared with 1; missing entry does not match. |
| `evaluators/getvalue.lua:54` | Get | Requested player key | Returns stored number, or numeric zero when absent. |
| `evaluators/openingmovesrelevant.lua:65` | Set | Opening | Number 1. |
| `evaluators/openingmovesrelevant.lua:70` | Set | Opening | Number 0. |
| `library/pgevents.lua:222` | Set | Contact | Number 0. |
| `library/pgevents.lua:228` | Set | Contact | Number 0. |
| `library/pgevents.lua:468` | Set | Contact | Number 1. |
| `library/pgtaskforce.lua:513` | Set | Attacks | Numeric allowance read from the registry, decremented by 1. |
| `library/pgtaskforce.lua:544` | Set | Gains | Numeric bounded GameRandom result. |
| `library/pgtaskforce.lua:548` | Set | Land | Numeric territory-count perception result. |
| `library/pgtaskforce.lua:549` | Set | Space | Numeric territory-count perception result. |
| `library/pgtaskforce.lua:554` | Get | Land | Number used in territory-count subtraction. |
| `library/pgtaskforce.lua:555` | Get | Space | Number used in territory-count subtraction. |
| `library/pgtaskforce.lua:562` | Set | Attacks | Numeric bounded GameRandom result. |
| `library/pgtaskforce.lua:565` | Set | Gains | Number 0. |
| `library/pgtaskforce.lua:577` | Set | Land | Numeric territory-count perception result. |
| `library/pgtaskforce.lua:578` | Set | Space | Numeric territory-count perception result. |
| `library/pgtaskforce.lua:601` | Get | Requested player key | Stored number, or numeric zero when absent. |
| `miscellaneous/gamescoring.lua:157` | Set | Fog | Number 1. |
| `story/story_empire_acti_m02_land.lua:174` | Set | Fog | Number 0. |
| `story/story_empire_acti_m03_space.lua:179` | Set | Fog | Number 0. |
| `story/story_empire_acti_m04_space.lua:71` | Set | Fog | Number 0. |
| `story/story_empire_actii_m05_land.lua:109` | Set | Fog | Number 0. |
| `story/story_empire_actiii_m09_land.lua:91` | Set | Fog | Number 0. |
| `story/story_empire_activ_m10_space.lua:170` | Set | Fog | Number 0. |
| `story/story_focus_demo_felucia_space_tactical.lua:81` | Set | Fog | Number 0. |
| `story/story_rebel_acti_m01_space.lua:241` | Set | Fog | Number 0. |
| `story/story_rebel_acti_m02_land.lua:151` | Set | Fog | Number 0. |
| `story/story_rebel_acti_m03_land.lua:90` | Set | Fog | Number 0. |
| `story/story_rebel_actii_m06_land.lua:241` | Set | Fog | Number 0. |
| `story/story_rebel_actiii_m07_space.lua:123` | Set | Fog | Number 0. |
| `story/story_rebel_actiii_m08_space.lua:139` | Set | Fog | Number 0. |
| `story/story_rebel_actiii_m09_land.lua:199` | Set | Fog | Number 0. |
| `story/story_rebel_actiii_m09_space.lua:123` | Set | Fog | Number 0. |
| `story/story_rebel_actiii_m10_land.lua:97` | Set | Fog | Number 0. |
| `story/story_underworld_acti_m02_space.lua:103` | Set | Fog | Number 0. |
| `story/story_underworld_acti_m02_space.lua:753` | Set | Fog | Number 1. |
| `story/story_underworld_acti_m02_space.lua:761` | Set | Fog | Number 1. |
| `story/story_underworld_actii_m06_land.lua:202` | Set | Fog | Number 0. |
| `story/story_underworld_actii_m09_space.lua:260` | Set | Fog | Number 1. |

## Current host boundary and mod limitations

The authoritative `Value` represents nil, boolean, binary64 `LuaNumber`, string,
stable host handle and recursively copied sequential lists. The binding reader
accepts those kinds; tables must contain exactly contiguous integer keys 1..n.
Associative or sparse tables fail with `EAWR-SCRIPT-0210`. Functions, threads and
unrecognised userdata also fail with that diagnostic. Recursive sequential
tables terminate at the binding's 16-level nesting limit with a script error;
they cannot cause unbounded recursion. Acyclic table aliases are copied and lose
identity. This binding boundary is distinct from the per-instance Lua graph
save mechanism described in [Lua persistence](../lua-persistence.md), which can
retain cycles and closures inside an instance.

The AI GlobalValue binding is narrower still: its registry maps a string to
`LuaNumber` (the AI's `Real` alias). Numbers are staged and committed; Get returns
the number or no value. Nil, boolean, string, sequential list and recognised
host handle cross the ordinary binding boundary but Set currently silently
ignores them. Thus **not every unsupported mod call currently diagnoses**.
Malformed Set arguments report a script error; graph kinds rejected before the
binding report `EAWR-SCRIPT-0210`. These are controlled script failures, not
native crashes. Wrapper rejection required by L-55 is also a fidelity gap when
a recognised handle reaches this numeric-only Set.

The desired failure contract for unsupported mod kinds is an explicit script
diagnostic naming the API and kind, leaving the registry unchanged and following
the scheduler's normal script-error handling. Fixing silent rejection, extending
the accepted kinds, and defining cross-instance table/function identity are
mod-support follow-ups (legacy EAWR-2209), not implemented by this stock-only decision.
No common-mod corpus was supplied or surveyed; no claim about common mods is made.

## Minimal deterministic design

Retain one shared string-to-LuaNumber registry for the AI host. This already
covers every observed stock value kind without a cross-instance graph store or
a change to `Value`. Preserve the binary64 number representation; do not narrow
it through host floats or integers. Preserve exact key bytes, including the
player-specific names, and distinguish an absent key from stored numeric zero.

Writes must retain the existing staging and ordered commit path rather than
mutating shared state during partitioned Lua service. Collision resolution must
follow stable command order, independent of worker count. Any owner-level save
or hash of this registry must enumerate keys canonically and encode the numeric
bits, so 1/2/4/8 workers agree. These are deterministic design requirements, not
a new claim about retail scheduling, immediate read-after-write visibility, or
checkpoint coverage in the current tactical host; those seams are outside this
value-kind survey. The existing per-instance graph codec alone does not save an
external registry.

Decision: close the stock **code expansion** as “not needed for stock.” Keep the
broader retail/mod fidelity gap explicit. This docs-only decision changes no
simulation, bindings, save format, hashes or replay pins and needs no replay or
installed battle run.
