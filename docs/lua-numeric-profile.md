# Authoritative Lua numeric profile

Contract for EAWR-246,
2026-09-26. Owner decision EAWR-254
selected option B: authoritative Lua numbers are IEEE 754 binary64 values computed by
integer-only code. The policy is in [multiplayer readiness](multiplayer-readiness.md); this
note fixes the rules later code relies on. The [P0 host](lua-runtime.md) keeps its hardware
doubles, its binary32 boundary (L-15) and its fixtures unchanged.

## Effective precision of the original

The Steam FoC executables (retail `StarWarsG.exe` and the FoC debug build) are x64 programs.
Static reading of the debug build shows the Lua VM's arithmetic and comparison opcodes
compiled to SSE2 scalar double instructions without fused multiply-add; the game code
never writes the floating-point control state; and the Direct3D 9 device is created
without `D3DCREATE_FPU_PRESERVE`, which on x64 only concerns the unused x87 control word.
FoC scripts therefore compute in binary64, round to nearest, ties to even, with
subnormals. The CRT is a statically linked UCRT (VS2015 or later). The private evidence
map (LN-E1 to LN-E5) is under the ignored `out/research/lua-numeric/`. The running retail
game confirms it: see [Retail confirmation](#retail-confirmation-376).

## Numbers and arithmetic

`LuaNumber` (`include/eawr/script/numeric/lua_number.hpp`) is a trivial 64-bit word holding
the binary64 encoding; it is Lua's `lua_Number`, so compiled chunks keep their 8-byte
numbers. It has no conversion from or to a floating type: upstream code that tries one
does not compile.

- Add, subtract, multiply, divide and square root round once to nearest, ties to even,
  with subnormals, signed zero and infinities as in IEEE 754 (`binary64.hpp`).
- Every NaN result is the canonical quiet NaN `0xFFF8000000000000`, the x64 indefinite
  value FoC produces for `0/0`. Negation of a NaN also yields it, so state and hashes
  never see NaN payloads. Chunk constants pass through `from_binary64`, which
  canonicalizes a NaN constant.
- Comparisons are IEEE: NaN is unordered, `-0 == 0`. As in Lua 5.0, `-0` and `0` are one
  table key and a NaN key raises `table index is NaN`.
- `^` needs a global `__pow`; the profile provides none, so it raises
  ``` `__pow' (`^' operator) is not a function```, as in FoC.

## Integer conversions

C casts in the upstream VM and libraries (`luaL_checkint`, `string.format('%d')`,
`string.rep`, table indices) follow the x64 FoC compiler: a signed target truncates
toward zero to int32 and yields INT32_MIN for NaN or values outside int32 (so
`string.format('%d', 3e9)` is `-2147483648`); an unsigned target truncates to int64
(INT64_MIN for NaN or outside int64) and reduces modulo 2^N. Integer to number
conversion is exact up to 2^53 and rounds to nearest-even above.

## Text conversions

Parsing (`decimal::parse_prefix`, used by the lexer, string coercion and `tonumber`)
accepts the UCRT `strtod` grammar in ASCII without locale: optional whitespace and sign,
then decimal digits with an optional `e`/`E` exponent, a C99 hexadecimal float (`0x1.8p3`),
`inf`/`infinity` or `nan`/`nan(chars)`, case-insensitive. There is no Fortran `d`
exponent. The result is correctly rounded (exact big-integer comparison, 780 significant
digits plus a sticky digit); overflow gives an infinity, underflow a subnormal or zero.
The Lua lexer still only forms decimal numerals; `tonumber("0x10")` and `"0x10" + 0` give
16, as with UCRT.

Formatting (`decimal::format_to`) is C99 printf for `e E f g G` with flags, width and
precision, printing the exact binary value rounded once to the requested digit with
**ties to even** (exact ties such as `string.format('%.0f', 2.5)` give `2`). Exponents have
at least two digits. Infinities print `inf`/`-inf`, the canonical NaN `-nan(ind)`, other
NaNs `nan`/`-nan`/`nan(snan)`; `E`/`G` upper-case them. `tostring` and concatenation use
`%.14g`, as Lua 5.0. `tonumber(s, base)` for bases other than 10 uses a 32-bit
`unsigned long` (the Win64 width): it saturates at 4294967295 and negates modulo 2^32.
Character classes, `string.lower/upper`, the lexer and number coercion use the ASCII "C"
locale, and string ordering is bytewise.

## Library surface

The profile opens the P0 set: base (with coroutine), string and table. FoC exposes no
math library: `math.*` appears in FoC and Remake scripts only in comments, and PGBase's
`Dirty_Floor` truncates with `string.format("%d", x)` for that reason
([numeric inventory](../plan/inventories/lua-numeric.json)). The integer backend still
offers correctly rounded `square_root` and `round_to_integral` for future bindings.

## Q24 boundary

`to_fixed` (`q24_boundary.hpp`) converts the exact binary64 value to the nearest Q24 raw,
ties to even, and fails with `EAWR-SCRIPT-0101` for infinity or NaN and
`EAWR-SCRIPT-0102` outside `[-2^39, 2^39 - 2^-24]`; there is no clamp or replacement
value. `from_fixed` rounds raw × 2^-24 once to nearest-even (exact for |value| < 2^29).
Integer arguments use `to_exact_integer`, which accepts only finite integral values in
int64 and otherwise fails with `EAWR-SCRIPT-0101`, `-0102` or `-0103`.

## Backend interface and numeric ABI

`backend.hpp` defines the `LuaNumberBackend` concept: stateless static operations on a
64-bit representation (arithmetic, comparison, integer conversion, parse, format, chunk
constant import/export, Q24 import/export) plus a `NumericAbi`. `ActiveBackend` selects
the backend at compile time; `LuaNumber`'s operators and the Lua text hooks call only the
backend. A Q24 backend (option A) implements the same concept and must re-encode binary64
chunk constants through `from_binary64`. The current ABI is
`eawr-lua-binary64-soft` version 1; any change to a rule in this note needs a new version,
and the ABI belongs to session identity, saved-state headers and state hashes.

## Build and checks

`eawr::lua_502_sf` compiles the unmodified upstream Lua 5.0.2 sources as C++ inside
namespace `eawr::script::sflua` through one wrapper per source (`src/script/sflua/upstream`).
Configuration macros select `LuaNumber`, an integer allocation-alignment union and C
library replacements (`sflua_upstream.hpp`); the P0 C runtime is unaffected.

- `tools/check_lua_numeric_boundary.py` (CTest `lua_numeric_boundary_project`) parses every
  translation unit of this build, including the upstream sources it compiles, with Clang
  and rejects any floating type, floating literal or `<cmath>`/`<math.h>`.
- `tests/script/numeric` checks explicit edge vectors and SHA-256 digests of SplitMix64
  sequences (one million arithmetic operations; comparisons, conversions, Q24, parsing and
  formatting) against data generated offline from hardware binary64 by
  `tools/generate_lua_numeric_vectors.py`; the test itself uses only the integer backend.
  VM tests cover coercion, loops, keys and the library, and compare scripts against the P0
  hardware-double VM on the running target, including byte-identical compiled constants.
- Scripts computing on the soft-float VM give the same state on every target and worker
  count: `lua_persistence_roundtrip` and `lua_bridge_routing` pin final state hashes (the
  second over world and script state, with script numbers crossing into Q24 as orders), and
  their `workers` modes, like `lua_sandbox_workers`, require identical results on 1, 2, 4
  and 8 workers. CI runs them on all five targets.

## Load budget

`lua_numeric_bench` runs a synthetic per-tick workload shaped on the space-AI closure's
static operation mix (a plan step scores 20 units, does PGBase's `Simple_Mod`, and formats
one debug message), on the soft-float VM and on a benchmark-only hardware-double twin of
the same sources. On the rig (Core i7-4790K), 3,000 ticks:

| Plan steps per tick | Soft-float Lua p99 | Hardware Lua p99 | Soft-float share, p99 |
| --- | --- | --- | --- |
| 8 | 91 µs | 54 µs | 38 µs |
| 64 | 636 µs | 317 µs | 320 µs |
| 256 | 2.53 ms | 1.25 ms | 1.28 ms |

Soft-float roughly doubles Lua time for arithmetic-heavy script code; single operations
cost 6–26 ns (add 15, multiply 7, divide 26) against about 1–3 ns in hardware. The step is
far heavier in arithmetic than FoC's closure, which has 29 additions and subtractions and
5 multiplications or divisions in 33 files. The design's starting budget (soft-float p99
≤ 1 ms, total Lua p99 ≤ 3 ms per 30 Hz tick) therefore holds up to roughly 200 such steps
per tick. EAWR-79 must measure the real FoC AI load per tick against it; the first lever
beyond that is exact fast paths (integer-valued operands), not option A.

## Retail confirmation (EAWR-376)

On 2026-09-27 the retail Steam FoC `StarWarsG.exe` ran the probe
`tools/validation/p1_capture/lua_number_probe.lua` in a Naboo land skirmish on the rig (fog
off, `Invoke-FocMapCapture.ps1 -LuaNumberProbe`, maintainer capture notes).
The probe evaluates 26 fixed cases in GameScoring's Lua state, compares each result with this
profile's text using the game's own string comparison, and shows one line with the verdict;
the game showed `LNP1 OK n=26`: every case matched. The line, read off the screenshots, is
recorded in `tests/script/numeric/foc-retail-probe.txt`, and the CTest
`lua_numeric_foc_retail_probe` requires the soft-float VM to produce it byte for byte.

| Case | Expression | Retail FoC |
| --- | --- | --- |
| A, B | `string.format('%.0f', 2.5)`, `('%.0f', 3.5)` | `2`, `4` |
| C | `tostring(123456789012345)` (an exact decimal tie at 14 digits) | `1.2345678901234e+14` |
| D | `string.format('%.14g', 0.1 + 0.2)` | `0.3` |
| E | `tostring(1e15)` | `1e+15` |
| F | `tostring(-z)` with `z = 0` | `-0` |
| G | `string.format('%.17g', 2^53 + 1)` (computed as `9007199254740992 + 1`) | `9007199254740992` |
| H | `string.format('%.17g', 1/3)` | `0.33333333333333331` |
| I | `%.0f` of 0.5, 1.5, -2.5 | `0`, `2`, `-2` |
| J | `%.1f` of 0.25, `%.2f` of 1.125 | `0.2`, `1.12` |
| K | `string.format('%.20f', 0.1)` | `0.10000000000000000555` |
| L | `tostring` of 1/0, -1/0, 0/0 | `inf`, `-inf`, `-nan(ind)` |
| M | PGBase `Dirty_Floor` (`%d`) of 3e9, -2.5, 2147483647.9 | `-2147483648`, `-2`, `2147483647` |
| N | PGBase `Simple_Mod` of (7,3), (-7,3), (7.5,2), (-1,100) | `1`, `-1`, `1.5`, `-1` |
| O | `tonumber('0x10')` | `16` |
| P | `2 ^ 3` under `pcall` | error (no `__pow`) |
| Q | `tostring(5e-324)` | `4.9406564584125e-324` |
| R | `%e` of 12345.6789, `%g` of 1e-5 | `1.234568e+04`, `1e-05` |
| S | `tostring(123456789012345678)` | `1.2345678901235e+17` |
| T | `tostring(9007199254740993)` (literal) | `9.007199254741e+15` |
| U | `%.2f` of 2.675, `%.3f` of 1.0005 | `2.67`, `1.000` |
| V | `for i = 0, 1, 0.1`: count and last `%.17g` | `11`, `0.99999999999999989` |
| W | sum of 1/i for i = 1..100, `%.17g` | `5.1873775176396206` |
| X | `tostring` of 1e100, -1e-7 | `1e+100`, `-1e-07` |
| Y | `%.15g` of 0.1*3, `%.17g` of 1e23 | `0.3`, `9.9999999999999992e+22` |
| Z | `%x` of 255.9, `%05.1f` of 1.25 | `ff`, `001.2` |

What this settles: FoC scripts compute in binary64 with round-to-nearest-even (G, H, V, W, Y);
the CRT prints the exact binary value (K) and rounds exact decimal ties to even (A, C, I, J, Z),
so it is a UCRT from the Windows 10 2004 SDK or later, not a ties-away build; infinities and
the x64 default NaN print the UCRT spellings (L); integer casts saturate to INT32_MIN (M); the
lexer and `tonumber` parse like UCRT `strtod`, subnormals included (O, Q, T); and there is no
`^` operator (P). The numeric ABI `eawr-lua-binary64-soft` version 1 holds; no change.

## Open items

- Negating the canonical NaN prints `-nan(ind)`, where FoC prints `nan`. The EAWR-376 probe did
  not cover `-(0/0)`; a probe version 2 can record it with other NaN sign cases.
- Table iteration order, `tostring` of tables and functions, GC metrics and the sandbox
  are in [Lua sandbox](lua-sandbox.md) (EAWR-247).
