# Space particle system walk

This walk covers the legacy particle ALO path used by space battles: model proxies,
event-created particle objects, their emitter groups, particle allocation and update,
and primitive submission. It reuses the existing hit, death, ability and fog rules;
their simulation decisions remain owned by those subsystems. No game code changes
are proposed here. Review baseline: `d623ec4f0b335f25427beb43191f6a02f6bcc448`.

Sources marked **debug build** were read without modifying the owner project.
PSE evidence IDs identify private receipts in ignored `out/research/`; names,
addresses and decompiler output belong only there. **Data** means authored data,
not proof that an inferred runtime policy exists. **Unverified** rows describe
questions, not FoC requirements. **Same** covers the stated observable rule,
not bitwise random equality. Gap owners G01 onward are implementation workstreams,
with tracking references below.

## Existing evidence reused

| Note | Already sourced rules | Reuse and boundary |
|---|---|---|
| [Particle lifetime](../particle-lifetime.md) | PL-01, PL-02 | Birth samples an age sampler; independent continuous capacity. **PS-47 corrects the claim that raw loading preserves the saved range through native post-load initialization.** PS-18 adds the burst branch and multiplicities. |
| [Battle presentation](../battle-presentation.md) | BP-10 to BP-20, BP-30 to BP-35, BP-40 to BP-43, BP-45, BP-47 to BP-50, BP-60 to BP-65 | Hit/death placement, owner lifetime, debris fire, engine/ability admission, brightness, hidden-host reset and missile trails. PS-04 confirms the native removal flag route. BP-44 and BP-64 are project policies. |
| [Space damage](../space-damage.md) | IS-09 | Ion proxy activation and drain; no new stun simulation rule. |
| [Space fog presentation](../space-fog-presentation.md) | FW-16 to FW-18, FW-21, FW-24 | Hull fade, death visibility and unfogged props. FW-19 explicitly makes shared particle opacity a project choice; this walk does not promote it to native evidence. |
| [Mass drivers](../space-mass-drivers.md) | MD-07 | Motion-based kite geometry; reused rather than redecompiled into a second geometry specification. |
| [Tactical time controls](../tactical-time-controls.md) | TM-01 to TM-03, TM-07 | Logical clock and pause; the exact graphics-delta producer is still U02. |
| [Particle mesh emission](../particle-mesh-emission.md) | Random mesh offset/size paragraphs, debug build | Random vertex/surface displacement. Storage-order every-vertex selection is the reference port's choice; PS-14 establishes a difference. The note has no rule IDs. |
| [Particle attachment visibility](../particle-attachment-visibility.md) | V-01 to V-07 | These are MIT reference rules, not independent FoC proof. Native hide/reset uses BP-48 and PSE-02; land respawn remains an existing gap. |
| [Particle system detach](../particle-system-detach.md) | D-01 to D-07 | MIT reference contract. PS-04 now sources the loaded flag and native scene-removal branch. Reference self-reference ownership is not a native requirement. |
| [Particle parent lifecycle](../particle-parent-lifecycle.md) | Unnumbered reference contract | Per-parent instances and their guards are EAWR/reference details. Native trail/death mechanisms are PS-16 and PS-22. |
| Maintainer rendering report, [plugin inventory](../../../plan/inventories/particle-plugins.json) | Inventory and reference provenance | All 1,666 inventoried particle occurrences are V1; no inventoried V2 occurrence. Counts include base, expansion and mod layers, not an effective FoC-only census. |

## Evaluation order and interfaces

The graphics update receives elapsed seconds. A group first handles its accumulated
time and hidden/empty gate, updates dynamic settings and wind, opens dependent
emission, checks host visibility, samples the host transform and its velocity,
advances emitters, synchronizes a completed cycle, removes an exhausted detached
group, and saves the transform for the next update (PSE-01). Dependent emitters are
advanced around their parents through the start/end phases (PSE-39, PSE-40, PSE-47,
PSE-48). Particle aging precedes motion and new emission. New births are advanced by
their time since the scheduled event within the current update (PSE-08, PSE-09).

Draw submission separately checks group and top-level owner visibility, owner alpha,
enabled phases and live geometry. It updates LOD masks, prepares eligible particles,
sorts when requested, uploads vertices, and draws ordinary and heat effects in
separate phases (PSE-21, PSE-25, PSE-27, PSE-17). A successful submission marks the
group rendered for its next update. Bounds include surviving particle extents,
including transformed linked particles, rather than only the host's location
(PSE-01, PSE-04, PSE-28).

The scene dispatcher first collects objects intersecting its camera frustum, then
submits enabled render modes (debug build PSE-OFF-02, PSE-OFF-03). The group's own
submission predicate only rejects hidden state (PSE-OFF-01); it is not the camera
test. EAWR snapshots the backend camera planes on the caller thread and checks
conservative bounds around every live drawable particle before preparing streams.
Those bounds include rotated quads and both kite families and are recomputed after
advancement or linked following. Unknown bounds or camera planes retain preparation.
This establishes a frustum test, without a distance threshold or an occlusion rule.
The viewer's retained 30 Hz clock and zero-delta generation initialization remain
the existing presentation policies described by PS-10 and PS-42.

