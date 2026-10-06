# V1 particle parent lifecycle

Continuous V1 trails follow PS-16 in the [particle walk](walks/particles.md).
At the beginning of an update, each dependent emitter captures its scheduling
clock and ages existing children once. Every surviving, draw-eligible parent
then visits that same captured clock along its own previous/current position
segment. Birth positions use scheduled event fractions and births receive their
residual age. Local detail must permit trails; masked parents emit no segments.

The V1 dependency chunk names birth and death child indices. A linked child has
one parent emitter. Invalid, duplicate and cyclic links fail parsing;
programmatic invalid links remain inactive. V2 parameters remain metadata only.
Each parent particle retains a stable numeric ID through packed compaction.
Bounded per-parent records retain association and lifecycle diagnostics; they
hold no independent emission clock. Existing trail children drain when their
parent retires. PS-22/PS-36 death bursts use the final retained parent position
once per retirement and obey death-spawn permission independently of draw masks.

Position and velocity sampling retain the reference port's local-frame policy.
Optional parent velocity inheritance remains the documented reference rule:
normalize the parent velocity and scale it by the lesser of scaled speed and
the authored limit. The V1 death creator disables this inheritance. PS-24 keeps
sampled velocity separate from birth-direction radial motion. Native dependent
velocity/alignment inheritance is still U03, without a new fidelity claim.
The MIT alo-viewer provenance is revision
`9bb0053919cc5df8377610d4f91b11d956d6c2f4`.

PS-19 admission uses independent per-emitter reserves within the caller's host
budget. Link bookkeeping is separately bounded at the smaller of the reserved
particle count and 65,536; this is a project safety policy, not a native emitter
allocation model. Each emitter segment visits at most 100,000 scheduled events
per update. Storage and scheduling scratch are reserved at construction, and
steady-state updates allocate nothing. Fixed local seeds and admitted steps
repeat IDs, positions, lifecycle counters and rendered stream hashes without
consuming authoritative simulation randomness.

EffectMode reports keep started/detached link counts, death bursts and refused
link counts for diagnostics. These counters do not imply native per-parent
emitter allocation.
