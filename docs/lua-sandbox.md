# Authoritative Lua sandbox and scheduler

Contract for EAWR-247,
2026-09-26: the sandbox, canonical iteration, script time and randomness, and the tick
scheduler of the authoritative Lua profile. It implements the
[multiplayer readiness](multiplayer-readiness.md) policy with the amendments listed there,
on the soft-float VM of the [numeric profile](lua-numeric-profile.md). The
[P0 host](lua-runtime.md) is unchanged. Policy identity `eawr-lua-sandbox-v3` (v2 meters the
table library and `string.rep`, EAWR-373; v3 adds metered pattern matching and the memory quota,
EAWR-375), random
stream identity `eawr-script-rng-v1`; changing a rule below needs a new identity, and both
belong to session identity, to saved state and to state hashes ([persistence](lua-persistence.md)).
v2 (2026-09-27) charges the table library's size-driven loops and `string.rep` to the budget
(Costs, below);
a v1 save or session is rejected. v3 (2026-09-27) charges pattern matching steps and adds the
memory quota (Costs, Memory quota); a v2 save or session is rejected.

Code: `src/script/sflua/sflua_sandbox.*` (inside the VM) and
`include/eawr/script/authoritative/` with `src/script/authoritative/` (scheduler). The
upstream Lua 5.0.2 `.c` sources stay unmodified; some of their compile wrappers changed.
`upstream/ltable.cpp` renames upstream `luaH_set` and `luaH_setnum` while including
`ltable.c` and defines the public names as a sandbox check (protected bindings, read-only
tables, weak and finalizer keys, identity assignment, sort comparator writes) followed by
the upstream call; `ltable.c`'s own calls (rehash) keep the upstream functions.
`upstream/lapi.cpp` does the same for `lua_newthread`, so a new coroutine is charged to
the memory quota and inherits the creating state's hook. The same renaming puts the memory
charge in front of `luaH_new` (`ltable.cpp`), `luaS_newlstr` and `luaS_newudata`
(`lstring.cpp`) and `luaF_newproto`, `luaF_newCclosure` and `luaF_newLclosure`
(`lfunc.cpp`), and lets `luaE_freethread` (`lstate.cpp`) drop a collected thread's stack
mark. `upstream/prototype_charges.hpp` redefines `luaM_growvector` for `lparser.cpp` and
`lcode.cpp` so that each item the compiler appends to a prototype is charged before its
array grows, and `upstream/lvm.cpp` routes `luaV_concat`'s `luaZ_openspace` through a
check of the result string's charge. `upstream/lstrlib.cpp` adds a metered copy of
`str_find`, `gfind`, `str_gsub` and `add_s` with the matcher, below the unmodified
`lstrlib.c`.

## Library surface

Each instance is one Lua state with base (and coroutine), string and table, as FoC. The
same rules apply to original scripts and mods.

- Replaced: `pairs`, `next`, `table.foreach` (canonical order, below), `table.sort` (pure
  comparator), `tostring` (identities), `collectgarbage`, `require` (session manifest).
  `string.find`, `string.gfind` and `string.gsub` are upstream's code line for line with
  charges added (costs, below): results and error messages are upstream's, which the
  `lua_limits_patterns` and `lua_limits_foc_patterns` tests check against the P0 runtime.
  FoC runs upstream Lua 5.0.2 semantics; the metering has no retail counterpart.
- Forbidden, faulting with `EAWR-SCRIPT-0203` and the function's name: `print`, `gcinfo`,
  `loadstring`, `loadfile`, `dofile`, `newproxy`, `setfenv`, `string.dump`. `io`, `os`,
  `debug`, `math` and `package` do not exist. Retail FoC also opens the fork's `security`
library (`crcstate`, `crc`, `dumpstrtable`, `md5`); no FoC script calls it, and it is absent
here.
- `collectgarbage(limit)` accepts an optional number and does nothing; any other argument is
  the upstream argument error. FoC's `PGBaseDefinitions` calls `collectgarbage(256)` when it
  loads, and no FoC script uses another form; collection is unobservable here.
