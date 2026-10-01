# Behaviour notes

These notes record format and runtime semantics used by the implementation. A rule describes its stated scope only; an explicit unknown is not a compatibility claim. Source material and test progress belong outside this directory.

## Formats and runtime

- [XML registry](xml-registry-edge-cases.md), with [retail admission](xml-registry-retail-addendum.md) and [file-service limits](xml-registry-file-service-addendum.md)
- [PGLua chunks](pglua-chunks.md), [Lua host](lua-script-model.md), and [declaration index](lua-api-declarations.md)
- [Tactical logical tick rate](tactical-tick-rate.md): 30 frames per second, pinned pending EAWR-43
- [Tactical time controls](tactical-time-controls.md): pause, fast forward and the game-speed steps (EAWR-459); [battle end](battle-end.md): the win/lose message and the end 7 s after the outcome (EAWR-453)
- [Space targeting](space-targeting.md), [space weapon fire, ship-level target choice and attack orders](space-weapon-fire.md), [space sensor visibility and query order](space-visibility.md), [hull and hardpoint damage, loss, death and repair](space-hardpoints.md), [projectiles, damage types, shields and regeneration](space-damage.md), [ship movement: move, turn in place and stop](space-movement.md), [orders: attacking out of range, attack-move and guard](space-orders.md), [unit abilities: power modes, timers and multipliers](space-abilities.md), [fixed-force victory and defeat](space-victory.md) and [fighter deaths: explode or spin away](space-fighter-deaths.md) (EAWR-447)
- [FoC tactical space AI](foc-tactical-ai.md): architecture, the selected XML and Lua, and the EAWR-79 function subset
- [Debug-build audit](debug-build-audit.md): per-claim verdicts of the simulation, Lua, AI, registry, camera and river notes against the FoC debug build (EAWR-265)
- [TED environments](p1-effective-environment.md) and [terrain-water selection](terrainwater-technique-selection.md)
- [Tactical camera input](tactical-camera-input.md) and [battle selection, orders and the tactical overview](foc-battle-selection.md) (EAWR-82)
- [Battle UI in the world: selection circles, bars, squadron icons, hardpoint reticles](foc-battle-world-ui.md) (EAWR-424)

## Subsystem walks

- [Squadrons](walks/squadrons.md): the fighter and bomber squadron per frame, in FoC's evaluation order, with the gaps against the remake (walk 1)
- [Capital-ship combat](walks/capital-combat.md): capital ships, frigates and corvettes in combat per frame (targeting, shields, hull, hardpoints, damage routing, death), in FoC's evaluation order, with the gaps against the remake (walk 2)
- [Weapons and projectiles](walks/weapons.md): a weapon hardpoint, a unit's own weapon and their projectiles per frame, in FoC's evaluation order, with the gaps against the remake (walk 3)
- [Ship movement](walks/movement.md): a ship's locomotor, its order's formation and its turn toward a target per frame, in FoC's evaluation order, with the gaps against the remake (walk 4)
- [Skirmish production](walks/production.md): credits and income, the build queues, the population cap, reinforcements and the station's upgrades and level-up in a space skirmish, in FoC's evaluation order, with the gaps against the remake (walk 5)
- [Tactical AI](walks/tactical-ai.md): the computer player's tactical decisions in a space battle (goals, plans, TaskForces, the unit-level AI service, damage tracking and the flee response, difficulty), in FoC's evaluation order, with the gaps against the remake (walk 6)
- [Space abilities](walks/abilities.md): every ability of the M2 space units (switching, the expiration and recharge countdown, autofire and the AI's use, the ion shot, `HUNT`, the interactions), in FoC's evaluation order, with the gaps against the remake and the EAWR-670 finding (walk 7)
- [Sensors, selection and battle UI](walks/sensors-ui.md): sensors and fog, picking and selection, and the battle UI rules (icons, bars, grids) per frame, in FoC's evaluation order, with the gaps against the remake (walk 8)
- [Space heroes and unique units](walks/heroes.md): skirmish roster, carried identities, command bonuses, hero abilities and UI, death and build limits, with sourced rules and implementation gaps (walk 11)

## Presentation

- [Shader translation](shader-translation.md) and [MeshGloss](meshgloss-programmable.md)
- [Unit animation](unit-animation.md): the clip FoC plays for each tactical state, death clones and the clip naming rule (EAWR-81)
- [Unit cards in the tactical command bar](foc-unit-cards.md): card grouping, stacking, bars and card clicks (EAWR-425)
- [Vegetation effects](vegetation-effects.md) (Tree.fx, Grass.fx)
- [MeshAdditive shader and reference billboard](meshadditive-sun-billboard.md), with [retail mode-7 placement](meshadditive-sun-mode7-retail.md)
- [Map effect admission](map-effect-admission.md), [mesh emission](particle-mesh-emission.md), [parent lifecycle](particle-parent-lifecycle.md), [attachment visibility](particle-attachment-visibility.md), and [system detach](particle-system-detach.md)

[TEMPLATE.md](TEMPLATE.md) gives the format for a new note. Lua declaration inventory entries establish presence in the stated scan scope; they do not establish signatures or prove absence from the engine.
