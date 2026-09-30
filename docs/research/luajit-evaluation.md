# LuaJIT for the script runtime: evaluation (EAWR-610)

Research for EAWR-610, 2026-09-29.
A modder suggested embedding LuaJIT instead of the stock Lua VM, on the grounds that it is "not a
new Lua, just a faster way to run the same Lua". This note checks that claim against the
project's three priorities: FoC fidelity including mods, performance parity with FoC, and
deterministic lockstep and replays. Nothing here changes code.

## The answer

**Not now.** LuaJIT is an excellent VM, but for this project it doesn't pay:

- **It runs a different Lua.** FoC runs Lua 5.0.2. LuaJIT runs Lua 5.1, and FoC's own scripts
  and every mod we scanned use 5.0 features that 5.1 removed: the implicit `arg` table,
  `for k, v in t do` over a table, and `table.getn` with 5.0's rules. Shims could cover most of
  this, but one of the gaps needs parser changes, and numbers print and convert differently
  from FoC.
- **It can't meet the lockstep contract without being rewritten.** It hard-codes hardware
  doubles in hand-written assembler and in the JIT, so our integer-only binary64 profile
  ([numeric profile](../lua-numeric-profile.md)) can't be used. Even with hardware doubles,
  NaN bits, out-of-range conversions, `^` and the number representation all differ between
  x64 and ARM64. The instruction budget, the memory quota and the whole-state save are all
  built into our VM, and they would have to be rebuilt on LuaJIT's internals, where compiled
  code skips the instruction counter.
- **There is almost nothing to speed up.** In the M2 battle, the FoC AI runs at most about
  1,500 Lua instructions in a tick and 77 on average. That is roughly 0.1 ms in the worst tick
  and a few microseconds on average, out of the 33.3 ms tick. The EAWR-601 melee runs no Lua at
  all, and there, projectiles take about two thirds of the tick.