EAWR's interfaces are `CpuSystem`, `EffectRegistry`, `AttachmentLifecycle`,
`UnitEmitters` and `BattleEffects`. Inputs are loaded ALO definitions, optional mesh
bindings, posed bone frames, presentation elapsed time, host visibility and gameplay
events. Vertex streams and lifecycle counters are outputs. Neither particles nor
their random draws feed the authoritative simulation. Event selection is a separate
exception: FoC's BP-63 uses synchronized randomness, while EAWR's BP-64 deliberately
does not.

## Rule table

### Creation and lifecycle

| ID | FoC rule and condition | Source | Ours: same / differs / missing, location | Gap owner |
|---|---|---|---|---|
| PS-01 | A model proxy runs at its posed bone; hardpoint damage and engine/ability prefixes control admission. World particles remain behind; linked particles follow the current emitter frame. | Debug build, BP-40 to BP-43, IS-09 | **Same** for covered space owners: `apps/viewer/src/unit_emitters_prepare.cpp`, `unit_emitters_frame.cpp`; `src/presentation/particles/cpu_system.cpp`, `follow_emitter`. | None; sub-model scope stays BP-46. |
| PS-02 | Hits create the detonation/absorb effect and optional target-list effect; deaths create the authored explosion; debris attaches fire at its root. Collision-attached effects use the contact bone and offset. | Debug build/data, BP-10 to BP-20, BP-30 to BP-35, BP-63 | **Differs**: `apps/viewer/src/battle_effects_impacts.cpp`, `battle_effects_prepare.cpp` parse collision attachment but place list effects free; facing loses bank in existing cases. | G15, collision placement; existing facing owner. |
| PS-03 | Hiding a proxy stops new emission and keeps existing particles updating; showing it resets the group and starts afresh. Hidden groups without particles skip update. This does not consult the leave-particles flag. | Debug build PSE-01, PSE-02; BP-48, BP-65 | **Same** for death-clone reset and ability drains: `apps/viewer/src/unit_emitters.cpp`, `unit_emitters_frame.cpp`, `include/eawr/presentation/particles/attachment_lifecycle.hpp`. Land idle respawn differs outside this space scope. | Existing land owner. |
| PS-04 | The one-byte root flag selects native scene-removal behavior: set retains the old group with no emitter and gives the host a cloned group; clear removes the group from the scene. The flag is distinct from hiding. | Debug build PSE-46, PSE-29, PSE-30; D-01 for file layout | **Same** observable drain-versus-release primitive: `src/presentation/particles/effect_registry.cpp`, `detach`. EAWR uses explicit handle ownership rather than the native clone/pool mechanism. | None for primitive; U01 for each host's call timing. |
| PS-05 | A group with no emitter stops independent emission, lets existing/dependent work finish, and removes itself once empty and not looping. A completed nonlooping synchronized cycle stops its trigger. | Debug build PSE-01 | **Same** exhaustion outcome: `src/presentation/particles/cpu_system.cpp`, `detach`/`finished`; `apps/viewer/src/battle_effects.cpp` releases finished drains. Child scheduling is separately PS-16. | None. |
| PS-06 | A particle object expires on its authored `Particle_Lifetime_Frames`; expiration destroys its owner, which reaches the graphics removal route. This lifetime is separate from each emitted particle's sampled age. | Debug build PSE-22; data and BP-16 | **Same** separate owner timer: `apps/viewer/src/battle_effects_prepare.cpp`, `battle_effects.cpp`; emitted ages are in `cpu_system.cpp`. | U01 for individual removal callers. |
| PS-07 | On the first admitted emitter update, a nonzero presimulation time repeatedly advances by **0.1 s** while accumulated single-precision internal time is at or below the authored target; it can advance beyond the target by one step. The outer freeze check precedes prewarming and is not repeated inside it. | Debug build PSE-18, PSE-04 | **Same** inclusive cadence and first-update admission: `src/presentation/particles/cpu_system.cpp`, `preroll`/`advance`. The 300 s work cap, aligned emitter starts and rebased public clock remain disclosed project policies. CPU target/equality/overshoot and freeze interaction contracts. | G09 bounded-work policy retained. |
| PS-08 | With a nonzero freeze time, an update whose resulting internal time exceeds that value permanently freezes existing data instead of advancing that complete update; equality still advances. Frozen emitters stop normal aging and emission. | Debug build PSE-04 | **Same** strict crossing and permanent latch: `src/presentation/particles/cpu_system.cpp`, `freeze_crossing`/`step`/`frozen`. CPU equality/crossing contracts also verify suppressed frozen linked following and continued updates of other emitters. | None for this update boundary. |

### Spawn and allocation

