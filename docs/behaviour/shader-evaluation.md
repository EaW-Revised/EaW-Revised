# Numeric shader updates

S-21 is project policy for consuming S-13..20 metadata. It establishes no new
claim about original expression arithmetic or renderer fidelity. Activation stays
off; the activation owner must still qualify resources, state representation,
cleanup and fallback reporting.

Prepare numeric plans outside drawing from the immutable effect, selected stage
and normalized P4 assignments. Retain only referenced numeric alias chains and
their terminal defaults. Only nonempty P1 preshader descriptors are expressions;
tag-only markers are not executable expressions. Unreferenced expression objects
do not prevent preparation. Referenced expression arithmetic and dynamic shader
expressions currently refuse with logical source and byte location. An empty
stage constant table never makes its preshader optional. The activation owner
must route each refusal through the existing explicit fallback ladder.

EFX-11 establishes the CLIT/FXLC section envelopes and constant inputs; it does
not establish literal encoding, instruction/operand/register-bank semantics,
precision/conversion rules or preshader output mapping. The retained independent
native metadata probes and debug-build research supply no qualified execution
contract for those bodies. Accordingly every nonempty preshader on the supplied
stage refuses unconditionally, and reached parameter/state/shader expressions
refuse; unrelated effect expression descriptors are not visited. Column-matrix
effect default storage is also unestablished and refuses. Engine-provided matrix
values still follow the reflected register orientation. These are explicit
capability limits, never permission to substitute an expression's default.

Each draw supplies typed logical numeric words for P3 engine identities and
parameter overrides. Time comes exclusively from the caller. Team colour is a
material parameter override, with no effect-name heuristic. Fade/environment
use the existing P3 input identities. Re-resolve root and terminal parameter
overrides on every update; first supplied alias identity wins. Missing parameter
overrides use the prepared default, while missing required engine inputs refuse.
State-only parameters and terminal aliases resolve semantics through the same P3
policy as stage constants, even without a matching stage CTAB entry. An engine
identity at a reached parameter requires the current engine input and never uses
that parameter's stored default. A preceding material override can replace the
remaining alias chain. Skin-palette aliases refuse pending input-policy
qualification, so they cannot bypass S-16's pre-skinned input checks.
Duplicates, kind/shape mismatch, noncanonical booleans and nonfinite floating
inputs refuse rather than silently converting or retaining an earlier draw.

Numeric words use logical row order. P3 register shape determines row/column
packing, array order and zero padding, with no colour conversion. Each ordered
P4 state assignment is resolved from the same current input snapshot and checked
by the existing S-19 domain converter. The resulting ordered literal list is an
exact state/variant predicate sequence; backend variant admission remains an
independent prerequisite. It does not authorize compiling a new variant during
a draw or omitting unsupported states. Duplicate state assignments retain order.

The caller allocates output storage once. Successful updates allocate no storage,
visit bounded inputs/links/words, and expose deterministic work counters. A failed
update invalidates the entire output, including any partially written numeric
words, so callers may upload only a successful valid result. Plan fields must
remain immutable after preparation. Resource updates remain with P3/activation.
