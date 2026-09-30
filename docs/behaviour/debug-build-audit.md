# Audit of implemented behaviours against the FoC debug build

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption. The code source is the FoC debug
  build with its program database (class, function and enum names, asserts), read on
  2026-09-26 under the [clean-room rule](../clean-room.md). Data claims were re-read in the FoC
  profile of the retail XML. Recordings are the P2-06 fidelity scenarios
  ([tests/fidelity](../../tests/fidelity/README.md)) and the EAWR-67 start capture.
- Bounded question: for each gameplay or simulation claim in the behaviour notes below, does
  the original code do what the note says? Issue
  EAWR-265.
- Scope done: priorities 1 to 4 of EAWR-265 (space targeting, hardpoints, visibility and query
  order, tick rate, skirmish start, unit data loading) in part 1, and priorities 5 and 6 (Lua
  host, compiled chunks, API declarations, FoC tactical AI, XML registry, tactical camera input,
  river groups) in part 2. Locomotion (EAWR-70) and the Lua number precision question (EAWR-246, settled
  by EAWR-263) are excluded.
- Part 2 also read the installed FoC data where a claim depends on it: the Lua manifest
  (`plan/inventories/lua-manifest.json`) and the terrain-track records of the eight FoC maps
  with ribbon groups.
- Evidence IDs AU-nn are opaque. Their map (symbols and locations) is private and stays under
  the ignored `out/research/`. Part 1 uses AU-01 to AU-83, part 2 AU-100 to AU-141, and the