| ID | FoC rule and condition | Source | Ours: same / differs / missing, location | Gap owner |
|---|---|---|---|---|
| PS-09 | Continuous emission requests one birth per interval of **1/rate**. Bursts request the authored count per authored interval. A first event waits until the start delay is reached. A burst count of zero permits unlimited cycles; group synchronization decides looping. There is no separately observed initial-population field in this path. | Debug build PSE-07, PSE-08, PSE-01 | **Same** basic count/rate/delay: `src/presentation/particles/alo_particles.cpp`, `parse_v1`; `cpu_system.cpp`, root scheduler. Detailed boundaries are PS-10. | None for basic schedule. |
| PS-10 | A zero-delta group update does nothing. First emission receives age `internal time - delay`. Later scheduling enters only when elapsed exceeds one interval, then visits scheduled events at or before current time, skips events already at least maximum age old, interpolates the emitter frame, and advances each birth by its residual age. | Debug build PSE-01, PSE-08 | **Differs**: `src/presentation/particles/cpu_system.cpp`, `step_segment`/`spawn_batch`, permits time-zero births and uses its own inclusive scheduler and current frame. Historical host samples in `apps/viewer/src/unit_emitters_frame.cpp` do not reproduce native within-update interpolation. | G10, emission timing. |
| PS-11 | The V1 property sampler dispatches constant, component range, cube, sphere, cylinder or pinched cylinder; an unknown selector returns the saved default vector. Solid sphere radius is uniform from zero to radius; direction uses uniform azimuth and cosine of polar angle. Hollow uses the full radius. Cylinder uses uniform radius (or hollow radius), azimuth and height from zero to authored height. | Debug build PSE-38, PSE-44, PSE-45 | **Differs**: `src/presentation/particles/alo_particles.cpp`, `parse_old_group`; `cpu_system.cpp`, `sample`, uses uniform tilt for spheres, giving a different directional distribution. Constant/range/cube and cylinder basic geometry are covered. | G05, shape sampling. |
| PS-12 | Pinched cylinder samples a height fraction within half the pinch fraction, randomly mirrors it, and uses radius `R * (4h² - 4h + 1)`; solid mode scales that radius by a uniform draw and hollow mode keeps it. Azimuth spans one turn and height is `h * authored height`. | Debug build PSE-43 | **Missing**: `src/presentation/particles/alo_particles.cpp`, `parse_old_group`, falls through to a zero point for this selector; `include/eawr/presentation/particles/particles.hpp` has no pinched-cylinder parameters. | G05, shape sampling. |
| PS-13 | Mesh modes are disabled, random vertex, random surface and every vertex. Random vertex/surface displace the sampled world point by normalized transformed normal times signed offset times sampled billboard half-extent times **0.5**; every vertex has no such offset. | Debug build PSE-12, PSE-26; existing mesh-emission note | **Same** offset and modes: `src/presentation/particles/cpu_system.cpp`, `initialize_particle`; `alo_particles.cpp` retains mesh mode. Mesh velocity alignment and selection distributions are U03. | None for stated offset. |
| PS-14 | Every-vertex emission requests vertex count for continuous events or burst count times vertex count for bursts. It shuffles vertex indices per batch and cycles that shuffled list if more births than vertices are requested. Capacity and energy can truncate the batch. | Debug build PSE-05 | **Differs**: `src/presentation/particles/cpu_system.cpp`, `spawn_batch`/`initialize_particle`, walks submeshes and vertices in storage order, without batch shuffle or energy scaling. | G06, mesh emission. |
| PS-15 | World-space positions are transformed at birth; linked positions remain in the emitter frame and are transformed with the current host for drawing. Linked velocity remains world-oriented and is inverse-rotated only for position motion. | Debug build PSE-12, PSE-10, PSE-15; BP-40 | **Same** covered moving-frame result: `src/presentation/particles/cpu_system.cpp`, `follow`, `follow_emitter`; `effect_registry.cpp`, `present`. | None. |
| PS-16 | A continuous dependent trail samples the parent's previous and current positions and schedules births along that segment, gated by the parent's draw mask and group trail permission. Dependent emitters update in the group's start/end phases. This is a shared dependent-emitter path, not evidence for one native emitter allocation per parent particle. | Debug build PSE-10, PSE-28, PSE-35, PSE-39, PSE-40, PSE-47, PSE-48 | **Differs**: `src/presentation/particles/cpu_system.cpp`, `ChildInstance`/`process_events`, owns an instance per parent and samples its current snapshot. `particle-parent-lifecycle.md` describes the reference port. | G11, dependent emission. |
| PS-17 | Dynamic energy gates each ordinary attempted birth with a presentation RNG draw; every-vertex count is truncated after multiplication by energy. At birth, sampled ordinary velocity is also multiplied by energy. Weather uses a separate all-slots initialization branch. | Debug build PSE-05, PSE-11 | **Missing** dynamic energy input in `include/eawr/presentation/particles/particles.hpp` and `src/presentation/particles/cpu_system.cpp`. Engine brightness is a draw factor, not energy. | G12, dynamic emitter settings. |
| PS-18 | Base capacity uses authored maximum age A. Continuous interval is 1/rate; burst interval is authored seconds and count is B. If interval <= A, allocate `ceil(A * births/second)`; otherwise allocate the event count. Clamp base to **5003**, then multiply by parent capacity and, for every-vertex emission with a bound mesh, vertex count. | Debug build PSE-07; PL-02 for continuous branch; live allocation reread | **Same** native requested base, long-interval branch and parent/every-vertex multiplication in `prewarmed_capacity.hpp`, `emitter_capacity_plan`. A separately exposed scheduling reserve retains whole-batch/one-step project allowance; host budgets partition admission before emission. [Capacity policy](../particle-capacity.md). | Project scheduling/host policy documented; U05 remains. |
| PS-19 | Each emitter has its own particle slots and free-slot list. Emission takes free slots and stops when empty; it does not replace the oldest live particle. Retirement returns the slot. | Debug build PSE-05, PSE-07, PSE-19 | **Same** independent emitter admission/free slots and no-oldest-recycle in `cpu_system.cpp`, `spawn_batch`. Packed particle storage is shared, but reserves cannot be borrowed. Stable slots also define the PS-35 mask denominator. | None for admission. |
| PS-20 | Reusable emitter objects are stored in a pool belonging to their shared definition. Pool insertion has no count test in the inspected function. Neither this nor the per-emitter limit establishes a global scene particle cap. | Debug build PSE-42, PSE-07; global ceiling **unverified**, U05 | **Differs** pool architecture: `src/presentation/particles/effect_registry.cpp` creates independently owned CPU systems; `map_effect_plan.cpp` and viewer owners impose aggregate budgets. Native global exhaustion equivalence is not established. | G03; G14 for U05. |

