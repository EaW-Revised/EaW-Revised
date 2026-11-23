# Lua API coverage scan

Static scan, no Lua execution and no build: which engine functions the Lua scripts of every supported mod call, in
which host they run, and which of those the host does not bind. It answers "which mod script breaks at start because
a host lacks a function" before a player finds it. The generated result is `docs/lua-api-coverage.md`.

```
python -m tools.lua_api_scan --workshop-root <folder with one dir per workshop id> --stock-root <unpacked stock view> \
    --out-dir <absolute dir> [--profiles remake-chain,raw] [--check-baseline | --update-baseline]
```

`--roots <file>` maps layer ids to directories instead (see `roots.example.json`; `EAWR_LUA_SCAN_ROOTS` works too). The
mod list is data: `scan_config.json` profiles, with layers from `docs/tag-coverage/coverage-profiles.json`.

## How it works

1. **Collect** (`collect.py`): per profile, the effective `data/scripts/**.lua` (mod layers, leaf first, over stock).
2. **Extract** (`extract.py`): calls through the shared Lua parser (`tools/inventory/lua_calls.py`), with the enclosing
   function. A script the strict parser rejects falls back to a token reader and is counted as approximate.
3. **Hosts** (`scan_config.json` `script_classes` and `hosts`): a script root belongs to a host by its path, with two
   data-driven refinements: story scripts count for the galactic story host only when a campaign's plot file names them,
   and GameObject scripts only when an XML file names them (an upper bound for the galactic object host). A script's calls
   include its literal `require` closure.
4. **Bindings** (`bindings.py`, `cxx.py`): what a host binds is read from the C++ registration sites listed in its row
   (file, optional function, registrar lambdas, `follow` helpers, name lists, the scheduler's infrastructure table, and configured Lua prelude method assignments) and the
   list of names the host registers as fault-by-name stubs. The four tactical rows (object, space plan, story, evaluators) share one scheduler and read the same sites; the ground row
   has no bindings and is parked, so its demand is reported only. Adding a host is adding a row.
5. **Matrix** (`scan.py`): per host x function x profile: call sites, scripts that call it themselves and through a
   library, stock versus mod sites, severity, timing and how many mods it blocks. Timing is `load` (script top level), `start`
   (reachable from the start functions through direct calls and `Create_Thread` literals) or `recurring` (events and ticks).
   Severity: load/start critical, recurring high, ground-battle-only parked. Calls to a Lua helper that a script of the
   closure defines, the Lua standard library and `intentionally_unbound` names are never gaps.
6. **Runtime cross-check**: `runtime_unbound` in the config lists names a live run reported unbound; each must appear in
   the static result or carry an explanation.

## Baseline and CI

`baseline.json` holds one `profile|host|kind|name` key per non-parked unbound call. `--check-baseline` exits 1 when a key
is missing from it (a new unbound call), and lists keys that no longer occur (drop them with `--update-baseline`, which is
what a binding fix does).

What CI checks today (CTest `inventory_contracts`, `tests/inventory/test_lua_api_scan.py`): the whole scan on a small
synthetic fixture against the real binding tables, the `--check-baseline` mechanism against a temporary baseline, and that
the committed `baseline.json` is well formed, sorted, names only profiles and hosts the config has, and lists no name the
host tables bind now. CI cannot compare the committed baseline with the installed mods: they are not on the CI runners.

The gate for new unbound calls in the real mod stack is therefore a **manual run, until a host that holds the mods
schedules it nightly** (no workflow does yet). Command for that host (exit 1 on a new unbound call):

```
python -m tools.lua_api_scan --workshop-root <workshop folder> --stock-root <unpacked stock view> \
    --out-dir <absolute dir> --check-baseline
```

Run it after any change to a host's binding tables or a mod update, and refresh with `--update-baseline` in the same
change that fixes a gap.

## Limits

- Bindings are read statically: a registration behind a helper that is not in `follow`, or one made from a runtime table,
  is invisible and shows as unbound (the safe direction). A shared registration function is counted for every host that
  calls it, even where a flag would skip part of it.
- Method calls are matched by method name, not by handle type.
- Calls whose receiver or callee cannot be named statically are listed under "unresolved", not guessed.
- A name in a host's `name_lists` (for example the story host's `implemented` list) is trusted as bound without a
  registration site: the host defines such names in Lua text of its own. A name wrongly listed there would show as bound.
- A literal `dofile` is followed like `require`; a computed one is not.
- A loop over a braced list of names that registers each one is read (braced or one-statement body); a name table built
  at run time is not.
- Timing is inferred per call site: a function is `start` only when direct calls or `Create_Thread` literals reach it from
  load or `Definitions`; state functions registered with `Define_State` count as recurring. A cell's severity is that
  profile's most severe call site; the row-level severity is the maximum over profiles.
- Scripts that exist only inside archives are not scanned.
