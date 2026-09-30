# V1 particle parent lifecycle

This describes the presentation CPU behavior for legacy V1 particle creator IDs `39`
(`EnhancedTrail`) and `40` (`Death`). The source model is the pinned MIT alo-viewer
revision `9bb0053919cc5df8377610d4f91b11d956d6c2f4`, especially
`DirectX9/ParticleEmitterInstance.cpp`, `Particles/CreatorPlugins.cpp`, and
`Particles/ParticleSystem.cpp` under `src/RenderEngine/`.

The V1 `0x36` dependency chunk names birth (`0x39`) and death (`0x37`) child emitter
indices. A linked child has exactly one parent. Invalid, duplicate, and cyclic links
fail parsing; programmatic definitions with invalid links remain inactive. V2 plugin
parameters remain metadata only.

Each spawned parent particle has a stable numeric ID. A birth link starts one child
emitter instance per parent particle, with its own next-spawn time and parent ID. The
instance samples its live parent's position and velocity when it emits. A child
particle begins at the sampled local position plus the parent particle's world
position. Its local velocity is transformed by the emitter basis; if V1 property
`0x43` enables link strength, the parent velocity contribution is the reference rule
`normalize(parent.velocity) * min(length(parent.velocity) * strength, maximum)`.
Legacy conversion uses property `0x28` as strength and the reference's unlimited
maximum for birth children. The V1 Death creator explicitly disables velocity
inheritance, even when `0x43` enables a nonzero `0x28` link strength. Inward speed
uses the emitter/system transform translation as its direction origin, including
for attached children whose local spawn offset is zero. The reference's optional
position and velocity alignment modes are V2 parameters and are outside this V1
behavior.

On parent death, attached child instances detach and stop future emission. Their
already emitted particles remain live until their own killer removes them. A death
link emits one burst at the dying parent's last position, once per death. Its V1
conversion uses the reference Death creator's forced burst timing: one batch,
start delay zero, and the child emitter's particle count per interval. Age and
terrain killing follow the CPU runtime's existing presentation-clock checks.

The global particle capacity bounds all roots, trails, and bursts. Active child
instances are also capped at the smaller of the particle capacity and 65,536.
The runtime counts dropped particles and refused instances. Root and child
schedulers each use a 100,000 event guard per advance. Particle IDs remain stable
when packed storage moves particles; a fixed seed and fixed step reproduce the same IDs,
positions, lifecycle counters, and rendered stream hashes. A surviving attached
parent retains the same ID and child association when an earlier particle dies
and packed storage moves that parent to a new index.

The EffectMode report records started and detached child instances, death bursts, and instance-capacity drops.
