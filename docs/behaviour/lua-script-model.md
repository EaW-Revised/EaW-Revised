# Lua Script Host Behaviour Contract

Scope: the bounded Lua 5.0.2 host and the pinned EaW smoke member. Unknown fork features are named below.

## Version and state lifecycle

| Rule | Behaviour |
|---|---|
| L-01 | Each script instance owns an isolated Lua state, global namespace, loaded-module set, coroutine registry, and host event context. Loading a module in one instance does not make it loaded in another. |
| L-02 | The bounded Phase 0 host opens only the Lua 5.0.2 base library (which also creates `coroutine`, `_LOADED`, `_G`, `_VERSION`, and the base `require`/load functions), string, and table. It does not open the upstream I/O/OS, math, debug, or dynamic-loader libraries. Lua 5.0.2 has no Lua 5.1-style `package` table in this lifecycle. Retail FoC also opens the fork `security` library in every state. Its members are `crcstate`, `crc`, `dumpstrtable` and `md5`, and no EaW or FoC script calls them ([audit](debug-build-audit.md#lua-script-model)). This host does not open it, exposes no security members, and reports that capability as unsupported. |
| L-03 | The state publishes the host script object as `Script` and a `LUA_PATH` string, then loads the requested module through the host file callback. The initial path is `./?.lua;./?.lc`; every configured script directory is appended in insertion order with its `.lua` pattern before its `.lc` pattern. A configured directory is normalized only by appending a slash when it has neither slash kind at the end; duplicate detection is exact string equality. Each state copies `LUA_PATH` when it is created, so a directory added later reaches only states created after it. |
| L-03a | `require(name)` uses `name` verbatim as the `_LOADED` key and substitutes it verbatim for every `?` in each path component. It returns a truthy cached value without opening a file. Otherwise it searches path components in order, executes the first loadable file with no arguments and requests one result, converts a nil/no return to boolean true, stores that value only after successful execution, and returns it. A false result is stored but is not a cache hit on the next request. Load, syntax, or execution failure is not cached and propagates as an error. A file that exists but fails to compile ends the search with that error; later path components are not tried. While a module runs, the global `_REQUIREDNAME` holds its name. The cache is not preseeded with an in-progress marker, so a recursive same-name request attempts another load unless the module writes `_LOADED[name]` itself. |
| L-03b | The fork redirects Lua file opens to the supplied logical file abstraction. The exposed base `loadfile` and `dofile` therefore use the same callback; they are not authority to bypass the logical source. `loadstring` consumes caller-supplied bytes and performs no file access. |
| L-04 | A non-pooled shutdown first prevents further script work, removes retained coroutines and events, and then closes the Lua state. All host callbacks that could re-enter the state must be detached before close. |
| L-05 | The reference also supports state pooling with a distinct global reset sequence: retail calls the script functions `Flush_G` and then `Base_Definitions`, clears the stack and collects garbage. Phase 0 may omit pooling, but must report pooling as unsupported; it must not claim that ordinary close/reopen reproduces pooled behavior. |
| L-06 | The compiled modules of the older archives use the PGLua header and add one 32-bit function-persistence identifier to every otherwise Lua 5.0.2-style prototype. The installed x64 game does not load them: its chunk loader requires `size_t` width 8, and every script has a `64Patch.meg` text source that shadows the compiled member, in the EaW and FoC profiles alike ([audit](debug-build-audit.md#lua-script-model)). A validated clean conversion removes the identifier and emits the standard header in the target VM's native ABI; it serves the EaW smoke fixture. The FoC runtime loads text sources. |
| L-06a | Whole-state save/restore, suspended-coroutine restoration, and saved function-reference fixups remain separate fork features. Phase 0 reports them unsupported and does not treat ordinary chunk conversion as persistence compatibility. |
| L-07 | Script bytes and module dependencies are obtained only from the supplied logical file interface. A load failure names the logical script/module and fails the instance start; it does not fall back to unrestricted native file access. The clean host must reject path escape at that interface even though the reference snapshot does not establish archive-name case folding or `..` normalization. |

The fork evidence also includes host file callbacks, per-thread alert handling, state persistence, a host assumption operation, and stable function persistence identifiers. The first smoke needs the file/call lifecycle and ordinary module conversion, but not whole-state persistence. The [compiled-chunk contract](pglua-chunks.md) defines the exact boundary.

## Host calls and values

| Rule | Behaviour |
|---|---|
| L-10 | A host request for a named Lua global returns “no result” when the name is absent or is not a function. It does not manufacture a callable value. |
| L-11 | Host-supplied parameters are delivered in their original order. The host requests one Lua result; a protected-call failure emits the script diagnostic path and produces no result. |
| L-12 | Lua-visible `Function_Call` takes a callable host userdata value first and forwards every remaining argument, in order, to that object. The current coroutine identity is established before dispatch. |
| L-13 | The userdata call returns zero Lua values when the host returns no value or an empty result collection. Otherwise its ordered host result collection becomes the ordered Lua return values. |
| L-14 | Supported boundary values are nil, boolean, number, length-bearing byte string, function, host userdata, coroutine, opaque pointer where an API explicitly permits it, and a sequential table. Strings preserve embedded zero bytes. Sequential table positions are one-based. Associative table conversion is opt-in, never an automatic substitute. |
| L-15 | Numeric host wrappers use single-precision storage at this boundary even though the Lua VM has its own numeric representation. Tests that cross the boundary must compare using the documented API tolerance rather than assuming lossless double precision. This is the P0 host's boundary, kept unchanged by the [numeric profile](../lua-numeric-profile.md). Retail FoC (x64) holds these values as binary64 in its host wrapper ([audit](debug-build-audit.md#lua-script-model)); an engine API may still narrow a value it keeps in a single-precision field. |
| L-16 | An absent registered engine API stops at that call and emits a distinct missing-API diagnostic containing the function or method name, logical script, line when available, instance, coroutine, and Lua traceback. It is not silently converted to nil or an automatic success stub. |

`Call_Function` (host asking Lua to run a named global) and Lua-visible `Function_Call` (Lua asking a host object to run) are different interfaces and must have separate diagnostics.

`P0-07-REQ` identifies the safety and diagnostic requirements imposed by the Phase 0 work contract rather than an inference about retail internals.

## Coroutines

| Rule | Behaviour |
|---|---|
| L-20 | Creating a script coroutine names a global Lua function and optionally supplies one initial parameter. Success returns a numeric, stable slot identifier. |
| L-21 | The root state retains every live coroutine so garbage collection cannot remove a yielded coroutine. |
| L-22 | A pump visits live slots in ascending creation order. A slot is resumed until it yields, returns, or errors. |
| L-23 | After each resume the pump reads the topmost value the coroutine left. Boolean `true` keeps the slot live; false, nil, no value or a non-boolean ends it; an error ends it and follows the script-error path. Retail does not tell a yield from a return: a coroutine that returns `true` keeps its slot and is called again from its start on the next pump, and a multi-value yield keeps the slot only when its last value is `true` ([audit](debug-build-audit.md#lua-script-model)); RO-4 confirmed the restart at runtime. The Phase 0 host and authoritative scheduler implement this rule. |
| L-24 | Ended slots are not compacted or reused during the script instance lifetime. A later coroutine receives a later identifier. |
| L-25 | `Create_Thread.Kill(current_id)` returns zero values and leaves the current slot live. `Kill_All` skips the current slot and ends every other live slot. Outside a script coroutine the current ID is −1: `Kill_All` ends every live slot and `Kill(valid_id)` ends that slot. Invalid IDs have no Lua return value and do not change a live slot. The debug build asserts on a self-kill and on an out-of-range identifier. Shutdown clears all slots before the state is closed. |
| L-26 | Creation accepts a global function name plus at most one meaningful initial value. That value is supplied only on the first pump; later resumes receive no host-supplied values. Additional arguments to the Lua-visible creation wrapper are ignored. |

## Events

| Rule | Behaviour |
|---|---|
| L-30 | Each coroutine has a first-in, first-out event queue and a parallel first-in, first-out parameter queue. Signalling appends one callback and its parameter value to the respective queues. |
| L-31 | `GetEvent()` removes and returns the oldest queued callback for the current coroutine. `GetEvent.Params()` separately removes and returns the oldest queued parameter value. Empty, out-of-range, and outside-coroutine reads return zero Lua values, not one nil. A queued null parameter is consumed and also returns zero values. `GetEvent.Reset()` clears both queues for every coroutine and returns zero values. |
| L-32 | The observed object-in-range registration interface appends handlers and cancellation removes every matching handler. If a callback mutates that registration list, the service restarts at the first registration. Registrations that completed their scan earlier in the same service call are skipped; the registration whose callback caused the mutation can be visited again. These mutation rules are specific to this observed interface, not a universal rule for every engine event family. In retail the events fire only inside the object's `Service_Wrapper` call. Candidates come from a box query around the object whose half extent is the registered distance on all three axes; decorations, objects pending deletion and the object itself are skipped; an optional player keeps only that player's allies and an optional type only that type; a fleet delivers each contained unit. |
| L-33 | Candidate-object discovery order is supplied by an engine collection and is not established as deterministic by this evidence. A fresh host must not advertise a deterministic candidate ordering without a separate observation contract. |

The callback and parameter queues deliberately can become skewed because reads are independent. The reference does not diagnose or repair skew; an implementation seeking original compatibility must not silently pair, discard, or reorder the remaining entries.

## Wide strings

The FoC debug build has no wide-string class and none of these member names ([audit](debug-build-audit.md#lua-script-model)). The rules below describe the GlyphX reference only; no FoC registration exposes them.

| Rule | Behaviour |
|---|---|
| L-40 | A wide string is a sequence of UTF-16 code units, matching the original Windows representation. Its length, indexing, and substring counts are measured in code units, not Unicode scalar values or bytes. |
| L-41 | Lua-visible explicit positions are converted from one-based to an internal zero-based position; count arguments remain counts. `append`, `assign`, `erase`, `insert`, and `replace` mutate the receiver and return a distinct wide-string wrapper containing the same post-mutation value. `reserve` and `resize` mutate the receiver and return zero values. `substr` does not mutate the receiver. |
| L-42 | A cross-platform host must not route this contract through a platform `wchar_t` whose width changes on another operating system. Supplementary characters therefore count as two UTF-16 units. |
| L-43 | The exact narrow-string encoding used by wide-to-narrow conversion is not present in the available snapshot. ASCII conversion is safe for initial acceptance cases; non-ASCII conversion policy and failure behavior are blocked pending observation. |
| L-44 | Found search positions are returned zero-based and pass through the single-precision host number wrapper. The native not-found sentinel also passes through that wrapper and is word-width dependent, so no portable numeric not-found oracle is asserted. `at(i)` returns the suffix beginning at the requested UTF-16 unit rather than one unit. |
| L-45 | Despite their messages naming both types, `compare`, all `find` variants, `insert`, and `replace` reject a wide-string argument because their actual type gate accepts only a narrow Lua string. `append` genuinely accepts either type; `assign` accepts only a narrow string. This is an original compatibility quirk, not a recommended new API shape. |

## Determinism boundary

This contract fixes ordering only where a rule says so. Lua table iteration, random-number state, wall-clock time, floating-point behavior across architectures, and engine collection ordering are outside the Phase 0 deterministic simulation contract. Script execution must not be treated as lockstep-authoritative merely because coroutine and event queues have specified local order.

## Original expected-outcome cases

### C-01: isolated exact-key module cache

Given two fresh script instances and synthetic modules `Exact` and `exact`, where `Exact` increments a global counter and returns nil.
Expected: A's first `Exact` returns true and runs once, A's second request returns cached true without a file open, `exact` is a distinct request, and B performs its own first load. Covers L-01 and L-03 through L-03b.

### C-01a: false and failing modules are retried

Given module `FalseModule` increments a counter and returns false, while `ErrorModule` increments another counter and raises an error.
Expected: `FalseModule` executes twice and returns false twice; `ErrorModule` executes twice and errors twice. Neither outcome produces a truthy cache hit. Covers L-03a.

### C-02: missing and failing host-to-Lua calls

Given one absent name, one global number, and one global function that raises a synthetic error.
Expected: all three produce no result; only the failing function follows the script-error diagnostic path. Covers L-10 and L-11.

### C-03: Lua-visible multi-return

Given a synthetic host userdata callable that receives three distinguishable values and returns three different values.
Expected: the host observes the original order and Lua receives exactly the returned order. A second callable returning an empty collection yields zero Lua results, not one nil. Covers L-12 through L-14.

### C-04: stable coroutine slots

Given coroutines A, B, and C created in that order, where A calls `coroutine.yield(true)`, B returns true normally, and C errors on its first pump.
Expected: A and B remain; C is ended; D's identifier is later than C's rather than reusing its dead slot. The next pump resumes A with no host-supplied values and starts B again from its function. Covers L-20 through L-24 and L-26.

### C-05: current coroutine survives kill-all

Given current coroutine A and waiting coroutines B and C.
Expected: A remains executing, B and C end, and the self-kill returns zero Lua values. A subsequent outside-coroutine kill-all ends A. Covers L-25.

### C-06: ordered events and independent queues

Given callbacks Red then Blue, with parameters 10 then 20, signalled to one coroutine.
Expected: callbacks arrive Red, Blue and parameters arrive 10, 20. One extra read from either interface returns zero values. After reset from any coroutine, both interfaces are empty for every coroutine. Covers L-30 and L-31.

### C-07: object-in-range mutation restart

Given one eligible candidate and registrations A, B, C, D in that order, where B cancels C the first time B is called and is otherwise inert.
Expected: A is called once, B is called twice because mutation restarts before B was marked complete, C is never called, and D is called once. This case does not constrain candidate discovery order. Covers L-32 and L-33.

### C-08: UTF-16 units

Given a synthetic wide string containing ASCII `A`, one supplementary Unicode character encoded as a surrogate pair, and ASCII `B`.
Expected: length is four units; the two surrogate units occupy distinct positions, `at(2)` returns the three-unit suffix beginning with the high surrogate, and `substr(2, 1)` returns only that high-surrogate unit. Non-ASCII conversion to a narrow string is not asserted. Covers L-40 through L-44.

### C-08a: mutating wide-string return

Given a receiver containing `AB` and a narrow argument `C`.
Expected: the receiver becomes `ABC`, one distinct wrapper containing `ABC` is returned, and length on both wrappers is three. Calling `compare` with that returned wide wrapper yields zero values because the original type gate rejects it; comparing with the narrow string `ABC` returns numeric zero. Covers L-41 and L-45.

### C-09: destroy cuts off re-entry

Given a yielded coroutine, one queued event, and a registered host callback in an instance.
Expected: the coroutine and queued event are released, the callback cannot re-enter the closed state, and the new event is rejected with an invalid-instance diagnostic. Covers L-04 and L-25.

### C-10: logical load failure and library boundary

Given a logical file interface that reports one requested module missing.
Expected: `_G`, `_VERSION`, `_LOADED`, `coroutine`, `require`, `loadfile`, `dofile`, `loadstring`, `string`, and `table` are visible; `package`, `math`, `io`, `os`, `debug`, dynamic loading, and fork-security members are absent. Capability metadata reports the fork-security library as unsupported. The missing module fails the start without native filesystem fallback. Covers L-02, L-03b, and L-07.

### C-11: missing engine API diagnostic

Given a synthetic script that calls one name absent from the explicit stub registry.
Expected: execution stops at that call and the diagnostic distinguishes a missing engine API from an ordinary script error while carrying the visible name, script, line when available, instance, coroutine, and traceback. Covers L-16.

## Real retail smoke contract

Use the normalized archive member `Data/Scripts/Story/Story_Empire_ActI_M02_Fondor_Land.lua`, SHA-256 `8ae61a1a0f1150dba3fd7d765cdb81c019df5ce38452d9d781de8c5895f5b983`, size 5,655 bytes. Invoke `Intro_Cinematic` with zero parameters.

This pinned Empire at War member and the `eaw` profile are the only required Phase 0 retail smoke acceptance target. The pinned member is the compiled chunk in `Config.meg`; the installed x64 game runs the `64Patch.meg` text source of the same script instead (L-06). A command-line tool may also support loading a caller-selected `foc` profile, but that general loader capability is not an FoC smoke claim: no FoC script, entry point, dependency closure, or expected call boundary is pinned here.

The bounded host surface registers only `Suspend_AI(number)` as a logging no-op that accepts a positive number and returns zero Lua values. It deliberately leaves `Lock_Controls` absent. `Intro_Cinematic` is persistence prototype 7. Its first three zero-based instructions load `Suspend_AI`, load numeric argument 1, and call it with one explicit argument; PCs 3–5 do the same for `Lock_Controls`. The expected visible sequence is exactly one successful `Suspend_AI(1)` invocation at call PC 2 followed by the first missing API `Lock_Controls(1)` at call PC 5; execution then stops at that unsupported boundary. Retail line vectors are stripped, so the diagnostic location is logical member plus prototype 7/PC 5, not a fabricated source line. This is a selected and statically evidenced test contract, not a claim that a Phase 0 runtime has already executed it.

The loader dependency closure inferred from literal module references is:

| Logical module | SHA-256 | Size (bytes) |
|---|---|---:|
| `PGStoryMode` | `aa47d713f4883779ff9e21b92f7e518a89ea6df3b6f8888b11f33b8727b8e7cc` | 1,189 |
| `PGStateMachine` | `eeff939ca865e876d52c2c4b7349b725bdad37ff6fb46806da4b11fcbd9c61c6` | 2,426 |
| `PGSpawnUnits` | `0281c058d3dfb46d11dea5d38af63a09fbbaef175a78fca2f69464582104ace6` | 2,671 |
| `PGMoveUnits` | `a1d4f766f517ea5a91323da582888ae6cec4ad7e8ae7721e34f9c2a3d6eef7de` | 2,230 |
| `PGCommands` | `3fbc6cfca9f4e1c5f2f4b20bdac50213a6299eca0b92b38abb3dcd34cea717a0` | 6,678 |
| `PGBaseDefinitions` | `3ab5c5e008fbdde380cd9cf27355439f44a5dbc8a6bd6c64228c5abdbab645cc` | 1,804 |
| `PGBase` | `16a44d223d1037f5a8f3bf9a869776ba08f8c284440251b0db1a0ae309e27469` | 4,123 |
| `PGDebug` | `6895d948c73e8256f23dd65227b0cc5c7a87501b47f636dbe610fbdd5dd192b9` | 1,688 |

The module-load request trace produced by executing the converted module initializers under the pinned upstream loader is, in order:

1. `PGStoryMode`
2. `PGStateMachine`
3. `pgcommands`
4. `PGBaseDefinitions`
5. `PGBase`
6. `PGDebug`
7. `PGSpawnUnits`
8. `PGBaseDefinitions` (truthy cache hit)
9. `PGMoveUnits`
10. `PGBaseDefinitions` (truthy cache hit)

Every initializer returned nil and therefore each first successful load was cached as boolean true. The eight unique dependency bytes match the hashes above. This closes the dynamic initializer closure for the selected converted corpus; a fresh host must reproduce the same logical request sequence before claiming its own hermetic smoke result.

All nine required chunks use only constant tags and opcode IDs covered by exact upstream/retail prototype-body pairs. Their fixed 32-bit retail representation can be re-encoded as a standard Lua 5.0.2 chunk for the target VM's native `size_t` width. Structural validation succeeded for every member of both pinned archives; the remaining smoke gate is host execution and dynamic dependency tracing, not an unknown chunk layout.

## Unknowns

| Gate | Unknown |
| --- | --- |
| LUA-G01 | Execution equivalence after the now-specified PGLua-to-5.0.2 conversion |
| LUA-G02 | Exact non-ASCII narrow/wide conversion and error policy (GlyphX only; FoC has no wide strings) |
| LUA-G03 | Word-width-specific wide-search not-found value and malformed-surrogate conversion (GlyphX only) |
| LUA-G04 | Clean-host reproduction of the pinned dynamic trace |
| LUA-G05 | Resolved: `crcstate` and `crc` return eight-digit CRC strings of the state or of a value, `dumpstrtable` prints the interned strings, `md5` returns a 32-digit digest of a string |
| LUA-G06 | Whole-state PGLua persistence (the state format restores prototypes by their file identifiers) |
| LUA-G07 | Resolved: the pump reads the topmost value (L-23) |

Readiness: lifecycle, exact-key module caching/search order, ordinary compiled-module conversion, the required EAW smoke opcode subset, calls, coroutine scheduling including L-23, event empty/reset/mutation behavior, UTF-16-unit rules, and the expected smoke request/call trace are implemented. Actual clean-host execution, FoC smoke qualification, and non-ASCII conversion remain explicitly gated. The authoritative scheduler supports whole-state persistence. The security library's member set is known (LUA-G05) and no script uses it. Compiler choice, quotas, and diagnostic schema are project policy recorded elsewhere rather than claims about original behavior.
