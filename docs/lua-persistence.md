# Authoritative Lua persistence and state hash

Contract for EAWR-248,
2026-09-27: the canonical save of the [authoritative script scheduler](lua-sandbox.md) and
the versioned script state hash built on it. It implements the save/hash prerequisite of
[multiplayer readiness](multiplayer-readiness.md). This is new-engine persistence for
checkpoints, resync and hashing; it does not read or write retail save files.

Identities: save format `eawr-script-save-v2` (v2 adds the memory quota and account,
EAWR-375), state hash `eawr-script-state-v1`, combined
hash `eawr-authoritative-state-v1`. Any change to a rule below needs a new identity.

Code: `ScriptScheduler::save`, `load` and `state_hash`
(`include/eawr/script/authoritative/scheduler.hpp`, `src/script/authoritative/persistence.cpp`)
and the Lua graph codec inside the VM (`src/script/sflua/sflua_persist.*`). The upstream
Lua 5.0.2 sources stay unmodified; three compile wrappers (`upstream/lbaselib.cpp`,
`lstrlib.cpp`, `ltablib.cpp`) add accessors that name their static C functions.

## When

A save is taken at a tick barrier: between two `service` calls, after the host's own
barrier work (creating instances, submitting events). Saving reads only; it allocates
nothing inside a Lua state, so taking one never changes a later tick. It runs serially:
the per-instance work is a few milliseconds at most and happens only at checkpoints and
hashes, outside the `script-instances` phase.

`load` replaces the whole script state of a scheduler created with the same session
settings, manifest and bindings. It decodes and validates the complete save before it
touches the scheduler; any defect leaves the scheduler exactly as it was and returns
`EAWR-SCRIPT-0213`. A state that cannot be represented makes `save` fail with
`EAWR-SCRIPT-0212`; an aborted session refuses with `EAWR-SCRIPT-0209`.

Nothing runs during a load: no chunk, no module initializer, no binding installation, no
`seal`. Module sources are compiled only to recover their prototypes.

## Suspension points

