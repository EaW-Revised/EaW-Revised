# Battle hint lookups and radar blips (Lua, space and land battles)

Debug-build walk, 2026-11-20, for the tactical hosts' unbound `Find_Hint`, `Find_All_Objects_With_Hint`,
`Add_Radar_Blip` and `Remove_Radar_Blip` (legacy EAWR-7977). The galactic forms of the radar calls
(a planet argument) belong to the galactic story host and are not covered here.

| Rule | Observed behaviour | Evidence |
|---|---|---|
| BHR-01 | A battle keeps one list of hint objects in creation order. An object joins it when its type carries the `HINT` behaviour (map markers, story props and the like). The object holds one hint string, which a map placement stores in its own data chunk and which units created by script or cinematic transports can also receive. | debug build; effective XML (`Behavior` rows with `HINT`) |
| BHR-02 | `Find_Hint(type or type name [, hint])` is valid in a battle only. No argument, a first argument that is neither an object type nor a string, and a type name that does not resolve are script errors. It walks the hint list and returns the first object whose original type is the asked type and, when the second argument is a non-empty string, whose hint equals it without case. A second argument that is not a string counts as absent. No match returns nil. | debug build |
| BHR-03 | `Find_All_Objects_With_Hint(hint)` takes exactly one string (a count or type mismatch is a script error), compares without case against every hint object whatever its type, and returns a table of the matches in list order; no match is an empty table. | debug build |
| BHR-04 | `Add_Radar_Blip(position or object, name)` in a battle takes exactly two arguments. A first argument without a deducible position and a second argument that is not a string are script errors. It adds a permanent blip at that position (tied to the object when one was given) under the CRC of the name. | debug build |
| BHR-05 | `Remove_Radar_Blip(name)` takes exactly one string and removes the blip stored under the CRC of that name. | debug build |

## Remake behaviour

- `Find_Hint` and `Find_All_Objects_With_Hint` read `AiSetup::hint_objects`, a creation-ordered list of type, hint string
  and object identity. An entry answers only while its object is live in the world view. The bindings follow BHR-02 and
  BHR-03 exactly, including every error case.
- **Open (fidelity list):** nothing fills the list yet. The hint string a map placement stores is not decoded from the
  map data, and marker placements are not world objects, so on stock maps every lookup answers nil or an empty table,
  which is also the retail answer for a map without hint objects. Because that answer would be silent, each call also records an unsupported-API diagnostic until the host marks map hints as hosted. Decoding the hint chunk and creating hint-bearing
  map objects is a follow-up; script-spawned hint strings (`Spawn_Unit` hints) wait for that work.
- `Add_Radar_Blip` and `Remove_Radar_Blip` check their arguments like BHR-04 and BHR-05 and record a diagnostic of kind
  unsupported-API per call; no blip is drawn because the battle radar's blip layer is not hosted. Ground-only
  behaviour is not separated here: both calls are mode-neutral in a battle.
- Neither pair changes simulation state, so replays, checkpoints and the 76-replay baseline are unaffected.
