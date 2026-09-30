# Particle attachment visibility

This describes how a particle system attached to an animated host bone reacts
to that bone's visibility. It covers when the attached instance is detached,
what happens when the bone reappears, and who releases what. The source model
is the pinned MIT alo-viewer revision
`9bb0053919cc5df8377610d4f91b11d956d6c2f4`:

- `src/RenderEngine/DirectX9/RenderObject.cpp`: `Update` (line 157),
  `SpawnProxy` (129), `KillProxy` (148), `ShowProxy` (265), `CheckAltLod`
  (305) and `ResetAnimation` (50).
- `src/RenderEngine/DirectX9/ParticleSystemInstance.cpp`: `Update` (line 7)
  and `Detach` (40).
- `src/Assets/Animations.cpp`: `IsVisible` (362), `GetVisibleEvent` (369) and
  `GetInvisibleEvent` (393).
- `src/main.cpp`: the frame loop calls `SetAnimationTime` and then
  `RenderObject::Update`.

What detach itself does (stop the roots and drain, or release at once) is in
[particle-system-detach.md](particle-system-detach.md).

## Source rules

| Rule | Behaviour |
|---|---|
| V-01 | A bone's visibility at time `t` is the stored bit of animation frame `floor(t * fps) mod frames`. There is no interpolation and no parent inheritance: a proxy reads only its own bone. |
| V-02 | Each update first advances every live proxy instance at the new animation time. It then checks every proxy slot against the window `[previous time, current time]`. |
| V-03 | A slot with an instance whose bone has an invisible frame in the window is killed. `KillProxy` detaches the instance and drops the slot's reference, so it runs once per hide: the slot is empty afterwards and no longer consulted for kills. |
| V-04 | A slot with no instance, marked visible by its static flag and ALT/LOD selection, spawns a new instance when its bone has a visible frame in the window. The spawn is back-dated to that frame's time. |
| V-05 | The killed instance is not reused. If `leave_particles` is set it keeps its self-reference and drains, still linked to the host object. A reappearance therefore runs a fresh instance next to the old drain. |
| V-06 | A draining instance still reads its host bone's transform on every update, whether or not the bone is visible. |
| V-07 | When an object is created, a proxy is spawned only if its static flag, its ALT/LOD selection and its bone's bind visibility all allow it. ALT/LOD changes show or hide proxies outside the animation path. |

The reappearance rule is therefore resolved by the reference (V-04, V-05): a
bone that becomes visible again gets a new instance, and the previous
instance's drain continues beside it. This is the reference implementation's
rule. Whether the shipped game behaves the same way was not observed (gate
A-04).

## EAWR contract

`particles::AttachmentLifecycle`
(`include/eawr/presentation/particles/attachment_lifecycle.hpp`) is
header-only. It composes the public `EffectRegistry` API: `spawn`,
`set_frame`, `set_mesh_frame`, `advance`, `detach` and `release`. It adds no
source file to any build and changes no registry, CPU or Player behaviour.

The owner calls `step(visible, frame, mesh, delta, camera)` once per
presentation sample. `visible` is the host bone's
`animation::Pose::bones[i].visible` from `Player::sample`, which applies V-01.
Each step does the following, in order:

1. **Visibility edge.** At most one of these happens:
   - Active generation and hidden: `EffectRegistry::detach` is called once.
     `released` drops the generation. `draining` moves it to the drain list.
     The slot is then empty, so later hidden samples do nothing (V-03).
   - No active generation, visible, and either no generation has started yet
     or the policy is `respawn`: one generation is spawned (V-04, V-05).
2. **Host frames.** The sample's emitter frame (and mesh frame, for a proxy
   mesh binding) goes to the active generation and to every drain (V-06).
3. **Advance.** Every live instance advances once. A generation's first
   advance is zero-delta, the same as the owner's first frame.
4. **Owner release.** A drain that reports `finished` is released exactly
   once, by this owner.

Other properties:

- **Seeds.** Generation 0 uses the owner's seed unchanged. Later generations
  use `generation_seed(seed, n)`, which is distinct, nonzero and
  deterministic. A host that never hides therefore reproduces the unmanaged
  stream bit for bit.
- **Merged stats.** `AttachmentStep::stats` merges every advanced instance in
  handle order. One instance passes through unchanged. Several instances add
  their counts, take the union of their bounds and chain their hashes with
  FNV. When no instance is live, the stats are empty with hash 0.
- **Bound.** At most `max_draining` drains are kept (default 8). A detach
  that would exceed the bound first releases the oldest drain early
  (`drains_cut_short`). The reference has no such bound (divergence A-05).
- **Release.** `release_all()` releases the active generation and every
  drain. The helper is not RAII: its owner calls `release_all()` before the
  registry goes away.
