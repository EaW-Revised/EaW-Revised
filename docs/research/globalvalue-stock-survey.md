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

## Shared scalar implementation

L-55 now has additional debug-build evidence: the shared-table save path writes
all key/value pairs through the Lua value serializer with no script owner. Its
load path first clears the table and then restores those pairs through the Lua
value loader, again without a script owner. A separate reset path clears the
same table. This establishes persistence of this external registry, beyond the
per-instance Lua graph save. A targeted writer cross-reference identifies the
save path as a caller. The evidence does not establish a Create operation;
no Create call appeared in the surveyed stock corpus or the inspected mod.

The game-owned AI engine now holds one ordered registry of binary64 numbers,
byte strings and booleans. Galactic story, AI and object bindings read that
registry. Set stages a command in the calling instance's transaction; the
ordered service tail commits it, including writes from object instances without
a plan. Missing keys return nil, and Set of nil removes a key. False, zero and
an empty string remain distinct from absence. The Stargate story serializer
returns a string consumed by other scripts, so numeric-only support loses its
state. Inspected mod calls also pass booleans and a list; this is not a survey
of all mods. No mod script or asset is copied into the repository.

The native records hash key bytes, value kind and payload canonically and retain
them for service-phase rollback. Numeric records keep their previous encoding.
Checkpoint encoding retains the numeric map field and appends
`global-scalars-v1` only when string or boolean entries exist. The optional tail
contains two sorted maps, strings then booleans; the decoder rejects empty tails,
duplicate keys across kinds, invalid booleans, trailing bytes and ordinary
archive quota violations before publication. Empty and numeric-only stores emit
no tail. The same scalar tail is used by tactical native checkpoint composition.
No checkpoint version or replay opcode changes.

Set admission uses project archive quotas, not bounds inferred from stock values:
keys and string values are at most 1,048,576 bytes, and the registry has at most
262,144 entries. All native collections together also share the archive's
1,048,576-node quota. A count-only walk of the same field inventory measures
existing usage once per batch; scalar-only collections contribute their size
without visiting or copying their values. Before mutation, the ordered batch
accounts for displaced and
new kind-27 record bytes plus the retained native owner's other record bytes.
That sum must fit the 32 MiB native component budget minus its existing 1,024-byte
archive reserve. A rejected write adds a runtime LoadNote and preserves the prior
value and native hash; deletion remains available to release capacity. Quota
refusal does not retire a script or abort the game. This admission work sums the
registry once per batch containing writes and then accounts each command locally.

## Remaining fidelity gaps

- Sequential tables are deliberately not copied into a shared graph: alias and
  cross-instance identity semantics need a separate design. They leave the
  registry unchanged, emit a nonfatal unsupported-API diagnostic and produce a
  runtime LoadNote. Associative/cyclic tables and functions fail at the ordinary
  binding boundary; galactic scripts retire with a LoadNote and the game continues.
- Engine wrapper values produce the script error required by L-55.
- `GlobalValue.Create` has no observed call site or proven contract and is not
  invented here.
- Reads see the preceding completed service's writes, plus earlier host services
  in the same barrier. Exact retail immediate Set/Get visibility remains unverified.
- The galactic story/AI/object host shares one store. Sharing that store with a
  separately constructed tactical child remains unverified and unwired.

The scalar-kind work improves the broader mod gap (legacy EAWR-2209), but does not
close cross-instance tables/functions or every boundary-kind diagnostic. The
stock numeric-only survey remains valid; it no longer describes the host's full
accepted scalar set.
