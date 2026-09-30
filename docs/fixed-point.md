# Fixed-point arithmetic contract

Coordinator decision, 2026-09-21. This freezes P0-03's new-engine arithmetic
interface before operator implementation. It does not establish original-game
numeric equivalence. Range data is `plan/inventories/numeric-ranges.json`; ADR-010 records the decision.

## Representation and measured capacity

`Fixed` contains signed int64 raw storage with exactly **24 fractional bits**.
Its value is raw / 16,777,216, range `[-2^39, 2^39 - 2^-24]`, and quantum
`2^-24`. All raw bit patterns are valid. `from_raw` is explicit and infallible.
There are no implicit floating or integer conversions.

The structurally parsed 208 map sources have maximum observed candidate dimension
36,000; exact width/height field naming still lacks a controlled editor save-diff.
This is a measured capacity screen, not a claimed legal coordinate bound. The
3-component squared screen is 3,888,000,000, exceeding signed Q32.32 capacity.
Q24 provides roughly 141 times stored-result headroom over that screen, and
1,374 times over the broad 200,000 damage × 2,000 multiplier screen. These
cross-tag products are conservative capacity checks, not asserted gameplay rules.

The smallest screened factor 0.0003 rounds to raw 5,033; 0.0005 seconds rounds
to raw 8,389. Scalar input rounding error is at most 2^-25. Unknown overshoot,
contributors, lifetime, path length and repeated composition counts remain
explicit integration limits. The library checks all mathematical results;
representable inputs alone never imply representable outputs.

## Finite durations outside the common scalar range

The observed decimal `999999999999999.0` must remain finite. Asset records retain
the exact lexeme. A future duration consumer uses a distinct nonnegative fixed
duration with `uint64 whole_seconds` and `uint32 fraction_raw` constrained below
2^24. Its value is whole_seconds + fraction_raw / 2^24. Decimal conversion rounds
once to nearest-even at the common quantum, carrying into whole_seconds; range
errors are explicit. This representation covers the measured large duration
without sacrificing subsecond precision. Tick/deadline comparisons use widened
integer cross-products. It is not implicitly convertible to `Fixed`.

P0-03 supplies the common scalar/vector library; this extended duration interface
is reserved for the first actual duration consumer, whose arithmetic and hashing
must be specified before use. Until then XML preserves the lexeme and checked
Fixed conversion reports overflow. No consumer may replace an overflow with zero,
saturation or an unlimited state. Negative sentinel meanings require field-specific
evidence; unproved meanings remain unresolved. This explicitly refines ADR-003's
single-storage suggestion while preserving integer-only authoritative arithmetic.

## Exact arithmetic and errors

Use checked named operations returning the shared `core::Result<T>`. Comparisons
are total and infallible. Do not introduce throwing/fatal arithmetic operators.
Stable diagnostic categories distinguish overflow, division by zero, negative
square root, malformed decimal, zero normalization and undefined angle. Debug and
Release have identical semantics; failure produces no numeric replacement value.

- Add, subtract and negation are exact, failing if the result cannot fit raw int64.
- Multiply divides the exact raw product by 2^24 and rounds once.
- Divide rounds the exact rational raw_a × 2^24 / raw_b once; zero denominator fails.
- Integer and signed rational constructors check range and round once.
- Decimal grammar is `[+-]?(digits[.digits?]?|.digits)([eE][+-]?digits)?`, with ASCII
  digits and no whitespace, locale separators, NaN or Infinity. At least one digit
  is required. Parse exactly without binary float. Accept at most 4,096 characters;
  classify larger input with a resource-limit diagnostic. Arbitrarily large exponent
  text must not allocate proportional storage: reason about zeros, underflow and
  overflow before scaling. Signed zero canonicalizes to raw zero.
- Every scale reduction uses nearest, ties to even, symmetric for negatives.
  Thus ±0.5 raw quantum becomes zero, ±1.5 becomes ±2, and ±2.5 becomes ±2.
  Underflow to zero is a valid rounded result. Range is checked after rounding.
- Integer extraction is explicit truncation, floor, ceiling or nearest-even.
  Never negate INT64_MIN in signed storage or rely on signed overflow/right shift.

