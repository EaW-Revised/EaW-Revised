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
immutable definitions, mesh data and allocator overhead. This also excludes the
per-instance every-vertex `mesh_vertices_` location array and `vertex_order_`
scratch array (16 and 4 payload bytes per vertex respectively on the validated
64-bit Windows build), not just the shared mesh geometry. Render streams reserve
at most four vertices and six indices per admitted particle, plus one pointer
when sorting is requested. Backend buffers/textures are additional and retain
existing backend allocation policy. No native scene-wide memory bound is claimed.

The synthetic seven-emitter explosion still requests 626 scheduling slots.
Contracts cover mixed dense/sparse roots, isolated bursts, parent/every-vertex
multiplication, host-budget partitioning, retirement and allocation-free dependent
updates. Caller sizing helpers remain conservative automatic budget recommendations;
they no longer determine which emitter may consume another emitter's space.


## Effect admission and reusable ownership (legacy EAWR-4722)

Battle and breakoff effects use a checked authored capacity sum rather than the
former 4096-particle system budget or 256-live-effect gate. Native and scheduling
requests remain separate per emitter; the sourced 5003 base still precedes parent
and actual bound-vertex multiplication. Unsupported emitters, cycles, missing mesh
bindings and arithmetic overflow refuse the complete effect with a diagnostic.
Explicit-budget registry consumers retain their existing policy.
Valid bursting emitters whose authored batch truncates to zero keep zero reserve
and are admitted, including alongside nonzero siblings.

This admission policy is a project choice. It is not a claimed retail scene-wide
limit or retail memory-failure rule. Immutable shared definition identity selects
compatible CPU layouts. Release returns backend objects through the existing backend
cache and retains reusable CPU payload in stable release order. Each admission uses
a new monotonically increasing handle generation; old handles never address reused
storage. Reuse resets particle IDs, random seed (including zero), frame, scheduling,
dynamic settings, visibility, brightness and drain state. Binding-specific mesh
instances and mutated shape definitions are discarded rather than reused.
Partitioned updates and ordered backend publication are unchanged.
The developer stream-bucketing switch applies when a layout is constructed;
live and idle layouts retain that setting until evicted. Bindings passed to
definitions without mesh emitters are cleared before admission and reuse.

Reservations cover the measured reserved particle/free-slot/event/child payload
and reserved render-stream/sort/bucket payload; immutable definitions, allocator
metadata and backend allocations are outside that CPU payload figure. Growth reserves
its checked planned payload before construction and reconciles actual vector capacity
before publication. Pressure evicts oldest idle CPU layouts first when reclaiming
idle payload can make the request fit; impossible requests preserve idle layouts.
The same rule applies to reconciliation of extra actual payload. Admitted live and
draining visuals are preserved. Backend or CPU allocation failure rolls back all new
ownership; it is distinct from budget pressure and definition/arithmetic refusal.
No active effect is stolen and no emitter reserve is silently clipped into success.

The shared presentation budget interface can accept a cached platform provider.
Its default implementation accounts CPU payload without inventing a process-memory
limit. Production currently never sets an admission budget: it uses accounting only,
so pressure refusal and pressure-driven eviction are exercised by injected contract
budgets rather than normal game admission. Device/process probes and GPU reservations are explicitly unavailable until
the renderer owner supplies them; unavailable GPU bytes are reported as null.
No RAM/VRAM fit, capture, performance or 4GB-hardware acceptance claim follows.
The model-projectile admission ceiling is outside this change.

Registry reports count requests, admissions, refusals by reason, CPU allocations,
reuse/eviction, live/idle payload and high water, plus per-live-emitter native,
scheduling, reserved, live and dropped particles. Per-layout retirement totals
survive teardown and idle eviction. Battle refusal samples record
the event birth and lifetime once, with reason and byte demand; samples have the
existing spawn-log diagnostic bound and an explicit full flag. Current wanted
ownership includes admitted draining records; exact hypothetical drain duration
for a refused effect is not reconstructed from cumulative refusal counts.
Registry refusal diagnostic text is sampled once per reason, while counters count
every refusal. `effects_dropped` counts refused requests across budget, allocation,
definition and arithmetic causes; it is not a budget-only counter. Zero-valued
name-map counters are omitted. A refused contact show-reset keeps the stopped group
hidden and retries on a later visible sample without replacing its old handle.