### Per-particle update and presentation

| ID | FoC rule and condition | Source | Ours: same / differs / missing, location | Gap owner |
|---|---|---|---|---|
| PS-21 | Each birth samples the current scalar age sampler. A live particle increments age and retires at **age >= sampled lifetime** before motion. The raw file sampler is not necessarily the current runtime sampler: PS-47 traces its replacement during post-load. | Debug build PSE-11, PSE-09; PL-01 only for raw decode/birth interface | **Same** sampler-at-birth and inclusive death boundary: `src/presentation/particles/cpu_system.cpp`, `initialize_particle`/`step_segment`. The raw decode versus post-load range distinction is covered under PS-47. | None for birth/death interface. |
| PS-22 | Retirement returns the slot and triggers an authored burst death child once when group death-spawn permission is enabled; the burst uses the parent's final position. | Debug build PSE-19, PSE-49 | **Same** final-position, once-per-death burst with independent local permission: `src/presentation/particles/cpu_system.cpp`, `process_events`. | None. |
| PS-23 | Draw color/alpha tracks use normalized age. Sampled RGB offsets are nonnegative integer byte offsets based on authored variation times **100**; locked RGB uses one shared draw. Alpha offset is a separate signed draw. Track bytes plus offsets clamp to 0..255. Group brightness multiplies all four byte channels at draw time. | Debug build PSE-11, PSE-13 | **Differs** variation and quantization: `src/presentation/particles/cpu_system.cpp`, `initialize_particle`/`update_particle`, uses positive floating offsets including alpha, with a different locked-alpha policy; `effect_registry.cpp` applies floating brightness. BP-45 engine factor remains covered. | G07, color variation. |
| PS-24 | Ordinary motion first adds acceleration times delta to velocity, then uses the resulting velocity plus radial velocity and optional current emitter-motion contribution to advance position. The simple path skips gravity within **0.01** of zero and acceleration squared length <= **0.01** unless gravity triggers the acceleration block. | Debug build PSE-10, PSE-28 | **Differs**: `src/presentation/particles/cpu_system.cpp`, `step_segment`, advances position with old velocity, then accelerates; it has no native simple-path thresholds. | G04, particle motion. |
| PS-25 | Authored gravity is world negative Z. Object-space acceleration rotates only the authored acceleration before world gravity is added; gravity does not rotate with the host. Radial acceleration is evaluated when an emitter exists; the simple path has its own activation gate. | Debug build PSE-10, PSE-28 | **Differs**: `src/presentation/particles/alo_particles.cpp` folds gravity into acceleration; `cpu_system.cpp`, `update_particle`, rotates that combined vector when local acceleration is enabled. Radial activation also differs. | G04, particle motion. |
| PS-26 | Enabled wind adds scene wind times delta times **0.5** to velocity. Enabled wind disturbances add a spatially weighted directional contribution to motion. No drag operation occurs in the inspected ordinary/simple paths; that is not proof for weather or other unwalked paths. | Debug build PSE-10, PSE-28 | **Differs**: `src/presentation/particles/alo_particles.cpp` converts legacy wind to response **0.01**; `cpu_system.cpp` has global wind acceleration but no native disturbance collection. Space maps with zero wind do not exercise it. | G04, particle motion. |
| PS-27 | With inherited motion enabled, position motion adds current emitter velocity times authored scale every update; it is not a one-time velocity addition at birth. Group velocity is current translation minus previous translation divided by that update's delta. | Debug build PSE-01, PSE-10, PSE-28 | **Same** ordinary root motion contribution: `src/presentation/particles/cpu_system.cpp`, `step_segment`, samples motion independently from render-only following. Dependent inheritance remains U03/PS-16. | None for root motion. |
| PS-28 | Billboard half-extent is `max(0, size track(relative age) * (1 + sampled size offset) * dynamic size) / 2`. Size offset is sampled uniformly from negative to positive authored variation. | Debug build PSE-11, PSE-26 | **Missing** dynamic size factor: `include/eawr/presentation/particles/particles.hpp`, `src/presentation/particles/cpu_system.cpp`; loaded half-width and sampled size variation otherwise match. | G12, dynamic emitter settings. |
| PS-29 | Rotation track is in turns and becomes radians with **2π**. Each rotation update applies a fresh signed variation draw, accumulates angle, wraps whole turns and selects one of **256** rotation bins; reverse rotation mirrors the bin. Initial-rotation-only runs the calculation at birth. | Debug build PSE-11, PSE-24 | **Differs**: `src/presentation/particles/cpu_system.cpp`, `update_particle`, uses continuous angles and lacks per-update legacy rotation variation/256-bin selection; initial rotation uses the reference conversion. | G08, sprite rotation. |
| PS-30 | Texture frame is truncated from the UV track at relative age. Ordinary atlas submission uses integer `sqrt(texture frame count)` as grid side, then quotient/remainder for row/column. | Debug build PSE-13, PSE-15; BP-54 | **Same** truncated UV frame and integer square-root grid in `cpu_system.cpp`, including nonsquare counts. Zero frame count safely uses one cell as project policy. | None for positive atlas counts. |
| PS-31 | Ordinary primitives can be triangles or quads; linked positions are transformed for drawing. Motion-based kite geometry falls back to ordinary corners at zero projected motion and stretches using authored length and motion/reference speed. | Debug build PSE-06, PSE-13, PSE-15, PSE-54, PSE-CAP-01 to PSE-CAP-03; MD-07 | **Same** ordinary triangle/quad selection, sourced corners/UVs and linked world positions in `alo_particles.cpp`, `render_stream.cpp`. Existing kite adapter remains; triangular kite combinations fail closed pending a geometry adapter. | U03 for world alignment and triangular kites. |
| PS-32 | Requested depth sorting applies only to opaque, alpha, depth-sprite alpha and bump alpha modes. With depth enabled, particles sort farthest first by squared distance to camera; ignore-depth collects eligible particles without sorting. | Debug build PSE-25, PSE-20, PSE-37 | **Same** authored flag, eligible modes and squared-distance descending sort in `alo_particles.cpp`, `render_plan.cpp`, `render_stream.cpp`. Ignore-depth/additive omit sorting. Equal-distance ties use stable IDs as project policy; unsupported depth-sprite modes stay fail-closed. | Unsupported depth-sprite adapter remains fail-closed. |
| PS-33 | Render mode chooses ordinary/depth-sprite/bump textures and primitive blend; ignore-depth is an independent flag and primitives disable face culling. Heat-marked emitters submit separately from ordinary emitters; native batched heat draws override primitive mode to alpha. | Debug build PSE-14, PSE-17, PSE-21; public effect states in rendering report | **Differs** coverage/pipeline: `src/presentation/particles/render_plan.cpp` supports selectors 0,1,2,3,10,11 and rejects 4..9,12,13; `render.cpp` uses its heat adapter. Native override is established, final heat compositing remains U03. | G13, renderer coverage. |