- Every global present before the first script runs (libraries and host bindings) is
  protected: writing it, raw or not, through `_G` or any alias, faults with
  `EAWR-SCRIPT-0201`. The `string`, `table` and `coroutine` tables and every binding
  object's member table are read-only. Binding objects are userdata whose metatable is
  hidden (`getmetatable` returns `"EAWR binding"`). Script-defined globals stay writable.
- Writing the key `__mode` or `__gc` into any table faults with `EAWR-SCRIPT-0202`, so no
  script can make a weak table or a finalizer. lauxlib's own size table for
  `table.getn`/`setn` is internal and exempt; no script can reach it.
- New coroutines inherit the instruction budget hook (upstream starts them without one).

## Keys and canonical order

Table keys may be booleans, numbers, strings, host handles and other references (tables,
functions, threads, userdata); light userdata cannot be a key. The canonical order is
`false`, `true`, numbers ascending, strings bytewise (a prefix first), host handles by
(kind, id), then other references by identity.

**Identities.** A reference receives the next identity of its instance (1, 2, ...) the
first time it is written as a table key or passed to `tostring`, and keeps it for the
instance lifetime; the instance anchors it, so it is never collected. Identities are never
derived from addresses, so the order depends only on the order of script operations.
Removing a key and inserting it again keeps its place. The identity quota (default 65,536
per instance) faults with `EAWR-SCRIPT-0204`. FoC's heaviest script registers 64 distinct
callback functions (a story mission); handles do not use identities, as keys or through
`tostring`.

**Host handles** are interned userdata: equal (kind, id) is one Lua value, so it works as
a table key the way FoC's per-script wrapper cache does. Ids are below 2^37.

**Iteration.**

- `pairs(t)` snapshots t's keys in canonical order when it is called. The iterator skips
  keys deleted since, reads current values, and never visits keys added since. It is
  a C closure, not `next`.
- `next(t, k)` returns the least current key greater than k (the least key for nil),
  whether or not k is still in t; a k without a place in the order is an error.
  `for k, v in t do` uses it, as upstream does.
- `table.foreach` iterates the same snapshot as `pairs`. `ipairs` is upstream.

**Costs.** `pairs` and `foreach` charge n × (⌈log2(n+1)⌉ + 1) instructions for sorting n
keys; `next` charges the n entries it scans, so a `next` loop costs O(n²).
`table.sort` charges the same as `pairs` for its n elements before it copies them, plus the
comparator's instructions.

The table library's C loops run over `table.getn` sizes, which `table.setn` or an `n`
field can set far beyond a table's entries; FoC's table library is upstream Lua 5.0.2's
(debug build). Each loop is charged one instruction per step, before it runs where its
length is known: `table.insert` its moves (n − pos after growing n to a later pos),
`table.remove` its moves (n − pos), `table.concat` the elements it will visit (up to the
first value that is not a string or number, where it raises), `table.foreachi` each call as
it makes it (it stops at the first non-nil result), and `getn` each entry as it counts it
when no `n` field or `setn` size holds the size, so an exhausted budget stops the count. `string.rep(s, n)` charges its n appends, which
an empty `s` makes free of memory. Results and error messages are upstream's.

**Pattern matching** is charged step by step, each step before it is taken, so a
backtracking pattern faults with `EAWR-SCRIPT-0205` within the budget. The units: 1 per
pattern item the matcher visits; the item's length (1 for a character or `.`, 2 for a
`%` class, the whole set for `[...]`) per subject character tested against it, charged as
soon as the item's end is known, so at most one scan of one item runs uncharged; 1 per
character `%b` scans; the capture's length per back reference; for a plain find, 1 per
position tried and the pattern's length per candidate compared; for `gsub`, per
substitution, the replacement string's length and the length of each capture it copies
(`%1`), or for a replacement function the length of the captures passed to it and of the
string it returns, which are copied into the result (the function itself is charged as
code; Lua 5.0.2 has no table replacement). A small `find` costs a few dozen units; `string.find(string.rep('a', 5000),
string.rep('a*', 20) .. 'b')` exhausts the default budget.

