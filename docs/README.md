# Documentation

References written (legacy #N), rendered as (legacy EAWR-N) in the public copy, point
to the project's earlier private issue tracker. They are kept for maintainers and are
not links.

Contracts, file-format facts, behaviour notes and how-tos for the remake. Start with
[build.md](build.md) to build and test, [clean-room.md](clean-room.md) for how original
behaviour is researched and implemented, and [behaviour/README.md](behaviour/README.md)
for the behaviour notes that the simulation and presentation code cite by rule ID.

Contributing a note: write one only when it records a decision or a format fact that later
code relies on, and merge it into an existing topic where one fits. Keep an old path only while
a test, a tool or a provenance reference still needs it.

- [architecture-decisions.md](architecture-decisions.md) — Architecture decisions.
- [asset-formats.md](asset-formats.md) — Portable CPU asset formats.
- [behaviour/foc-river-group-selection.md](behaviour/foc-river-group-selection.md) — FoC river and effect-group draw selection.
- [behaviour/lua-api-declarations.md](behaviour/lua-api-declarations.md) — Lua-visible Declaration Inventory Contract.
- [behaviour/lua-script-model.md](behaviour/lua-script-model.md) — Lua Script Host Behaviour Contract.
- [behaviour/map-effect-admission.md](behaviour/map-effect-admission.md) — Map attached-effect admission snapshot.
- [behaviour/meshadditive-sun-billboard.md](behaviour/meshadditive-sun-billboard.md) — MeshAdditive sun billboard (P1 environment rendering source research).
- [behaviour/meshadditive-sun-mode7-retail.md](behaviour/meshadditive-sun-mode7-retail.md) — Retail mode-7 (`sun`) billboard placement (P1 environment rendering, gate G-01).
- [behaviour/meshadditive-mode6-glow.md](behaviour/meshadditive-mode6-glow.md) — FoC sunlight glow placement and Coruscant material facts.
- [behaviour/meshgloss-programmable.md](behaviour/meshgloss-programmable.md) — MeshGloss programmable behavior and prototype lighting.
- [behaviour/p1-effective-environment.md](behaviour/p1-effective-environment.md) — Effective TED environment, lighting, fog and sky inputs (P1 lighting, map loading and environment rendering, WP-13).
- [behaviour/particle-attachment-visibility.md](behaviour/particle-attachment-visibility.md) — Particle attachment visibility.
- [behaviour/particle-mesh-emission.md](behaviour/particle-mesh-emission.md) — V1 EnhancedMesh CPU emission.
- [behaviour/particle-parent-lifecycle.md](behaviour/particle-parent-lifecycle.md) — V1 particle parent lifecycle.
- [behaviour/particle-system-detach.md](behaviour/particle-system-detach.md) — Particle system detach and drain.
- [behaviour/pglua-chunks.md](behaviour/pglua-chunks.md) — PGLua Compiled-chunk Contract.
- [behaviour/README.md](behaviour/README.md) — Behaviour notes.
- [behaviour/shader-translation.md](behaviour/shader-translation.md) — Shader Translation Behaviour Contract.
- [behaviour/space-targeting.md](behaviour/space-targeting.md) — Automatic space hardpoint opportunity targeting.
- [behaviour/space-weapon-fire.md](behaviour/space-weapon-fire.md) — Ship-level target choice, attack orders and the weapon fire cycle.
- [behaviour/tactical-camera-input.md](behaviour/tactical-camera-input.md) — Tactical camera input and cinematic free camera (project policy).
- [behaviour/TEMPLATE.md](behaviour/TEMPLATE.md) — Behaviour note template.
- [behaviour/terrainwater-technique-selection.md](behaviour/terrainwater-technique-selection.md) — TerrainWater technique selection.
- [behaviour/xml-registry-edge-cases.md](behaviour/xml-registry-edge-cases.md) — XML registry edge cases.
- [behaviour/xml-registry-file-service-addendum.md](behaviour/xml-registry-file-service-addendum.md) — XML registry initial file-service boundary.
- [behaviour/xml-registry-retail-addendum.md](behaviour/xml-registry-retail-addendum.md) — Retail XML registry clarification.
- [build.md](build.md) — Build and test.
- [camera.md](camera.md) — Camera implementation policy.
- [clean-room.md](clean-room.md) — Clean-room rule for the remake.
- [estimate.md](estimate.md) — Work estimate, measured phase effort and re-estimates.
- [fixed-point.md](fixed-point.md) — Fixed-point arithmetic contract.
- [graphics-features.md](graphics-features.md) — Graphics features: retail options, viewer status, Forward+ extras.
- [inventories.md](inventories.md) — Offline inventory contracts.
- [lua-runtime.md](lua-runtime.md) — Lua 5.0.2 bounded runtime.
- [lua-numeric-profile.md](lua-numeric-profile.md) — Authoritative Lua soft-float binary64 numbers.
- [lua-sandbox.md](lua-sandbox.md) — Authoritative Lua sandbox, canonical iteration, script time/RNG and tick scheduler.
- [lua-persistence.md](lua-persistence.md) — Canonical save/load of the authoritative Lua graph and the versioned script state hash.
- [prototypes/godot.md](prototypes/godot.md) — Godot RenderingServer prototype B.
- [prototypes/sdl-wgpu.md](prototypes/sdl-wgpu.md) — SDL3 and wgpu-native prototype A.
- [releasing.md](releasing.md) — Releasing: tags, draft releases and what they contain.
- [rendering.md](rendering.md) — Rendering policies and fixtures.
- [replay-format.md](replay-format.md) — Synthetic replay format v1.
- [research/source-inputs.md](research/source-inputs.md) — Source inputs and bounded findings.
- [shaders.md](shaders.md) — Shader translation spike.
- [simulation.md](simulation.md) — Deterministic simulation harness.
- [skirmish-start.md](skirmish-start.md) — M2 skirmish tick zero: start rules, census, the m2-start replay.
- [tag-coverage.md](tag-coverage.md) — XML tag registry: every FoC (class, tag) has a status and a target in our code or a sourced reason it has none; the gate, the load trace.
- [traces.md](traces.md) — Behaviour traces, fidelity scenarios and the trace comparer.
- [ui/mod-hud-survey.md](ui/mod-hud-survey.md) — How six mods change the FoC HUD; D2 recommendation and mod-compat fixtures.
- [ui/ui-layer.md](ui/ui-layer.md) — UI layer: retail sources, layout rules, architecture and plan.
- [unit-data.md](unit-data.md) — M2 unit tables: loading rules, hardpoint positions in Q24, content identity.
- [vfs.md](vfs.md) — Virtual file system contract.
- [xml-model.md](xml-model.md) — XML object model and variant resolution.