- **Policies.** `ReappearancePolicy::respawn` is the reference rule.
  `ReappearancePolicy::stay_detached` never starts a second generation. It is
  an explicit opt-in for owners that must not stack instances.
  `ReappearancePolicy::reset` is FoC's group visibility reset
  (FoC debug build, BP-48): on reappearance the group is reset (it
  clears every particle, the burst count and the internal clock), so the
  drains are released (`drains_reset`) as the new generation starts. The
  death clones use it (battle-presentation BP-48, EAWR-429). The first
  generation still waits for the first visible sample under every policy.

## EffectMode demonstration

`--eawr-effect-visibility <respawn|stay-detached>` turns the lifecycle on for
an `--eawr-effect-attach` or proxy-mesh host:

- The attach path reads the named bone.
- The proxy path reads `ProxyMeshBinding::proxy_bone`.

Without the option the mode is unchanged: the host's visibility is ignored,
as before, and the report's `visibility.requested` is false. The option is
rejected with a `give_up` report if:

- its value is not `respawn` or `stay-detached`, including a trailing flag
  with no value;
- it is combined with `--eawr-effect-detach-frame`;
- there is no attachment or proxy host.

The dry run, the replay validation and the graphical run each own one
lifecycle over their own registry, and they walk it identically. Each run
works as follows:

- **Camera.** It frames the union of every drawn frame.
- **Replay check.** The graphical per-frame events must equal the replay's.
- **Captures.** Intermediate captures are planned from the replay:
  - `pre-hide`: the last drawn frame before the first hide.
  - `hidden-draining`: midway through a drawn drain.
  - `hidden-released`: the frame an immediate release emptied.
  - `reappeared`: midway through the first reappeared span.
- **Empty frames.** An empty frame is accepted only when the host is hidden
  or has no live instance. Its decoded capture must then contain no drawn
  sample at all (`empty_after_detach`).
- **Report.** A `visibility` block records:
  - policy, bone and bone index, `leave_particles`
  - `generations`, `detaches`, `drains_released`, `drains_cut_short`,
    `released_at_end`, `peak_live_rids` and `replay_matches`
  - per-frame `visible`, `spawned`, `detached`, `drains_released`,
    `live_instances` and `active`
  - its captures
- **End of run.** Whatever is still live is released at the end
  (`lifecycle.released_by` is `end_of_run`). If nothing is live, it is
  `visibility`.

## MapMode idle-clip owner

`--eawr-map-effect-animation <none|idle>` (default `none`) is the MapMode
owner of V-02 to V-06 for land maps. With `none`, MapMode samples only the
bind pose, as before: a static bind pose has no visibility edge, so nothing
detaches, and the report is unchanged byte for byte apart from timing.

With `idle`:

- **Clip.** Each placement that has an admitted attachment, or a proxy whose
  bone is hidden in bind pose, is checked for the idle clip the scene builder
  recorded (`scene::Placement::idle_animation`). A clip is bound only when
  `idle_animation_status` is `corpus_naming_observed`: one `Player` per
  placement, created with `assets::load_animation` and
  `Player::create(model, &clip)`. Otherwise the placement keeps the static
  path and the report names `clip_absent`, `clip_decode_failed`,
  `clip_binding_failed` or `clip_rate_unsupported` (a frame rate that is not
  integral, which the exact clock below cannot sample). The clip choice is an
  EAWR observation of the corpus naming, not a source rule; A-04 stays open.
- **Admission.** Bind-pose admission (`plan_map_effects`) is unchanged. A
  bind-hidden proxy stays unadmitted and gets no capacity. When its bone is
  bind-hidden and the clip shows it, the report counts
  `unadmitted_visible_edges`; nothing spawns (A-03).
- **Owner.** Every admitted record of a clip-bound placement gets a
  `particles::MapAttachmentOwner`
  (`include/eawr/presentation/particles/map_attachment_owner.hpp`, header-only).
  It drives one `AttachmentLifecycle` with `respawn` and one drain
  (`map_owner_max_draining`). Generation 0 waits for the first visible sample.
  Before that, at prepare time, the owner's system is spawned once into a
  throwaway registry whose backend creates nothing but accepts exactly what
  the Godot backend accepts on drawability, material validation and texture
  resolution. A non-drawable emitter therefore fails the record even if the
  clip never shows the bone. Shader compilation and texture upload are only
  exercised by the first real spawn.
- **Clock.** MapMode's particle clock is the sample index `n` (0 to
  `--eawr-map-particle-frames` − 1). The pose is sampled once per placement
  per sample at `n/30 s`, in `loop` mode, through `Player::sample_tick`
  (`map_owner_sample`): the frame position `n × fps / 30` is reduced modulo
  the playable frames in 64-bit integers, so the frame index is exact and the
  interpolation fraction is `(n × fps mod 30) / 30`. A binary32 time
  wrapped by `fmod` is not exact when the clip duration is not representable
  (7 frames at 15 fps read 8 of 600 samples one frame early); `sample_tick`
  exists for this and `Player::sample` is unchanged for its other consumers.
  It requires an integral frame rate. EffectMode keeps its binary32
  `n × (1/30 s)` expression. The advance delta is 0 for `n = 0` and 1/30 s
  after, as for the static path.