### Detail, visibility and timing

| ID | FoC rule and condition | Source | Ours: same / differs / missing, location | Gap owner |
|---|---|---|---|---|
| PS-34 | Graphics settings apply `ParticleDetail` to the global particle LOD. Authored Default_0..3 values are **0.4, 0.6, 0.8, 1.0**; heat is disabled in Default_0 and Default_1. Global detail and group-local LOD are distinct inputs. | Debug build PSE-33; data `graphicdetails.xml` | **Same** independent detail/local/heat inputs in `particles.hpp`, `effect_registry.cpp` and live viewer owners. Explicit live controls; default 1 with heat enabled matches highest authored detail. `_LOD` proxy-name selection remains separate. | Settings UI integration remains. |
| PS-35 | For ordinary emitters except nonweather every-vertex, draw eligibility uses `K = ceil(slot count * clamp(local LOD,0,1) * global detail)`. A slot is eligible when `(index * 60659 + 60913) mod slot count < K`; K=0 hides all. This changes draw masks, not slot allocation. | Debug build PSE-03, PSE-25 | **Same** mask formula with stable emitter-local slot identity in `cpu_system.cpp`; `render_stream.cpp` rejects masked geometry. **Differs** denominator: the existing caller pool count, without resizing or changing exhaustion boundaries. Weather metadata is programmatic; its initialization/loading remains outside the space slice. | G03 for independent native capacity and its mask denominator. |
| PS-36 | At local LOD <= **0.5**, enable only the first `ceil(emitter count * 0.5)` emitters and disable death spawns, trails and terrain collision. At **0.5 < LOD <= 0.7**, enable first `ceil(count * 0.7)`, allow death spawns but disable trails/collision. At **0.7 < LOD <= 1**, allow all. Disabled parents propagate below-LOD status to their child links. | Debug build PSE-32, PSE-34, PSE-16 | **Same** inclusive local bands, emitter counts, child-link propagation and death/trail/collision permission in `cpu_system.cpp`. Global detail never changes local admission. | U04 for a camera-driven local producer. |
| PS-37 | Below-LOD emitters reject births; their existing particles still age when present. A draw-ineligible parent no longer produces continuous trail children. | Debug build PSE-05, PSE-08, PSE-28, PSE-35 | **Same** separate birth, aging and draw gates in `cpu_system.cpp`. Per-parent links stay dormant while trail births are gated, allowing living parents to resume after a detail increase. | None for these gates. |
| PS-38 | In game, after a group has a transform, an unrendered group accumulates delta and returns while the total is **< 0.1 s**. At 0.1 or above, or when rendered last time, it advances using the full accumulated delta. Hidden empty hosts then skip work; hidden hosts with live particles can drain. | Debug build PSE-01, PSE-21 | **Same** accumulated gate in `src/presentation/particles/effect_registry.cpp`, including equality, successful-submission latch and draining particles. Completed empty drains skip CPU work. Existing sampled presentation clock and zero-delta generation initialization remain PS-10/PS-42 policies. CPU contracts cover saved ages and deterministic work across worker schedules. | G10, retained presentation clock. |
| PS-39 | Particle groups maintain world bounds from live particles and refresh scene culling after update. Draw submission independently checks hidden state, enabled particle/heat phases and nonempty prepared geometry. This establishes culling integration, not a universal camera-distance kill rule. | Debug build PSE-01, PSE-04, PSE-21, PSE-41, PSE-OFF-01 to PSE-OFF-03 | **Same** frustum-before-preparation boundary: `render_stream.cpp`, `particle_bounds` includes emitted world trails and conservative primitive extents; `effect_registry.cpp` skips culled preparation/upload and hides retained backend geometry. Camera re-entry rebuilds before showing; unknown bounds/camera fail open. | U04 for any additional occlusion/local-LOD policy. |
| PS-40 | Attached group submission rejects a code-hidden top-level owner or a model light-scale alpha **< 0.01**; equality submits. Particle RGBA separately receives group brightness. Continuous sharing of the hull's fog fade into group brightness is not established. | Debug build PSE-21, PSE-13; FW-19 | **Differs** by documented project choice: `apps/viewer/src/unit_emitters_frame.cpp` multiplies shared hull opacity into streams and retains them through the FW-18 hide point **0.025**. | G14, U02; existing fog owner. |
| PS-41 | Generic nonweather camera-distance thinning was not established. Weather-specific cutoff/fade fields occur in the draw setup; they cannot be generalized to space smoke, engines or impacts. | **Unverified** U04; debug build PSE-13 for weather-only branch | **Missing** a verified native distance policy; `src/presentation/particles/render_stream.cpp` builds every eligible particle. No new distance threshold is justified by this walk. | G14, culling evidence. |
| PS-42 | Particle graphics consumes elapsed seconds, including accumulated nonrendered intervals and variable residual birth ages; this path does not impose a fixed 1/30 s step. A zero-delta group update returns. | Debug build PSE-01, PSE-08 | **Differs** intentional presentation clock: `apps/viewer/src/unit_emitters_frame.cpp`, `battle_effects.cpp`, use 30 Hz samples; `cpu_system.cpp` itself accepts variable nonnegative deltas. | G10; U02 for producer/pause. |
| PS-43 | Particle births/color variation/energy use presentation randomness, separate from synchronized gameplay event-list selection. Camera/draw LOD can change presentation work and child generation. | Debug build PSE-05, PSE-11, PSE-28; BP-63/BP-64 | **Same** separation in `src/presentation/particles/cpu_system.cpp` and `effect_registry.cpp`; deterministic local seeds are an EAWR rule, not a requirement for FoC bitwise particle equality. BP-64 event selection remains a documented deviation. | Existing event-selection owner. |
| PS-44 | Logical simulation holds during pause and advances faster under tactical speed/fast-forward. Exact propagation to particle graphics delta, rotation-only render updates and fog brightness during pause is not traced by this walk. | TM-01 to TM-03, TM-07; particle-specific part **unverified**, U02 | **Same** EAWR project hold/speed policy in `apps/viewer/src/unit_emitters_frame.cpp`, `battle_effects.cpp`; native particle fidelity is pending. | G14, timing evidence. |
| PS-45 | `No_Particle_Below_Detail_Level` and `Allow_Particle_Model_Culling` are authored on particle object types. Data alone does not establish their comparator, default, or whether they gate creation, model admission or later rendering. | Data `particles.xml`; runtime **unverified**, U06 | **Missing** these tag consumers in `apps/viewer/src/battle_effects_prepare.cpp`. Registry marks both todo. | Existing tag owners; G14 runtime evidence. |
| PS-46 | No V2 particle occurrence exists in the accepted inventory. Creator/plugin names in that reference catalogue are not evidence that FoC's V1 runtime invokes those plugins. | Data inventory, counts; native V2 semantics **unverified**, U07 | **Missing** V2 parameter execution in `src/presentation/particles/alo_particles.cpp`, `parse_v2`; retained metadata does not claim application. | G14, corpus-triggered follow-up. |

