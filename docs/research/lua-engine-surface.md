# Lua engine surface: what the original registers, per script host

Question (SK Lua coverage rule): which Lua functions does the original engine register at all, which of them can each of
our script hosts serve, and which are never called by any script we know. Read-only walk of the debug build on the
research host (headless Ghidra; no retail code or addresses kept here). The inventory is
[plan/inventories/lua-engine-surface.json](../../plan/inventories/lua-engine-surface.json); the full registration list is
[plan/inventories/foc-lua-registrations.json](../../plan/inventories/foc-lua-registrations.json) (names only, receivers named neutrally).

## Finding 1: the engine has one surface, not one per host

The registration is not per host. Every script host the original starts (object scripts, object-type scripts, story
plots and sub-plots, scoring, plan definitions and plan behaviours, free-store scripts, evaluators, the target-contrast
parser, the attach-script command) calls the same two command-set registrars when it creates its Lua state: one for the
utility commands, one for the global commands. The wrapper tables (script, game object, game object type, player, AI target
location) are registered by each wrapper's own initialiser, which runs when the wrapper is created, for any host that
touches one. So the debug build registers all 145 globals and 327 receiver-specific members (275 distinct member names)
on every host. A name that works in one host and not in another in the original is a runtime-context effect
(for example no galactic map in a battle), not a missing registration. Whether a given call then does anything in a given
host is not established by this walk (D-04 in docs/behaviour/lua-api-declarations.md); a per-host behaviour note is the
follow-up for any family a mod depends on.

Consequence for our plan: the gap of a host is simply the engine surface minus what that host binds. Nothing in the
engine suggests the galactic hosts are meant to bind less than the battle hosts.

## Finding 2: what our hosts bind of that surface

Static read of our binding tables (the scan of tools/lua_api_scan, run 2026-11-23). A "stub" is a name the host registers
as a fault-by-name stub (calling it raises "is unbound"); a stub counts as not bound.

| Host | Globals bound / 145 | Members bound / 275 | Stubbed names |
|---|---|---|---|
| tactical_object, tactical_story, tactical_plan_space, tactical_evaluators | 47 | 130 | 0 |
| galactic_ai | 49 | 133 | 0 |
| galactic_evaluators | 46 | 122 | 0 |
| galactic_object | 20 | 14 | 92 |
| galactic_story | 24 | 11 | 89 |
| scoring | 9 | 11 | 0 |

Counts are distinct engine names; `Namespace.Function` calls count under their namespace global.

## Finding 3: names no known script calls

Scanned stack: stock plus the nine mod profiles of scan_config.json. A global counts as called when a script calls it or any
`Name.Function` under it. A member name counts as called when any receiver's script call uses that name, so the member list is
a lower bound (a name used on one receiver hides the same name on another).

- 27 of 145 globals are never called.
- 105 of 275 member names are never called by name on any receiver (some of these are bound by a host already; the inventory lists where).

The 27 globals, grouped from their names only (descriptions are from the name, unverified):
UI component effects (`GUI_Component_*`, `GUI_Text_Color`, `Activate_Retry_Dialog`), terrain-hazard probes
(`Is_Point_In_Asteroid_Field`, `Is_Point_In_Ion_Storm`, `Is_Point_In_Nebula`), base-type pickers (`GetNextGroundbaseType`,
`GetNextStarbaseType`, `WaitForGroundbase`), camera (`Rotate_Camera_To`), script plumbing (`BlockForever`, `LastService`,
`LastUnitService`, `PlanDefinitionLoad`, `_DebugBreak`, `LUA_PATH`, `lc`) and the context variables the engine injects into
plan and evaluator scripts (`Budget`, `FreeStore`, `Object`, `PlayerObject`, `Target`). The context variables are set per
script, not called, so "never called" is expected for them. None of the 27 is bound by any of our hosts today.

Members by receiver (neutral receiver names; a receiver the original's own scripts spell keeps that spelling):

| Receiver | Registered | Never called |
|---|---|---|
| GameObjectWrapper | 131 | 29 |
| taskforce | 34 | 9 |
| PlayerWrapper | 33 | 4 |
| GameObjectTypeWrapper | 17 | 7 |
| land_taskforce | 16 | 2 |
| space_taskforce | 14 | 2 |
| budget | 10 | 9 |
| galactic_taskforce | 7 | 3 |
| story_event | 7 | 1 |
| create_thread_command | 6 | 5 |
| sfx_commands | 5 | 5 |
| story_plot | 5 | 1 |
| fow_reveal_command | 4 | 4 |
| script | 4 | 4 |
| ThreadValue | 3 | 2 |
| ai_target_location | 3 | 1 |
| discrete_distribution | 3 | 1 |
| galactic_freestore | 3 | 0 |
| GlobalValue | 2 | 2 |
| find_target_command, game_random_command, get_time_command | 2 each | 2 each |
| ai_freestore, GetEvent, blocking_status, position, weighted_type_list | 2 each | 1 each |
| find_planet_command, script_host, user_value | 1 each | 1 each |
| fow_cells | 1 | 0 |

## What this does for the work order

The Lua gap plan (the planned-row file of the SK Lua coverage rule) covers names scripts already call and miss. The never-called names are the second
list: no mod we know breaks on them today, so they rank below every called gap, but a mod outside the scanned stack can.
Pick them up by receiver family after the called gaps of the same host, starting with the receivers a called family
already needs (GameObjectWrapper, PlayerWrapper, taskforce).

## Limits

- Names only: a registration shows a name exists on a receiver, not its signature, return values or side effects.
- Names built at run time are not in the list (the extractor reads literal names).
- Per-host availability beyond registration (Finding 1) is unverified; behaviour notes need their own debug-build evidence.
- The never-called lists come from a scan that needs the mods, so CI does not regenerate them; CI checks only that they stay inside the registration index, disjoint from the baseline and consistent with the host binding tables.
- The called-name side depends on the mods on the scanning host; rerun the scan with a different mod set and the lists move.
