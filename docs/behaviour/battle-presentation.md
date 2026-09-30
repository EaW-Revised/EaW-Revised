# Space battle presentation (P2-17, EAWR-80)

## Applicability

What FoC draws for a space battle's shots, hits, shield hits and deaths, so the viewer can present
the authoritative tactical session (`--eawr-live-session`) the way the game does. Sources: the
FoC debug build `corruption/StarWarsI.exe` read in GhidrAssist (evidence IDs PB-01 to PB-22,
PB-30 to PB-35, PB-40 to PB-52 and PB-60 to PB-63, map private, `out/research`), retail FoC recordings on the rig
(RC-391-01, EAWR-391; RC-427-01, EAWR-427), the public FoC shader source (`MeshShield.fx`, `Shield.fxh`), the model data
and the FoC XML (`PROJECTILES.XML`, `PARTICLES.XML`, `HARDPOINTS.XML`,
the space unit files and `GAMECONSTANTS.XML` of the pinned `corruption` megs). Nothing here is
simulation: the viewer reads the published `TacticalSnapshot` and never feeds back, so the
session's hashes are the same with or without it (UI-07).

## Interface

- The snapshot publishes the projectiles in flight after each tick (ascending ID) and the tick's
  combat events. A `projectile_hit` event carries in `outcome` how the hit was taken:
  `hit_outcome_shield_absorbed` or `hit_outcome_armor_reduced` (BP-12, BP-13). Neither changes
  state; they only pick the effect.
- Events fire on the first presented frame whose interpolation reaches their tick, oldest first.
- Particle effects run on the presentation tick (one 30 Hz sample per session tick), so a paced
  capture shows them at battle time.
- Colours are the stored 8-bit values / 255, as the particle adapters take them.

## Rules

### Projectiles