[inference sweep](#inference-sweep-2026-09-26) IS-01 to IS-16. Evidence IDs
  of earlier notes (E72-nn, TR-En) are theirs.

## Verdicts

| Verdict | Meaning |
|---|---|
| confirmed | The code does what the claim says. A note may still gain a detail. |
| differs | The code does something else, or the claim leaves out a case that changes an outcome. The note is corrected. |
| not found | The code path was looked for and not located in this pass. |
| not checkable | A static reading cannot settle it (runtime order, float equivalence, data-dependent tree order). |
| n/a | A data, owner or remake (project) rule with no original-code counterpart to check. |

Where code and recordings disagree the recording wins. No such disagreement was found: every
recording that bears on an audited claim (S-01, S-02, S-03, the EAWR-67 capture, and in part 2 the
owner's Naboo river stream) agrees with the code.

A conclusion that the original never does something rests on code alone. Such conclusions are
marked as needing a runtime check and listed under [runtime observations](#runtime-observations-needed).

## Summary

| Note | Claims | Confirmed | Differs | Not found | Not checkable | n/a | Corrected |
|---|---|---|---|---|---|---|---|
| [space-targeting.md](space-targeting.md) | 26 | 19 | 5 | 0 | 2 | 0 | yes |
| [space-hardpoints.md](space-hardpoints.md) | 35 | 25 | 0 | 0 | 3 | 7 | yes (details) |
| [space-visibility.md](space-visibility.md) | 21 | 8 | 4 | 0 | 1 | 8 | yes |
| [tactical-tick-rate.md](tactical-tick-rate.md) | 10 | 5 | 0 | 0 | 2 | 3 | yes (TR-04, TR-U2) |
| [skirmish-start.md](../skirmish-start.md) | 9 | 2 | 1 | 1 | 0 | 5 | yes |
| [unit-data.md](../unit-data.md) | 11 | 4 | 2 | 0 | 1 | 4 | yes |
| [lua-script-model.md](lua-script-model.md) | 55 | 33 | 5 | 10 | 2 | 5 | yes |
| [pglua-chunks.md](pglua-chunks.md) | 36 | 22 | 2 | 0 | 1 | 11 | yes |
| [lua-api-declarations.md](lua-api-declarations.md) | 10 | 1 | 0 | 0 | 0 | 9 | yes (FoC relation) |
| [foc-tactical-ai.md](foc-tactical-ai.md) | 47 | 12 | 3 | 4 | 0 | 28 | yes |
| [xml-registry-edge-cases.md](xml-registry-edge-cases.md) | 23 | 3 | 0 | 2 | 0 | 18 | yes (detail) |
| [xml-registry-retail-addendum.md](xml-registry-retail-addendum.md) | 6 | 0 | 0 | 0 | 0 | 6 | no |
| [xml-registry-file-service-addendum.md](xml-registry-file-service-addendum.md) | 1 | 0 | 0 | 0 | 0 | 1 | no |
| [tactical-camera-input.md](tactical-camera-input.md) | 16 | 2 | 0 | 1 | 0 | 13 | yes (original facts) |
| [foc-river-group-selection.md](foc-river-group-selection.md) | 11 | 1 | 3 | 1 | 0 | 6 | yes |

Hardpoint counts include the six G-H gates; targeting and visibility counts include their
gates. The Lua, chunk and AI counts include their gates and cases. A gate that the audit
resolved, fully or in part, is counted as confirmed.

## Space targeting

| Claim | Verdict | Reason |
|---|---|---|
| R-01 | differs | The service gates and the no-clear-on-rejection rule hold (AU-01). Missing: for a hardpoint owned by an AI player, both opportunity-enable flags count as on whenever its maximum recharge is at most `Hardpoint_Recharge_Cutoff_For_Opportunity_Fire` (3.0 s in FoC), whatever the hardpoint data says (AU-14). |
| R-02 | differs | A successful retained attempt ends processing without an event (AU-01). Missing: when the parent is inside a nebula and its position is fogged to the retained target's owner, the retained attempt counts as failed. |
| R-03 | confirmed | A failed retained attempt clears the reference and forces a scan in the same service (AU-01). |
| R-04 | confirmed | The scan runs when the truncated half of the logical rate is strictly less than the frames since the last scan; the frame is recorded before the search (AU-01). |
| R-05 | confirmed | Parent opportunity-fire suppression, then active stealth, both before the draw; the nebula check uses the parent's position against the visited player's fog and spends a visit without advancing (AU-02). |
| R-06 | confirmed | One synchronized draw over all player slots; owner, neutral and non-enemy slots advance; an absent slot spends a visit in place; the search stops only on an accepted priority 1.0 (AU-02). |
| R-07 | confirmed | Box half extent 1.1 times the current range around the parent, over the visited player's owned collidable objects, in collection order (AU-03). The current range is the data range times one plus the range modifiers, times the garrison multiplier while the parent is in limbo (AU-10). |
| R-08 | differs | The filter set holds (AU-04), but two details change results. A turret hardpoint must be able to point at the candidate's position before priority and aim points are looked at. The candidate's own opportunity-fire-disabled flag is tested only after the aim stage and the priority comparison, so a disabled candidate still consumes its aim-point draw. |
| R-09 | differs | Lower wins and ties keep the first (AU-03), but the declared FoC `Fighter` set is Transport 1.0, Bomber 2.0, Fighter 3.0 (data; recording S-01 agrees), not the values the note gave. The lookup rules were also missing: an exact type entry wins; otherwise the smallest weight among matching category entries; exclusions give no priority; a category the set does not list ranks after every listed one instead of being excluded; a parent type restricted from attacking the category gives no priority; a parent without a targeting behaviour or without a set scores every candidate 1.0 (AU-06 to AU-09). |
| R-10 | differs | Order holds (AU-05). Missing: a target without hardpoints consumes no hardpoint draw, and the hardpoint pass skips only destroyed hardpoints, not untargetable ones. |
| R-11 | confirmed | Weapon midpoint to point, three-dimensional, inclusive; soft radius added for bones and hardpoints only when the parent has a current projectile type, always for the fallback (AU-05). |
| R-12 | confirmed | A new target is tried at once; failure clears it silently; success raises the acquisition signal; a retained success raises none (AU-01). |
| R-13 | confirmed | The opportunity path passes no hardpoint reference (AU-01). Added detail: the firing attempt then aims at the target's nearest live targetable hardpoint and falls back to the aim-point search only when there is none (AU-12, AU-13). |
| R-14 | confirmed | The scan frame advances whether or not a target is found (AU-01). |
| R-15 | confirmed | Neither the scan nor the firing attempt tests line of sight or obstruction (AU-01 to AU-05, AU-12). |
| C-01 to C-08 | confirmed | Each follows from the rules above; C-01, C-03 and C-04 are also backed by recordings S-01, S-02 and S-03. Counted as eight claims. |
| G-01 | not checkable | The collection comes from a spatial tree in collection order; its order depends on tree history. EAWR-469 read the tree and implements it (space-targeting CO-01 to CO-12). |
| G-02 | not checkable | Binary32 against Q24 equivalence needs runtime comparison. |
| G-03 | confirmed | Recipient resolved: the signal goes to the squadron when the parent leads its squadron, to nobody for other squadron members, and to the parent otherwise; the payload names the target and the hardpoint (AU-01). Timing stays with S-03. |

## Space hardpoints

| Claim | Verdict | Reason |
|---|---|---|
| HD-01 | confirmed | Hull and hardpoint maxima times the space multiplier; the AI difficulty health multiplier applies only to AI-owned objects; then one plus the health modifiers (AU-28). |
| HD-02 | confirmed | A hit on a live destroyable hardpoint damages only it; a hit on a destroyed one does nothing; a hit on a non-destroyable hardpoint goes to the hull (AU-20, AU-21). |
| HD-03 | confirmed | Health clamps at zero and the hardpoint's effects switch off there (AU-22). |
| HD-04 | confirmed | Threshold is data; the retail reticle colours come from the EAWR-72 research (E72-13) and were not re-read. |
| HD-05 | n/a | Data. |
| HD-10 | confirmed | A destroyed or disabled weapon hardpoint may not fire (AU-11). |
| HD-11 | confirmed | Last engine lost: engines off for good, TURBO and SPOILER_LOCK end, maximum speed times the engines-disabled modifier (AU-23, AU-24). |
| HD-12 | confirmed | Last shield generator lost: shield off, DEFEND ends (AU-23). |
| HD-13 | confirmed | Space spawners drop destroyed destroyable bays and launch nothing without one (AU-30). Added detail: an active DEFEND also blocks launches. That the list holds the fighter bays stays an inference. |
| HD-14 | confirmed | The special-ability hardpoint switches its ability off (AU-23). |
| HD-15 | confirmed | A lost weapon hardpoint recomputes the AI directed firepower (AU-23). |
| HD-20 | confirmed | A hull at zero is death; removal timing is the remake's. |
| HD-21 | confirmed | The flag kills on the last destroyable hardpoint (AU-20). |
| HD-22 | confirmed | Otherwise hardpoint loss never touches the hull (AU-20). |
| HS-01 | confirmed | Each object service, if alive with at least one destroyable hardpoint: the hull cap, then the hardpoint drag, then each hardpoint's own service (AU-25, AU-26). |
| HS-02 | confirmed | Cap at min(1, S/T + C) of the maximum (AU-25, AU-27). |
| HS-03 | confirmed | Excess against min(1, h + C); nothing when not positive (AU-26). |
| HS-04 | confirmed | Loss proportional to each live hardpoint's current health over T; the drag never kills (AU-26). Retail takes T from the data maxima and S/T from the instance maxima; with no difficulty or modifiers, as in M2, they are the same. |
| HS-05 | n/a | Remake arithmetic. |
| HR-01 | confirmed | Per paying player per service, one amount for one cost; each paying player repairs in the same service (AU-29). |
| HR-02 to HR-05 | confirmed | Destroyed stops; unpaid drops out; hull raise when below the combined fraction; at full health the repair ends and re-enables the hardpoint (AU-29). Counted as four claims. |
| HR-06, HR-07 | n/a | Data and owner rules. Counted as two claims. |
| HD-30 to HD-32 | n/a | Remake command rules (the Lua call was not re-read). Counted as three claims. |
| G-H1 | confirmed | Resolved in part: within one object's service the drag runs before that object's weapons. The order between projectile hits and other objects' services stays open. |
| G-H2 | not checkable | The flag default was not read. |
| G-H3 | not checkable | Disables are EAWR-74 and EAWR-76 scope. |
| G-H4 | not checkable | The repair event's admission checks were not read. |
| G-H5, G-H6 | confirmed | Retail changes hardpoint art only at destruction (AU-22); retail computes in binary32 (AU-26). Counted as two claims. |

## Space visibility

| Claim | Verdict | Reason |
|---|---|---|
| V-01 | n/a | Data. |
| V-02 | confirmed | Tags apply in document order, so the last single-value occurrence wins (AU-80). |
| V-03 | differs (runtime, RO-1) | The craft carry no `REVEAL` behaviour (AU-44, AU-48), as the code reading found, but fighters do reveal: RO-1 (EAWR-275) recorded a lone `X-Wing` and a lone `TIE_Fighter` squadron revealing through the squadron's team container, which has `REVEAL` with range 800. The reveal is centred on the members' bounding box, follows the formation, and survives the leader's death. The craft's own authored ranges go unused. EAWR-271 moves the fighter sensor profile from the craft to the squadron container. |
| V-04 | differs | Team sharing is confirmed: a revealer reveals for every allied player (AU-44). But an allied object is exempt from fog only when it reveals itself; an allied craft without `REVEAL` is fogged by cells like any other object (AU-40). |
| V-05, V-06 | differs | Retail contact is by fog cells, not an exact disc: a revealer marks a rasterised circle of cells whose radius is the range divided by the cell size, rounded; an object is seen when any of its sample points lies in a marked cell (AU-43, AU-47). Height is ignored, as the note says. Counted as two claims. |
| V-07 | n/a | Remake arithmetic. |
| V-08 | n/a | The simulation reads the current cell state. Retail presentation re-checks every 30 frames and fades (AU-49); the remake's binary contact is a project choice. |
| V-09 | confirmed | Clarified: weapon targeting applies fog for every owner, AI included; `AIUsesFogOfWarSpace` only relaxes fog queries made without the force flag (AU-41). |
| V-10 | confirmed | Map owners are players and their revealers reveal for their allies (AU-44). Added detail: an object without the hide-when-fogged or team behaviour is never fogged; every M2 map object has the hide-when-fogged behaviour. |
| Q-01 | n/a | Project order; target scans use the CO rules since EAWR-469. |
| Q-02 | confirmed | Targeting box as R-07 (AU-03). |
| Q-03, Q-05 | n/a | Project rules. Counted as two claims. |
| Q-04 | confirmed | Retail queries one player's owned collidable objects at a time (AU-03). |
| F-01, F-02 | n/a | Presentation, project. Counted as two claims. |
| G-V1 | confirmed | Resolved: the cell rule is traced (AU-43 to AU-47), and a revealer re-reveals only after moving at least one cell width in the plane. |
| G-V2 | confirmed | Resolved: dense (nebula) cells are revealed with the range times `Dense_FOW_Reveal_Range_Multiplier`, normal cells with the full range (AU-45, AU-47). |
| G-V3 | not checkable | Presentation flags not read. |
| G-V4 | confirmed | Resolved: revealers reveal for every allied player (AU-44). |

## Tactical tick rate

| Claim | Verdict | Reason |
|---|---|---|
| TR-01 | confirmed | 30 on reset and for every game mode (AU-60, AU-61). |
| TR-02 | confirmed | Five steps per mode, default the middle; the logical rate is untouched (AU-60, AU-62). |
| TR-03 | confirmed | Game time is frames divided by the logical rate (TR-E4; object Lua service uses the same). |
| TR-04 | confirmed | The seconds-to-frames helper rounds half away from zero on the binary32 product (AU-63). Added: several subsystems do not use it and truncate instead (weapon recharge and pulse delay, AU-64; the opportunity rescan interval, AU-01). |
| TR-05, TR-06 | n/a | Project rules. Counted as two claims. |
| TR-U1 | not checkable | Needs the EAWR-43 cadence measurement. |
| TR-U2 | confirmed | Resolved: half away from zero after a binary32 multiply and add. |
| TR-U3 | not checkable | Wall-clock pacing. |
| TR-U4 | n/a | Covered by TR-04 above. |

## Skirmish start

| Claim | Verdict | Reason |
|---|---|---|
| Players | n/a | IDs and colours are project and owner rules (SK-12 colours match the capture). |
| Markers | confirmed | The k-th spawn marker in object-list order, head insertion (the EAWR-67 code reading and capture; not re-read here). |
| Station | not found | The skirmish station placement path was not located in this pass; the EAWR-67 capture puts the station on its marker. |
| Companies | confirmed | All default and fleet companies are created at the one spawn marker with its facing (the EAWR-67 code reading). |
| Map objects | differs | Retail remaps map-object ownership by faction, not by TED owner index: objects of a playable faction are deleted, objects of a non-playable faction go to that faction's real player, and without one a non-discardable decoration goes to the Neutral player and anything else is deleted (AU-70). |
| Entity IDs, transforms | n/a | Project. Counted as two claims. |
| Economy, launches | n/a | Owner rules (SK-30, SK-31, SK-23). Counted as two claims. |

## Unit data

| Claim | Verdict | Reason |
|---|---|---|
| Single-value last occurrence | confirmed | Tags apply in document order (AU-80). |
| Most-derived variant layer | confirmed | A variant is a copy of its base with its own tags applied on top (AU-83). |
| Empty and placeholder values | differs | Missing from the note: an empty tag value and the value `TBD` change nothing, so the inherited or earlier value stays (AU-80, AU-81). |
| List tags | differs | Retail list semantics depend on the tag. `Squadron_Units` and the two spawned-unit lists append, including onto a list a variant inherits. `Squadron_Offsets` and `Fire_Inaccuracy_Distance` replace the inherited list in a variant, where each occurrence also replaces the one before. Each occurrence of a spawned-unit list tag reads one type and count pair only (AU-82). No M2 type exercises a difference. |
| List splitting | not checkable | The separator set of the retail tokeniser was not read. |
| Decimals | confirmed | The retail number parse reads the leading number and ignores trailing text, so `0.8f` reads 0.8 (AU-82). |
| Booleans | confirmed | Canonical values agree. Retail reads true for `1` or a value starting with Y or T in either case and false for anything else (AU-82); added to the note. |
| Abilities, recharge, spawners, unresolved rows | n/a | Project and owner rules. Counted as four claims. |

## Lua script model

| Claim | Verdict | Reason |
|---|---|---|
| L-01 | confirmed | Every script instance opens its own Lua state, with its own globals, module table, coroutine registry and event handler (AU-100). |
| L-02 | differs | No I/O, OS, math, debug or loader library is opened, as the note says. But retail opens the fork `security` library in every state, after base, string and table (AU-100). Its members are `crcstate`, `crc`, `dumpstrtable` and `md5` (AU-101). No FoC or EaW script calls them (Lua manifest), so leaving them out of the Phase 0 host changes no script. |
| L-03 | confirmed | `Script` and `LUA_PATH` are published at state creation; the path string, the slash rule and exact duplicate detection match (AU-100, AU-102). Added detail: each state copies `LUA_PATH` when it is created, so a directory added later reaches only states created after it. |
| L-03a | confirmed | Retail `require` is upstream Lua 5.0.2 (AU-103). Added detail: a file that exists but fails to compile stops the search with an error rather than trying the next path component, and `_REQUIREDNAME` holds the name while the module runs. |
| L-03b | confirmed | The state's file handler opens files through the engine file system, and the base loaders use it (AU-100, AU-104). |
| L-04 | confirmed | Shutdown sets the exit flag, drops every thread and the thread table, signals shutdown, detaches the event handler and then closes the state (AU-120). |
| L-05 | confirmed | Pooling exists with its own reset (AU-120). Added detail: the reset calls the script functions `Flush_G` and then `Base_Definitions`, clears the stack and collects garbage. |
| L-06 | differs | The FoC x64 loader accepts only chunks whose `size_t` width byte is 8, with 8-byte string lengths (AU-105, AU-106). The pinned 32-bit members cannot load in the retail game. In the installed EaW and FoC profiles every effective script is a text source from `64Patch.meg`, which shadows every compiled member (AU-140). The persistence field is read but ignored outside whole-state loads (AU-106, AU-107). |
| L-06a | confirmed | Whole-state save and load are separate code paths that use the persistence identifiers (AU-107). |
| L-07 | confirmed | A failed module load fails the start. Added detail: the debug build offers to reload the script and retries on yes (AU-104). |
| L-10, L-11 | confirmed | Absent or non-function names give no result; parameters in order; one result; a failed protected call takes the alert path and gives no result (AU-115). Added detail: a nil result is also no result. Counted as two claims. |
| L-12 to L-14 | confirmed | The host object is called through its `__call` metamethod: the userdata comes first, the rest follow in order, the current coroutine is set before dispatch, the result list becomes the Lua results and an empty or missing list gives none. Tables become maps only when the callee opts in (AU-112, AU-114). Counted as three claims. |
| L-15 | differs | The x64 host wrapper stores numbers as binary64 (AU-112). The P0 host keeps its binary32 boundary by the [numeric profile](../lua-numeric-profile.md) decision (EAWR-263); the note now states both. |
| L-16 | n/a | A Phase 0 requirement (`P0-07-REQ`), not a retail rule. |
| L-20, L-21 | confirmed | A coroutine names a global function and takes at most one initial value; the identifier is the zero-based slot index. Live threads are held in a global table named `LuaThreadTable` (AU-109, AU-141). Counted as two claims. |
| L-22 | confirmed | The pump visits slots in index order and resumes each once (AU-110). |
| L-23 | differs | The pump keeps a slot whenever the topmost value left by the resume is boolean true, whether the coroutine yielded or returned. A coroutine that returns true is therefore called again from its start on the next pump (AU-110, AU-111). A yield of false, nil, a non-boolean or an error ends it, as the note says. Needs a runtime check (RO-4). |
| L-24 | confirmed | Slots are appended and never reused (AU-109). |
| L-25 | confirmed | `Kill` of the current slot does nothing (the debug build asserts); `Kill_All` skips the current slot; out-of-range identifiers do nothing (AU-116). |
| L-26 | confirmed | The initial value is pushed on the first pump only; the Lua-visible creator ignores extra arguments (AU-110, AU-141). |
| L-30, L-31 | confirmed | Per-coroutine first-in first-out event and parameter queues, independent reads, empty reads give no value, reset clears every coroutine (AU-113). Counted as two claims. |
| L-32 | confirmed | Registrations append, cancellation removes every match, and a mutation restarts the scan at the first registration that has not completed (AU-118, AU-119). Added detail: the events fire inside the object's `Service_Wrapper` call; candidates come from a box query around the object with the registered distance as the half extent on all three axes; decorations, objects pending deletion and the object itself are skipped; the optional player filter keeps allies of that player and the optional type filter one type; a fleet delivers each contained unit. |
| L-33 | not checkable | Candidates come from the spatial collection in tree order (AU-119). |
| L-40 to L-45 | not found | The FoC debug build has no wide-string class and none of its member names (AU-117). The rules describe the GlyphX reference, not FoC. Counted as six claims. |
| C-01, C-01a, C-02, C-03, C-05, C-06, C-07, C-09 | confirmed | Each follows from the confirmed rules above. Counted as eight claims. |
| C-04 | differs | B returns true, so retail keeps its slot and starts it again (L-23). |
| C-08, C-08a | not found | Wide strings (L-40 to L-45). Counted as two claims. |
| C-10, C-11 | n/a | Phase 0 host policy. Counted as two claims. |
| Smoke contract | n/a | A Phase 0 EaW fixture. The pinned member is the shadowed 32-bit chunk; the retail x64 game runs the `64Patch.meg` text source of the same script (AU-140). |
| LUA-G01 | not checkable | Execution equivalence needs a run. |
| LUA-G02, LUA-G03 | not found | Wide strings are not in FoC (L-40 to L-45). Counted as two claims. |
| LUA-G04 | n/a | Project. |
| LUA-G05 | confirmed | Resolved: the member set is `crcstate`, `crc`, `dumpstrtable` and `md5` (AU-101). |
| LUA-G06 | confirmed | Resolved in part: whole-state loads use the file's persistence identifiers (AU-107). |
| LUA-G07 | confirmed | Resolved: the pump reads the topmost value, so a multi-value yield keeps the slot only when its last value is true (AU-110). |

## PGLua compiled chunks

| Claim | Verdict | Reason |
|---|---|---|
| Header: signature, version byte, int width, instruction width, field widths, number width, sentinel | confirmed | The FoC loader checks each of them (AU-105). Added details: the version byte must equal the pinned value exactly, the endianness byte selects byte swapping, and the sentinel is compared by its integer part. Counted as seven claims. |
| Header: endianness byte | confirmed | Same value; the loader swaps when it differs (AU-105). |
| Header: `size_t` width 4 | differs | The FoC x64 loader requires 8 (AU-105). The pinned members are 32-bit chunks that the retail x64 game never loads; the effective scripts are `64Patch.meg` text sources (AU-140). |
| Scalars, signed counts, instruction words, binary64 numbers, strings, inherited source name | confirmed | As the note says (AU-106). Added detail: a string count is `size_t` wide, 8 bytes in the x64 format. Counted as six claims. |
| Quotas | n/a | Project policy. |
| Prototype record order | confirmed | Upstream 5.0.2 order with the identifier after the first line (AU-106). |
| Stripped retail members | n/a | Data. |
| Persistence identifier values | n/a | Data (1 to N in depth-first order). |
| Persistence identifier use | differs | An ordinary load ignores the file value. Each prototype gets the first free slot of its state's prototype list, numbered from 1 in creation order; only a whole-state load uses the file values (AU-106, AU-107). The file sequence matches the ordinary numbering only for the first chunk loaded into a fresh state. |
| Constant tags 0, 3 and 4; others rejected | confirmed | AU-106. |
| Instruction layout, RK threshold, opcode table | confirmed | The VM decodes the upstream 5.0.2 layout and opcode set (AU-108). Counted as three claims. |
| Opcode IDs 28 and 32 | confirmed | Resolved: 28 runs the upstream `FORLOOP` and 32 the upstream `SETLISTO` (AU-108). |
| Conversion contract, upstream build, rejection contract, fixtures S-01 to S-11, call analysis, smoke subset | n/a | Project tooling. Counted as seven claims. |
| PG-G01 | not checkable | Needs a run. |
| PG-G02 | confirmed | Resolved (opcodes 28 and 32 above). |
| PG-G03 | confirmed | Resolved in part: the whole-state format restores prototypes by identifier (AU-107). |
| PG-G04 | n/a | Project. |

## Lua API declarations

The index describes the GlyphX reference revision, not FoC. Part 2 compared it with the Lua
registrations of the FoC debug build (AU-117).

| Claim | Verdict | Reason |
|---|---|---|
| D-01, D-02, D-04, D-05 | n/a | Interpretation rules. Counted as four claims. |
| D-03 | confirmed | FoC registers 20 globals and 61 member names that the index does not list, among them `AITarget`, `Budget`, `FreeStore`, `Object`, `PlayerObject`, `Target` and every TaskForce method (AU-117). An index miss is not an engine miss. |
| Scope (GlyphX revision) | n/a | Project input. FoC relation added to the note: every index global is registered in FoC; the 22 wide-string methods are not. |
| C-01 to C-04 | n/a | Project cases. Counted as four claims. |

## FoC tactical AI

Rules tagged data, guide, author or inference with no engine-side claim are n/a. The engine
rules below were read in the goal, budget, planning, freestore and perception code.

| Claim | Verdict | Reason |
|---|---|---|
| AI-01 to AI-03, AI-09, AI-11 to AI-13, AI-15, AI-20 to AI-26, AI-30 to AI-32, AI-40, AI-41, AI-44, AI-45, AI-52 | n/a | Data, guide, author, inference or project. Counted as 23 claims. |
| AI-04 | confirmed | Goal desire comes from the engine's equation evaluator; evaluator scripts are called through it (AU-122, AU-131). |
| AI-05 | differs | In galactic mode the budget splits credits between categories, as the guide says. In tactical mode no budget code rations anything: every goal counts as affordable, the credit bookkeeping runs only in galactic mode, and each category's budget equation only gives it a normalised share. The shares order the per-category maintenance, largest first. A zero share does not switch a category off (AU-123 to AU-125). |
| AI-06 | confirmed | Added: when several plans fit a goal, the engine draws one with weight equal to that plan's recorded success rate plus one, on the synchronized game random stream (AU-126, AU-127). |
| AI-07 | not found | Active goals whose plan fails the target-contrast test are abandoned, magic plans excepted (AU-124). How contrast sizes the TaskForce was not traced. |
| AI-08 | confirmed | Both rates are seconds of mode time. At most every `ServiceRate` seconds the engine pumps the freestore script's threads; at most every `UnitServiceRate` seconds it calls `On_Unit_Service` for each freestore unit that no plan has reserved, in hash order (AU-130). |
| AI-10 | confirmed | The probability argument is exactly the chance of the best-scored target; otherwise the other targets are drawn with weights equal to their scores. An optional last argument limits the distance from the TaskForce (AU-128). |
| AI-14 | differs | `Game.ForceVisibility` is the fraction of the other players' objects of the requested category that the AI player can see (AU-132); it is 1 only when all of them are visible. Whether fog-off makes that so needs a runtime check (RO-6). A zero Tactical_Untargeted share does not disable the category (AI-05). |
| AI-42, AI-43 | confirmed | All 106 entries of AI-42 and the AI-43 names are registered in the FoC debug build, including the 27 the index misses. TaskForce methods are split between a generic set and a space-only set (AU-117). Counted as two claims. |
| AI-50 | confirmed | Resolved: the loop order is given below the table. |
| AI-51 | not found | The evaluator gets the player and target as globals and the script string and number parameters as arguments, then its clean-up function runs (AU-131). The upper-casing of string parameters and the archive-only loading were not traced. |
| AI-53 | differs | The goal draw, the plan draw and both target searches use the synchronized game random stream (AU-127). `Reachable_Target` is an ordinary draw with the AI-10 probability rule, not a static random (AU-129). |
| AI-G01 | not found | Not traced. |
| AI-G02 | confirmed | Resolved: the weighted draw of AI-06, reproducible from the synchronized random stream. |
| AI-G03 | n/a | Fixture inputs. |
| AI-G04 | confirmed | Resolved: see AI-05. |
| AI-G05 | confirmed | Resolved in part: see the loop order below. |
| AI-G06 | confirmed | Resolved in part: every entry exists in FoC (AU-117); signatures still need per-API notes. |
| AI-G07 | not found | Not traced. |
| AI-G08 | confirmed | Resolved in part: a goal fails activation when its build-time limit is positive and the build-time estimate exceeds it; otherwise it activates (AU-124). How the estimate treats unaffordable units was not traced. |
| C-01 to C-04 | n/a | Data-derived and project cases. Counted as four claims. |

Goal loop order in the FoC debug build (AU-121 to AU-125):

1. Proposal. Each goal-system service evaluates a bounded number of (goal function, target)
   pairs from the AI player's list for the current mode, continuing where the last service
   stopped. The number per frame is the non-trivial pair count of the last full pass divided by
   five seconds of frames, rounded up, capped at 20 divided by the number of AI players
   (rounded), and at least 1. So one full pass takes about five seconds of game time.
2. Desire. Desire is the paired equation's value plus the goal's per-failure adjustment times
   its recent failures plus its per-activation-failure adjustment times its recent activation
   failures. A pair is proposed only when the desire is above zero.
3. Maintenance. After a full pass the budget shares are recomputed, then each category is
   maintained, largest share first. Active goals that are finished, lost their plan, or fail
   the contrast test drop out; weaker cullable ones become candidates for abandonment.
4. Activation. Candidates are taken in desire order while the category has resources (in
   tactical mode it has none, so this pass is expected to be skipped), then drawn at random with
   weight equal to desire until the category reaches its active count plus the AI player's goal
   set extension size or runs out of candidates. Each activation re-checks proposability,
   duplicates, plan validity and the build-time limit.
5. Plans. Activated goals get their drawn plan (AI-06); abandoned active goals finish.

Step 4's skip in tactical mode rests on the budget code alone and is listed as RO-5.

## XML registry

| Note | Claim | Verdict | Reason |
|---|---|---|---|
| edge cases | R-07 | confirmed | A missing object file prints an error and asserts; when the assertion returns the file is left out and later entries are read. A failed registry read returns failure (AU-133). |
| edge cases | R-08, R-11 | not found | The inheritance and registration order was not re-read in this pass; the earlier debug-build reading stands. Counted as two claims. |
| edge cases | R-09 | confirmed | Added detail: at most ten parse passes; later passes re-parse only the files that own types still incomplete (AU-133). |
| edge cases | R-10 | confirmed | The debug build continues the same way once its assertion returns (AU-133). |
| edge cases | R-01 to R-06, R-12 to R-14 | n/a | EaW builds, data, LSP and project rules; the project targets FoC only. Counted as nine claims. |
| edge cases | C-01 to C-06, G-01 to G-03 | n/a | Project cases and EaW unknowns. Counted as nine claims. |
| retail addendum | XREG-A1 items 1 to 5, freighter limits | n/a | EaW retail build. Counted as six claims. |
| file-service addendum | XREG-A2 | n/a | EaW retail build. |

## Tactical camera input

The note is project policy; its thirteen sections are n/a. Three of its unresolved
observations have a code answer (AU-134 to AU-136).

| Claim | Verdict | Reason |
|---|---|---|
| Sections (statement kinds, XML source policy, binding schema, free camera, host transitions, land and space loops, Alt pan, rates, Ctrl orbit, overrides, input defaults, limits) | n/a | Project policy. Counted as thirteen claims. |
| Meaning of the camera-lock tags | confirmed | Resolved in part: `Land_Tactical_Camera_Locked` and `Space_Tactical_Camera_Locked` set the tactical camera's lock flag when the mode starts and when a map loads; that call also resets pitch, yaw, field of view and distance to their defaults. What the flag then blocks was not traced. |
| Rotate drag, mouse units, middle click, wheel | confirmed | Resolved in part. A rotate drag turns yaw by `Yaw_Per_Mouse_Unit` and also tilts pitch by `Pitch_Per_Mouse_Unit`, clamped to the pitch range; yaw is wrapped and clamped to its range. Mouse motion reaches the camera as a change in normalised screen position times 4 (times 1 while Ctrl is held), and the camera multiplies it by 100. A middle click released without dragging resets the view; the wheel zooms. Needs a runtime check of the screen normalisation (RO-7). |
| A comparable free camera | not found | No free-flight mode was found in the tactical camera controller; not a claim that none exists. |

## River groups

| Claim | Verdict | Reason |
|---|---|---|
| Group record layout (points, widths, texture, parameter block) | confirmed | Matches the retail terrain-track loader (AU-137). |
| Mini `0x01` selects river water | differs | Retail names `0x01` the draw mode. Water, road and river are a separate track type in parameter `0x10` (0 water, 1 road, 2 river), and the renderer splits passes by that type: water tracks in the water-decoration pass, the rest in the track pass (AU-137, AU-138). In the installed maps Naboo's four groups are river tracks (mode 5) and the Bespin and Utapau station lights are road tracks (mode 4) (AU-139). |
| Mini `0x03` is the flow | differs | Retail names `0x03` the U shift rate and `0x04` the V shift rate (AU-137). Which texture axis runs along the ribbon was not traced. |
| Seven-map classification | differs | `um11_raiders_of_the_lost_holocron` has 42 water-type tracks among its 231 mode-4 groups; retail draws them in the water-decoration pass (AU-139). The other counts agree. |

PI-8 follow-up: the `um11` exception above describes the eight family-zero maps
in the river comparison. A full effective FoC land-map scan finds other
type/mode mismatches; see [the track index table](foc-river-group-selection.md).
| Family zero gives no water plane | not found | Not traced. |
| Submission of both modes, overlay data, Naboo data, captures, visual mapping, selection policy | n/a | Data, captures and project rendering. Counted as six claims. |

A gameplay rule sits in the same code: the water elevation at a point is the terrain height
plus the track's custom height when the point lies on a river-type track, and the map's water
elevation otherwise (AU-138). Naboo's river tracks have custom height 0 (AU-139).

## Remake implementation check

The audited notes were compared with `src/sim`, `src/units` and `src/skirmish`. Targeting is not
implemented yet (EAWR-73), so its corrections change no code.

- Priority sets: `units` loaded `Attack_Priorities` and `Hard_Point_Priorities` only. The FoC
  sets also carry unit, category, property and hardpoint exclusions (every space set excludes
  the `NotOpportunityTarget` property and the destroyable asteroids). EAWR-73 needs them. Done in
  EAWR-270: the exclusions, entry kinds, unit property flags and the set-side R-09 scoring
  ([unit data](../unit-data.md#priority-sets)).
- Sensors: `sensor_table` gives a profile to every type with `Space_FOW_Reveal_Range`,
  including the four craft that have no `REVEAL` behaviour; per RO-1 the reveal belongs to the squadron container instead (V-03, EAWR-271). The contact test is the exact
  disc of V-07, a project choice.
- Map objects: `build_start` keeps every map object and makes one player per TED owner index
  (Skirmish start, Map objects). On Coruscant the owners are Neutral (index 3) and Hutts
  (index 7, the resource containers), both non-playable, so no object would be deleted. The
  owner confirmed the containers are Hutt-owned in play (orange on the minimap), neutral
  destructible mines (RO-2, owner EAWR-312). Done in EAWR-272: `build_start` makes the retail
  skirmish players and remaps or deletes map objects by faction
  ([skirmish start](../skirmish-start.md)); Coruscant deletes nothing.
- Durability (`sim::tactical`, EAWR-72) matches the confirmed hardpoint rules.

Part 2 compared the Lua, AI, camera and river notes with `src/script`, `src/presentation` and
the viewer.

- Lua host: `script_host.cpp` opens base, string and table. Leaving out `security` changes no
  script (L-02). The L-23/LUA-G07 pump difference recorded by this audit was resolved in EAWR-292:
  the host now reads the topmost result and restarts a thread after `return true`.
- Compiled chunks: the Phase 0 conversion path serves the EaW smoke fixture only. The FoC
  runtime loads text sources, which the VFS already selects (L-06), so no conversion is needed
  there.
- Tactical AI: not implemented yet (EAWR-79). Every engine entry of the EAWR-79 selection exists in FoC
  (AI-42), so the member and receiver binding of AI-44 has a complete name list to bind.
- Camera: the map cameras follow the FoC middle-button law since PI-9 (EAWR-295): Ctrl + middle
  drag rotates and tilts by the XML per-mouse-unit rates in screen-fraction units (RO-7), a
  plain middle drag translates and a middle click resets the view.
- Rivers: `terrain::visible_rivers` keeps groups by draw mode 4 or 5 and ignores the track type.
  For the installed maps this draws the same groups, except that the 42 water-type tracks of
  `um11` would go to the water pass in retail. No land water elevation exists yet.

## Findings for open tickets

For EAWR-73 (weapon fire), from the firing attempt read for R-13 (AU-12):

- The attempt checks the planar distance from the weapon midpoint to the aim point against the
  range plus the target's soft radius, and rejects a point closer than the minimum range.
- A turret hardpoint must have the target's position inside its fire cone before anything
  else.
- The recharge after a shot is a synchronized random whole number of hundredths of a second
  between the minimum and the maximum, converted to frames by truncation. The pulse delay also
  truncates.
- AI-owned hardpoints with a maximum recharge of at most 3.0 s always opportunity-fire (R-01).

For EAWR-79 (tactical AI), from the goal, planning, freestore and Lua code (AU-117 to AU-132):

- Implement the goal loop in the order given under [FoC tactical AI](#foc-tactical-ai). In
  tactical mode the budget shares only order the categories.
- Draw from the simulation RNG at the same points as retail: the desire-weighted goal draw,
  the plan draw weighted by success rate plus one, and both target searches (probability `p`
  of the best target, otherwise weighted by score).
- The freestore runs its script threads every `ServiceRate` seconds and calls
  `On_Unit_Service` every `UnitServiceRate` seconds for unreserved units. Retail visits those
  units in hash order; the remake needs a fixed order, such as entity ID.
- Evaluator scripts see `PlayerObject` and `Target` as globals, take the script string and
  number parameters as arguments, and are followed by `Evaluator_Clean_Up`.
- `Game.ForceVisibility` is a visible fraction, not a constant (AI-14).
- The in-range events of L-32 fire only inside `Service_Wrapper`.

## Proposed issues

| ID | Title | Scope |
|---|---|---|
| PI-1 | units: load priority-set exclusions, exact-type entries and property flags | `load_priority_sets` reads only `Attack_Priorities` and `Hard_Point_Priorities`. The R-09 lookup also needs the unit, category, property and hardpoint exclusions and each type's property flags. Needed before EAWR-73 chooses opportunity targets. |
| PI-2 | sim: fighter fog reveal follows FoC's actual source (EAWR-271) | Filed re-scoped after the owner's report that fighters do reveal: find the real source of fighter reveal and match it; do not remove fighter sensors unless RO-1 shows retail fighters reveal nothing. |
| PI-3 | skirmish: retail map-object ownership by faction | `build_start` makes one player per TED owner index and keeps every object; retail remaps by faction and deletes playable-faction objects (Skirmish start). Done in EAWR-272. |
| PI-4 | units: retail list-tag, empty-value and boolean parse semantics | Variant list tags, empty or `TBD` values and boolean spellings differ from the unit-table value rules ([unit-data.md](../unit-data.md)). No pinned type is affected; align before the tables load more types. |
| PI-5 | sim: retail fog-cell contact rule (fidelity) | Rules v1 uses an exact disc and ignores dense cells (V-05 to V-07, G-V2). Decide after RO-3 whether M2 needs the retail cell rule. |
| PI-6 | script: coroutine pump keeps a slot on a true result | The host ends a coroutine on a normal return and rejects multi-value yields; retail keeps any slot whose topmost value is true and restarts a coroutine that returned true (L-23, LUA-G07). Align after RO-4. |
| PI-7 | inventories: FoC Lua registration index | The declaration index is GlyphX and misses 20 FoC globals and 61 member names, and lists 22 wide-string methods FoC lacks (AU-117). Add a names-only FoC index (visible name, receiver class) for EAWR-79 stubs and missing-API reports. |
| PI-8 | presentation: classify terrain tracks by track type (fidelity) | `visible_rivers` selects by draw mode. Retail selects the water pass by the track-type parameter and draws every other track in the track pass; `um11` has 42 water-type tracks. Land water elevation from river tracks belongs to later land work. |
| PI-9 | camera: FoC rotate-drag law (fidelity) | Retail rotate drags also tilt pitch by `Pitch_Per_Mouse_Unit`, move in screen fractions times 4 (1 with Ctrl) and a middle click without a drag resets the view. Adopted after RO-7 (EAWR-295): see the middle-button law in tactical-camera-input.md. |

## Runtime observations needed

| ID | Observation | Settles |
|---|---|---|
| RO-1 | In a FoC space skirmish with fog on, does a lone X-Wing or TIE Fighter squadron reveal enemy units that no other own unit sees? | V-03 (code reading says no; owner says yes) Answered 2026-09-26: yes, through the squadron container (V-03). |
| RO-2 | Who owns the eight Hutts resource containers on Coruscant at the start of a retail skirmish (or are they removed)? | Answered 2026-09-26 (owner EAWR-312): Hutt-owned, orange on the minimap, neutral destructible mines. |
| RO-3 | Contact at the edge of a reveal range, one cell inside and one outside. | V-05, V-06 cell rule |
| RO-4 | In the debug build, a script thread function that returns `true` after logging once: is it logged once, or again on every pump? | L-23 (code says again) |
| RO-5 | A FoC space skirmish with the AI log on: does any tactical goal activation take the desire-ordered pass, or do all come from the random draw? | AI loop step 4 (code says the draw only) |
| RO-6 | The same AI log with `AIUsesFogOfWarSpace` off: the value of `Game.ForceVisibility` and the Tactical_Untargeted share at the start and after first contact. | AI-14 |
| RO-7 | In FoC space and land at 1280 x 720 and 1920 x 1080: yaw per full-width rotate drag, with and without Ctrl, and whether a vertical drag tilts. | Camera rotate law, PI-9 |

## Left for a later pass

- XML registry: the inheritance and registration order (R-08, R-11) was not re-read.
- Tactical AI: contrast sizing of TaskForces (AI-07), the upper-casing of evaluator strings and
  archive-only loading (AI-51), a GC-only tactical configuration (AI-G01) and the threat grid
  (AI-G07).
- Camera: what the lock flag blocks.
- Rivers: the water-plane gate for family zero and the ribbon's texture axes.

## Inference sweep 2026-09-26

Issue EAWR-332. `docs/` and `plan/`
were searched for original-behaviour claims resting on inference ("project reading",
"inference", "inferred", "assumed", "unconfirmed", "not traced", "static reading", "likely",
"probably", "presumably", "reading only", "needs a runtime check"). Each claim was checked in the
FoC debug build, with the claims the code implements first. Evidence IDs IS-01 to IS-16 are
opaque; their map stays under the ignored `out/research/`. Hits that are remake policy, format
facts or planning text are left out. So are claims the audit above already settled. Verdicts use
the table above; "corrected" means the note or list was wrong and has been fixed, and
"unverified" means this pass did not settle the claim.

| Claim | Where (note; code) | Verdict | Evidence |
|---|---|---|---|
| A tag authored twice in one layer keeps its last value (`Targeting_Max_Attack_Distance` 2000 then 800, `Space_FOW_Reveal_Range` 1200 then 1000), which the Phase 2 list called unconfirmed. | Phase 2 fidelity list, [unit-data.md](../unit-data.md); `src/units/unit_tables.cpp` | confirmed | IS-01 to IS-03 (object and hardpoint parsers visit tags in document order; float and string fields overwrite). The fidelity lines are removed. |
| `HP_Empire_Station_One_01` authors `Fire_Bone_B` three times and "retail may fire from all three". | Phase 2 fidelity list; `unit_tables.cpp` `fire_b` | corrected | IS-02 to IS-04: `Fire_Bone_B` is one string field, so retail keeps only `FP01_LC_03`, as the unit tables do. Weapon fire belongs to EAWR-320. |
| `Dense_FOW_Reveal_Range_Multiplier` means the sensor range inside a nebula. | Phase 2 fidelity list, [space-visibility.md](space-visibility.md) G-V2; not loaded | confirmed | IS-07, IS-08. Added detail: a type without the tag gets 0.5; dense cells are those under the obstacle circle of a nebula, asteroid field, impassable asteroid or ion storm; the plain reveal range has a floor of 10. |
| An absent `Should_Be_Destroyed_When_All_Hardpoints_Destroyed` means no. | [space-hardpoints.md](space-hardpoints.md) HD-21, G-H2; `unit_tables.cpp`, `sim/tactical/durability.cpp` | differs | IS-05, IS-06: the FoC default is yes. The last-hardpoint check runs when a hardpoint hit leaves that hardpoint at zero or below. EAWR-340. |
| A spawner's launch list is its fighter-bay hardpoints. | space-hardpoints.md HD-13; `sim` squadron launch flags | confirmed | IS-11: every hardpoint of type fighter bay, in hardpoint order. |
| Hardpoints without repair values cannot be repaired. | space-hardpoints.md HR-06; `durability.cpp` `repair_frame` | confirmed (reason corrected) | IS-09, IS-10: the repair order is offered only for a star-base object's hardpoint below full health. The service never reads the amount, so a zero amount would repair forever without gain. The remake's amount check gives the same M2 outcome. |
| Retail lets a player repair another player's object, or a hardpoint without values. | space-hardpoints.md G-H4 | unverified (in part) | IS-09, IS-10: neither the event nor the service checks the owner. The step that picks the click action for an enemy star base was not traced. |
| A skirmish unit spawned on a marker takes the marker's facing. | [p1-effective-environment.md](p1-effective-environment.md) R-ROT-04, Phase 2 fidelity list; `src/skirmish/start.cpp` | confirmed | IS-12, IS-13. Added detail: the marker's facing also sets the player's reinforcement facing, and each company is created in free space near its marker (search distance 2500), not on it. The fidelity line on the facing is removed. |
| The k-th player of a team starts at the k-th team spawn marker. | [m2-skirmish.md](../../plan/phase-2/m2-skirmish.md) SK-11 | confirmed | IS-12. The object order of the marker search was not re-read. |
| TED `0x17` is likely the shadow colour. | p1-effective-environment.md G-SHD-01; viewer shadow floor (EAWR-225) | confirmed | IS-15. `0x15`, whose consumer was unknown (G-ENV-01), is the sky background colour. |
| TED owner index 7 is Hutts in the FoC faction order. | Phase 2 fidelity list; `scene::faction_order` | confirmed | IS-16 did not find the map-load step; owner EAWR-312 confirmed the containers are Hutt-owned in play (orange on the minimap), neutral destructible mines. RO-2 is answered. |
| A sky's orientation triple is converted from degrees twice. | p1-effective-environment.md R-SKY-03 | unverified | Not settled: the sky objects are created through the generic object path with (tilt, 0, z-angle). The remake converts once. Gate G-SKY-02 and a runtime check stay. |
| Damage emitters show from "damaged" and stay on once destroyed. | [asset-formats.md](../asset-formats.md) hardpoint table; `scene::hardpoint_art` | differs | E72-03 (HD-04, G-H5): retail changes hardpoint art only on destruction. EAWR-329 fixed the viewer and note. |
| Damage emitters on a bone that no listed hardpoint names stay drawn at spawn. | Phase 2 fidelity list (EAWR-136) | confirmed (static) | EAWR-284 reading: the hide step runs only below each listed hardpoint's `Damage_Particles` bone. Needs a runtime check; EAWR-329's area. |
| A hardpoint model that repeats its owner's skeleton is drawn in the owner's model space. | [Phase 1 README](../../plan/phase-1/README.md) (EAWR-32); viewer space population | unverified | The attach step was not found in this pass. The rule matches the two Coruscant stations it came from. |
| A lone Y-wing reveals 600 besides its squadron's 1000. | space-visibility.md G-V5 | unverified | Not re-read; the part 1 reading found no suppression. A Y-wing staging capture stays open. |
| With zero credits the space production goals fail activation (inert). | [foc-tactical-ai.md](foc-tactical-ai.md) AI-31, the production row | unverified | Not re-read (AI-G08 was confirmed in part by AU-124). No AI code exists yet (EAWR-79). |

Totals: 9 confirmed (one with its reason corrected), 3 corrected or differing, 4 unverified
(one of them in part). Code against evidence: one mismatch, the HD-21 default, which changes
tactical outcomes and hashes and is filed as EAWR-340 on board EAWR-7. The dense multiplier, the
reveal cells and the free-space placement were already on the Phase 2 fidelity list.

Runtime observations added:

| ID | Observation | Settles |
|---|---|---|
| RO-8 | A FoC space map whose sky authors a non-zero z-angle: does the sky turn by that angle, or by the angle converted twice? | R-SKY-03, G-SKY-02 |
| RO-9 | A retail station whose hardpoints are all destroyed except the non-destroyable ones: does the next hit on a non-destroyable hardpoint kill it? | EAWR-340 edge case |