### Post-load correction to the existing age note

| ID | FoC rule and condition | Source | Ours: same / differs / missing, location | Gap owner |
|---|---|---|---|---|
| PS-47 | Raw decoding reads the serialized age sampler, but ordinary group post-load allocation replaces its low/high bounds with sorted endpoints **max(0, A - A * variation)** and **A**. Allocation also runs in construction/reallocation. Sampler mode and the saved constant default remain intact; the rebuilt bounds govern nonconstant births. Thus the raw saved high bound does not govern ordinary loaded range-mode smoke. | Debug build PSE-54, PSE-55, PSE-50, PSE-06, PSE-53; live scalar bounds-setter read | **Same** raw decode versus post-load distinction: `alo_particles.cpp`, `parse_v1`, retains raw bounds/mode; `particles.hpp`, `postload_lifetime_range`, rebuilds the runtime copy used by `cpu_system.cpp` and effective capacity in `prewarmed_capacity.hpp`. CPU raw/range/constant and smoke/ion population contracts. PL-01 corrected. | None for this post-load age rule. |

For the smoke example in the lifetime note, the raw saved range is 1.44..14.56 s,
while this complete native post-load path rebuilds it as 1.44..8 s from metadata
8 s and variation 0.82. The raw loader read and the birth sampler read are both
real; joining them without the intervening allocation gave the wrong preserved-range
claim. This walk leaves the existing code and note untouched and records the
corrective evidence for their owner. A retail smoke/population check should accompany
the follow-up because the prior fix addressed an owner-visible defect.