| ID | Rule |
|---|---|
| BP-01 | `Projectile_Custom_Render` 1 draws the projectile as a laser beam, 2 as a laser kite; any other value draws its `Space_Model_Name`. A beam or kite loads no model even when the type names one (`Proj_Ship_Medium_Laser_Cannon_Green` names `W_LASER_MEDIUMG.ALO` and draws a beam). PB-01. |
| BP-02 | Beams and kites lie along the projectile's axis from `B1 = -Projectile_Length/2` to `B2 = +Projectile_Length/2` on model Y, centred on the projectile. The axis runs **against** the flight: `B1` is the front end, `B2` the back. A projectile flies along its facing (its facing vector rotated onto +X, times its speed, each service), and every object's model transform carries the fixed +90 degree turn about Z (p1-effective-environment R-ROT-01, R-ROT-04), so the model nose is -Y and model +Y (`B2`) points backwards. For a kite this puts the long point behind the shot: the `p_particle_master.tga` glow cell (a round, centred blob) is centred on the projectile, so the short, thick end leads and the long, thin end trails, as the owner sees in FoC (2026-09-27). PB-02, PB-04, PB-14. |
| BP-03 | A kite samples `p_particle_master.tga`: an 8 x 8 atlas cell at `Projectile_Texture_Slot / 8`, corners (0,1), (0,0), (1,0), (1,1) of the cell, tinted by `Projectile_Laser_Color` (RGBA). PB-02, PB-03. |
| BP-04 | A kite is a camera-facing diamond around the projectile's position: two side points at plus and minus the screen-perpendicular times `w`, a point along the screen-projected axis `B1 -> B2` (towards `B2`, the trailing end) at `w` times the axis's across-view length (at least 1), and one towards `B1` (the leading end) at `w`; the cell's (0,0) corner is on the long trailing point and (1,1) on the short leading one. `w = Projectile_Width x (Laser_Kite_Z_Scale_Factor x z + 1)` with `z` the normalised view depth (depth minus near, over far minus near). PB-04. |
| BP-05 | Kites draw as additive quads, depth tested (less or equal), no depth write, no culling. PB-05. |
| BP-06 | A beam samples `W_Laser_Pill.tga`: a 4 x 4 atlas cell at `Projectile_Texture_Slot / 4`, white. PB-06, PB-09. |
| BP-07 | A beam is a six-point pill: a point beyond each end along the screen-projected axis at `w`, and two points at each end at plus and minus the screen-perpendicular times `w`; cell coordinates (1,0), (.5,0), (1,.5) at `B1` and (0,.5), (.5,1), (0,1) at `B2` (the laser runs along the cell's diagonal). `w = Projectile_Width x (Laser_Beam_Z_Scale_Factor x z + 1)`. PB-07, PB-08. |
| BP-08 | Beams draw as additive triangles with no culling; the depth test is skipped only by a debug switch with a steep camera, so the remake always tests depth. PB-09. |
| BP-09 | Projectiles are `HIDE_WHEN_FOGGED`. The remake shows a projectile while the local player sees its shooter or its target (U-02). |

### Hits

| ID | Rule |
|---|---|
| BP-10 | A hit the shield took whole spawns the projectile's `Projectile_Absorbed_By_Shields_Particle`, placed and faced by BP-17 to BP-19, then one of the target type's `Shield_Hit_Particles` at the same place and facing (BP-20). PB-10, PB-15. |
| BP-11 | Any other hit spawns `Projectile_Object_Armor_Reduced_Detonation_Particle` when the hull armor multiplier was at most 0.75 and the type names one, else `Projectile_Object_Detonation_Particle`, at the contact point. PB-10. |
| BP-12 | "The shield took it whole": the shield stage took at least the damage it was given (FoC's absorbed flag). In the remake: the absorbed amount is positive and not below the shield-scaled damage (space-damage DG-06). PB-11. |
| BP-13 | The armor multiplier is the one that scaled the damage reaching the hull stage; with nothing reaching it the multiplier counts as 1. PB-11. |

### Shield hits (EAWR-415)

| ID | Rule |
|---|---|
| BP-17 | The shield-hit particle faces a direction. When the target's model has a SHIELD sub-object (the first sub-object named `SHIELD`, case ignored), it is the collision normal at the contact, turned round when it points along the projectile's velocity. Without one it is the projectile's velocity reversed and normalised. The four M2 ships (`RV_NEBULONB`, `RV_CORVETTE`, `EV_TartanCruiser`, `EV_ACCLAMATOR`) have a SHIELD mesh; the two level-1 stations (`RB_STATION_01`, `EB_STATION_01`) have none. PB-15, PB-17; model data. |
| BP-18 | The facing is built from the origin towards that direction: yaw `atan2(y, x)` in degrees in [0, 360) (0 when x and y are both 0), pitch the negated elevation (0 when z and x are both 0), roll 0. FoC then adds 90 degrees to the **pitch** (not the yaw), and the transform is Rz(yaw) Ry(pitch) Rx(0). The particle's local +Z therefore lies along the direction and its local +Y is horizontal. A particle with `Particle_Attach_To_Collision` (all three `Projectile_Shield_Absorb_*`) takes that transform at the contact as its offset from the hit bone, so no model turn applies. A free particle is placed like any object and gets the fixed +90 degree turn about its own +Z after the facing (R-ROT-01). PB-15, PB-16. |
| BP-19 | While a unit's shield is above zero and neither depleted nor in an ion storm, its SHIELD sub-object is collidable, so a projectile meets the shield mesh before the hull. The mesh test runs on each triangle of the projectile's frame step: it is two-sided, the nearest hit past the step's start wins, and the normal is the hit triangle's face normal as wound (BP-17 turns it to face the shooter). The nearest hit among all collidable sub-objects gives the contact and the normal; the SHIELD mesh does not always enclose the hull (the Tartan's collidable `SHADOW` mesh reaches past its bubble). The remake's contact lies on the target type's collision box (space-damage DG-31). The viewer therefore casts the projectile's own frame step as FoC does: from the hit event's origin (the projectile's position at the start of the frame) along its flight (origin to contact), one frame step (the shot's `Max_Speed`) long. It uses the first triangle it meets among the target's collidable meshes and its SHIELD mesh, posed as the target is drawn that frame. A step that begins inside the SHIELD bubble therefore meets the bubble where the shot leaves it. When the step meets no triangle because the box stopped the shot first, the cast goes on from the same start past the whole mesh (`battle_effects.shield_hits.ahead`, against `in_step`); it never starts behind the step. The cast runs in the model's own space against triangles kept once per type (the segment is transformed, not the mesh; the 0.0001 determinant floor is rescaled so it still holds in world space), behind a bounding sphere and boxes over runs of 32 triangles (`shield_hits.casts`). The segment fraction of a hit lies in (0, 1): a surface exactly at either end of the step is not met. For a model without a SHIELD sub-object that only moves the particle. When the line meets no triangle, the particle stays at the event's contact and faces back along the flight (`battle_effects.shield_hits.mesh_missed`). Such a hit is one the box took and FoC's meshes would have let pass (about one in six in the duel). PB-17, PB-18. |
| BP-20 | A hit the shield took whole also spawns one entry of the target type's `Shield_Hit_Particles`, drawn with the synchronized random generator, at the same position and facing as BP-18. No M2 unit names any (only the cinematic capital ships and the land bombing-run units do), so M2 shows none; the viewer draws the entry BP-63 and BP-64 pick for a type that names some. PB-15, PB-62. |
| BP-21 | In the debug build, `Take_Damage -> COLOR_FLASH -> Set_Light_Scale` causes every hit whose damage the shield takes in part or whole (the shield stage's absorbed amount above zero; also any hit during an ion storm) to start a colour flash on the unit: the model's light scale RGB starts at `Shield_Flash_Scale` (1.0, 1.1, 1.25) and returns linearly to the normal scale over `Shield_Flash_Duration` (0.1 s); a new flash restarts a running one. The flash does not show or hide the SHIELD sub-object (BP-22). PB-19, PB-20. The owner sees no visible hull flash on retail shield hits (EAWR-438), only the small shield ripple, so the viewer leaves the flash off by default. `--eawr-live-shield-flash on` (EAWR-427, EAWR-438) starts it for each `projectile_hit` with `hit_outcome_shield_absorbed`, at the tick's start (as the hit's particles are born), as a per-instance light scale (`GodotRenderer::set_light_scale`, the `eawr_unit_light_scale` instance uniform) on the unit's own surfaces, multiplied into the bump-colorize and RSKIN adapters' LIGHT_SCALE.rgb terms (every M2 hull). A hit the shield takes only in part does not flash: the hit event carries only the whole-absorbed flag (fidelity list). The unit's hardpoints' attached models keep their own light scale (they are objects of their own; unverified). `live_session.shield_flashes` counts the flashes started. |

### Model projectiles and hit particles (EAWR-456)

| ID | Rule |
|---|---|
| BP-60 | A projectile whose type is neither a beam nor a kite (BP-01) and names a `Space_Model_Name` is drawn as that model, like any object: at its position, turned by its facing triple (roll 0, pitch, yaw) through R-ROT-01 (Rz(yaw) Ry(pitch) Rz(+90), so the model's -Y nose points along the facing) at its `Scale_Factor`. Its model's particle proxies run as any object's (BP-40 admission). Of the M2 projectiles only `Proj_Ship_Concussion_Missile` is one: `W_concussion_missile.alo`, a `missile` mesh (`MeshGloss.fx`) and a `p_concussion` trail proxy, scale 1. The Acclamator's `HP_Acclamator_Weapon_FC` and both level-1 stations (`HP_Rebel_Station_One_CCM`, `HP_Empire_Station_One_00`) fire it. The proton torpedo is a kite (`Projectile_Custom_Render` 2) and loads no model. PB-01, PB-63; XML, model data. |
| BP-61 | A homing projectile faces its own yaw and pitch (space-damage MS-02 to MS-05; pitch positive downward), so a missile turns as it homes. Any other projectile flies along its facing (BP-02), so its step gives it: the yaw is the step's heading in [0, 360), the pitch its negated elevation. Remake rule between ticks, as for units: the position lies straight between the two ticks' positions, the yaw turns the short way and the pitch moves between the two (`presentation::space::interpolate_projectile`). |
| BP-62 | Remake rule: the viewer draws each model projectile on a placed ship from a pool of 32 per projectile type, set up with the population. A projectile keeps its slot for its whole flight; a slot its projectile left stays hidden for one frame before another takes it, so the trail it ran stops before the model shows elsewhere. A projectile that finds every slot taken is not drawn (`battle_effects.projectile_models.<type>.refused`). Model projectiles follow BP-09's visibility. The trail runs in `UnitEmitters`; a stall's catch-up sample poses the slot from that tick's snapshots. When the projectile's flight ends its proxies stop at once, as BP-44 stops a unit's; whether FoC's removal of the projectile lets the trail drain (particle-system-detach D-02) is unverified. `battle_effects.projectile_models_drawn` counts the drawn frames, `populate.live_units.projectile_slots_drawn` the slots composed. |
| BP-63 | A hit the shield did not take whole spawns, after its detonation particle (BP-11), one entry of the target type's `Damage_Hit_Particles` at the contact: the entry's index is a uniform draw in [0, n - 1] from the synchronized random generator. A hit the shield took whole spawns, after its absorb particle, one entry of `Shield_Hit_Particles`, drawn the same way, at the same place and facing (BP-20). Both tags are lists of object types; the remake reads every such tag of the type and splits each on commas. Each entry attaches to the hit bone when it sets `Particle_Attach_To_Collision`, as the detonation does; the viewer places both free (fidelity list). No M2 unit type names either list. PB-60 to PB-62; XML. |
| BP-64 | Remake rule: the viewer's draw for BP-63 never touches the simulation's random streams. It is a fixed 64-bit mix of the hit's projectile ID and the list (damage or shield), taken modulo the entry count (`presentation::space::hit_particle_pick`), so every run and viewer shows the same entry for the same projectile. The projectile is the one in flight at the end of the tick before the hit from the event's shooter and weapon whose position is the event's origin. When none matches (launched and spent within one tick, or that tick has left the snapshot history), a mix of the event's tick, shooter, weapon, target and origin stands in (`battle_effects.hit_picks.by_projectile`, `by_event`). FoC's draw also advances its synchronized stream; the remake's simulation does not model that draw, so a type that names these lists would shift FoC's later draws but not the remake's (fidelity list; none in M2). |

### Shield shell (EAWR-427)

| ID | Rule |
|---|---|
| BP-22 | A unit's SHIELD sub-object (the first named `SHIELD`, case ignored, BP-17) is hidden by its ALO: the four M2 ships' SHIELD meshes carry the hidden flag. The game shows it only while the unit's `DEFEND` ability runs: turning the ability on or off sets the sub-object's code-hidden state to the ability's inverse, and a loaded game restores it the same way. Nothing else shows it: no hit, no shield strength and no faction reaches it (the shield behaviour only switches its collision, BP-19, and starts BP-21's flash). PB-20, PB-21; model data. |
| BP-23 | The SHIELD submesh draws with its authored material. On the Nebulon-B and the Acclamator that is `MeshShield.fx` t0/t0_p0: additive ONE/ONE, no depth write, depth test LESSEQUAL, no culling, no fog. Per vertex it forms three UV sets from the authored UV: Tex0 = BaseUVScale x UV, Tex1 = DistortUVScale x UV, Tex2 = WaveUVScale x UV, each scrolled along v by the effect clock times its own rate (BaseUVScrollRate, DistortUVScrollRate, WaveUVScrollRate). The diffuse is the vertex colour x saturate(N.z + EdgeBrightness) x Color (N the model-space normal), saturated as a colour output. Per pixel it samples the distortion texture at Tex2 and the energy texel (BaseTexture) at Tex0 + 0.25 x distortion.rg. It then multiplies that texel by the wave texel sampled at Tex1 and by the diffuse. The source crosses the wave and distortion sets (WaveUVScale drives the distortion lookup); the viewer keeps that. Nebulon-B: Color (1, 1, 1, 1), EdgeBrightness 0.1, BaseUVScale 16, WaveUVScale 1, DistortUVScale 1, rates -0.15, -0.15 and -0.25, textures `shield_color.tga`, `NB_ShieldWave.tga`, `NB_ShieldRipple.tga`. The Acclamator's are the same except EdgeBrightness 0.5. The colour comes from the textures, so both factions' shells are the same blue. The Corellian corvette's and the Tartan's SHIELD submeshes are `alDefault.fx`, and neither type has DEFEND. PB-22; shader source, model data. |
| BP-24 | Of the M2 types the Nebulon-B and the MC80 have `DEFEND` (`Expiration_Seconds` 15, `Recharge_Seconds` 60 on the Nebulon-B and 40 on the MC80, `Supports_Autofire`). The MC80's model names `MeshShield.fx` with the Nebulon-B's shield textures (model data; its shell parameters are not listed in BP-23). A retail rig recording (RC-427-01, fog off, Highest) of a Tartan firing at a Nebulon-B shows the shell at +10.1, +12.7, +19.8 and +22.2 s into the duels. It also shows it in a still with no shot in view. It does not show it at +0.5 s, before any hit, or at +29.4 s, after 15 s of DEFEND. What makes the game fire DEFEND on its own is unverified. The remake's simulation has no ability state yet (EAWR-76). The viewer composes the shell for every live unit whose type has `DEFEND` (`populate.live_units.shield_shells`) and shows it while `LivePose::defend_active` is set. Until EAWR-76 feeds that flag, `--eawr-live-defend on` sets it for every unit; the default is off, the least visible choice. The shell's clock is the presentation clock in seconds (FoC's effect clock is not recovered; only the scroll phase depends on it). The shell is not part of a unit's pick box. |

### Deaths

| ID | Rule |
|---|---|
| BP-14 | A destroyed unit spawns its type's `Death_Explosions` particle at its last position and facing (`Large_Explosion_Space` on the Nebulon-B, `Large_Explosion_Space_Empire` on the Tartan). The breakup is the death clone's (EAWR-363). XML. |
| BP-15 | A destroyed hardpoint spawns its `Death_Explosion_Particles` at the hardpoint's point on the unit (space-hardpoints), as an object of its own: facing the unit's facing, at that particle type's own `Scale_Factor` (1.0 for `Large_Explosion_Space`), not the unit's. The Nebulon-B's weapon and engine hardpoints all name `Large_Explosion_Space`, the same explosion as the unit's own death. XML, PB-46. |
| BP-16 | A particle object lives `Particle_Lifetime_Frames` frames (30 for the large explosions); then its system detaches and its particles drain. XML. |

### Unit emitters: engines and hardpoint damage (EAWR-394)

A space unit's model carries particle proxies (sub-objects that run a particle ALO on a bone).
FoC shows or hides them by two routes: the hardpoint states (PB-40, PB-41, the [hardpoint state
art](../asset-formats.md#hardpoint-state-art) study) and the model's emitter types, which are
proxy-name prefixes (PB-42). The remake runs them in `UnitEmitters`
(`apps/viewer/src/unit_emitters.cpp`).

| ID | Rule |
|---|---|
| BP-40 | Each admitted proxy of a unit the local player sees runs at its bone on the unit's drawn pose: the bone's bind frame composed with the unit's model transform, set every drawn frame. Particles already emitted keep their own world positions unless the particle system's translater links them to the emitter, so a moving emitter leaves its trail behind. A linked particle (the Emitter translater; every engine glow, as `pe_corvetteengines` and `pe_nebulonengines`) keeps its position in its emitter's frame: FoC's renderer places it with the emitter's transform as it is drawn, so it turns with the ship as well as moving with it and stays on its nozzle through a turn (FoC debug build, EAWR-433). Its velocity stays a world vector: FoC's position update rotates an object-space acceleration by the emitter's current transform once, adds it to that world velocity, and turns the velocity into the emitter's frame only to move the position (FoC debug build, EAWR-439). Admission is the static space map's (authored visibility, alternate and LOD tags, particle-system references). Remake rule for the moving frame; the admission is the attached plan's. |
| BP-41 | A hardpoint's damage emitters are the proxies on its `Damage_Particles` bone and below it. They are hidden when the unit is created and shown when the hardpoint is destroyed; there is no health-fraction trigger (a damaged hardpoint looks intact, space-hardpoints HD-04 / G-H5). They loop until the unit dies. On the Nebulon-B each destroyed weapon hardpoint starts one `p_hp_stardestroyer_damage` (smoke and fire) at its `HP_*_EmitDamage` bone, the destroyed engines two at `HP_E_EmitDamage`. A destroyed hardpoint whose type sets `Engine_Death_Hide_Engine_Particles` also hides the proxies below its `Engine_Particles` bone (no FoC XML sets it). PB-40, PB-41. |
| BP-42 | Engine emitters are the proxies whose names start with `pe` (compared case-insensitively over the prefix, in the model and its attached sub-models): the Nebulon-B's `pe_nebulonengines` on its `engines` bone, the Tartan's `pe_tartanengine_sml` and `pe_tartanengine_lrg`. Their particle systems are EnhancedMesh emitters: they emit from the surface of the first mesh on the proxy bone's parent (the Nebulon-B's `engines` mesh, the Tartan's `engines_small` and `engines_big`), bound as the static attached plan binds it, and that mesh's frame moves with the unit like the emitter's. The space locomotor shows them in every service while the unit's engines are online and hides them once the engines are permanently off-line (space-hardpoints HD-11: the last engine hardpoint destroyed). A destroyed Nebulon-B engine hardpoint therefore stops its engine particles and starts its two damage emitters, besides its `Large_Explosion_Space` (BP-15). PB-42, PB-44. |
| BP-43 | The emitter types FoC hides when it creates an object stay hidden: turbo engines (`pte`), power to weapons (`pptw`) and missile shield (`pgw`). The turbo engines replace the engine emitters while `TURBO` or `SPOILER_LOCK` runs, and `pptw` shows while `POWER_TO_WEAPONS` runs ([space abilities](space-abilities.md) AB-31, AB-32, EAWR-76): the corvette's `PTE_Corvetteengines`. The Tartan's `Pte_tartanengine_sml` and `Pte_tartanengine_lrg` would show under `TURBO`, which its data does not give it. Ion-stun (`pi`) and the other types keep their authored visibility; the Tartan's `pi_damage_elec_cap00` and `pptw_ptwsa` are authored hidden. PB-42, PB-43. |
| BP-44 | A unit that is not drawn this frame (hidden from the local player, or gone from the session) runs no emitters: they stop at once, and restart when it is drawn again. Remake rule (U-07). |
| BP-45 | While the engines are online FoC sets the engine emitters' brightness to 0.2 + 0.8 x speed / maximum speed, clamped to 0..1, every service, immediately (a stopped ship 0.2, one at full speed 1); a temporarily disabled engine flickers between 0 and 0.2. The particle renderer multiplies every particle vertex's RGBA by the brightness (b, b, b, b) as it sets the particle up for drawing, so a ship at rest shows dim engine glows and a moving one full ones, as the owner's retail recordings of a Nebulon-B and a Corellian corvette show (2026-09-27). The remake applies it to the engine emitters (`unit_emitters.engine_brightness` reports it); the 0..0.2 flicker is not modelled (M2 has no temporarily disabled engines). PB-44, PB-45, PB-48. |
| BP-65 (EAWR-559) | **Engine boost fades out.** When the turbo swap ends (`TURBO` or `SPOILER_LOCK` switched off, AB-31) the turbo engine emitters (`pte`) are hidden and the normal ones (`pe`) show; the same swap runs the other way when the mode starts (**unverified** for the start direction and for fighters, whose engine glow drains when `SPOILER_LOCK` starts: only the corvette's switch-off was recorded, the rest follows from the same mechanism). A hidden engine emitter stops emitting, but the particles it already emitted are drawn until they are gone, whatever the particle system's leave-particles flag (the corvette's `pte` and `pe` systems clear it): after `TURBO` ends the big orange flame block gives way to the normal engine glow with a faint red trail of the old flame behind it for about a second (rig recording, still 1.2 s after the switch-off, 2026-09-29, retail image). Before this rule the flame vanished within three ticks. Only the swap drains: the engines going permanently off-line (BP-42) still stop their emitter at once (unverified, no recording); a ship that is not drawn stops at once (BP-44). A drain is released when it is empty, or after 10 s. The speed itself already ramps: AB-24. `unit_emitters.engine_drains` reports the started, finished and cut-short drains. Evidence: rig recording (stills); debug build for the drain of a hidden proxy (BP-48). The lifetime of the trail is the particle system's own; the recording gives only its look (**unverified**: its length to the frame). | rig recording; BP-48 |
| BP-46 | A hardpoint's `Model_To_Attach` proxies are not run (none of the M2 attachments has one). A death clone runs its own model's proxies (BP-47 to BP-50). Remake scope (fidelity list). |

Remake rule for unit emitters after a presentation stall (EAWR-406; not FoC behaviour, FoC steps and
draws on one thread): a frame that reaches many ticks at once runs each 30 Hz sample it catches
up on at its own presented tick, as a paced frame would have. The ship's pose and visibility come
from the snapshots of that tick, and the hardpoint and engine states from its newer one. So a
hardpoint destroyed inside the stall starts its damage emitters at the tick its state changed,
they are aged from there, and a moving ship leaves its engine and smoke trail along its path.
The emitter clock starts like the battle effects' (at the first frame, or at the birth of the
oldest tick it reaches). The brightness (BP-45) is not replayed per sample: FoC and the remake
multiply it into the particles only when they are set up for drawing, so only the drawn frame's
value can be seen. A sample whose snapshots have left the history (64 ticks) has no known pose:
no ship runs emitters over it (least visible, as BP-44), and they start again at the oldest
sample the history holds, so a damage emitter born earlier is aged only from there
(`unit_emitters.unknown_samples`, `start_log`).

Remake rule for frames between samples (EAWR-433): the emitters advance on the 30 Hz clock, but the
viewer draws faster. A frame due no sample draws every running unit emitter again at its own
drawn pose, camera and brightness without advancing it: the linked particles (BP-40) follow the
emitter to that frame, and the streams are built again (`EffectRegistry::present`,
`unit_emitters.presented_frames`, and per started emitter `start_log[].presented`). Before, such a frame showed the last sample's glow on a model already
posed one frame on, a jitter on every moving thruster. Death clone proxies, battle effects and
breakoff props still draw only on samples (fidelity list).

### Death clone emitters (EAWR-421)

A death clone (unit-animation UA-07) is a model like any other: its proxies are sub-objects on its
bones, and its death clip moves and hides those bones. That is where a capital ship's burning
pieces come from; the unit's `Death_Explosions` (BP-14) is only the first fireball. The remake
runs them in `UnitEmitters` as well.

| ID | Rule |
|---|---|
| BP-47 | Every sub-object of a model, meshes and proxies alike, is given its bone's transform in the current animation pose each time the pose is evaluated, and is hidden while that bone is invisible in the pose (a skin reads its visibility bone). So a clone's proxies ride the pieces its death clip throws and share their visibility. PB-49. |
| BP-48 | A particle proxy whose host is hidden stops emitting; the particles already emitted live on (they drain), and the group keeps updating while it has visible particles. When the host shows again the group is reset and emits afresh. A group whose host is hidden and that has no particle left does not update at all, so a proxy hidden from the start (the explosion proxies, bind-hidden) emits nothing until its bone first shows. This is the V-03 to V-05 rule of particle-attachment-visibility, with the drain whatever the system's leave-particles flag; every M2 clone effect sets that flag anyway. PB-50, PB-51. |
| BP-49 | A proxy whose hidden flag is set in the ALO is created hidden and never shown; one whose name carries `_ALT`/`_LOD` follows the alternate and LOD selection. No M2 clone proxy has either. PB-52. |
| BP-50 | The M2 clones' proxies and their bone visibility in the `DIE_00` clips (30 fps; the clone model's data): the corvette's `rv_corvette_d` (86 frames) carries four `p_rebelsmokedeath` (smoke and fire) and one `p_debris01` (a fragment cloud), shown from frame 0 to the last frame their piece is visible (74, 80 and 83), and three `p_explosion_big00`, each shown only in that last frame of its piece. The Tartan's `ev_tartancruiser_d` (86 frames): four `p_imperial_midshipsmoke` and one `p_debris01` to frame 74 to 83, three `p_imperial_explosion_big00` in their pieces' last frames. The Nebulon-B's `rv_nebulonb_d` (152 frames): ten `p_rebelsmokedeath` and two `p_debris01` to frame 129 to 147, and `p_explosion_big00` in three of its pieces' last frames (a fourth is never shown). The Acclamator's `ev_acclamator_d` (151 frames): eight `p_imperial_midshipsmoke` and one `p_debris01` to frame 129 to 148, three `p_imperial_explosion_big00` shown from frame 129, 139 and 148 to the end. The stations' `rb_station_01_break` (1051 frames) and `eb_station_break_break_01` (1001 frames) run fire billows, electrical damage and chains of explosions over some 30 s and a huge explosion as each section goes. ALO/ALA data. |

Remake rules for death clone emitters (EAWR-421):

- Each proxy runs one `particles::AttachmentLifecycle` (reset policy, BP-48: a piece that shows
  again drops its old drain and starts afresh, EAWR-429) at its bone's frame in the
  clone's drawn death pose (the clip frame and 0.5 s blend of UA-08) composed with the clone's
  model transform, one step per 30 Hz sample of the emitter clock, with the bone's visibility in
  that pose. A stall's catch-up samples pose the clone at each sample's own tick, as the unit
  emitters do, a clone that fades within the stall included: its ship is retired only after the
  frame's emitters ran the samples it still stood in (EAWR-429).
- A hidden proxy's drain keeps reading its bone's frame (V-06).
- When the clone leaves (UA-08's fade, or its clip could not start), what still runs stops
  emitting and drains where it last stood; FoC's removal detaches the groups the same way
  (unverified for the clone; the stations are the only M2 clones that leave).
- A proxy whose system emits from a mesh (EnhancedMesh: the stations' fire billows, explosion
  chains and electrical damage) emits from the first mesh on its bone's parent, bound as the engine
  emitters bind theirs (BP-42), with that mesh posed by the clip like the proxy.
- The particle lifetimes are the renderer's (the V1 age randomizer is on the fidelity list, EAWR-406),
  so the drains may last somewhat longer than FoC's.
- `unit_emitters.death_clones` reports the clones planned, the instances started and hidden per
  proxy, the proxies not run with the reason, and a start log.

### Death debris sprites (EAWR-434)

The small pieces that fly off a dying unit are particles, not models. ALO and XML data; the cell
rule is from the FoC debug build (rendering.md, "V1 particle size and bump lighting").

| ID | Rule |
|---|---|
| BP-51 | Every small death piece of an M2 type is a billboard of `p_particle_master.tga`, an 8 x 8 atlas numbered row by row from the top left, with white vertex colour and alpha blending (selector 2): cell 5 (a cluster of hull fragments), cell 6 (a plate above a thin strut with limb-like struts, which reads as a tumbling figure from a distance), cell 7 (one hull chunk) and cell 13 (a lattice girder). No M2 death effect names another texture for them, and the atlas holds no crew figure. The clone smoke also sheds cell 4, a white branching crack drawn additively. |
| BP-52 | The sources, with full-width sizes: `p_debris01` (one per piece group on every capital clone, two on the Nebulon-B) bursts 20 of cell 5 and 15 of cell 6 at size 10 (spread 0.52), living 2 s and fading over the last 0.38. The `debre` emitters of the explosions burst cell 7 shrinking from their size to 0: `p_explosion_big00` (`Large_Explosion_Space`: the corvette's and Nebulon-B's death and their clones' piece explosions) 20 at size 10 for 4 s; `p_imperial_explosion_big00` (the Tartan's and Acclamator's clone pieces) the same; `p_explosion_empire_big00` (`Large_Explosion_Space_Empire`: the Tartan's and Acclamator's death) 6 of cell 7 and 6 of cell 13 at size 10 for 3 s. The clone smoke (`p_rebelsmokedeath`, `p_imperial_midshipsmoke`) emits 2 of cell 6 per second at size 15 for 3 s, and cell 4 sparks (the rebel ones 5 per second for 1.5 s, white to cyan-green to orange; the imperial ones 15 per second for 0.5 s, white to orange). |
| BP-53 | Stations: `Huge_Explosion_Space` (`p_explosion_huge00`) bursts 20 of cell 7 at size 10 and `Huge_Explosion_Space_Empire` (`p_explosion_empire_huge00`) 20 at size 15; the break clones' explosion chains add cell 7 at size 7 and cell 5 at size 11 (`p_explosion_chain`, `_chain4`, `_chain5_e`) or both at size 7 (`_chain3`, `_chain3_e`, `_chain_e`). Fighters: `Small_Explosion_Space` (X-wing, Y-wing; `p_explosion_small00`) bursts 15 of cell 7 at size 7 and `Small_Explosion_Space_Empire` (TIE interceptor; `p_explosion_empire_small00`) 20 at size 5, both for 1 s. |
| BP-54 | A particle's cell is the integer part of its UV track at its relative age; the `debre` tracks (7, then 2 at the end, mode 2) hold cell 7 all their life. So the sprites differ by faction and size class, not by ship: rebel capital deaths shed chunks and fragment clusters, imperial ones fewer chunks with girders, all with the same fragment art. |

The remake draws these emitters as the data gives them (EAWR-434 compared texture, cell, size and
tint with the owner's corvette video and the EAWR-81 rig stills; they match).

### Breakoff props (EAWR-391)

A hardpoint can throw a piece of the ship off when it is destroyed. The piece is an ordinary game
object with a debris behaviour, not a particle.

| ID | Rule |
|---|---|
| BP-30 | A destroyed hardpoint whose type names a `Death_Breakoff_Prop` creates one object of that type, owned by the neutral player, at the hardpoint's world position: the translation of its `Attachment_Bone` on the ship's current model, or of `Fire_Bone_A` when it has no attachment bone. This is the same point its `Death_Explosion_Particles` start at. The object takes the ship's facing triple. The hardpoint's model is removed in the same step (asset-formats, "Hardpoint state art"). The damage path always does this; the health-setting path that passes the suppress flag does not. PB-30, PB-31. |
| BP-31 | A `SpaceBehavior` `DEBRIS` object is serviced every logical frame. Each service adds `Debris_Movement_Vector` to its position in world axes (source units per frame, whatever its facing) and adds `Debris_Facing_Rotate_Vector` to its facing triple (x roll, y pitch, z yaw, in degrees). The facing is drawn by the R-ROT-01 transform (p1-effective-environment), so the piece tumbles about all three axes. PB-32, PB-33. |
| BP-32 | The lifetime is whole seconds. `Debris_Min_Lifetime_Seconds` and `Debris_Max_Lifetime_Seconds` are read as whole numbers. At creation a draw from the synchronized random generator in [min, max], both ends included, sets the expiry frame to the creation frame plus logical frames per second times the draw. With a maximum of 0 or less, or a draw below 1, the object never expires. PB-34, PB-35. |
| BP-33 | At creation a debris object that is not already dead attaches its `Debris_Attached_Particle` (for example `Space_Debris_Fire_Large`, `p_fire_big00.alo`) at its root bone, so the fire rides and turns with it. The fire's own `Particle_Lifetime_Frames` (3000) outlasts every M2 debris lifetime. PB-34. |
| BP-34 | When the expiry frame is reached, the debris service destroys the attached fire and gives the object lethal damage. It then dies like any object: it spawns its `Death_Explosions` (`Medium_Explosion_Space` for the Nebulon-B pieces) and is removed (`Remove_Upon_Death`). PB-33. |
| BP-35 | M2 breakoff hardpoints (FoC XML). The Nebulon-B's four weapon hardpoints each throw their own turret piece (`Hardpoint_Breakoff_Nebulon_Weapon_FL`, `_FR`, `_BL`, `_BR`; the model is the hardpoint's `Model_To_Attach`; the pieces drift at plus or minus 0.2 sideways and -0.5 down per frame, 15 to 25 s). Its engines throw nothing. The Acclamator's FL, FR, FC, BL and BR weapons each throw a piece; its BC weapon, engines and fighter bay do not. The Rebel level-1 station throws pieces from its comm array, TBL and shield generator. The Empire level-1 station throws pieces from its comm array, CM, LC, TBL and shield generator. The Corellian corvette and the Tartan have no destroyable hardpoints. XML. |
| BP-36 | Retail check: with Nebulon-B hardpoints destroyed one by one in FoC (stills fog off), burning turret pieces leave the ship and drift down and apart from it, still burning some 10 s after the last hardpoint died. The stills are too sparse to show a piece's end explosion (BP-34 rests on the debug build). RC-391-01. |

Remake rules for breakoff props (not FoC behaviour):

- The viewer draws each prop and never tells the simulation about it, so the session's hashes do
  not change. A prop spawns when the local player saw its ship at the tick of the hardpoint's
  death. It starts from that tick's snapshot pose. When that snapshot has left the history (a
  presentation stall longer than 64 ticks), what the player saw at that tick is unknown and no
  piece is thrown: the least visible choice, never a later frame's visibility (EAWR-401). Its clock
  starts like the other events' (born at presented tick t - 1), and the pieces' effect clock
  starts at the first frame or at the birth of the oldest tick that frame reaches, as the
  battle effects' does. A fire or explosion whose particle lifetime is over by the frame that
  reaches its birth is not started (EAWR-401).
- A frame that reaches many ticks at once handles their breakoff events oldest first; the pieces
  whose lifetime ran out before an event's birth end (with their explosions) before that event
  is handled (EAWR-401).
- The lifetime draw is keyed on the event (unit, hardpoint, tick) instead of the synchronized
  random generator, so every run shows the same lifetime.
- The attachment point is the unit tables' attachment bone in its bind frame (the same point the
  hardpoint's explosion uses). FoC reads the bone on the current, possibly animated, pose.
- The fire is removed at once when its prop expires; the explosion covers the moment.
- If a hardpoint is repaired and destroyed again while its first piece still flies, the second
  piece is not shown.

## Cases

| ID | Input | Expected |
|---|---|---|
| BP-C1 | A Nebulon-B front-left laser shot (`Proj_Ship_Turbolaser_Red`) in flight | one kite, width 5 before depth scaling, colour (238, 71, 54, 255)/255, cell (2, 0) of `p_particle_master.tga` |
| BP-C2 | A Tartan laser shot (`Proj_Ship_Medium_Laser_Cannon_Green`) in flight | one white beam of `W_Laser_Pill.tga` cell (3, 0), length 12; no model |
| BP-C3 | A Tartan shot hitting the Nebulon-B while its shield holds | `Projectile_Shield_Absorb_Medium` where the shot's line enters the Nebulon-B's SHIELD mesh, its local +Z along that triangle's normal turned towards the Tartan |
| BP-C6 | A shot the Rebel station's shield takes whole | the absorb particle where the shot's line meets the station's collision meshes, its local +Z back along the flight, its local +Y horizontal |
| BP-C4 | The same shot once the shield is down (armor multiplier above 0.75) | `Medium_Damage_Space` at the contact |
| BP-C5 | The Tartan's hull reaches zero | `Large_Explosion_Space_Empire` at its last position, detaching after 30 frames |
| BP-C7 | A Nebulon-B in the live session, engines online | one `pe_nebulonengines` emitter at its `engines` bone, following the ship |
| BP-C8 | Its `HP_Nebulon_Weapon_FL` destroyed | `Large_Explosion_Space` at the hardpoint, then one `p_hp_stardestroyer_damage` looping at `HP_F-L_EmitDamage` |
| BP-C9 | Its `HP_Nebulon_Engines` destroyed | `pe_nebulonengines` stops; two `p_hp_stardestroyer_damage` loop at `HP_E_EmitDamage` |
| BP-C10 | A Tartan in the live session | `pe_tartanengine_sml` and `pe_tartanengine_lrg` run; its `Pte_` proxies do not |

## Event timing in the remake

The remake's presentation runs apart from the simulation thread, so one frame can reach several
ticks at once. These are remake rules, not FoC behaviour:

- An event of tick t is born at presented tick t - 1, the moment the frame starts moving towards
  t (as the death clones' clocks do); a frame that reaches several ticks ages each effect only by
  the 30 Hz samples after its own tick.
- A projectile hit is shown when the local player saw its target in the last frame the target
  was in the session, so the lethal hit, whose target leaves the session that tick, shows too.
  The impact stands at the contact point the hit event carries.
- The events come from the session's event log, kept apart from the snapshot history: after a
  presentation stall every hit and death still plays (FoC steps and presents on one thread, so a
  hitch there delays events rather than dropping them). The log keeps only what the view
  presents (projectile hits, hardpoint and unit destructions; not orders, acquisitions or shots)
  and is bounded twice: ten minutes of ticks and 16 MiB of records, counted as the record
  headers plus 40 bytes per destruction and 104 per hit on x64, so at worst 16 MiB plus the
  heap's per-block overhead (below 22 MiB) however many shots a battle fires. Should a bound
  drop ticks before a frame reached them, the gap is reported (`live_session.event_gaps`).
- Effects keep battle time, also when the session ran ahead of the first frame: the effect clock
  starts at the oldest event that frame reaches, each effect is born at its own tick and aged by
  the ticks since, and one whose lifetime is over by the frame that reaches its tick is skipped
  (`battle_effects.expired`) rather than spawned. A death after a stall shows at the unit's last
  drawn pose.

## Unknowns

- U-01 (answered 2026-09-27): `B1 -> B2` points against the flight (BP-02), from the debug build
  and the owner's FoC play; the beam's pill is point-symmetric, so only the kite shows it.
- U-02: FoC hides a projectile by the fog at its own position; the remake has no fog grid in the
  live session (space-visibility, fidelity list) and uses the shooter's or target's visibility.
- U-03 (answered 2026-09-27, EAWR-415): the rule for models with a SHIELD sub-object is BP-17 to BP-19. The
  M2 ships all have one; the earlier reading turned the facing's yaw by 90 degrees, but the debug
  build turns its pitch (BP-18).
- U-04: a debris object has `Tactical_Health` 100 and `Is_Decoration` No. Whether FoC's shots hit
  it, whether it blocks movement, and how fog hides it (it belongs to the neutral player) are
  unknown. The remake's pieces are presentation only and are shown while they fly (fidelity
  list).
- U-05: whether the fire destroyed at expiry vanishes at once or drains is not settled; the
  remake removes it (the explosion hides the moment).
- U-06 (answered 2026-09-27): the engine brightness (BP-45) reaches the particles in the renderer's
  per-particle set-up, which multiplies the vertex colour by it (PB-48), as the retail recordings show.
- U-07: what FoC does with a unit's running emitters when it dies (the death clone takes over) or
  when the fog hides it: the remake stops them at once (BP-44), the least visible choice.
- Not drawn yet: detonation and
  shield-absorb particles following their target (`Particle_Attach_To_Collision`),
  `Explosion_Jitter_Factor`, sounds.
- U-08 (EAWR-427): what makes the game fire a unit's `DEFEND` on its own (`Supports_Autofire`), and
  so when the shield shell shows in a real battle, is unverified. The viewer draws the shell only
  on request (BP-24).
- A hit the shield took only in part draws its detonation at the event's contact on the collision
  box. In FoC that contact is on the SHIELD mesh as well (BP-19), because the mesh is collidable
  whenever the shield is up.
- Hyperspace arrival is a 149-frame scripted locomotor move (hidden for its first 25 frames, 35
  for reinforcements, then flown in at the cinematic hyperspace distance per frame, faction arrival
  sound at frame 35; PB-13). It moves the unit, so it belongs to the simulation; the M2 session has
  no arrival yet and the viewer draws none.