Dot/cross, quaternion products and affine rows sum exact widened products before
one final rounding per component. There is no intermediate-overflow error when
the exact cancelled final result is representable. Use a fixed **192-bit** signed
intermediate or an equally capable proven representation. Four products of two
INT64_MIN values sum to 2^128 and need 130 signed bits; a signed 128-bit accumulator
is insufficient. This is a bounded internal arithmetic facility, not a public
general multiprecision library. Use portable unsigned limbs or equivalent code
on every target; MSVC cannot be assumed to provide `__int128`.

## Geometry conventions

Vec2/Vec3 store x,y[,z]. Coordinates use a right-handed X-right, Y-forward, Z-up
frame; adapters preserve source coordinates until explicit presentation conversion.
Angles are **turns**, one full revolution = raw 2^24. Positive rotation follows
the right-hand rule. This is a new-engine convention, not an inferred asset convention.

Quat stores x,y,z,w with identity (0,0,0,1). Composition is the Hamilton product:
`compose(a,b)` applies b then a to column vectors. It does not implicitly normalize.
Mat3x4 stores three rows of four values, translation in the final column, acting
on homogeneous column vectors. Composition likewise applies its right operand
first. Transforming a vector excludes translation; transforming a point includes it.
Identity rotation plus translation (2,3,4) maps point (1,0,0) to (3,3,4), while
the same vector stays (1,0,0). Quat (0,0,1,0) rotates (1,0,0) to (-1,0,0).
Cross((1,0,0),(0,1,0)) = (0,0,1).

Length rounds sqrt(sum(raw_component^2)) directly into raw units; it must not
first store the squared sum in Fixed. Normalization divides by the unquantized
square root of that widened sum, avoiding a rounded-zero/intermediate length.
Zero vectors/quaternions fail normalization. Full-domain nonzero inputs must be
normalizable even if their standalone Fixed length would overflow. Quaternion to
matrix conversion requires a normalized quaternion: squared norm must differ
from one by at most eight quanta, otherwise return an explicit domain error.

## Square root and trigonometry

Scalar sqrt correctly rounds the exact nonnegative root to nearest-even. Negative
inputs fail. Sin/cos accept every raw turn value and reduce by exact integer modulo
one turn before approximation. Cardinal values are exact: sin(0)=0, sin(1/4)=1,
sin(1/2)=0, sin(3/4)=-1, with the analogous cosine values. Periodicity is exact.
Angle wrapping produces [-1/2,1/2), so +1/2 wraps to -1/2.

Atan2(y,x) produces turns in [-1/2,1/2). Axis results are exact; (0,0) is undefined
and fails. atan2(0,negative) = -1/2. Negative-zero ambiguity does not exist.
Sin/cos and atan2 error is at most four output quanta against the exact function
of already-quantized inputs. Normalized vector/quaternion components have the same
four-quantum absolute budget. An integer CORDIC or equivalent bounded algorithm
may use finer internal precision. Document fixed iteration bounds and commit
reproducible constants generated offline with a pinned high-precision tool;
builds never call platform libm to create tables.

Exact scalar/rescaled geometry arithmetic contributes at most half a quantum of
rounding error per returned scalar. These local budgets exclude input quantization
and do not promise arbitrary accumulated trajectory/transform accuracy. Near-threshold
original-game comparisons require separate behavioral evidence and test vectors.

## Required independent oracle and deterministic sequence

Before accepting the implementation, freeze a versioned original sequence manifest
under tests/math specifying generator, seed, operand derivation, operation distribution,
all output/error encodings, and the expected SHA-256 for exactly 1,000,000 operations.
Use SplitMix64 with a documented fixed seed and uint64 wrapping, not a standard-library
distribution. Exercise full raw bit patterns, injected extrema, zero, halfway,
underflow/overflow and cancellation cases. Do not discard difficult operands.

An independent Python integer/Fraction oracle produces exact expected scalar,
sqrt and widened-geometry outcomes without invoking production code. The million
sequence may focus on these exactly specified operations. Approximate transcendental
and normalization functions receive separate high-precision oracle vectors spanning
axes/quadrants, wrap endpoints, tiny/large ratios and full-domain normalization,
with the budgets above. Their produced raw results must also agree across targets.

Hash a specified fixed-width little-endian encoding of every successful result and
explicit error outcome; never a native struct or diagnostic prose. The five CI jobs
execute the sequence and upload digest/vector evidence; the dependent comparison
rejects missing jobs, missing outputs, oracle mismatches or cross-target differences.
An oracle is not merely a second spelling of the production rounding/limb algorithm.