## Creator-family coverage

The complete V1 creator mechanisms reached here are independent shape/mesh emission,
continuous dependent trails and parent-death bursts. Their sampler and mesh branches
are PS-11 to PS-16; initial population, bursts and synchronized repeats are PS-07,
PS-09 and PS-10. Weather is a separate initialization/update path, outside the space
ordinary-particle fidelity claim. The native sampler's pinched cylinder is absent
from the port even though the port supports additional reference shapes.

For completeness, the reference plugin catalogue's creator IDs are 0 Point, 1 Trail,
2 Sphere, 3 Box, 4 Torus, 5 Mesh, 6 Terrain, 34 Shape, 35 EnhancedMesh,
39 EnhancedTrail, 40 Death, 43 AlignedShape, 50 OutwardVelocityShape and
60 HardwareSpawner. EAWR implements the V1 conversions to 34, 35, 39 and 40;
the other catalogue creators are unsupported. V2 parameters remain metadata only.
These are reference catalogue terms, not native debug-build identifiers or proof
of native V2 behavior. U07 requires an actual effective-data occurrence before a
V2 expansion is warranted.

## XML inputs and registry gaps

Particle ALO rates, samplers, curves, mesh modes and flags are binary effect data,
not XML tags. `graphicdetails.xml` supplies `ParticleDetail` and `HeatDistortions`;
`gameconstants.xml` supplies the visual-status attachment-bone list through its
existing owner. Creation consumes `Space_Model_Name`, `Scale_Factor`,
`Particle_Lifetime_Frames`, `Particle_Attach_To_Collision`, projectile detonation
tags, `Shield_Hit_Particles`, `Damage_Hit_Particles`, `Death_Explosions`,
`Death_Explosion_Particles`, `Debris_Attached_Particle`, `Damage_Particles`,
`Engine_Particles` and `Engine_Death_Hide_Engine_Particles` through the referenced
BP/IS rules. Ability-specific owner/target particle and bone tags remain owned by
the ability walk; this walk does not claim to implement those abilities.

| Registry tag | Current gap / boundary | Owner |
|---|---|---|
| `Allow_Particle_Model_Culling` | Particle-class todo; U06 must locate the runtime reader before choosing a camera policy. | Presentation tag coverage (legacy EAWR-653). |
| `No_Particle_Below_Detail_Level` | Particle-class todo; Small/Medium_Damage_Space author 0.7, Large_Damage_Space 0.5. Equality/default semantics are U06. | Data/economy tag coverage (legacy EAWR-654). |
| `Projectile_Lifetime_Detonation_Particle` | Projectile-class todo: expiry detonation is an event-creation interface, distinct from particle aging. | Presentation tag coverage (legacy EAWR-653); weapon owner. |
| `Projectile_Object_Creation_Particle` | Projectile-class todo: creation effect is distinct from a model proxy trail. | Presentation tag coverage (legacy EAWR-653); weapon owner. |
| `Death_Explosions_End` | UniqueUnit-class todo: final death effect belongs to the death-sequence owner. | Presentation tag coverage (legacy EAWR-653). |
| Ability owner/target particle and bone tags | Sensor jamming, planet destruction, super laser and generic unit-ability particle rows remain todo for their authored space classes. | Ability tag coverage (legacy EAWR-760); ability walk. |

`Particle_Lifetime_Frames` and `Particle_Attach_To_Collision` are marked applied
by the registry. The latter's parse/application citation does not erase BP-63's
free-placement gap. Registry classification is separate from this walk's observed
behavior comparison. No registry rows are changed by this docs-only PR.

## Gaps and evidence work

The particle-system tracking issue (legacy EAWR-1466) owns these implementation tickets.
Each workstream has one bounded outcome; tickets can split implementation later.
Existing owners are linked rather than duplicated. Highest immediate battle impact:
G01 detail work, G02 off-screen preparation, G03 independent capacity, G04 motion
integration and G09 prewarmed ion/engine population. These rankings are qualitative;
this walk makes no new frame-time claim.