- **Frame.** The host frame is the placement's Q24 transform composed with
  the proxy bone's sampled Q24 model frame (`fixed_model_frame`, the
  conversion bind-pose admission uses), converted by `source_emitter_frame`.
  A sample whose pose equals the bind pose reproduces the admission frame bit
  for bit. Drains keep reading the bone (V-06).
- **Mesh.** The map mesh still draws the bind pose. Emitters follow the
  sampled bone, so the report says `mesh_pose: "bind"`. No visual alignment
  of the mesh with its effects is claimed, and no animated mesh pose exists.
- **Capacity.** A drain is a full instance, so each owner reserves one more
  instance of its capacity (`drain_headroom`). If the admitted allocation plus
  the headroom exceeds the attached budget, the run fails closed with
  `drain_headroom_exhausted` before any owner exists. Ordinary map particle
  placements get what the allocation and the headroom leave. Every sample
  checks that live attached capacity stays within allocation plus headroom.
- **Gates.** The particle clock runs while any static instance is live or any
  owner is unreleased (`has_work`), so an owner hidden at its first sample
  still advances. The effects-off comparison capture is taken when anything
  is drawn (`has_live`), and always when owners exist, so a final phase in
  which every owner is empty is still compared.
- **Empty at capture.** An owner that drew no bounds at capture is accepted
  empty when its last sample held no particle: `hidden_by_clip` (no
  generation or drain live) or `live_no_particles` (a live generation or
  drain before its first particle, after a start delay or late reappearance,
  between bursts, or after its last particle died). It projects no region, so
  any pixel it changed counts as outside every bound. An owner with particles
  but no bounds (`live_undrawn`) fails. The pixel check still fails closed
  when no placement was either verified in pixels or accounted empty; the
  report counts `owners_empty_at_capture`.
- **Release.** `release()` releases every owner (active generation and drain)
  before any static handle, and before the registry and backend are destroyed.
- **Failures.** A pose that does not sample, a frame that is not
  representable in Q24, a composition overflow, a non-drawable emitter at
  prepare or at spawn, or a lifecycle error fails that record with its identity and the
  sample number. MapMode then releases everything and reports failure.
- **Refused.** `idle` is refused with fog (fog evidence binds one resource per
  emitter, not per generation), without `--eawr-populate`, with
  `--eawr-map-effects off` (nothing would be spawned or verified), and on
  space maps. An unknown value or a missing value is refused.
- **Report.** `map_attached_effects` gains `effect_animation`, the clock,
  `attached_budget`, `drain_headroom`, `live_capacity_limit`, `policy`,
  `max_draining`, `mesh_pose`, `animation_failure` and
  `owners_empty_at_capture`. Each record gains an
  `owner` block: `kind` (`idle_clip`, `static` or `none`), the clip path,
  status and provenance, `clip_status`, and for owners `bone`, `samples`,
  `generations`, `detaches`, `drains_released`, `drains_cut_short`,
  `peak_live_instances`, `released` and the events (sample, visibility,
  spawned generation, detach state, drains released or cut short, live
  instances). These keys exist only under `idle`.

## Divergence and gates

| Gate | Observation | Effect here |
|---|---|---|
| A-01 | The reference scans every animation frame in the update window (V-03, V-04). | EAWR reads the visibility of each presentation sample from `Player::sample` (EffectMode) or `Player::sample_tick` (MapMode, exact frame index). A hidden interval shorter than one sample step can be missed, and so can a visible one. Samples at 1/30 s over clips at 30 fps or slower cover every clip frame; on EffectMode's binary32 clock a loop whose duration is not exact can read a sample one frame early. |
| A-02 | The reference advances an instance before it kills it, and back-dates a new spawn to the visible frame (V-02, V-04). | EAWR detaches before the advance, the same order as `--eawr-effect-detach-frame`, and starts a new generation with a zero-delta advance at the sample time. Timing differs by at most one sample. |
| A-03 | Spawning also depends on the proxy's static visibility flag and its ALT/LOD selection (V-04, V-07). `ResetAnimation` kills proxies whose bind visibility is hidden. | Not modelled in the helper or in EffectMode. MapMode's admission already classifies hidden proxies; any ALT/LOD selection belongs to its owner. |
| A-04 | The shipped game's proxy-visibility behaviour and reappearance rule were not observed. | Only the reference rule is implemented. No original-game fidelity is claimed. |
| A-05 | The reference keeps any number of drains alive. | EAWR bounds drains per attachment (default 8). A drain cut short by the bound is released early and counted. |
| A-06 | The reference's owner is `RenderObject`, and every attached proxy of a live object goes through V-02 to V-04. | EffectMode drives the helper, and MapMode does for admitted land attachments of placements with a bound idle clip, only under `--eawr-map-effect-animation idle`. The clip is an EAWR corpus-naming observation, not the original's clip selection. MapMode draws the mesh in bind pose. Hardpoint, death and proxy-replacement owners are not wired: MapMode has no event source for them (see G-02 in particle-system-detach.md). |