**`table.sort`** runs the upstream quicksort on a private copy of `t[1..getn(t)]` and
writes the result back, so tie order is upstream's. While a comparator runs, any table
write (including through a binding) faults with `EAWR-SCRIPT-0206`; upvalue writes are not
table writes.

**`tostring`** of a table, function, thread or userdata without `__tostring` is
`"<type>: "` followed by its identity as 16 upper-case hex digits, the shape FoC's `%p`
prints on x64. A host handle prints `"userdata: "`, its kind as 8 and its id as 16
upper-case hex digits (`userdata: 00000001:0000000000000014`), so handle strings sort in
canonical key order. Distinct objects print differently and one object always prints the same.
FoC's `PGBase` formats `tostring(CurrentEvent)`, a function, for every event it pumps
(inside `ScriptMessage`); rejecting it would fault every script that receives an event.

## Modules

The session pins a `ModuleManifest`: logical paths (ASCII upper case, `/` separators; empty,
`.`, `..`, absolute and drive-qualified paths rejected) with their bytes. Its digest covers
the policy identity and each (path, SHA-256 of bytes). `require(name)` follows L-03a: the
exact request string is the `_LOADED` key; the patterns `./?.lua`, `./?.lc`, then each
configured directory's `?.lua` and `?.lc` are tried in order against the manifest; the
chunk runs with no arguments and one result while `_REQUIREDNAME` holds the request, nil
becomes `true`, and failures are not cached. A module that fails to compile ends the
search. A request that matches no manifest module faults with `EAWR-SCRIPT-0207`, as does
any precompiled chunk: FoC ships its scripts as source. Compiling is the only place where
identities are not assigned (the compiler's own tables use table keys).

## Time and randomness

Script time is completed ticks. The session fixes the tick duration as a fraction
numerator/denominator of seconds, both 1 to 2048. During the service of tick T,
`GetCurrentTime()` is (T − 1) × duration rounded once to binary64 and
`GetCurrentTime.Frame()` is T − 1; the first service sees 0.

`ticks_until(seconds)` turns a duration into whole ticks with the exact integer ceiling of
seconds ÷ duration, so a deadline never fires early. Binary64 0.1 s is slightly more than
1/10 s and takes four ticks at 30 Hz. Zero of either sign is the next service; negative,
NaN, infinite and durations of 2^40 ticks or more are rejected by the calling API.

Random words are `random_word(seed, tick, instance, draw)`, a counter-based stream (a fixed
chain of SplitMix64 finalizers), so an instance's draws never depend on other instances or
worker counts. Each instance draws from index 0 in every tick. A bounded draw in [0, b)
rejects words below 2^64 mod b, each rejected word consuming a draw; a unit draw is
(word >> 11) × 2^-53, exact. The test `lua_sandbox_clock_rng` freezes words, draws and a
10,000-word digest. `GameRandom.Free_Random` cannot be registered as a binding; FoC's
`GameRandom` API itself belongs to EAWR-79.

## Scheduler

`ScriptScheduler` owns instances with stable nonzero IDs. Bindings are registered before
the first instance; a binding whose top-level name is an infrastructure object or a global
the sandbox installs (`Sandbox::installs_global`, read off a freshly opened sandbox: the
base, coroutine, string and table libraries and their replacements) is rejected with
`EAWR-SCRIPT-0210`, never installed over it. `create_instance` opens a sandbox, installs the infrastructure and
host bindings, seals the globals, and runs the root module's chunk under the service
budget; a failing chunk leaves no instance. Output a chunk stages is committed with the
next service.

**Events.** An event has the key (tick, producer system, entity, producer sequence) and
is delivered in key order. The simulation submits events for future ticks with producer
IDs from 16. Scripts post events (producer 1, delivered the next tick) and start timers
(producer 2, key tick = the deadline), so timer ties resolve by deadline, instance and
registration order. A `dispatch` event calls the handlers registered with
`Register_Event(name, f)` with the L-32 restart: after a handler changes the registration
list, the scan restarts at the first handler not yet completed. A `thread_signal` appends
the named global and a parameter to one thread slot's callback and parameter queues
(L-30/L-31); `GetEvent`, `GetEvent.Params` and `GetEvent.Reset` read them, so the two
queues skew exactly as in FoC.

**Threads** follow FoC's thread pump (debug build) and L-20 to L-26. `Create_Thread(name,
param)` returns the next slot number (never reused); the slot's function is the global's
value at creation, and the parameter reaches only the first resume. A pump visits slots in
ascending order and re-reads the slot count, so a thread created during a pump runs in the
same pump. After each resume the pump reads the topmost value the thread left (L-23 as
corrected from the FoC debug build in EAWR-287): `true` keeps the slot, anything else or an
error ends it. Retail does not tell a yield from a return, so a thread that returns `true`
keeps its slot and starts its function again, without its parameter, on the next pump; a
multi-value yield is decided by its last value. This is one policy point,
`keep_slot_after_true_return` in `scheduler.cpp`, pending the runtime check RO-4 (EAWR-296);
false gives the P0 rule (a normal return ends the slot). `_ScriptExit()` ends the pump after the current thread
and removes the instance after the tick, keeping its output. `GetThreadID()` is -1 outside
threads; `ThreadValue(name)`, `ThreadValue.Set(name, value)` and `ThreadValue.Reset()` keep
string-keyed values per thread and do nothing outside threads, as FoC's `ThreadValue`.

**One service.** For tick T: serially route due events to their instances in key order;
then service every instance (the inbox in order, then the thread pump); then serially commit
in ascending instance ID: commands in (issuer, sequence) order, posted events and timers
into the pending set, diagnostics; finally remove exited and faulted instances and drop
events addressed to them.

**Transactions.** Every resume, handler call and timer delivery is a transaction over the
instance's host state: staged commands, posts, timers, their sequence counters and the
random draw index. A Lua error rolls them back and reports `EAWR-SCRIPT-0211`; the thread
ends, as in FoC. The Lua heap is not rolled back.

**Budget and quotas** (session settings; defaults): 4,000,000 instructions per instance
and service, checked every 128 instructions; 65,536 identities; 4,096 thread slots;
65,536 queued events (callbacks and parameters) and dispatch restarts; 65,536 pending
timers; 65,536 registrations; 64 MiB of logical memory. Exhausting one is a fault: it is sticky (every following
instruction raises again, so `pcall` cannot absorb it), the instance stops at once, its
whole tick output is dropped, the instance is removed, and a diagnostic names it. The same
counts on every target, so every peer faults at the same instruction. An unprotected Lua
error (allocation failure) or a C++ exception aborts the session (`EAWR-SCRIPT-0209`); the
scheduler then refuses further work.

**Memory quota.** Each instance's Lua objects are counted in logical bytes, owned by
the simulation: fixed sizes that are the same on every target and do not depend on the
allocator, on table or stack capacities or on when the collector runs. A string is 32 +
its length; a table 64, plus 32 per non-nil entry; a closure 32, plus 8 per upvalue slot
of a Lua closure and 16 per value of a C closure; an upvalue 32; a userdata 40 + its
payload; a thread 8,192, which covers 512 stack slots, plus 16 per slot above them; a
prototype 128, plus 8 per instruction (with its line information), 16 per constant, 8
per child prototype, 16 per local variable record and 8 per upvalue name.

The account is the last measurement plus what was charged since. Charges come before the
object exists: every string a script operation asks for (whether or not an equal string
is already interned), every new table, every write into a nil slot of a table, every
closure (a Lua closure's slots are charged as if each opened a new upvalue), userdata and
thread, every prototype the compiler creates and each item it appends to one, and at
every call the running thread's stack above its high-water mark once it is past the
allowance. A concatenation (`..`, and `table.concat`, `string.rep` and `gsub` results,
which concatenate through it) checks its result string's charge before it allocates and
fills the buffer for that string, so a result over the quota faults first. A measurement
walks the graph reachable from the registry and the main thread over the edges the save
follows ([persistence](lua-persistence.md)), and from a prototype also to its nested
prototypes, constants, source name, local and upvalue names, and sums the sizes above; it runs at the end of an instance's service once half the last
measurement (at least 256 KiB) was charged since, or when a stack went past the
allowance, so garbage stops counting and the walks cost a fixed share of the charges.
A save and its load hold the same graph, so they measure the same; both numbers are
saved.

The quota is exceeded when the account passes it, at a charge or at a measurement; that
is a fault like the others (`EAWR-SCRIPT-0208`, "memory quota exhausted"), raised at the
charge. Where raising would unwind host code (host pushes outside a protected call, a
binding's body) the fault is recorded and raised at the next instruction or when the
binding returns. The account bounds live memory from above: garbage made since the last
measurement still counts, so a script that keeps close to the quota and churns can fault
before its live objects reach it.

**Workers.** The instance service runs as one partitioned phase, `script-instances`
(`executor.execute_phase` with `sim::tick_partition_count` and `sim::partition_range`; the
phase map in [simulation](simulation.md)): 64 contiguous ranges of the instances in ascending
ID for every worker count, each
instance on one thread at a time, reading only its own state and staging only its own
output. An instance's memory measurement runs in its own range after its service. Bindings must be thread-safe and may only use their `BindingContext`.
`lua_sandbox_workers` runs the representative scenario on the `platform::ThreadWorkerAdapter`
pool with 1, 2, 4, 8 and the hardware worker count, reversed
instance creation, a shifted heap and several process locales, and requires identical
per-tick digests and command streams. `state_digest()` covers the scheduler state and every
instance's global data in canonical order; it is a test digest. The authoritative hash is
`state_hash()` over the canonical save ([persistence](lua-persistence.md)).

## Command routing

`ScriptedTacticalSession` (`tactical_bridge.hpp`) runs one tactical session with its
scripts. A tick: submit the local players' input with the keys it carries (UI-07
`CommandScheduler::take`); submit the previous service's script commands in commit order;
step the world; service the scripts on the same executor. Scripts therefore see the tick
they follow, and their commands execute in the next one: never during a script call.

A script command becomes a tactical player command through the translator registered for
its verb (EAWR-79 registers FoC's), a pure function of the command that names the issuing
player, the units and the payload; script numbers cross into Q24 through `to_fixed`. The
key is the router's: the next tick to execute and the issuer's next sequence, one past the
latest key the world accepted from either path, so a player's UI input and its script share
one sequence without collisions. A translator checks the arity and types of the arguments
and crosses ids through `to_exact_integer`; a verb without a translator, a failed
translation (one that throws included) or a command the world refuses (an undeclared
issuer) is dropped with `EAWR-SCRIPT-0214`, reported with the tick it was input to, and
leaves the world and the issuer's sequence untouched; refused player input is reported and
dropped too. A unit id that names no live unit is not a drop: the unit can die between the
service and the tick, so the world rejects that order when it executes it. Each script command is
routed exactly once and enters the world's replay like any player command, so a headless
replay of `record()` reproduces every world tick without a script (`lua_bridge_routing`).
The tick's hash is `authoritative_state_sha256` over the world and script hashes; the live
session turns it off (`set_authoritative_hash`, EAWR-895) and asks for the script hash only on
request, since it reads only the world's hash. A failed
world step or script service is terminal.

Presentation reaches authoritative scripts only through this input: `step` is the only
call that services the scheduler, and UI state enters only as player commands, so hover,
camera and HUD state cannot resume a script or draw from its random stream.

## Diagnostics

| Code | Meaning |
|---|---|
| `EAWR-SCRIPT-0201` | write to a protected binding or a read-only table |
| `EAWR-SCRIPT-0202` | weak table or finalizer key |
| `EAWR-SCRIPT-0203` | forbidden function, unorderable key or value without identity |
| `EAWR-SCRIPT-0204` | identity quota exhausted (fault) |
| `EAWR-SCRIPT-0205` | instruction budget exhausted (fault) |
| `EAWR-SCRIPT-0206` | table write inside a `table.sort` comparator |
| `EAWR-SCRIPT-0207` | module not in the manifest, invalid path or precompiled chunk |
| `EAWR-SCRIPT-0208` | thread, event, timer, registration or memory quota exhausted (fault) |
| `EAWR-SCRIPT-0209` | session abort: unprotected Lua error or host exception |
| `EAWR-SCRIPT-0210` | invalid host request (binding name, event, timer duration) |
| `EAWR-SCRIPT-0211` | script error in a thread, handler or timer (rolled back) |
| `EAWR-SCRIPT-0214` | script command not routable: no translator, failed translation or refused by the world |

## Load

`lua_script_bench` steps the representative scenario (`tests/script/authoritative/scenario.hpp`:
per instance a PGBase-style sleeping thread that scores and sorts up to 24 units, formats a
debug line and draws random numbers, a second thread that updates a handle-keyed table,
function-keyed timer tables serviced with `pairs` on every pump, and events every seventh
tick) at 30 Hz and prints the per-tick service time. On the rig (Core i7-4790K), 3,000
ticks after 30 warm-up ticks:

| Instances | Workers | Median | p99 | Maximum |
| --- | --- | --- | --- | --- |
| 64 | 1 | 0.18 ms | 0.40 ms | 0.64 ms |
| 64 | 4 | 0.07 ms | 0.15 ms | 0.20 ms |
| 64 | 8 | 0.07 ms | 0.14 ms | 0.19 ms |
| 256 | 1 | 0.91 ms | 2.44 ms | 5.43 ms |
| 256 | 4 | 0.29 ms | 0.76 ms | 1.09 ms |
| 256 | 8 | 0.24 ms | 0.59 ms | 0.89 ms |

The design budget (total Lua p99 at most 3 ms per 30 Hz tick) holds for 256 instances even
on one worker, though the single-worker maximum is above it. The worker rows run on the EAWR-276
pool (`platform::ThreadWorkerAdapter`); 8 workers are the rig's four cores with SMT. This
scenario is heavier in table work than in arithmetic; the soft-float share of Lua time is measured by
`lua_numeric_bench` (EAWR-246), about half for arithmetic-heavy code. EAWR-79 measures the real FoC
AI load.

## Open items

- The game build's live session does not run scripts yet; hosting `ScriptedTacticalSession`
  on its simulation thread, world events as script events, and the story-driven HUD outputs
  (`FLASH_GUI`, `FORCE_CLICK_GUI` as immutable presentation messages whose clicks enter the
  command path once) come with EAWR-79's bindings and EAWR-236's presentation bridge.
- FoC's per-object service gate (`ServiceRate`, `LastService`, a first offset drawn from the
  sync random stream) and `GameRandom` belong to EAWR-79 with the object script bindings.
- The memory quota is per instance; a session-wide total (many instances each near their
  quota) is not bounded.
- Mod gap (EAWR-610 scan): Fall of the Republic's EaWX cross-plot serialiser round-trips values
  through `string.dump` and `loadstring`, which this sandbox forbids; that mod's feature fails
  here until a safe equivalent exists (unverified whether its tactical scripts reach it).
