# Particle lifetime

| Rule | Behaviour | Evidence |
|---|---|---|
| PL-01 | Raw legacy decoding reads a scalar age sampler separately from lifetime and variation metadata. Ordinary group post-load allocation replaces its bounds with the sorted endpoints `max(0, A - A * variation)` and `A`. It preserves sampler mode and the saved constant default. Births therefore use that default in constant mode and the rebuilt range otherwise. Raw saved bounds alone do not describe the ordinary loaded range-mode effect. | Debug build: raw decode, group post-load allocation, scalar bounds setter and particle birth sampling. Authored damage smoke data; particle walk PS-47. |
| PL-02 | Independent continuous emitters allocate the ceiling of rate times maximum age, bounded to 5003 particles per emitter before parent or every-vertex multiplicity. Automatic production pools sum independent emitter bounds rather than applying that ceiling once to the whole system. | Debug build: emitter allocation uses the continuous rate and maximum age, clamps the result, then applies parent and vertex multiplicities. |

The hardpoint damage smoke effect authors lifetime metadata of 8 seconds and
variation of 0.82, with a raw saved age range of 1.44 to 14.56 seconds. Its
continuous rate is 5 particles per second, its particles use world space, and
emitter-motion inheritance is disabled. Ordinary post-load rebuilds this
range-mode sampler to 1.44 to 8 seconds, with a mean of 4.72 seconds. The
authored asset remains intact; the runtime copy receives the post-load range.

The decoder keeps raw sampler bounds and mode available for inspection. The CPU
runtime applies PL-01 to its private legacy definition before births. A true
constant sampler keeps its default; equal range-mode bounds are still rebuilt.

On an admitted first update, nonzero prewarming advances in 0.1-second steps
while accumulated single-precision elapsed time is at or below the target
([particle walk](walks/particles.md), PS-07). Equality can admit one more step;
float accumulation determines the actual duration. The first ordinary update
checks freeze before prewarming, and prewarm steps bypass that check. Later
ordinary updates admit equality and permanently freeze the emitter on the first
strict crossing, skipping the entire crossed update (PS-08). Frozen particles
retain their state and stop following their owner.

The CPU's 300-second prewarm work bound and its rebased shared presentation clock
remain project policies. Starts align by complete prewarm step counts; freeze
uses each emitter's independently accumulated elapsed time. Counts include the
ordinary caller update after prewarming.

Automatic unit-proxy and map attachment capacity uses the effective sampler's
larger bound for independent continuous shape or random mesh emitters. A positive
freeze time bounds ordinary accumulation, with an allowance for admitted initial
prewarm. Each emitter includes one birth plus one scheduling step of slack:
1/30 second for cold streams, 0.1 second for prewarmed streams. The independent
production ceiling remains 5003 per emitter before the system sum, and the caller
bounds that sum. Scheduling slack is a presentation runtime allowance, separate
from the original-game allocation rule.

The electrical stun stream remains 3500 births per second with a 0.5-second age
and no prewarm, retaining roughly 1750 live particles and its existing cold pool.

Burst, parent and every-vertex families retain the caller's fallback policy. In a
mixed system, one fallback reserve remains for those families alongside the derived
independent bounds; their multiplicities are not inferred. Unsupported or invalid
definitions retain the same reserve. Explicit attachment-capacity overrides and
aggregate map, impact and diagnostic host budgets remain authoritative. Particle
aging and random draws remain outside the authoritative simulation.