Upstream Lua 5.0.2 refuses a yield across a C call or metamethod ("attempt to yield across
metamethod/C-call boundary"), so the only suspension point is `coroutine.yield` called
from a Lua function, and every host binding returns before the barrier (the
`BindingContext` has no yield). At a barrier every thread is therefore idle, unstarted,
finished, or suspended with this exact frame shape: the base C frame, Lua frames that
called the next frame with `OP_CALL`, a last Lua frame at an `OP_CALL` or `OP_TAILCALL`,
and the `coroutine.yield` C frame. Anything else (a running call, an active hook, another C
frame) is an unsupported C continuation and the save fails. The load checks the same shape
against the compiled prototypes, including that each frame's saved instruction is the
call whose register is the next frame's function.

## What is saved

Header, in order: the save format, sandbox policy, random stream and numeric ABI
identities; the session settings (seed, tick duration, every quota, script directories);
the module manifest digest; the binding names in registration order; the completed tick.
A load compares each part with its own session and names the first that differs.

Then the pending events and timers in key order (key, target, kind, name, thread,
arguments, parameter), and the instances in ascending ID, each with:

- host state: instance tick, draws of the tick's random stream, command, post and timer
  sequences, next handler ID, mutation generation, registration, queued event and pending
  timer counts; every thread slot in slot order (finished slots stay as holes) with its
  flags, registry references and both event queues (callbacks and parameters separately,
  so skewed queues stay skewed); event handlers by name with IDs and function references;
  the staged output of an instance created since the last service (commands, posts,
  timers, diagnostics);
- the Lua graph (below).

Host values nest lists at most 64 deep (`max_value_list_depth`; a flat list is depth one).
`submit_event` rejects a deeper argument or parameter with `EAWR-SCRIPT-0210`, so every
admitted event can be saved and loaded; values converted from Lua stop at 16. Should a
deeper value reach the writer anyway, `save` fails with `EAWR-SCRIPT-0212`.

### The Lua graph

Every object reachable from the registry and the main thread, numbered 1, 2, ... in
breadth-first order: the registry first, the main thread second, then each object's
references in a fixed order. Tables list their metatable, then their pairs in canonical
key order (booleans, numbers, strings bytewise, host handles by (kind, id), other
references by identity, as `pairs`). Because every reference key has an identity and
identities follow script operations, the numbering and the bytes are the same on every
target, worker count, allocator and collection schedule. Unreachable garbage is not
saved.

| Kind | Encoding |
|---|---|
| 1 string | u32 length, bytes |
| 2 table | u32 metatable (0: none), u32 pair count, pairs as (key, value) |
| 3 Lua function | u32 prototype, u32 environment table, u8 count, u32 upvalue objects |
| 4 C function | stable name, u8 count, upvalue values |
| 5 userdata | u32 metatable (only zero-length binding objects) |
| 6 host handle | u32 kind, u64 id (its metatable is the handle metatable) |
| 7 handle index | u32 count, u32 handle objects in (kind, id) order |
| 8 thread | u8 main, u32 globals, hook (mask, allow, init, base count, count), u32 stack size and values, u32 frame count and frames |
| 9 prototype | chunk name (`@PATH`), u32 depth, u32 child indices |
| 10 upvalue | u8 open; open: u32 thread, u32 stack slot; closed: value |

A value is a u8 tag: nil, false, true, number (u64 payload bits under the numeric ABI) or
reference (u32 object). A frame is base, top and state, and for a Lua frame the saved
instruction index and the tail call count. Only the live stack below the top is saved;
the slots above it are dead (the collector clears them too). Stack and call-info
allocation sizes are not state: a load allocates what the frames need, with a
power-of-two call-info array so the "stack overflow" depth stays the same.

After the object count come the special objects (registry, main thread, handle metatable,
identity anchors, handle index, size table), the next identity, the memory account (the
last measurement and the bytes charged since, [sandbox](lua-sandbox.md)), and the
sandbox's read-only tables and protected global names as ascending object lists. A save is
refused while a thread's stack mark is pending; the end of every service settles it.

**Code identity.** A prototype is named by its chunk (the manifest module it was compiled
from) and its path of child indices from the chunk's main function. The sandbox records
the path of every prototype it compiles; a load compiles each named module once and
follows the paths. The manifest digest in the header pins the bytes, so the compiled code,
constants and saved instruction indices match.

**Host references.** C functions are saved by stable names (`base.*`, `coroutine.*`,
`string.*`, `table.*`, the sandbox's `sandbox.*` and the infrastructure's `host.*`, plus
`coroutine.wrapper` and `string.gfind_iterator`), never by address. The table refuses two
names for one address, so a linker that folds identical functions cannot make the name
ambiguous. Host bindings are generic closures whose upvalue is the binding's index in the
session's registration order, which the header pins. Host handles are saved as (kind, id)
and interned again on load.

**Cursors.** Iterator state is ordinary graph state: a `pairs` iterator is a closure over
the table, its key snapshot, position and count; `string.gfind` over subject, pattern and
position; `coroutine.wrap` over its thread; a `next` or `ipairs` loop keeps its key in a
register of the suspended frame.

**Weak size table.** lauxlib's `table.getn`/`setn` size table has weak keys. It keeps a
table key only when that table is reachable without it, and those keys follow its other
keys in object order.

**Open upvalues.** An upvalue open on a reachable thread is saved as that thread's stack
slot, so a closure and the suspended frame keep sharing the variable. An upvalue open on
an unreachable (abandoned) coroutine can never change again and is saved closed.

## Validation on load

Before building anything the load checks: every length and count against the remaining
input, its quota or format bound; every object reference, kind and value tag; the special
objects and that the registry holds them; identities 1..n on distinct reference objects
with n within the identity quota; the memory account within the memory quota; that
script tables have no reference key without an
identity, no `__gc` and no `__mode`; every handle in the handle index exactly once; every
C function name and the upvalues it expects (a `pairs` or `gfind` position inside its
range, a binding index below the binding count); thread hooks equal to the instruction
budget hook; the suspended frame shape; open upvalue slots inside their stack and unique;
thread slots within the slot quota, queue, registration and timer counts equal to what
the save holds and within their quotas; each registry reference of a slot or handler
holding the right kind of value; instance ticks consistent with the completed tick. Only
then are objects allocated, with the collector off, and the scheduler switched over.

**Decode memory.** Decoded items are appended one at a time as their bytes are read, never
reserved for a count, and each is charged against a budget of 128 bytes per input byte
(`decode_budget_per_input_byte`; the densest real encoding, a closed nil upvalue, needs
about 60). A forged count therefore allocates no more than the bytes behind it decode to,
and an allocation failure during a load is one more rejection with `EAWR-SCRIPT-0213`.

## State hash

`state_hash()` is SHA-256 over `eawr-script-state-v1`, a zero byte and the canonical save.
The save holds every value that can affect a later tick: the Lua graph, identities,
scheduler queues and counters, random draw positions and next IDs. Existing replay
formats and their v1/v2 goldens are unchanged; the tactical tick hash does not include
scripts until EAWR-79 runs scripts in the tick.

`authoritative_state_sha256(tick, world, script)` combines one completed tick's world
and script state hashes under `eawr-authoritative-state-v1` (identity, u64 tick, both hex
strings, each length-prefixed): the per-tick hash a scripted session exchanges ([multiplayer
readiness](multiplayer-readiness.md), tick agreement).

`state_digest()` (EAWR-247) stays a test digest.

## Tests

`lua_persistence_tests` (CTest `lua_persistence_*`):

- `roundtrip`: a scenario with cycles, aliases, metatables, closures sharing upvalues,
  suspended coroutine stacks with tail calls, `coroutine.wrap`, `gfind` and `pairs`
  cursors, an abandoned coroutine's upvalue, handle and function keys, size-table
  entries, finished, failed, killed and restarting thread slots, skewed event queues,
  timers, registrations, an exiting instance and module initializers with side effects.
  Saved at 12 barriers (including before the first tick, with staged chunk output) and
  continued: every tick's commands, diagnostics, removals and state hash equal the
  uninterrupted run, and re-saving a loaded state reproduces the bytes. Also fixes the
  final state hash, the same on every target.
- `workers`: the same with 1, 2, 4 and 8 workers (identical save bytes), and the EAWR-247
  load scenario (24 instances) saved at tick 45 and continued on four workers.
- `corruption`: every truncation and 3,000 random byte mutations of a save; each is
  rejected without changing the target scheduler or decodes to another valid state that
  still runs, and the target then continues exactly like the reference.
- `rejects`: edited module, other seed, missing binding, truncated and trailing input,
  thread slot, registration and identity quotas exceeded under a matching header, and an
  aborted session refusing to save.

## Open items

- Wiring the script state hash into the tactical session's per-tick hash and checkpoint
  format belongs to EAWR-79 (scripts in the tick) and EAWR-250 (checkpoints, resync).
- The save is uncompressed and unbounded apart from the format bounds; EAWR-252 measures
  checkpoint sizes of real FoC script graphs.
