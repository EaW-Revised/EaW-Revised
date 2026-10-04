# Particle burst capacity

The [particle walk](walks/particles.md), PS-18, sources independent burst
allocation from the debug build. For maximum age `A`, count `B` and interval
`I`, the native base is `ceil(A * B / I)` when `I <= A`, otherwise `B`.
Each emitter clamps its base to 5003 before dependent or every-vertex
multiplication. PS-19 confirms independent free slots: an exhausted emitter
stops admitting new births and does not recycle its oldest particle.

EAWR's automatic production bound now includes independent burst shape and
random mesh emitters alongside continuous emitters. A burst reserves
`trunc(B) * ceil((D + S) / I)`, capped at 5003 before summing emitters.
`D` is the effective maximum sampled age, bounded by freeze with admitted
prewarm as described in [particle lifetime](particle-lifetime.md).
`S` is one ordinary 1/30-second or prewarm 0.1-second scheduling step.
Whole-batch rounding and this scheduling allowance are deliberate project
policies for the current presentation scheduler, rather than native formulas.
They keep a boundary event from borrowing another independent emitter's space.

Each CPU system partitions its caller's explicit aggregate host budget at
construction into independent emitter reserves. The first pass gives each valid
emitter up to an equal share; the second distributes unused space in authored
order. Admission never borrows another emitter's unused slots. A budget smaller
than the number of emitters can leave some reserves empty; this is an explicit
host safety choice, not evidence for a native global ceiling (PS-20/U05).

The capacity plan exposes `native_requested`, `requested` including scheduling
allowance, and `reserved` after host budgeting for every emitter. Native base
uses authored maximum age and the continuous event interval or burst interval,
including the long-interval event-count branch. Base is clamped to 5003 before
parent and bound every-vertex multiplication. The scheduling allowance described
above follows the same multiplication order. Forward links resolve parent-first;
invalid links and cycles reserve nothing. Multiplication saturates before host
budgeting, without integer wraparound. Constant sampled ages longer than metadata
can increase the scheduling reserve; native requested remains metadata-based.

Stable draw slots and the PS-35 affine mask use each admitted emitter reserve
as denominator. Retirement returns that emitter's slot without replacing its
oldest live particle. Requested/spawned/dropped counters expose exhaustion.
Particle, free-slot, event and child-instance storage is reserved at construction;
steady root/trail/death updates do not allocate. Stable sorted particle IDs permit
parent lookup without rebuilding allocating hash tables each update. The retained
65536 child-instance limit is a separate project work budget, not a native ceiling.

For caller budget `H`, particle storage and free slots each have at most `H`
elements, event scratch at most `2H`, and child links at most `min(H,65536)`.
`reserved_memory_bytes()` reports their actual reserved payload bytes, excluding
immutable definitions, mesh data and allocator overhead. Render streams reserve
at most four vertices and six indices per admitted particle, plus one pointer
when sorting is requested. Backend buffers/textures are additional and retain
existing backend allocation policy. No native scene-wide memory bound is claimed.

The synthetic seven-emitter explosion still requests 626 scheduling slots.
Contracts cover mixed dense/sparse roots, isolated bursts, parent/every-vertex
multiplication, host-budget partitioning, retirement and allocation-free dependent
updates. Caller sizing helpers remain conservative automatic budget recommendations;
they no longer determine which emitter may consume another emitter's space.