| Owner | Rules | Work and size | Tracking |
|---|---|---|---|
| G01 | PS-22, PS-34 to PS-37 | Separate global draw masks/local emitter and child gates with deterministic work-count tests. Explicit live settings; default 1. | Implemented; settings UI and U04 producer remain. |
| G02 | PS-38, PS-39 | Accumulated unrendered updates and independent frustum culling before preparation/upload are implemented with particle-extents, re-entry age and deterministic work contracts. | Implementation (legacy EAWR-1468); lit pan eye confirmation pending. |
| G03 | PS-18 to PS-20 | Independent emitter reserves, native requested sizing, parent/vertex multiplication and explicit host/scheduling policies are documented in [capacity policy](../particle-capacity.md). Native global pool equivalence remains U05. | Implementation follow-up (legacy EAWR-1469). |
| G04 | PS-24 to PS-26 | Correct motion order and separate world gravity; cover native radial/zero thresholds and wind scaling. M. | Implementation follow-up (legacy EAWR-1470). |
| G05 | PS-11, PS-12 | Correct sphere direction distribution and load/sample pinched cylinders without silent zero-position fallback. M. | Implementation follow-up (legacy EAWR-1471). |
| G06 | PS-14 | Shuffle every-vertex batch selection and cycle it under truncation. S. | Implementation follow-up (legacy EAWR-1472). |
| G07 | PS-23 | Reproduce native signed alpha and byte-scaled color variation; preserve documented engine brightness. S. | Implementation follow-up (legacy EAWR-1473). |
| G08 | PS-29 | Load per-update rotation variation and reproduce 256-bin selection/initial-only behavior. S. | Implementation follow-up (legacy EAWR-1474). |
| G09 | PS-07, PS-08, PS-47 | Post-load age range, inclusive 0.1 s presimulation and strict freeze crossing are implemented with CPU smoke/ion population contracts. PL-01 is corrected; the 300 s prewarm bound and rebased shared clock remain disclosed project policies. | Implementation (legacy EAWR-1475); smoke eye confirmation remains held (legacy EAWR-1329). |
| G10 | PS-10, PS-42 | Reconcile birth boundaries/residual aging and interpolate within graphics updates; assess fixed presentation sampling explicitly. M. | Implementation follow-up (legacy EAWR-1476). |
| G11 | PS-16 | Match dependent trail segment scheduling and group phase order; compare simultaneous parents. M. | Implementation follow-up (legacy EAWR-1477). |
| G12 | PS-17, PS-28 | Expose dynamic energy and size separately from brightness, with correct birth/velocity/count effects. M. | Implementation follow-up (legacy EAWR-1478). |
| G13 | PS-30 to PS-33 | Preserve legacy sort and primitive/atlas policy; inventory unsupported blend/depth families before adding adapters. L; performance. | Implementation follow-up (legacy EAWR-1479). |
| G14 | PS-20, PS-40 to PS-46 | Settle U01 to U06 with targeted native readers or paired captures; U07 is conditional on a new corpus occurrence. No speculative runtime changes. M; performance for U04/U06. | Evidence follow-up (legacy EAWR-1480). |
| G15 | PS-02 | Retain contact bone/frame ownership for collision-attached target-list effects; preserve unrelated event selection. M. | Implementation follow-up (legacy EAWR-1481). |
| Existing facing owner | PS-02 | Complete bank/facing for hardpoint and death explosions. | Facing defect (legacy EAWR-820). |
| Existing land owner | PS-03, PS-04 | Native flag/removal evidence is now PSE-46/PSE-29; opt-in land respawn/reset difference still remains. | Land attachment policy (legacy EAWR-821). |

## Unverified questions and deciding evidence

| ID | Remaining question | Minimal evidence that decides it |
|---|---|---|
| U01 | Projectile removal, hardpoint replacement and clone expiry callers: which remove a host immediately and which detach/drain? PS-04 establishes the flag branch, not all callers. | Targeted caller read or short close-view missile/death/replacement capture with both root flag values. Reuse existing missing-ion owner reproduction (legacy EAWR-1447), rather than presume its cause. |
| U02 | Exact graphics delta under pause/speed and continuous particle fog opacity/rotation updates. FW-19 remains a project choice. | Trace scene delta and group brightness setters; pair a paused/speed-switched damaged ship and a fog crossing, with a stationary reference particle. |
| U03 | Mesh choice weighting, velocity-normal alignment, parent inheritance, world-aligned corners and final heat compositing. Existing reference port mechanisms are not all independently sourced here. | Targeted mesh/frame/renderer readers; fixed staged meshes and paired emitter captures, including simultaneous trail parents. |
| U04 | Any additional occlusion dispatch or distance-driven local LOD producer beyond the established scene frustum collection. No universal space-particle distance threshold was established. | Read remaining culling/local-LOD setter callers; paired pan/zoom away-and-back capture that checks reentry ages and long trails outside the host bounds. |
| U05 | Global scene/pool resource ceilings, pool eviction and exhaustion under pressure. Per-emitter 5003 is not such a ceiling. | Targeted pool/allocator consumers and an isolated stress capture with allocation counters. |
| U06 | Runtime readers, defaults and equality for the two XML quality/culling tags, and which hit effects each quality setting suppresses. | Follow tag-table fields to consumers, then stage identical hits at Default_0..3; do not infer threshold semantics from tag names. |
| U07 | Native V2 creator semantics if a future effective-data occurrence is found; the current accepted corpus has none. | An actual asset occurrence plus its load/update reader; no capture or implementation needed solely because a reference plugin ID exists. |

The performance boundary is therefore explicit: hidden-empty skipping, accumulated
unrendered updates, authored/local LOD gates and a draw mask are sourced. Arbitrary
distance killing, using only the host's bounds, changing every spawn to mimic a
draw mask, or dropping elapsed time on reentry would require new evidence.