Keep the Lua 5.0.2 VM and optimise it only if a profile ever shows Lua as a hotspot. Look at
LuaJIT again only if the conditions in the [recommendation](#recommendation) are met.

## Costs, benefits and risks

| | What LuaJIT would bring or cost | Weight for this project |
|---|---|---|
| **Benefit: speed** | Its interpreter is several times faster than stock Lua, and its JIT is far faster on loops over numbers and tables. | Low: Lua is ≤ 0.3% of the worst M2 tick and ~0.02% on average (see [Benefit](#benefit)). FoC scripts spend their time in engine calls, which the JIT doesn't compile. |
| **Benefit: ecosystem** | It is widely used and well tested, and modders know it. | Low: FoC modders write for FoC's 5.0; EaWX tests its framework on PUC Lua 5.1, not LuaJIT. |
| **Cost: language gap** | Lua 5.1 semantics, not 5.0 (see [Compatibility](#compatibility)). | High: FoC's own scripts and all six mods we scanned need shims; one gap needs parser changes. |
| **Cost: determinism** | Hardware doubles only. NaN bits, out-of-range conversions and the number representation differ between x64 and ARM64. The JIT compiles different code depending on what ran before. | Blocking for authoritative scripts: breaks option B (EAWR-254) and ADR-010. |
| **Cost: sandbox, budget, persistence** | Our instruction budget, memory quota, canonical `pairs`/`next` and whole-state save are built into the Lua 5.0.2 VM. | High: each would have to be rebuilt on LuaJIT's internals. The instruction budget only works if the JIT is off. |
| **Cost: build** | A custom build: a host tool generates the VM from assembler sources (DynASM). Separate paths for Windows (`msvcbuild.bat`) and POSIX (`make`). | Medium: a new CMake integration for five CI targets, with host/target pairs for cross builds. |
| **Risk: maintenance** | Upstream is active (commits in September 2026) but has essentially one maintainer. Releases are rolling snapshots of the v2.1 branch. | Medium: we would pin a commit and carry our patches. |
| **Risk: platforms** | x64 and ARM64 on Windows, Linux and macOS are supported. On macOS the hardened runtime needs `MAP_JIT` (`LUAJIT_ENABLE_OSX_HRT`). Where runtime code generation isn't allowed (iOS, consoles), only the interpreter runs. | Low to medium. |
| **Risk: security** | FFI, `jit.*` and loading bytecode (`load`/`loadstring` accept LuaJIT bytecode, which isn't verified) must stay closed to mods. | Medium: all three can be closed (`LUAJIT_DISABLE_FFI`, no `jit` library, loaders limited to source), but a wrong build setting would expose them. |
| **Risk: debugging** | LuaJIT has the 5.1 debug hooks, but line and count hooks are only serviced by the interpreter. | Low: EAWR-525 debugs retail FoC's own Lua and doesn't depend on our VM; our own tools would need changes. |
| **Licence** | MIT, like Lua 5.0.2. | None. |
| **Binary size** | A static library of a few hundred KB, several times our Lua build. | Negligible. |

## What we run today

- **VM.** Upstream Lua 5.0.2, unmodified except for one portability patch (a 32-bit instruction
  word on LP64), is built twice ([Lua runtime](../lua-runtime.md)):
  - The **P0 compatibility host** (`third_party/lua`) uses hardware doubles and runs tools and
    fixtures.
  - The **authoritative VM** (`src/script/sflua`) compiles the same sources as C++, with
    `lua_Number` replaced by a 64-bit integer word that encodes binary64. Every operation runs
    in integer code: rounding, NaN canonicalisation, `%.14g` and UCRT-style text, and x64-style
    integer casts ([numeric profile](../lua-numeric-profile.md), EAWR-246/#254 option B). The
    running retail game confirmed the profile (EAWR-376).

  Libraries: base with coroutine, string and table, as FoC. There is no `math`, `io`, `os` or
  `debug` library.
- **Sandbox, budget, persistence.** Canonical key order for `pairs`/`next`, protected globals, no
  weak tables or finalizers, an instruction budget per service, and memory and pattern-matching
  metering. These live inside the VM through wrappers around the upstream sources
  ([sandbox](../lua-sandbox.md)). A whole-state save and state hash walk the VM's own objects
  ([persistence](../lua-persistence.md)).
- **Where scripts run.**
  - The FoC tactical space AI: the freestore and plan scripts through the EAWR-79 host, in the
    tick's partitioned `script-instances` phase ([simulation](../simulation.md)).
  - Galactic conquest and story scripts: not yet (M3).
  - Presentation-side Lua: none. The viewer and UI have no Lua today.

## Compatibility

We scanned FoC's effective script set (370 files) and six workshop mods with a token scanner
that reports counts only: Republic at War, Fall of the Republic (EaWX), two Empire at War
Remake releases, Remake Clone Wars and one other mod (about 3,500 loose files in all). No script
text was copied.

"FoC" and "Mods" give how many files use each feature, in FoC's scripts and across the six mods.

| Difference (Lua 5.0.2 in FoC → LuaJIT 2.1 / Lua 5.1) | FoC | Mods | Shim? Does it change behaviour? |
|---|---|---|---|
| **Implicit `arg`.** In 5.0, a vararg function sees its extra arguments as a table `arg` (with `n`). LuaJIT has no `arg` (PUC 5.1 keeps it only under a compile-time compatibility option), so `arg` is a nil global and the call fails. | 10 files (the shared debug-print helpers in `pgdebug.lua` and `gamescoring.lua`, story scripts) | 4 of 6 mods, up to 16 files (EaWX's logger and event bus) | Yes, by rewriting the source (`local arg = {n = select('#', ...), ...}` at the start of such a function, on the same line) or by patching the parser. The behaviour is the same if `n` is kept. |
| **`for k, v in t do` over a table.** 5.0 iterates a table given directly. In 5.1 it fails with "attempt to call a table value". | 3 files (land AI) | all 6 mods, 1–9 files each | Yes, but only by rewriting the source: wrap the expression in a helper that returns `next, t` for a table and passes other values through. A shim can't reach it at runtime. The behaviour is the same, with one extra call per loop. |
| **`table.getn` and `table.setn`.** 5.0 honours an `n` field, then the `setn` size, then counts up to the first nil. LuaJIT's `getn` is `#t`, which can return any border when the table has holes, and LuaJIT has no `setn`. | `getn` in 20 files; no `setn` | `getn` in 14–103 files per mod; no `setn` | Yes: replace `getn` with a function that has 5.0's rules. It is slower than `#` but gives the same results. Without it, any table with nil holes can give a different count. |
| **`unpack`** follows `getn` in 5.0 and `#` in 5.1. | 11 files (`unpack(arg)` in the debug helpers) | 1–14 files | Yes, with the same 5.0 length rule. Without it, trailing nil arguments get lost. |
| **`collectgarbage(256)`.** 5.0 sets a threshold. In 5.1 a number is an invalid option and raises an error. | 1 file, which every script loads (`pgbasedefinitions.lua`) | 5 of 6 mods | Yes: a wrapper that accepts the 5.0 forms. Garbage collection can't be observed in our sandbox anyway. |
| **Number formatting.** LuaJIT has its own formatter: exact, ties to even, like FoC's. But `string.format('%d', 3e9)` prints `3000000000` where FoC prints `-2147483648` (LuaJIT converts through int64), and NaN prints `nan` where FoC prints `-nan(ind)`. | `%d` in 2 files (`Dirty_Floor` in `pgbase.lua`); `tostring` in 211 files | `%d` in 1–2 files per mod | Only by patching LuaJIT's formatter and its number-to-integer conversions (a C fork). Cases L and M of the EAWR-376 retail probe would fail without the patch. |
| **Integer arguments** (`string.rep`, `string.sub`, table indices) from out-of-range numbers. FoC saturates to INT32_MIN. LuaJIT's result depends on the CPU (below). | rare | rare | A C fork. |
| **`^` operator.** FoC has no `__pow`, so `^` raises an error (EAWR-376 case P). In LuaJIT, `^` is built in and calls the platform's `pow`. | 0 | 0 | Parser or VM patch. As it stands, a script that fails in FoC would run here, with results that can differ between platforms. |
| **`%`, `#` and `...` as expressions; `string.gmatch`, `select`.** These are 5.1 additions. | 0 | 0 | Nothing breaks, but mods written against our engine could use them and then fail in FoC. |
| **`string.gfind`**, removed in 5.1 (only an alias under PUC's compatibility option; LuaJIT has none). | 0 | 0 | An alias to `gmatch`. The two differ only for a pattern anchored with `^`. |
| **Yield across `pcall`, metamethods and iterators.** LuaJIT allows it; 5.0 raises an error. | 0 `pcall` | 0 `pcall` | Nothing breaks, but more states become possible suspension points, and our save codec assumes 5.0's. |
| **`require` and `package`.** 5.0 uses `LUA_PATH`, `_LOADED` and `_REQUIREDNAME`. 5.1 uses `package.*` and `module`. | `require` in 368 files | everywhere | Our host already replaces `require`, so this is manageable. The `package` table must be hidden. |
| **Error message text.** 5.0 quotes as `` `x' ``, 5.1 as `'x'`, and some wording differs. | 0 scripts parse messages | 5 `error` calls (EaWX) | Diagnostics only. Not worth a shim. |
| **`string.dump` + `loadstring` round trips.** LuaJIT writes its own bytecode format. | 0 | EaWX's cross-plot serialiser | Works within one VM. Our sandbox forbids both anyway ([sandbox](../lua-sandbox.md#open-items)). |
| **Metatables and `__gc`.** No `__gc` on tables in either version. `__index` and `__call` work the same. | 0 | EaWX: `setmetatable` in 7 files | No gap. |
| **`math.*`, `loadlib`, `table.setn`, nested `[[ ]]`, `%` upvalues** (5.0 features that 5.1 removed or changed). | 0 | 0 | Unused; no gap. |
| **Size limits.** LuaJIT limits a function to 65,536 constants; 5.0 allows 262,143. | not measured | not measured | Unverified. A mod with one huge data table could hit it. |

Every gap except the formatting and conversion rows can be closed without changing behaviour
if the source is rewritten when it loads. The formatting and conversion rows need patches to
LuaJIT's C code. The `^` row needs a patch to the parser or VM, because a shim can't turn a
built-in operator back into an error. A LuaJIT that runs FoC scripts and mods unchanged is
therefore a fork of LuaJIT with a source rewriter in front of it. It is not LuaJIT as
downloaded.

## Determinism

**Custom `lua_Number`: not possible.** LuaJIT's value representation is a NaN-tagged 64-bit
word: a double, or a tag plus a 47-bit pointer in GC64 mode (`lj_obj.h`). Arithmetic is
written in per-architecture assembler (`vm_x64.dasc` is about 146 KB, `vm_arm64.dasc` about
128 KB) and in JIT backends that emit SSE2 or ARM64 floating-point instructions. There is no
switch for an integer-encoded binary64. Porting option B would mean rewriting the interpreter's
arithmetic on every architecture and disabling or rewriting the JIT's number handling. At that
point it is no longer LuaJIT.

**Hardware doubles instead?** IEEE `+ − × ÷ √` round the same on x64 SSE2 and ARM64 when FMA
is off. So LuaJIT with hardware doubles could, in principle, give identical results on both
CPUs. In practice, these differ:

- **FMA.** The JIT only fuses multiply-add on ARM64 with `-Ofma`. LuaJIT's
  [running docs](https://luajit.org/running.html) say it "is not enabled by default at any
  level, because it affects floating-point result accuracy", and warn about the lower
  determinism. GCC contracts C code by default in GNU mode, though, so LuaJIT's own C (constant
  folding, number parsing) must be built with `-ffp-contract=off`.
- **NaN bits.** `0/0` gives `0xFFF8…` on x64 and `0x7FF8…` on ARM64. LuaJIT doesn't make NaNs
  canonical, so saves, hashes and anything that reads the bits would differ. Our profile makes
  every NaN `0xFFF8000000000000`.
- **Out-of-range conversion to integer.** x64 `cvttsd2si` gives the "integer indefinite"
  value. ARM64 `fcvtzs` saturates, and NaN converts to 0. Integer arguments, `%d` and table
  indices from such numbers therefore differ between CPUs.
- **Number representation.** x64 builds default to one number type. ARM64 builds use
  dual-number mode, with int32 and double values (`LJ_ARCH_NUMMODE`, `lj_arch.h`). The
  results are meant to be the same, but the two CI architectures run different code paths.
- **Traces.** Which code runs compiled depends on hot counters, trace aborts and blacklisting,
  and so on what ran before. A JIT bug then shows up as a desync on one machine or in one
  replay, which is the hardest kind of bug to reproduce. Our soft-float VM has no second code
  path.
- **`^` and `math`** call the platform's `libm` (`pow`, `sin` and so on), which isn't correctly
  rounded and differs between the MSVC, glibc and Apple libraries. FoC has neither, so both
  must stay closed.

**Interpreter only (`-joff`).** This removes the trace-dependence and makes the instruction
counter usable again: LuaJIT services count and line hooks in its interpreter dispatch
(`lj_dispatch.c`), and compiled traces don't call them. It keeps the hardware-double problems
above, which then need patches in two assembler VMs and the C library. LuaJIT's interpreter is
still faster than stock Lua (LuaJIT's own benchmarks put it at several times PUC Lua 5.1). We
didn't measure it here, because this PC is off limits for benchmarks and an offload run wasn't
worth it for a Lua share this small (next section).

**Across platforms.** We need identical bits on x64 and ARM64 across Windows, Linux and macOS,
with the MSVC, clang-cl, GCC and Clang builds. LuaJIT would need every item above patched and
then tested on all five CI targets, just as the soft-float VM already is. It checks this with
SplitMix64 digests of a million operations and state hashes at 1, 2, 4 and 8 workers.

**Verdict.** LuaJIT can't meet the current contract (option B, ADR-010). To accept hardware
doubles, the owner would have to reopen EAWR-254 and ADR-010. Even then, a patched LuaJIT
interpreter would be the only safe mode, and most of the speed would be gone.

## Benefit

| Source | Lua in a battle | As a share of the 33.3 ms tick |
|---|---|---|
| `foc_plan_battle`, M2 Coruscant battle, 18,000 ticks (AI journal, eye-check run) | At most 1,536 VM instructions in a tick, 77 on average, in at most two plan instances plus the freestores | Estimated at 20–60 ns per soft-float instruction: ≤ 0.1 ms in the worst tick (≤ 0.3%), about 5 µs on average (~0.02%) |
| The engine step of the same battle (FoC AI host, C++ and Lua together) | about 0.1 ms a tick ([tactical AI](../behaviour/foc-tactical-ai.md#cost)) | ~0.3% |
| `lua_script_bench`, synthetic, 64 script instances ([sandbox](../lua-sandbox.md#load)) | p99 0.40 ms on 1 worker, 0.15 ms on 4 | 1.2% / 0.5% |
| `lua_script_bench`, 256 instances (stress) | p99 2.44 ms on 1 worker, 0.76 ms on 4 | 7% / 2.3% |
| `lua_numeric_bench`, arithmetic-heavy (EAWR-246) | Soft-float costs about 2× hardware Lua; 256 plan steps p99 2.53 ms | 7.6% for a synthetic step far heavier in arithmetic than FoC's scripts |
| EAWR-601 melee (S, 145 units) | no Lua: the melee gives scripted orders; projectiles take about 65% of the tick | 0% |
| Galactic conquest and story scripts | not run until M3 | — |

The per-instruction range is an estimate from the EAWR-246 operation costs (6–26 ns per soft-float
operation, plus dispatch). A precise number needs `foc_plan_battle` timed on an offload host.
EAWR-601 will report the script and AI share separately; the coordinator will add it here if it
lands before this note merges.

**Is Lua a hotspot? No.** Even an infinitely fast Lua would save at most about 0.1 ms in the
worst M2 tick. In the synthetic stress case, which is 100 times the real instance count, the
partitioned `script-instances` phase already keeps Lua under 1 ms on four workers. FoC's
scripts are thin glue: they sleep, query perception and give orders, so the time goes into
engine calls. The JIT couldn't compile those calls, because a trace stops at a classic C-API
function. Compared with retail, retail FoC runs the same Lua 5.0.2 with hardware doubles, and
our soft-float VM is at most about 2× slower on arithmetic, on a share of the tick that is
already negligible. Performance parity with FoC depends on projectiles, collision, pathing and
rendering, not Lua.

## Alternatives

1. **Keep the VM and optimise it when a profile says so.** The EAWR-246 note already names the first
   lever: exact fast paths for integer-valued operands in the soft-float backend, which cover
   most of FoC's script arithmetic. Others are the host's binding dispatch and canonical `pairs`
   snapshots. Neither shows up today. Cost: small and local. Risk: none to fidelity or lockstep.
2. **LuaJIT only for presentation-side scripts.** There aren't any today, and FoC has no UI Lua.
   It would mean two Lua dialects for modders and two runtimes to build and secure. Worth
   reconsidering only if a large mod-facing presentation scripting surface appears, and even
   then it should be a 5.0-compatible dialect.
3. **Revisit later behind a switch.** The `ScriptHost` and backend interfaces would allow another
   VM, but a switch that changes script semantics splits mod behaviour and multiplayer
   sessions. It isn't worth keeping one open without a need.

## Recommendation

Keep the Lua 5.0.2 VM (hardware doubles for tools, soft-float binary64 for authoritative
scripts). Don't embed LuaJIT for authoritative scripts, and don't add it for presentation now.
For the modder, the short version is this:

- FoC's Lua is 5.0, and LuaJIT is 5.1. FoC's own debug helpers and every mod we scanned use
  5.0-only features.
- Our lockstep multiplayer needs Lua numbers that are bit-identical on every machine.
  LuaJIT's hardware doubles and JIT can't guarantee that.
- Lua is under half a percent of a battle tick today, so speed isn't the problem to solve.

**What would have to be true to reconsider:**

1. **Lua becomes a hotspot.** A measured profile with AI in every slot, or M3 galactic conquest
   and story scripts, shows script VM time above the EAWR-246 budget (p99 > 3 ms a tick, or a
   visible hitch where FoC has none), and that is still true after the soft-float fast paths and
   more partitioning.
2. **Determinism no longer needs our own number code.** The owner reopens EAWR-254 and ADR-010 to
   accept hardware doubles in authoritative scripts, or scripts leave authoritative state.
3. **Fidelity can be shown.** A LuaJIT fork plus source rewriter runs FoC's full script set and
   the major mods (EaWX, Republic at War, the Remake) with results identical to the 5.0.2 VM,
   including the EAWR-376 retail probe.
4. **Our runtime services exist for it.** The instruction budget, memory quota, canonical
   iteration and whole-state save are rebuilt and tested on LuaJIT.
