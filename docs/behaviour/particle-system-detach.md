# Particle system detach and drain

This describes how a presentation particle-system instance ends: the V1 system
flag `leave_particles`, what detaching does to root and child emitters, when a
draining instance counts as finished, and who releases it. The source model is
the pinned MIT alo-viewer revision `9bb0053919cc5df8377610d4f91b11d956d6c2f4`
under `src/RenderEngine/`: `DirectX9/ParticleSystemInstance.cpp` (`Detach`,
`Update`), `DirectX9/ParticleEmitterInstance.cpp` (`Detach`,
`CheckDestruction`, `UpdateParticle`), `DirectX9/RenderObject.cpp`
(`KillProxy`, `SpawnProxy`) and `Particles/ParticleSystem.cpp` (the root `0x0002`
chunk). The companion parent/child rules are in
[particle-parent-lifecycle.md](particle-parent-lifecycle.md).

## Source rules

| Rule | Behaviour |
|---|---|
| D-01 | The V1 `0x0900` root may end with a one-byte `0x0002` chunk after the emitters. A nonzero byte sets `leave_particles`; an absent chunk leaves the default, which is set. |
| D-02 | An owner ends a proxy effect by detaching the instance and dropping its reference (`KillProxy`); replacing a proxy detaches the previous instance the same way (`SpawnProxy`). |
| D-03 | With `leave_particles` clear, detach drops the instance's self-reference, so the owner's release destroys it at once: every emitter and every live particle goes with it. No death event runs. |
| D-04 | With `leave_particles` set, detach visits only root emitter instances (those without a parent particle). Each one is marked detached and its next spawn time is cleared, so it never emits again, including a batch still waiting on its start delay. |
| D-05 | Detach kills nothing. Live particles keep updating: modifiers, velocity, acceleration and the translater still run, and the killer still retires them on its own terms. |
| D-06 | Child emitter instances attached to a live parent particle are not roots, so detach leaves them spawning. When their parent dies they are detached as usual and a death link still spawns its burst once. |
| D-07 | A detached emitter instance with no scheduled emission and no live particle deletes itself; when the last emitter instance is gone the system instance drops its self-reference. |

## EAWR contract

`CpuSystem::detach()` implements D-04 to D-06 for the CPU port. It sets one
flag that stops the root scheduler. It does not touch emitter state, particle
storage, IDs, the random stream or child instances, so repeating it changes
nothing. `CpuSystem::finished()` holds only when the system is detached, no
particle is alive and no child emitter instance remains. An undetached system
is never finished, even when it is empty and waiting on a delayed or periodic
root batch.

`EffectRegistry::detach(handle)` returns `core::Result<EffectDetachState>`:

- `leave_particles` clear: the instance is released through the existing
  `release` path (every backend resource destroyed once) and the result is
  `released`. The handle is dead afterwards.
- `leave_particles` set: the CPU system is detached and the instance, its plans
  and its backend resources stay. The result is `draining`. Later advances keep
  uploading residual streams. `EffectFrameStats::detached` and
  `EffectFrameStats::finished` report the state after each advance.
- A draining instance detached again returns `draining` and is not reset.
- An unknown or released handle gets the existing
  `EAWR-PARTICLE-0006` unknown-effect diagnostic, as `release` does. Handles are
  never reused.

The owner releases a finished draining instance explicitly with `release`,
which stays unconditional and can also cut a drain short. The registry does
not release anything by itself. This replaces D-07's reference-counted
self-release with an explicit owner step, so no resource is freed behind a
handle the owner still holds.

## EffectMode demonstration

`--eawr-effect-detach-frame <uint32>` schedules one detach. The value is a
zero-based advance index and must be below `--eawr-effect-frames`. Index 0
detaches just before the zero-delta first advance; index `n` detaches just
before advance `n`. The dry run, the replay validation and the graphical run
apply the same schedule. Without the option the mode behaves exactly as
before. After a `released` result, every later frame skips its advance. After
a drain finishes, the mode releases the instance once and skips later
advances. The end-of-run release only runs if the instance is still live.

With a schedule the camera frames the union of every drawn frame's bounds, so
all captures share one camera. The mode captures three frames:

- `pre-detach`: the advance before the detach.
- One post-detach frame: `draining` is midway through the drawn drain;
  `released` is the frame the immediate release emptied.
- The final frame.

A frame with drawn bounds is checked against its projected bounds. An empty
frame is accepted only after a successful detach, and then its decoded capture
must contain no drawn sample at all (`empty_after_detach`). A scheduled run
that never drew a particle fails. The report adds `run.detach_frame`,
per-frame `index`, `advanced`, `detached`, `finished`, `released` and
`resources`, a `detach` block (requested frame, `leave_particles`, result,
replay result, release frame and cause, capture evidence) and
`lifecycle.released_by`.

## Divergence and gates

| Gate | Observation | Effect here |
|---|---|---|
| G-01 | In the reference, a death-link emitter instance is attached to its dying parent after that parent's attached list was detached. It is never marked detached, so D-07 never deletes it and a system that fired a death burst never drops its self-reference. | EAWR treats an exhausted burst as finished work. A drained system finishes when its particles and child instances are gone. Whether the original game keeps such instances is unverified. |
| G-02 | The original game's call sites for detach (unit death, proxy replacement, hardpoint loss) and the owner's release timing were not observed. | The registry primitive, the EffectMode schedule and a Player-visibility owner (`AttachmentLifecycle`, [particle-attachment-visibility.md](particle-attachment-visibility.md)) are implemented. EffectMode calls it, and MapMode does under the opt-in `--eawr-map-effect-animation idle` for admitted land attachments whose placement has a bound idle clip. There is no death, hardpoint or proxy-replacement wiring: no such event reaches the viewer. |
| G-03 | No original capture was made. | Visual equivalence of the drain with the original game is not claimed. |
