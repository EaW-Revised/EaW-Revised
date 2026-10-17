# Galactic conquest: galaxy presentation and front end

This FoC walk covers what the galactic mode draws and plays, and the galactic
conquest (GC) front end: the galaxy scene and its layers, planet models and
colours, trade-route lines, fleet route lines, corruption and destruction
effects, galactic music and event sounds, the campaign setup screens, the
save/load menus, the battle loading screen, the battle summary seen from a
campaign and the campaign end. It records the debug build, the effective stock
XML and the dialog resources inspected on 2026-10-07. The earlier rows claim no retail observation; WGP-42 separately identifies the supplied reference reviewed for the wave-6 camera request. Evidence IDs EGP-01..20 resolve to private research kept outside the
committed tree.

The original gap census below records the initial implementation baseline.
Later sections identify their comparison bases separately; the presentation
readiness section records the current camera/audio providers and remaining
loading-clone seam. XML names below are the mod-facing names.

## Scope and boundaries

Objects are the galaxy art object, planets, galactic fleets, trade routes and
their line objects, corruption particles and lines, and the dialogs of the
front end and of the galactic-to-tactical hand-over. Rules are ordered by the
entry point that runs them: scene construction, per-frame render service,
object state changes (ownership, corruption, travel), music changes, then the
dialogs in the order a player meets them.

These neighbours own the inside of their subjects; this walk links them:

- HUD, input, camera, radar, advisor and history: [the UI/sensors walk](gc-ui-sensors.md),
  WGUI-01..92. Camera flights and lighting interpolation are WGUI-57..65; the
  planet alternate model index (overview **0**, focused **1**, undiscovered
  **2**) is WGUI-05/60.
- Travel, conflicts and the tactical hand-over: [the movement walk](gc-movement-control.md),
  WGM-07..82. Map choice, tactical victory set-up and the tactical side of a
  space battle from the galaxy are the gc-tactical walk (WGT) (legacy EAWR-1961);
  auto-resolve is the gc-autoresolve walk (WAR) (legacy EAWR-1959).
- Corruption, sabotage and planet destruction as actions: [the corruption/heroes walk](gc-corruption-heroes.md),
  which defers their presentation here.
- Session setup, save and load: [the session/save walk](gc-session-save.md),
  WGS-01..68. Story plots and story dialogs are the gc-story walk (WST)
  (legacy EAWR-1960).
- Tactical audio, music streams and mixing: [the audio walk](audio.md),
  SND-01..72. Tactical results: [the battle-flow walk](battle-flow.md),
  WBF-42..50.

## Galaxy scene

| ID | Rule | Source |
| --- | --- | --- |
| WGP-01 | Galactic mode builds one world scene plus three layer scenes: **background**, **midground** and **midforeground**. The world scene's ambient colour is code **0.05, 0.05, 0.05**. Light 0 takes its heading about source Z and elevation from the **Y and Z** components of `Galactic_Zoom_Out_Light_Angle` (stock **0 160 58**, degrees; X is ignored). The wiki tutorial "How to change the Galactic Conquest view?" shows 0 90 70 for this tag; that is its example value, not stock (stock XML reads 0 160 58), so the stock value stands, intensity code **0.5**, diffuse white; lights 1 and 2 get intensity **0**. Zoom flights later interpolate these values (WGUI-62). The source vector toward the light is `(sin heading × cos elevation, −cos heading × cos elevation, sin elevation)`; the two components must not be swapped. | debug build EGP-01, EGCAM-01; XML EGP-15 |
| WGP-02 | The galaxy backdrop is authored as a planet-class object, `Galaxy_Core_Art_Model`: model `w_galaxy00.alo` with an idle animation, position **0,0,0**, scale **1.0**, `In_Background` yes, `Camera_Aligned` no, `Show_Name` no, behaviour IDLE only, credit value **0**. Every stock campaign lists it first in `Locations`, so the backdrop exists only because the campaign places it. Galactic particles author layer flags (`In_Midground` for the planet corruption particles, `In_Midforeground` for the corruption line particle). Layer routing is WGP-01b; the three scenes are verified. | XML EGP-15; debug build EGP-01 |
| WGP-03 | Each render-service call first services camera effects with the real elapsed time. Then, while the galactic mode is paused and `Animate_During_Galactic_Mode_Pause` is false (stock **no**), the shared render service receives **0** elapsed time: model animation and particles freeze during pause. While a cinematic plays, the cinematic system drives the mode camera. Rendering binds the mode camera before the shared mode render. | debug build EGP-02; XML EGP-15 |
| WGP-01a | Shared rendering draws background, midground, midforeground, then world, clearing depth between scenes. Each nonworld scene temporarily uses camera depth interval **10..100000**, then restores the previous world interval. | debug build EGP-19: shared object-manager render; private receipt `out/research/gc-scene-boundaries-2/decompile/index.json` |
| WGP-01b | Object scene selection tests `In_Background`, `In_Midground`, then `In_Midforeground`: the first true flag selects that layer; no true flag selects world. Model insertion uses that selected scene. | debug build EGP-20: object scene selection and model insertion; private receipt `out/research/gc-scene-boundaries-4/decompile/index.json` |

## Planets

| ID | Rule | Source |
| --- | --- | --- |
| WGP-04 | A planet draws `Galactic_Model_Name` at `Galactic_Position` scaled by `Scale_Factor`, with its idle animation looping (`Loop_Idle_Anim_00` yes on all 65 stock entries) and `Pre_Lit` no. Stock: **64** planets plus the backdrop, **37** distinct planet models, scale **1.8** on 50 planets, every position at Z **10** (the scroll plane, WGUI-57). `Mouse_Collide_Override_Sphere_Radius` (stock mostly **19.77**) is the pick sphere; `Name_Adjust` and `Show_Name` place the name label. The alternate model index is WGUI-05/60. `Facing_Adjust` is registry foc-ignores. The reader of `Camera_Aligned` (yes on every planet) is U-02. | XML EGP-15; registry EGP-15 |
| WGP-05 | Planet geometry colour uses the planet's **apparent** faction for the local observer (WGUI-08), not its actual owner: primary colour = faction `Color`; secondary = faction `Display_Font_Color`, or white **255,255,255,255** when the caller asks for the plain variant. | debug build EGP-03 |
| WGP-06 | The planet icon update reads the apparent faction. A null apparent faction changes nothing (the previous colours and icon stay). Otherwise it records that faction as the icon faction, sets the geometry colours as WGP-05 (secondary = `Display_Font_Color`), sets the command-bar planet affiliation icon to the faction's `Icon_Name` and marks the icon as present in the world. | debug build EGP-03 |
| WGP-07 | The planet behaviour's per-faction fleet-icon add is **empty** in the inspected build: it looks up three factions by name and draws nothing. Orbit fleet markers therefore come only from the HUD (WGUI-28, WGUI-92), not from a model attached to the planet. | debug build EGP-08 |
| WGP-08 | Destruction to ruins first clears corruption: a corrupted planet is set to no corruption and its corruption visuals are refreshed (WGP-17 removes particle, lines and paths) before the corruption-change handler runs. With the destroy flag it also notifies the story system and sets the destroyed flag. Stock `Destroyed_Galactic_Model_Name` is `W_planet_Asteroids.alo` on 63 planets (Alderaan's live model is already that model and authors none). The planet-destruction ability authors `Target_Particle_Effect` `Planet_Explosion_Galactic`, a **30**-frame decoration particle. The model swap to the destroyed model is U-03; the action itself is the corruption/heroes walk. | debug build EGP-04; XML EGP-15 |
| WGP-186 | The focused planet station retains its attachment translation and unit scale, and accumulates a positive source-Z rotation in radians: angle += r × `Galactic_Zoom_In_Station_Rotation` (stock **0.04**). Its ordinary caller is the enabled galactic HUD render-service tail, after radar updates. GFC-47 qualifies **r = max(0, (float(s1) − float(s0)) × logical FPS / 1000)**, where s0/s1 are synchronizer frame samples within the outer iteration; r is neither wall seconds nor milliseconds nor galactic-mode frames. At logical FPS 30, one admitted frame gives r=0.03 and stock angle increment 0.0012. A zero input skips the ordinary pose write; insertion forces a zero-input pose write. Insertion/removal do not reset the accumulated angle. Galactic-only pause freezes the mode but does not pause this synchronizer; synchronizer pause refuses admission and gives no rotation advance. WGM-119 speed settings alter target admission FPS, independently of fixed logical FPS and mode movement multipliers. Replay fast-forward uses a separate synchronizer admission policy. Admission uses an integer-millisecond period, floor(1000/target FPS), with at most one advance per outer call and no full-period catch-up backlog (GFC-47). The viewer retains only sub-period carry and defaults to target 30 (33 ms period). Production rate and synchronizer-pause inputs remain unwired to their real providers; only the fixture exercises synchronizer pause. Profile target-FPS integration remains open under WGM-119 and physical render admission under GFS-U10. Attachment placement is GHUD-S78; `Galactic_Zoom_In_Station_Offset` does not replace the authored bone. | debug build enabled HUD render tail, station pose update, admission/begin/end steps and trigonometric rotation helper; XML data; GFC-47 |
| WGP-182 | The planet-organisation (zoomed ground) display picks one of the land-art meshes in the organise GUI model by the planet's `Zoomed_Terrain_Index` (an integer; read by the command bar's organisation update) and hides the rest. The stock model has **13** such meshes, indices **0..12**, each mapping a cell of one of two land-art textures; stock planets author **0..12** (index 0 on 15 planets). Authoring an index with no mesh is unverified. The wiki pages on adding land art say the vanilla model has 12 such objects and that a 16-slot replacement exists, and count the new mesh as 13; the stock model therefore has one more mesh than the wiki's count (its example adds the next index, 13). The wiki's claim that index 8 is a blank cell is not checked. | debug build F2170-E09; XML data, stock model mesh names |
| WGP-09 | The selected planet's galactic cursor is the one model named in code, `w_planet_select_neutral.alo` (stock meshes: two inner rings and two dotted outer rings, MeshAdditive, and a MeshAlpha darkening disk), loaded hidden when galactic mode starts. The data also ship Empire, Rebel, pirate and ring variants that neither code nor XML names: they are not used. The cursor is added when a revealed planet is selected and again on every planet-data refresh while the galactic camera is zoomed out; otherwise, and when the selection clears, it is removed. Adding it puts it in the galactic scene, starts its idle animation from the start (looping) and sets its light scale to the faction `Color` (RGB, alpha **1**) of the planet's owner, or of the `Neutral` faction while the local player cannot see the planet's ownership; an unchanged faction keeps the running cursor. Its transform is rebuilt each time at unit scale on the planet: Z points from the planet to the camera, X = (0, 0, 1) × Z and Y = Z × X, each normalized. Hover highlight is WGUI-80. `GUI_Planet_Flash_Level` (**10**) and `GUI_Planet_Fade_Duration` (**0.5**) have registry rows but no verified reader (U-02). | debug build EGP-17 (galactic command-bar mode start, planet selection, planet-data refresh, cursor add/remove); XML EGP-15 |

WGP-05/06 implementation boundary: the inspected debug-build geometry-colour
path consumes only the secondary colour, updating an existing HUD name. It does
not consume the primary colour. The stock planet surfaces use Planet.fx and
MeshAdditive, which have no unit-colourisation input in the verified adapters.
Retain both decoded affiliation colours for the pending planet HUD seam (legacy EAWR-1155); this
path does not establish a material or glow tint. Other retail affiliation
indicators, including radar or ownership rings, remain an open research item.

## Trade-route lines

| ID | Rule | Source |
| --- | --- | --- |
| WGP-10 | A trade-route line style has a name (upper-cased) and six relation slots. `Settings_For_Faction Name=` **Enemy, Ally, Good, Evil, Corrupt** select their slot; any other name selects **Neutral**. Inside a slot, a value `TBD` is ignored with a warning, an empty value is ignored, and an unknown key warns. Stock keys are `Color_Zoomed_Out`, `Color_Zoomed_In`, `Width` (**3.0** everywhere) and `Render_Mode` (**ADDITIVE** everywhere). The stock Enemy and Good zoomed-in colour is authored `128, 0 0, 255` (a comma is missing); how the colour parser reads it is U-04. | debug build EGP-05; XML EGP-15 |
| WGP-11 | A route's relation type compares the owners of its two endpoints. Both present, neither neutral, not enemies and the **same faction object**: if the player profile asks for red/green faction colours, an endpoint owner allied to the local player gives **Ally**, otherwise **Enemy**; else a faction whose name equals `Good_Side_Name` (stock REBEL) gives **Good**, `Evil_Side_Name` (EMPIRE) gives **Evil**, any other faction **Corrupt**. Every other case is **Neutral**, so a route between two different factions, or with a neutral or empty end, is drawn neutral. | debug build EGP-06; XML EGP-15 |
| WGP-12 | Creating a route's line takes the style slot of its type, maps the slot's render-mode name, removes old sparks and draws one line between the endpoint positions with the slot's width, texture and spark texture. Its first colour is `Color_Zoomed_Out × zoom + Color_Zoomed_In × (1 − zoom)`: the **opposite** weighting to WGP-13. A successful line becomes visible and, unless the type is Neutral, gets sparks. A type update recreates the line (same scene and ends) only when the type differs from the current one. No stock XML authors the texture or spark texture, so their defaults are U-04. | debug build EGP-06 |
| WGP-13 | A zoom update acts only when the zoom value changes and a slot is current: colour = `Color_Zoomed_Out × (1 − z) + Color_Zoomed_In × z`; sparks are on when the type is not Neutral **and** z is not exactly **1**. The zoom value is stored even when nothing else changes. Fidelity note: keep both weightings as they are; a created line shows the swapped blend until the next zoom change. The caller that supplies z is U-04. | debug build EGP-06 |
| WGP-14 | Sparks: remove all and stop animating; when on, use the line colour × **0.7** and add **8** sparks at signed positions **0.15, 0.4, 0.55, 0.89, −0.45, −0.7, −0.5, −0.89** (the sign gives the direction), each with **50** and size **3.5 × 3.5**, then animate at rate **10**. All values are code. | debug build EGP-06 |
| WGP-15 | A route starts with style name `DEFAULT` (code) before its XML is parsed, then takes its **own copy** of the named style, so each route's line keeps its own zoom, type and sparks. Stock data: **329** routes, of which **15** name `Visible_Line_Name` `DEFAULT` explicitly; WGP-10's upper-casing matches it to the one stock style `Default`. A name with no style is U-04. | debug build EGP-05; XML EGP-15 |

## Fleets in the galaxy

| ID | Rule | Source |
| --- | --- | --- |
| WGP-16 | A galactic fleet is the `Galactic_Fleet` container: model `w_planet_fleet.alo`, `Pre_Lit` yes, scale **1.0**, `Show_Name` no, `Layer_Z_Adjust` **0.5**, radar icon scale **200** in space. A type with `Galactic_Fleet_Override_Model_Name` (stock only the Death Star: `w_planet_fleet_ds.alo`, `Galactic_Fleet_Override_Icon_Index` **4**) replaces that model; when the override applies (any member, the strongest member) is U-05. A jump order recolours the fleet model with its **owner's** faction `Color` (unit colourisation), whatever the jump's outcome. | XML EGP-15; debug build EGP-07 |
| WGP-17 | A travelling fleet keeps a route line only while it has a destination planet **and** is locally visible (WGUI-30/31); otherwise the line is deleted. The line runs from the fleet to its immediate destination object (or, without one, the locomotor's desired position), pulled back toward the fleet by the fraction `destination planet model box X-extent ÷ distance`. Colour = the fleet **owner's** faction `Color` (actual owner, unlike WGP-05), width code **2.0**, texture `Fleet_Movement_Line_Texture_Name` (stock `w_galaxy_line.tga`), additive, in the world scene. The route length is stored with it. | debug build EGP-07; XML EGP-15 |
| WGP-18 | The drag band of a fleet order uses `Fleet_Hyperspace_Band_Texture_Name` (stock `w_galaxy_line.tga`); its geometry and cancel rules are WGUI-52/77. Faction `SFXEvent_Exit_Into_Hyperspace` and `SFXEvent_Arrive_From_Hyperspace` (stock `Unit_Ship_Hyperspace_Exit` / `_Enter`) are the travel sounds. The jump start itself starts no sound or effect; whether departure or arrival play them, and any jump visual, is U-05. | XML EGP-15 |

## Corruption and galactic effects

| ID | Rule | Source |
| --- | --- | --- |
| WGP-19 | While a planet is corrupted **or** a corruption transition is active, it carries exactly one attached particle of type `Corruption_Particle_Name` (stock `Heavy_Galactic_Corruption_Particles`, midground layer); an attached particle of another type is destroyed and replaced. Otherwise any attached particle is destroyed. An attached particle is then configured from the planet's corruption state (`Particle_Brightness/Scale/Energy_Per_Corruption_Level`, stock brightness **0.3, 1.0**; the mapping is U-06). | debug build EGP-09; XML EGP-15 |
| WGP-20 | While corrupted or corruption is increasing, a planet without lines visits every **other** planet adjacent to it by galactic path. A corrupted neighbour gets a corruption line from this planet when this planet has no active transition. A neighbour that is corrupted or increasing gets a corruption path: always when this planet's transition is active, otherwise only when no path between them exists. This runs only when the active mode supplies the scene the lines live in. | debug build EGP-09 |
| WGP-21 | When a planet is not corrupted, each of its corruption lines is removed from the other planet too (paths as well unless a transition is active), the line's particle object is destroyed and the list cleared. Without an active transition every path line is taken out of the scene and deleted. Stock line and path data: line particle `Galactic_Corruption_Line_Particles` (midforeground), `Corruption_Line_Radius` **12.5**, `Corruption_Line_Start_End_Offset` **18**, `Corruption_Line_Grow_Seconds` **7**, `Corruption_Path_Color` **36,36,0,255**, `Corruption_Path_Width` **16**, `Corruption_Path_Offset` **0,0,0**. How these shape a line or path is U-06. | debug build EGP-09; XML EGP-15 |
| WGP-22 | Galactic one-shot effects are particle types placed at a planet: `Sabotage_Particle_Effect` and the destruction ability's target effect are both `Planet_Explosion_Galactic` (galactic model `p_explosion_empire_small00.alo`, **30** frames); `Hack_Super_Weapon_Particle_Effect` is `IG88_Death_Star_Hack_Explosion` (galactic model `p_explosion_galactic_underworld.alo`). Their spawn point and layer are U-06; the actions are the corruption/heroes walk. | XML EGP-15 |

## Galactic music and sounds

| ID | Rule | Source |
| --- | --- | --- |
| WGP-23 | Galactic ambient music is the **local player's** faction `Strategic_Map_Music_Event` (stock Empire, Rebel and Underworld galactic map events; 2 s fade-in, 2 s fade-out of the previous track, volume 60, looping playlists). No local player or faction gives no event. There is no superweapon override in this selector, unlike tactical SND-50. | debug build EGP-10; XML EGP-15 |
| WGP-24 | Starting galactic ambient music requires the strategic sub-mode. Unless forced, it does nothing while the current music mode is strategic ambient, the tactical superweapon attack sequence, the demo finale splash or scripted music, or while demo attract runs; so returning to an already playing galactic track does not restart it. Otherwise it starts WGP-23's event in the strategic-ambient music mode. | debug build EGP-10 |
| WGP-185 | Battle-result summary activation selects global `Music_Event_Battle_End_Summary_Screen_Win` or `_Lose` from the local result, in a distinct summary music mode. Missing events leave the current music unchanged; stock authors both blank. Closing a campaign battle summary requests unforced strategic ambient music (WGP-23/24), for auto-resolved and fought battles alike. This result-screen cue is separate from campaign-end strategic win/lose music (WGP-25). | debug build result-dialog activation/deactivation and galactic resume trace; stock audio XML |
| WGP-25 | Campaign-end music needs a winner, a loser and the local faction. When the winner **is** the local player (by ID, not by alliance), play the local faction's `Music_Event_Strategic_Win_Vs_Faction` for the loser's faction unless already strategic-win; otherwise, unless already strategic-lose, its `Music_Event_Strategic_Lose_Vs_Faction` for the winner's faction. A missing event plays nothing and leaves the current music. The lose lookup takes the first `_Vs_Faction` entry for that faction in source order and otherwise the plain `Music_Event_Strategic_Lose` (also for a null opponent); the win lookup likewise takes the first matching entry, otherwise the plain `Music_Event_Strategic_Win`, including a null opponent. Repeating the same outcome mode does nothing. | debug build EGP-10, targeted strategic result/faction getter trace; XML EGP-15 |
| WGP-26 | Battle-pending and battle-load music share one event: faction `Music_Event_Tactical_Space_Battle_Pending` / `_Land_Battle_Pending` and audio `Music_Event_Battle_Load_Screen` all name `Tactical_Battle_Loading_Loop_Event` (volume 55, looping, `MSS_Internal_Loop` yes so the stream keeps looping while loading starves the music service). The pending sounds are faction `SFX_Event_Tactical_Space_Battle_Pending` / `_Land_` (stock `RHD_` / `EHD_Battle_Pending`). The local player's battle alert selects the pending SFX and music by land/space and starts both when present. Activating the battle-load screen selects the audio load event; normal activation keeps pending/load streams alive but kills other active music first. Starting the already active event leaves that stream and its mode unchanged (SND-53), including the stock pending-to-load transition. The low-memory path suppresses load music, and a tutorial player clears active music before starting it. | XML EGP-15; debug build battle alert and load-screen activation trace |
| WGP-27 | Galactic 3D audio uses `Audio_Galactic_3D_Saturation_Distance_Mod` **1.0**, `_Rolloff_Distance_Mod` **1.0** and `_Listener_Z_Pullback_Dist` **0.0**; mode end restores the rolloff (WGS-67). The normal listener uses the same camera-to-plane projection and height clamp as SND-12 with the galactic pullback plane; orientation is flattened camera forward with downward up. Cinematics use the camera position. A missing camera falls back to position zero, forward up and up along source Y. The galactic event sounds are faction data emitted by the movement, economy and corruption rules: planet gained/lost control, planet corrupted (`GUI_Corruption`), enemy fleet approaching, planet revealed (audio `GUI_Planet_Mouse_Over`, WGUI-05), capture bonus (`Unit_Light_Saber_On`), population cap, credit cap, mission added, new construction, starbase upgraded, blockade. This walk owns only the data-to-sound mapping. | XML EGP-15; debug build galactic listener and camera projection/orientation trace; interfaces WGM/WGEP/WGCH |
| WGP-181 | The galactic fleet move acknowledgement follows the local player's move order after the interface has queued it. Orders the interface refuses first are silent: a fleet that needs an ability to move, an access-restricted destination, or a raid-only fleet aimed away from its raid target. A short or same-planet drag cancels; that cancel plays the negative-feedback cue only when the cursor is more than about 141 px from the start planet. The acknowledgement is skipped while the galaxy is paused, though the order still goes out. One object speaks: the best-ranked selected object under BA-20's ranking and its quirk, where a fleet stands for its best-ranked member, including transported and flagship contents. Its type's `SFXEvent_Fleet_Move` plays, or its ordinary move event when that tag is absent. Galactic moves have no group, asteroid/nebula or assist-chain variants. **Ours:** the cue follows the session's accepted movement of a local-seat fleet, ranking the fleet's members; a later simulation refusal stays silent. The cue is unattached and transported contents are not searched. Squadron containers resolve their ranking and move response through the first authored `Squadron_Units` craft (BA-24); build responses remain on the container. Non-hero-ability substitutions of the ordinary move event are not applied. | debug build GC3075-E01..E10; BA-24; effective squadron/craft XML |
| WGP-182 | Galactic command-bar buttons each play only their own component's sounds; there is no global default. `Click_SFX` plays on a real left release unless the button is disabled, hidden or dragging. `Right_Click_SFX` needs a right-click handler. `Mouse_Over_SFX` plays once per hover entry, and only from the normal state, so disabled or selected buttons are silent. The objectives rebuild with announce plays the objective icon's click SFX, and the teletype text plays its component's mouse-over SFX on each reveal step. **Ours:** component-authored click and mouse-over on command-bar, fleet/ground, radar, black-market, hero/weapon/corruption menus, reference-page opener and pending-battle buttons; right-only queue cancellation uses `Right_Click_SFX` instead of `Click_SFX`. Hidden and disabled buttons are silent, with no shared default cue. Named `GalacticAudio::objectives_rebuilt` and `teletype_revealed` seams read the objective icon click and tutorial-text mouse-over fields, with synthetic qualification. Live story/objective publication remains open in the story-notification publication work (legacy EAWR-2042); callers own reveal cadence and GFC-40 suppression. | debug build GC3075-E14..E18 |
| WGP-183 | Galactic production speech and fallback SFX follow WGEP-96/97 and WGP-63 for the **local** builder only. Underway plays on the admitted build-button submission, before the request; completion and cancellation play on the session's completed/cancelled notification when the entry's recorded builder is local. Stock types mostly author only the SFX (for example `RHD_Build_Vehicle`, `Unit_Complete_Nebulon`, `RHD_Unit_Canceled`). Non-local speech admission (WGEP-96) is not played: the observer may not hear another player's production. The strategic completion planet ping is not part of this cue. | WGEP-96/97, WGP-63; effective XML |
| WGP-187 | Presentation notifications and the frame already drawn can differ. Our sound adapter obtains the latest session frame after reading its event batch, so a move following same-slot fleet merges ranks the current combined members. Explicit queue cancellation identifies its local supplying player directly; retained build identity still qualifies completion and automatic cancellation. Reports distinguish requested events, decoded playback starts and allocation failures. | Project adapter policy; WGP-181, WGEP-96; native live-campaign and production audio GPU cases |
| WGP-184 | Dialog gadgets use fixed event names, not command-bar data: `GUI_Common_Dialog_Button_Press` on an active press, `GUI_Common_Dialog_Button_Hilight` on cursor entry over the button part, and `GUI_Common_Dialog_Listbox_Entry_Select`. Galactic fleet icon drags play `GUI_Drag_Fleet` once the held icon moves more than 12 px, and a same-owner fleet merge drop plays `GUI_Drop_Fleet`. **Ours:** GC battle, reference and save/load dialogs opt into the existing UI sound router for active button/check/radio presses and hover, list/combo selection, and slider minus/plus presses and hover (track/thumb and programmatic slider updates stay silent); the fleet drag plays `GUI_Drag_Fleet` when the GUI drag activates (its activation distance, not the 12 px threshold, is the WGUI-U04 seam) and `GUI_Drop_Fleet` on a merge drop. | debug build GC3075-E11..E13, E19..E20 |

## Front end

### Planet corruption objects in a fought space battle

| ID | Rule | Source |
|---|---|---|
| WGP-188 | Every existing player's planet-specific upgrade list is a separate tactical creation family. At a space conflict, create each entry with a matching faction affiliation and `DUMMY_UPGRADE` behavior for that layer at zero position and zero facing, owned by that player. Automatic corruption upgrades and mission rewards use this family; they do not search a station's purchasing menu or become fleet members. A child return preserves the strategic entitlement and discards its temporary tactical holder. | debug build CB-E01; WGT-118; WGCH-162/163; authored faction and upgrade data |
| WGP-189 | A corruption upgrade's `Bounty_On_Faction_Ability` contributes a source-owned percentage/category for its authored target factions, with optional allied recipient propagation. At a hostile final-blow death, each ally of the killer receives its own faction-filtered category maximums summed, multiplied by the victim's current-type bounty, rounded to nearest whole credit with minimum one when both bounty and percentage are positive. Zero override suppresses payment; negative or absent override uses the nonnegative maximum authored category default for the actual single-player or multiplayer tactical mode. The credits use the ordinary credit adjustment and cap, while collected bounty records the rounded payment. Source removal removes only its contribution. Our campaign tally is saved in child state domain 9 and returned for the local observer; campaign replay setup domain 3 replaces domain 2. | debug build CB-E02..04; WGCH-65/164; authored `Default_Bounty_By_Category_SP/MP`, `Override_Bounty_SP/MP` and nested bounty fields |

The native contracts cover same-category overlap, separate categories, an allied
source, a nonparticipant modifier owner, source death before another payout,
credit caps, collection disablement and checkpoint continuation on 1/2/4/8
workers. Installed space entry checks three faction-owned racketeering objects
at the origin and preserves the nonparticipant's treasury on return. No new
periodic entity service is introduced: payments use the existing ordered death
notifications, and retained modifier sources are inspected only on those events.

Nearby strategic death gates, auto-resolve bounty and per-kill fly text remain
the distinct WGCH-166/WAR-60 and WGCH-LC-U03 seams. Collection enablement is a
content provider input; the live story toggle is not added by this change.

| ID | Rule | Source |
| --- | --- | --- |
| WGP-28 | The GC front end is a set of resource dialogs: main menu (load game, single player, multiplayer, options, tutorial, credits, exit); single-player menu (new story campaign, custom campaign, skirmish, a hint list); custom-campaign staging (map list with official/custom map buttons, preview image, description box, easy/normal/hard radio buttons, faction drop-down, options, start, cancel); campaign advanced options (credits slider, starting and maximum tech-level sliders, allow-auto-resolve drop-down, random-events check box, defaults, accept); save and load (game list, name field, delete; load can show recordings); in-game options (audio, video, gameplay, keyboard, network); story-campaign selection (difficulty and two faction buttons). Their layout is retail data the dialog parser already reads. | dialog resources EGP-16 |
| WGP-40 | Opening custom-campaign staging: the dialog is centred with its close button hidden; the custom-maps button is disabled; the credits option takes the profile's last multiplayer credits (when set) and the game timer the profile's last timer; the map list is filled by the shared staging list for the campaign game type (U-08); difficulty defaults to **normal**; the description shows the selected entry's description or stays empty; the faction drop-down lists every **playable** faction by display name and, with nothing selected, selects the **last** playable faction. Opening it issues `Main_Menu_Music_Event` (stock the Consortium theme, volume 80, looping) in the main-menu music mode. If the same event is already playing, its track and loop phase continue; only a silent player starts it (FES-03). | debug build EGP-13 (event on open); XML EGP-15; owner retail observation (2026-10-12, continuous music on staging open/Back) |
| WGP-183 | The faction list on the campaign start screen is drawn in two combo boxes whose width is a layout constant in the executable's dialog resources, not data; a wiki tutorial on widening the faction list of the galactic conquest start screen enlarges those widths in the interface resources. It is a presentation limit of the original, not a rule: the remake's front end must list every faction of a custom campaign without truncating or scrolling away long names. Not checked in the debug build (resources are outside the code walks). | wiki "How to expand the factions list..."; unverified |
| WGP-29 | Starting a custom campaign: nothing happens without a list selection. The selected entry is matched by **localized display name** (`Text_ID`) against every listed campaign, first equal wins; with no match the last campaign visited stays selected. Then clear the menu backdrop, stop demo attract, take the faction by index. With custom options on, the starting and maximum tech levels are raised by the faction's displayed-tech-level adjustment. Apply the options and the multiplayer game type. Single player: start the campaign of the same `Campaign_Set` and the chosen faction, solo, at the chosen difficulty. The other branch starts the selected entry, gives AI control (the opposite faction's basic AI name and the difficulty) to players of the other faction, assigns the starting active player and refreshes visibility. | debug build EGP-13 |
| WGP-30 | Stock listing data: FoC lists five campaign files; its GC entries are `Campaigns_Underworld_GC.xml`: **6** scenarios × 3 factions = **18** entries grouped by `Campaign_Set`, with `Sort_Order` 1..6, `Text_ID` / `Description_Text`, `Show_Completed_Tab` true, `Supports_Custom_Settings` true and campaign camera `Camera_Distance` **1200**, `Camera_Shift_X` **40** or **600**, `Camera_Shift_Y` **0** or **−320**. How the list is filtered and sorted and what the camera fields drive are U-08 (with WGS-U02). | XML EGP-15 |

## Galactic to tactical and back

| ID | Rule | Source |
| --- | --- | --- |
| WGP-31 | A campaign battle's loading dialog takes the planet object (it must behave as a planet) or a location type. The dialog shows the **actual owner's** faction: the title `"<prepare text> <planet display name>"`, the same order as the skirmish title BLD-02 (prepare text `TEXT_PREPARE_FOR_LAND_BATTLE` or `TEXT_PREPARE_FOR_SPACE_BATTLE` outside missions; the printf arguments were re-read from the disassembly 2026-10-12, an earlier reading had them reversed) and the faction name, both in the owner's `Display_Font_Color` (white without an owner), plus the planet's **basic** credit income formatted `%4.0f` ("0" without a planet object). Weather, terrain, population and wildlife come from the `Describe_*` texts; tactical information (colour **50,200,255**), advantage and history fill three non-selectable lists (a skirmish shows only history). Mission battles add `MT` / `MB` + upper-case map name text keys, showing the key itself when the text is missing. Icons: weather icon with `i_pa` replaced by `load`, map preview `.ted`→`.dds` under the textures folder (hidden when absent), `Native_Icon_Name`, code overlay `load_overlay.tga`. | debug build EGP-11 |
| WGP-32 | The loading dialog renders the planet itself: a clone of the planet's current model (or the type model scaled by `Scale_Factor`) on its first animation type, alternate model **1**, light scale 1, rotated **150°** about Z. Its camera framing uses `Battle_Load_Planet_Direction` (stock **0, 0.75, 0.55**) and the cloned model's bounds; WGP-51 gives the precise extent-vector factor and unnormalized direction, qualifying the earlier diagonal/distance shorthand. Light 0 intensity **0.5** at angles **−30°, 45°**, lights 1–2 off, ambient `Battle_Load_Planet_Ambient` (**0.1, 0.1, 0.13**); viewport `Battle_Load_Planet_Viewport` (**0, 1, 1, 0**); frustum **±0.25**, near **1**, far **10000**, no clear; looping playback. | debug build EGP-11, GPR-05; XML EGP-15 |
| WGP-33 | Loading a campaign battle resets the command-bar components before the map loads and reports load progress **0.75** after the starting forces exist, **0.9** after the camera points at them, and **1.0** at the end, then removes the progress callback. A failed map load shows a message naming the map. Map choice, the destroyed-planet replacement in the tactical map and the tactical victory conditions are the gc-tactical walk. | debug build EGP-14 |
| WGP-34 | The battle summary in a campaign is the shared results dialog (WBF-42..47) with these campaign rules: the winner is the auto-resolver's winner when the battle was auto-resolved, and the battle time then shows **00:00:00**; the location line shows the location type's display name; the picture is the **local player's** faction `Land_`/`Space_Win_Image` or `_Lose_Image`; a bounty line (`TEXT_SUMMARY_BOUNTY_COLLECTED`) shows when the local player collected a bounty or its faction benefits from corruption; two efficiency figures show enemy combat power lost as a percentage of all combat power lost and its complement; the continue button reads `TEXT_BUTTON_RETURN_TO_GALACTIC` outside skirmish. | debug build EGP-12 |
| WGP-35 | After a won **space** battle in a campaign the summary prepares an invasion offer: take the first planet of the location type and collect the alive transports, with cargo, in the winner's fleets in its orbit. The offer is dropped when the planet's surface is not accessible or the planet already belongs to the local player. M3 keeps the offer but resolves the land battle by auto-resolve. | debug build EGP-12 |

## Campaign end

| ID | Rule | Source |
| --- | --- | --- |
| WGP-36 | The galactic mode's own win/lose message is **empty**: unlike a tactical battle, a campaign shows no overlay message (`Win_Lose_Message_Font_Size` is a tactical value). The campaign end is the score screen of WGP-37. | debug build EGP-18 |
| WGP-37 | The campaign score screen needs a victory with a player. Ordinary multiplayer results only record local quit intent and show nothing; explicit quit can enter the screen under WGS-118. Otherwise it destroys every open dialog and opens the full-screen score dialog with its close button hidden. The headline is `TEXT_VICTORY` when the winner **is** the local player, else `TEXT_DEFEAT`; the list shows the local faction's authored `Victory_Text` / `Defeat_Text`. The title and military, conquest and kill efficiency figures come from scoring-script control results; WGP-97 qualifies their keys, conversion and missing-value display. The quit button reads `TEXT_BUTTON_MAIN_MENU` (multiplayer `TEXT_BUTTON_CONTINUE`). | debug build EGP-18, ECE-01; stock XML ECE-X01; WGS-118 |
| WGP-38 | The score picture is the local faction's win image (winner is local) or lose image, choosing the **space** or **land** variant with a 50/50 draw from the presentation random stream (not the synchronized one), so it never touches simulation determinism. | debug build EGP-18 |
| WGP-39 | Campaign-end faction movie selection requires enabled victory movies and non-multiplayer. The **winner's** faction supplies `Superweapon_Win_Movie` for the last-enemy-planet, faction-leader or superweapon-destruction victory kinds, otherwise `Generic_Win_Movie`. The local callback graph continues through `Finale_Movie`, optional `Finale_Movie_2`, then `Credits`; natural credits completion has a separate `Post_Credits_Movie` consumer. Stock Underworld has no generic/superweapon win movie but authors both finale parts. Shutdown precedes the initial request. WGP-105..111 qualify the enabled gate, empty/failing requests, skip behaviour, stock names and exit ownership; a retained finale does not mean every conquest plays it. | debug build EGP-18, ECE-01..04; XML EGP-15, ECE-X01 |

## Existing behaviour notes

| Existing contract | Comparison | Boundary here |
| --- | --- | --- |
| [UI/sensors](gc-ui-sensors.md) WGUI-05, 57..65 | same alternate-model and camera/lighting interface; missing scene layers, planet colours and route lines there | WGP-01..07, 12..13 |
| WGUI-28..32, 52, 77, 92 | same orbit-marker and band interface; missing world route line and fleet model there | WGP-07, 16..18 |
| [Movement](gc-movement-control.md) WGM-06, WGM-39..82 | same pause/resume music hook and conflict interface; missing loading/summary presentation there | WGP-24, 31..35 |
| [Corruption/heroes](gc-corruption-heroes.md) WGCH-09, 27..31 and its deferred presentation row | same actions; their particles, lines and explosions missing there | WGP-08, 19..22 |
| [Session/save](gc-session-save.md) WGS-04..14, 33, 60, 67, U02 | same campaign start and save interfaces; missing the menu selection and option mapping there | WGP-28..30 |
| WGS-U07 (final victory screen, movie and exit branches) | partly settled here: the score screen, its picture and the win/finale movie choice | WGP-36..39 |
| [Audio](audio.md) SND-50..55, 71, 72 | same music-event execution and stream fades; differs in the selector (local faction galactic event, no superweapon override) and adds strategic win/lose music | WGP-23..27 |
| [Battle flow](battle-flow.md) WBF-42..47 | same results dialog; differs in campaign winner, time, picture, bounty, efficiency and continue text | WGP-34..35 |
| Skirmish loading screen, BLD rules, open work (legacy EAWR-1923) | same dialog builder; differs: campaign battles use the space/land battle dialogs with the owner faction, income and the tactical and advantage lists | WGP-31..33 |

## Our code and what it can reuse

| Area | Ours today | Reuse |
| --- | --- | --- |
| Galaxy scene, planets, backdrop | missing | model loading, animation and the Godot renderer (`src/presentation/godot/renderer*.cpp`, `src/presentation/animation`), sun/environment lighting (`src/presentation/space/environment_scene.cpp`, `src/presentation/lighting`) |
| Route lines and sparks | missing; no line primitive in the renderer | particle/beam rendering paths (`src/presentation/particles`) for additive textured strips |
| Corruption and explosion effects | corruption attachment in the midground; native-tested connection and one-shot lifecycle with producer seams | ALO particle system (`src/presentation/particles/alo_particles.cpp`, `effect_registry.cpp`) |
| Music and sounds | tactical music director and SFX (`src/presentation/audio/sfx.cpp`, `apps/viewer/src/battle_audio*.cpp`) | add the strategic music modes and the galactic listener |
| Front-end dialogs | dialog resource parser (`src/data/ui/dialog_catalog.cpp`, `dialog_script*.cpp`), UI gallery, skirmish setup mode (`apps/viewer/src/skirmish_setup_mode.cpp`) | campaign staging, options, save/load built on the same dialog kit |
| Loading screen | skirmish loading screen in progress (legacy EAWR-1923) | campaign variants of the same model |
| Battle summary | `src/presentation/ui/battle_results.cpp` (WBF-45) | campaign rows of WGP-34/35 |
| Movies | HUD movie player (`src/presentation/godot/ui/movie_player.cpp`, Theora conversion) | full-screen campaign movies |

## M3 scope

The corruption presentation adapter admits a planet only when the displayed local
observer has a discovered record and the published observer predicate says it is
corrupted. This is a conservative project admission policy, not a newly sourced
retail visibility rule. It loads `Corruption_Particle_Name` and that type's
`Galactic_Model_Name` from effective data and draws the authored emitter unchanged
in its authored layer (stock midground, WGP-19). Committed observer-admitted
transition facts supply WGP-47 brightness/size/energy interpolation. WGP-20/21
retain one reciprocal line identity and clear it on removal; WGP-48 paths use
authored endpoints, RGB, width and band texture, while WGP-49 line particles
receive the local pinched-cylinder volume and logical-frame growth. Render
frames between logical updates retain the last submitted growth. Restore clears
presentation resources and reconstructs them from the restored publication.
Degenerate or negative-height line geometry fails closed pending U-06 capture.
Each occupied layer and the line owner independently has a project safety budget
of 262144 reserved particle slots, with at most 4096 per effect. Admission charges
the actual CPU reservation after spawn, so small effects can exceed 64 instances
per owner; exhaustion is visible in the report's `refused_effects` and
`line_refused_effects` counters. These are project limits, not retail limits.
Connection admission compares route endpoints once per update and rebuilds cached
adjacency only when that list changes. It visits at most two neighbor records per
route, with logarithmic identity lookups and retained per-frame lookup buffers.
The native contract checks those work counts at two galaxy sizes; committed
transition admission and rendered endpoints likewise use indexed identities.
The dynamic size/energy API is opt-in; existing tactical consumers retain their
prior scheduling and random stream. Synthetic `corruption-spread` fixtures
exercise drawing, completion, type replacement and cleanup and are never
campaign state. The explicit `CorruptionPublication` seam still carries sequenced
sabotage, destruction and hack descriptors, but their production/rendering remains
separate work. U-06 keeps actual bone transforms, degenerate geometry and retail
pause/load continuity qualified.

M3 needs a playable galaxy, so it needs WGP-01..07 (scene, planets, colours),
WGP-10..17 (route lines and fleet lines), WGP-23..24 (galactic music), the
custom-campaign front end WGP-28..30, the campaign loading screen WGP-31..33
for space battles and the campaign summary WGP-34 for space battles and
auto-resolved conflicts. Corruption visuals WGP-19..22 come with the
Consortium track (S5). Campaign-end music WGP-25 and the campaign-end screen
WGP-36..39 come with victory (S2); the win movies need full-screen movie
playback.

Waits for M4 (playable land battles): the land variants of WGP-31..33 (land
loading dialog for a fought land battle) and the land invasion flow after
WGP-35 beyond its auto-resolve hand-over. Waits for M6 (multiplayer): the
network campaign branch of WGP-29, the multiplayer branch of WGP-37,
multiplayer staging and voting dialogs,
and per-observer presentation of WGP-05/06 for more than one human.

## XML and GameConstants inventory

### Event audio integration seams

The retained live event feed maps ownership changes to local gain/loss sounds,
`discovery_feedback` to one reveal sound per player/service (WGUI-05), and
`conflict_pending` to the involved local player's land/space pending sound and
music (WGP-26/46). The live publication retains the conflict owner's participants
and layer before selection can retire that record; delayed presentation uses an
exclusive cursor rather than polling current conflict state.

Named producer seams remain for campaign outcome (WGP-25 needs a sourced loser
identity, which `victory_matured` does not carry), enemy fleet warning,
population/credit limit notices, mission addition, independent slicer/black-market
construction-option changes, starbase upgrades and blockade
feedback. Their WGP-27 sound mappings exist; state changes or admission failures
do not substitute for an authoritative notification rule.

The main-branch audio audit also finds live local fleet-move acknowledgements,
production completion/cancellation feedback and battle-summary ambient restoration
(WGP-181/185/187). Qualified technology-setter construction unlocks also publish
their local recipient through the retained feed (SND-68/WGEP-130).
These existing consumers are not missing WGP-27 producers. Production/fleet and
command-bar sounds were implemented by the advisor-feedback work (legacy EAWR-3091);
the later dialog/objective presenter work (legacy EAWR-3801) adds separate UI sounds.
WGP-46's load selector now keeps current music when the authored load event is
absent, before any stream stop or mode reset. The engine-free selector covers
pending/load reuse and tutorial/low-memory stop decisions; the viewer uses the
normal branch because tutorial-player and low-memory providers remain absent.
Its deterministic contract tests use renamed opponent factions for the ordered
WGP-25 lookups rather than a stock faction list.

Campaign-end music remains a named WGP-25 seam: the live end view supplies a
winner and its faction, but no authoritative loser. A targeted debug-build check
confirms that both participants are required and the winner's exact player ID
selects win versus lose; faction equality cannot replace that gate. The checked
selector has no direct code caller references, so indirect/script delivery is a
research lead, not a sourced campaign loser provider. A bounded follow-up checked
the pending-result consumers, campaign-end dispatch and score activation: these
do not establish an eliminated-player or other-opponent selection for the missing
argument. Do not substitute the last tactical loser for a campaign outcome.
First-capture reward has a
committed credit-award owner. Due corruption completion now publishes one retained
local owner-or-beneficiary notice after type/visibility/story refresh and before
entitlement rewards (WGP-54/WGCH-75), selecting corrupted versus removed from the
resulting type. Startup installation and ruins cleanup do not use that timed
feedback branch. First-capture feedback follows the actual credited award and
recorded first-capture history (WGP-53/WGM-76), including an award clamped by the
credit cap; it is not a notification for every windfall. The exclusive live
cursor retains each qualified recipient once through skipped presentation frames.
No sound is synthesized from snapshot
differences, generic ability settlement, credit clamps or construction rejection.

Evidence EGP-15 holds the per-file tag tally and the registry comparison
against [the tag registry](../../tag-coverage/statuses.json). This docs-only
walk applies no tags and changes no registry row.

| Tags / constants | Stock value | Registry |
| --- | --- | --- |
| Planet `Galactic_Model_Name`, `Destroyed_Galactic_Model_Name`, `Galactic_Position`, `Scale_Factor`, `Camera_Aligned`, `Name_Adjust`, `Show_Name`, `Mouse_Collide_Override_Sphere_Radius`, `Loop_Idle_Anim_00`, `Pre_Lit`, `In_Background`, `GUI_Model_Name`, `Zoomed_Terrain_Index` | WGP-02/04/08 | land-or-galactic (Planet) |
| Planet `Native_Icon_Name`, `Encyclopedia_Weather_Icon`, `Describe_*`, `Land_Tactical_Map`, `Space_Tactical_Map` | WGP-31 | land-or-galactic |
| TradeRouteLine `Color_Zoomed_Out`, `Color_Zoomed_In`, `Width`, `Render_Mode`; TradeRoute `Visible_Line_Name` | WGP-10..15 | colour tags have **no row**; `Width`/`Render_Mode` rows cover other classes only; `Visible_Line_Name` land-or-galactic |
| Container `Galactic_Model_Name`, `Pre_Lit`, `Layer_Z_Adjust`; SpaceUnit `Galactic_Fleet_Override_Model_Name`, `_Icon_Index` | WGP-16 | land-or-galactic; Container `Pre_Lit` todo (legacy EAWR-650) |
| `Fleet_Movement_Line_Texture_Name`, `Fleet_Hyperspace_Band_Texture_Name` | `w_galaxy_line.tga` | todo presentation (legacy EAWR-653); land-or-galactic |
| `Corruption_Particle_Name`, `Corruption_Particle_Line_Name`, `Particle_*_Per_Corruption_Level`, `Corruption_Line_*`, `Corruption_Path_*` | WGP-19..21 | land-or-galactic |
| `Sabotage_Particle_Effect`, `Hack_Super_Weapon_Particle_Effect`, Planet_Destruction_Ability `Target_Particle_Effect`; Particle `In_Midground`, `In_Midforeground` | WGP-22, WGP-02 | land-or-galactic; todo (legacy EAWR-653); target effect **no row**; layer flags todo (legacy EAWR-650) |
| Faction `Color`, `Display_Font_Color`, `Icon_Name` | WGP-05/06/17 | applied/todo for tactical use; GC use not established |
| Faction `Strategic_Map_Music_Event`, `Music_Event_Strategic_Win/Lose` and `_Vs_Faction`, `Music_Event_Tactical_Space/Land_Battle_Pending`, `SFX_Event_Tactical_*_Battle_Pending`; Audio `Music_Event_Battle_Load_Screen`, `Audio_Galactic_3D_*` | WGP-23..27 | todo presentation (legacy EAWR-653); land values land-or-galactic |
| Faction planet/fleet/hyperspace `SFXEvent_*`; Audio `SFXEvent_Planet_Revealed`, `SFXEvent_Planet_Capture_Bonus_Reward` | WGP-18, 27 | land-or-galactic; `SFXEvent_Arrive_From_Hyperspace` applied (tactical) |
| Faction `Land/Space_Win_Image`, `Land/Space_Lose_Image` | WGP-34 | land values land-or-galactic; space values todo (legacy EAWR-653) |
| `Battle_Load_Planet_Viewport`, `_Direction`, `_Ambient` | 0,1,1,0; 0,0.75,0.55; 0.1,0.1,0.13 | land-or-galactic |
| `Animate_During_Galactic_Mode_Pause`, `Galactic_Zoom_Out_Light_Angle`, `GUI_Planet_Flash_Level`, `GUI_Planet_Fade_Duration` | no; 0 160 58; 10; 0.5 | land-or-galactic |
| Campaign `Campaign_Set`, `Sort_Order`, `Text_ID`, `Description_Text`, `Is_Listed`, `Show_Completed_Tab`, `Supports_Custom_Settings`, `Camera_Distance`, `Camera_Shift_X/Y` | WGP-29/30 | land-or-galactic |
| Faction `Finale_Movie`, `Finale_Movie_2`, `Generic_Win_Movie`, `Superweapon_Win_Movie`, `Post_Credits_Movie`, `Victory_Text`, `Defeat_Text`; Movie `Movie_File`, `Halt_Game_Music`, `Pause_Game_While_Playing`, `Cannot_Skip`, `Text_Crawl_*` | WGP-37..39; WGP-97 | presentation-later (SCOPE-MENU / SCOPE-CINEMATIC); stock authoring includes the text tags and Consortium's second finale. WGP-37's earlier `*TextID` spellings were not XML tag names. This walk does not apply these tags in production. |

Registry notes for a later sourced correction (no lease taken in this walk):
`Color_Zoomed_In`, `Color_Zoomed_Out` (TradeRouteLine) and `Target_Particle_Effect`
(Planet_Destruction_Ability) are read by the debug build (EGP-05/06, XML) but
have no row.

## Unverified and how to settle it

| ID | Unsettled fact | Settles it |
| --- | --- | --- |
| U-01 | Closed: layer routing, draw order and nonworld camera depth interval are WGP-01a/01b | Debug-build EGP-19/20; targeted render, scene-selection and model-insertion receipts cited in those rows. No retail capture is claimed. |
| U-02 | Readers of `Camera_Aligned`, planet label placement, `GUI_Planet_Flash_Level` / `_Fade_Duration` (the selection model is settled by WGP-09) | Read planet model set-up and highlight code |
| U-03 | When the destroyed planet model replaces the live model | Read the destroyed-flag readers; staged Death Star destruction capture |
| U-04 | A `Visible_Line_Name` with no matching style, texture/spark defaults, the zoom value's source, the parse of the comma-less colour, when lines are retyped | Read the route constructor and line callers; capture lines across ownership changes at overview and focused zoom |
| U-05 | Fleet model override condition, galactic jump/arrival sounds and visuals | Read fleet model selection and the jump start; capture a Death Star fleet and a fleet jump |
| U-06 | Capture-needed: WGP-47..50 sources corruption parameter mapping, path endpoints/RGB, pinched-cylinder line inputs/growth, and sabotage/hack attachment/layer routing. | Service-map C-06: two adjacent planets across increase/completion/removal, overview/focus, nested pause, child return and save/load. Record emitter brightness/size/energy, path RGB, line percentage/height/radius and transforms; trigger revealed/hidden human/AI sabotage and hack. Qualify visible attachment and pinched-volume growth, degenerate geometry and restored lines; no retail result is claimed. |
| U-07 | Capture-needed: WGP-44..46 source strategic-win fallback, pending/load callers and galactic listener placement. | Record ordinary and missing/matching-empty opponent music, land/space alert, tutorial and low-memory loading, then camera overview/focus/cinematic listener placement. Verify event transitions and audible results; compare the supplied camera reference only under its documented aspect and home location. |
| U-08 | Partly settled: WGP-65..74 source staging filter/order/profile/support/options and the static story/preview routes; WGS-152..164 source save/load handlers. Runtime profile/mod/network admission, actual first-use tutorial/story flow, non-ASCII/tie presentation and campaign camera consumers remain unqualified. | GHUD-C17 staging/advanced/story captures, including stale profile and unsupported variant; actual first-use tutorial Yes/No and accepted story start; compare camera defaults/overrides separately. Save/load runtime failure/restore recipes remain WGS-U04/U05. |
| U-09 | Partly settled: WGS-116..119 source score buttons/exits, and WGP-97..111 source scoring controls/counters and movie continuations. Actual story/conquest gate persistence, skip/failure presentation and native casualty forwarding retain ECE-U01..03. In-game galactic menu and galactic battle-alert presentation remain outside this campaign-end walk. | Use ECE-U01..03 and WGS-U07 for bounded campaign-end captures; separately inspect/capture the galactic menu and battle alert. |
| U-10 | Retail handling of different rotation keys on matching planet zoom-alternate bones is unverified. Urban01's idle has different body spins for ALT0 and ALT1 at the same time; Kessel's match. The viewer's `synchronize_zoom_rotation` seam shares the overview spin relative to each first authored pose, preserving the focused reference orientation, translation, scale and visibility. Different reference orientations can compensate different mesh-local bases, as in Ryloth. Three local Coruscant inward zoom trials at nominal 15/30/45 seconds show smooth visible body motion: tracked full-body feature pairs have approximately 0.1–1.4 degrees per decoded frame of screen-plane rotation, with no distinct 6-degree impulse. This is consistent with the viewer's continuous result, but camera similarity measurements are not axial bone angles and the capture does not identify the ALT switch moment, so they do not verify the exact retail correction rule. | Local 2026-10-09 Coruscant `idle15`, `idle30`, `idle45` capture set, `capture-report.md` / `index.json`, 60 fps movies and transition PNGs; ignored body tracking measurements in `out/gc-rotation-fix-2/retail-body-flow.json`. Identify the native alternate switch and compare axial body orientation across it to settle the exact rule. Raw images remain local. |

## Gap tickets

All rules are missing in our code (0 same, 0 differs). Sizes use the planning
rate in [the estimate](../../estimate.md) ("Measured recalibration
2026-10-02"); hours are calibrated agent-hours.

| Gap | Rules | Module | Size / hours | Depends on |
| --- | --- | --- | --- | --- |
| G1 Galaxy scene, planets and planet colours: fidelity on the viewer skeleton (legacy EAWR-1994) | 01..09 | new viewer galactic mode on the renderer, animation, lighting | L, rendering/VFX 4.5 h (legacy EAWR-2000) | galaxy model and snapshot (legacy EAWR-680), viewer skeleton (legacy EAWR-1994); stub: static campaign data |
| G2 Trade-route and fleet route lines | 10..18 | new line primitive in the renderer; galactic mode | M, rendering/VFX 2.25 h (legacy EAWR-2002) | G1; ownership (legacy EAWR-1114); stub: fixed owners |
| G3 Corruption and galactic one-shot effects | 19..22 | particles, galactic mode | M, rendering/VFX 2.25 h (legacy EAWR-2003) | G1, G2, corruption state (legacy EAWR-1141) |
| G4 Galactic music and event sounds | 23..27 | music director, SFX | M, UI/HUD 3.25 h (legacy EAWR-2004) | session (legacy EAWR-1149), victory (legacy EAWR-1150); stub: fixed local faction |
| G5 Custom-campaign front end and save/load menus | 28..30, 40 | dialog kit, skirmish setup mode, session setup | L, UI/HUD 6.5 h (legacy EAWR-2009) | session setup (legacy EAWR-1149), save/load (legacy EAWR-681); stub: start a fixed campaign |
| G6 Campaign battle loading and summary screens | 31..35 | loading screen (legacy EAWR-1923), battle results | M, UI/HUD 3.25 h (legacy EAWR-2010) | viewer hand-off (legacy EAWR-1995), skirmish loading screen (legacy EAWR-1923), tactical hand-over (legacy EAWR-1115, EAWR-1116), auto-resolve (WAR); stub: synthetic hand-over record |
| G7 Campaign end: score screen and win movies | 36..39 | dialog kit, movie player (legacy EAWR-237) | M, UI/HUD 3.25 h (legacy EAWR-2011) | victory dispatch (legacy EAWR-1150), scoring script stats; stub: a forced victory |

Tracking: GC presentation walk (legacy EAWR-1962); all seven gaps are sub-issues of the
GC UI and presentation track (legacy EAWR-1952). There are **40 missing, 0 same,
0 differs** rules. Total calibrated effort **25.25 h** (rendering/VFX 9.0 h,
UI/HUD 16.25 h) on top of the viewer skeleton and hand-off tickets.

## Wave 6: initial galactic camera

| ID | Rule | Source |
| --- | --- | --- |
| WGP-41 | The ordinary galaxy overview uses global `GMC_InitialPitchAngleDegrees` (stock **−30°**) and `GMC_InitialPullbackDistance` (**700**). Its transform places the focus, rotates about X by the fixed **90°** basis adjustment and the authored pitch, then translates **local +Z by +700**. The matrix helpers compose these X rotations on the right and translate through the resulting basis; stock world eye offset from focus is **(0,−606.218,+350)**, i.e. **30° above the XY plane**, with no extra camera-transform inversion before storing the world transform. This path adds no yaw rotation and reads neither campaign-selection `Camera_Distance` nor the focused-planet radius multiplier for its normal overview distance. Positioning copies the requested focus coordinates and clamps **only Z** to `Galactic_Scroll_Plane` (stock **10**), then recalculates the transform; the inspected clamp adds no XY bounds or pullback limit. Cinematic and battle-tilt paths are separate. Camera initialization retains existing scroll, recalculates, establishes the scroll plane/clamp and normal state; its campaign argument supplies no initial distance or home position. Fresh construction starts scroll at **(0,0,0)** and no zoom target. | debug build G6-P01; stock XML G6-P02; WGUI-57..60 |
| WGP-42 | Campaign start looks up `Home_Location` for the local faction by the **first case-insensitive faction-name match**, resolves its type and takes the first existing object of that type. For a planet it sets discovery, refreshes local intelligence and requests planet selection with camera positioning, recording it as the home planet. In overview, that selection stores a delayed selection and positions the galactic camera through WGP-41 before completing selection later; it does not enter the focused-planet camera merely to choose a starting home. Missing home/type/object leaves this branch without a selection or first-owned-planet fallback. Stock Equal Footing authors Underworld home **Hypori**, whose position is **(160,−320,10)**. The supplied retail overview shows selected Hypori and matches that location, but its metadata records manual selection before the zoom sequence; it does not establish the exact first rendered startup frame. Later story/cinematic camera requests remain independent. | debug build G6-P03; stock XML G6-P02; supplied retail reference G6-P04 |

| WGP-43 | Galactic world-camera setup supplies frustum inputs left/right **−0.25/+0.25**, top/bottom **+0.1875/−0.1875**, depth **10..10000**, and a full viewport. The frustum helper sets its perspective-angle argument to **atan(right−left)**, here **atan(0.5)**, approximately **0.463648 radians / 26.565°**, and aspect to the absolute width/height ratio, initially **4/3**. Transform refresh reads back that perspective argument and reapplies it with the actual monitor aspect. The projection helper uses X scale `1/tan(angle/2)` and Y scale `aspect/tan(angle/2)`: this is the **full horizontal field of view**, with vertical angle `2*atan(tan(angle/2)/aspect)`. Preserve that argument and update aspect; these values do not authorize a guessed conventional field-of-view default or deriving the angle by dividing by the near depth. | debug build G6-P01/05/09; WGUI-57..60 |

## Wave 6: galactic music and listener

| ID | Rule | Source |
| --- | --- | --- |
| WGP-44 | Strategic win music scans authored opponent-faction entries in order and returns the first matching **resolved faction identity**, including an empty event on a matching entry. A null opponent, empty list or absent match uses the faction's ordinary strategic-win event. It does not substitute an arbitrary opponent's event or fall back after an explicitly matching empty entry. | debug build G6-P06; WGP-25 |
| WGP-45 | Without a cinematic, galactic listener position uses the camera look-ray helper with `Audio_Galactic_3D_Listener_Z_Pullback_Dist` (stock **0**); this parameter is the helper's requested **world Z plane**, not an extra overview-camera distance. The helper intersects the negative camera Z-axis ray with that plane, then caps travel along that axis at the absolute difference between camera Z and requested Z when the intersection is farther. During a cinematic, use actual camera translation and the listener-distance getter returns **0**. Orientation is computed from the camera separately. Missing camera supplies position **(0,0,0)** and orientation vectors **(0,0,1)** / **(0,1,0)** after reporting the invalid state. | debug build G6-P07; stock XML G6-P02; WGP-26 |
| WGP-46 | Battle alert first activates the command-bar alert for its pending location, then selects the **local player's faction** land/space pending SFX and music; each present event starts, with music in the strategic-pending mode. Battle-load dialog activation reads global `Music_Event_Battle_Load_Screen`. With an event and low-memory rendering it kills active music and does not start that event in this branch. Otherwise tutorial status or a previous mode other than strategic-pending/load can kill old music, then the load event starts in load mode. Missing authored events do not manufacture replacement music. Full handoff/playback observation remains U-07. | debug build G6-P08; WGP-24/27/31 |

## S5 preparation: corruption presentation consumers

Research dated 2026-10-09, static debug-build reads and effective XML. No
retail capture is claimed. The
[corruption/hero service map](../gc-corruption-frame-schedule.md) owns the
clock/consumer qualification recipes; simulation action admission remains in
[corruption/heroes](gc-corruption-heroes.md). S5P evidence maps and raw reads
stay in ignored research storage.

| ID | Rule | Source |
| --- | --- | --- |
| WGP-47 | Corruption parameter configuration obtains source values from current type and destination values from post-transition type. Each authored `Particle_Brightness/Scale/Energy_Per_Corruption_Level` list uses element **0 for none**, element **1 for every valid nonempty type**, rather than eight type-specific levels. Invalid type reports an invariant and returns **1**. Desired parameters linearly interpolate source/destination by clamped transition fraction `(frame-start)/(end-start)`; a zero end or equal start/end returns fraction **1**, an inverted interval reports invalid state and returns **0**. The default frame argument **0** reads active galactic time. The emitter receives a brightness-vector setting, size and energy immediately, with zero setting delay. Stock brightness is **0.3→1.0**, size/energy **1→1**. | debug build S5P-01; stock XML S5P-D01; WGCH-51; WGP-19 |
| WGP-48 | Corruption paths are simple additive lines in the active scene, between planet positions with `Corruption_Path_Offset` added to **both** endpoints. They use `Fleet_Hyperspace_Band_Texture_Name`, path width/color and no spark texture. During an increase from none, RGB is multiplied by transition fraction; during removal from nonempty, by **1−fraction**; otherwise factor **1**. Each RGB component truncates to a byte, while alpha is set to **255**. Missing line instances are recreated before applying color. This is RGB modulation, not an alpha fade or a line whose endpoint follows transition progress. | debug build S5P-02; WGP-20/21; XML S5P-D01 |
| WGP-49 | A corruption-line particle object is created neutral-owned at the midpoint, with a counterpart record at the other planet that owns no second particle. Growth starts at sentinel **−1**. At the first emitter update or a forced transform reset, orient its local Z toward the neighbour, translate to the source plus the authored start/end offset, and set cylinder height to centre distance minus **twice** that offset. The caller supplies radius, height and previous percentage to each particle stream's local **pinched-cylinder** emission volume. Then it clamps an already >=1 percentage to 1, otherwise adds `1/(Corruption_Line_Grow_Seconds × logical FPS)`. Stock radius/offset/grow are **12.5/18/7 s**: 1/210 per eligible update at 30 Hz. Submission precedes increment/clamp, so there is no clamp of the newly incremented value in that invocation. An unresolved neighbour hides the emitter. No valid emitter/group means this growth branch does not advance. Degenerate direction, sub-36 separation and exact visible pinch geometry remain capture questions. | debug build S5P-03; WGP-20/21; XML S5P-D01 |
| WGP-50 | Successful prepared sabotage and hack resolve their configured effect type and attach it to the planet without a named bone. The attachment helper creates it with the planet's position, facing and owner, then attaches to bone **0** with zero local offset. The ordinary prepared sabotage path has no local-reveal test for its one-shot; direct AI structure sabotage gates effect and faction sound on local planet revelation. Hack starts its faction event before consuming the hacker and later attaches its effect after ruins/orbit/starbase/neutralization work. Both stock one-shot particle types author **30-frame** lifetime and no layer flag, so WGP-01b routes them to **world**, unlike corruption planet particles in midground and line particles in midforeground. This establishes initial placement/routing, not a captured world transform of animated bone 0 or particle lifetime under pause. | debug build S5P-04; XML S5P-D01; WGCH-30/66/77; WGP-01b/22 |

At comparison base `d7180d985ebf9b85c421ef3dc9d6a09234652296`, these four
galactic consumers are **missing**, not supplied by generic tactical particle
support. New counts: **WGP-47..50: 4 missing, 0 same, 0 differs**. Existing
corruption visual work retains implementation ownership (legacy EAWR-2003);
qualification tracking is the S5 service map and its presentation child (legacy EAWR-2501, EAWR-2510), **M, 2/4/8 h** optimistic/likely/pessimistic agent-hours. WGP-03's
render-pause gate is independent of the logical growth/transition clock.

## Presentation implementation readiness

Read-only debug-build research and effective stock XML/Lua audit dated
2026-10-10. GPR evidence IDs resolve to ignored private receipts. No new retail
recording or audible result is claimed. The companion
[camera readiness rows](gc-ui-sensors.md#presentation-implementation-readiness),
WGUI-123..129, cover camera action routing, reversal/refocus, event scroll and
radar-guide synchronization. Local loading time inputs below are not a global
frame schedule: ordinary outer admission is
[GFS-21](../galactic-frame-service-schedule.md), and loading/return caller
qualification stays with the frame/session programme.

| ID | Rule | Source |
| --- | --- | --- |
| WGP-51 | Loading preview with a planet object clones its **current model**, preserving that model path rather than reselecting an ordinary live-planet asset; without an object, a provided location type supplies its galactic model and the clone explicitly receives type `Scale_Factor`. A nonplanet object rejects this builder. The clone is assigned the first animation **type**, looping from its reset setup, alternate **1**, unit RGBA light scale, and its existing transform composed with **150° about source Z**. Camera target is that transform's translation. With renderable world bounds, framing scalar is **0.5 × length(bounds extent vector) / tan(current preview-camera FOV/2)**; without a renderable, the bounds start as the box from **(−1,−1,−1)** to **(1,1,1)**. Eye is target plus the **raw authored** `Battle_Load_Planet_Direction` times that scalar: no direction normalization occurs. Stock direction has length approximately **0.930054**, so the scalar alone is not the final eye distance. Framing reads FOV before the final WGP-32 frustum assignment. Retain these inputs independently; no native missing-model recovery is established. | debug build GPR-05; stock XML GPR-D01; WGP-32 |
| WGP-52 | Fresh loading activation creates the preview owner and builds its model information, pauses synchronized gameplay and records wall-clock start/previous times. Loop playback requests animation index **0**, blend duration **0**, frame-rate factor **1** and frame-based elapsed-time input **0**. The local loading-render routine samples wall time, computes seconds since its previous **invocation** and since activation, and updates its previous-time stamp before the render guard. Only when time since its last rendered frame reaches **50 ms** does that branch service preview cinematics with total elapsed seconds, render-think the UI and cinematics with the sampled invocation delta, and render/swap the UI scene; the other branch sleeps briefly. These are local inputs/guards, not a promised 20 Hz frame rate or proof that animation advances by each 50 ms interval. Deactivation destroys the dialog, clears the retained planet, shuts down/deletes the preview owner and resumes synchronized gameplay. A later fresh activation constructs a new preview. Complete animation phase, load-stall and callback cadence remain U-11. | debug build GPR-05; WGP-03/32; frame/session provider seam |
| WGP-53 | In the ownership processor, a local new owner requests its faction `SFXEvent_Planet_Gained_Control`. Local old-owner loss requests `SFXEvent_Planet_Lost_Control` only when a nonnull new owner has a **playable faction**; loss to null or a nonplayable faction does not take this sound branch. The selected events come from the **local faction**. Capture-bonus sound comes from global audio `SFXEvent_Planet_Capture_Bonus_Reward` only for the local recipient of WGM-76's first-capture-by-type award, after marking the captured type and awarding credits. None of these requests forces playback or attaches a planet. Ownership mutation/admission remains the movement owner; a semantic owner-change event alone lacks the new-faction playability needed for native loss feedback. | debug build GPR-06; WGP-27; WGM-76 |
| WGP-54 | At an admitted due corruption-transition completion, finalize the type and refresh its visuals before feedback. A resulting empty corruption type requests faction `SFXEvent_Corruption_Removed`; a nonempty type requests `SFXEvent_Planet_Corrupted`. Recipient is the local owner when the planet owner equals the local player, otherwise the designated corruption player **only if that player is local**. This branch does not broadcast to allies or every observer. Discovery feedback separately requests global audio `SFXEvent_Planet_Revealed` once after a qualifying refresh pass that discovered one or more planets and has a positive player argument, not once per discovered planet (WGUI-05/93). Corruption/discovery due clocks and outer service order remain their existing owners. | debug build GPR-06; WGUI-05/93; WGCH-51; WGP-27 |
| WGP-55 | Strategic population warning is an admitted **local-player** service observation: previous stored usage must be **>0**, current usage (including queued production) must be **greater than previous**, and current usage must be **>= current maximum capacity**. Then try faction `SFXEvent_Strategic_Pop_Cap_Reached`, and store current usage whether or not the sound exists or plays. Initial zero→full, an unchanged full count and a capacity-only reduction do not satisfy this local branch. This differs from tactical no-reinforcement-room latching. Fleet-approach feedback uses WGUI-114's local recipient, destination/allegiance/enemy/orbit-presence gates and per-player **5000 ms** wall-clock throttle; that throttle is not a general sound cooldown. | debug build GPR-06; WGUI-114/122; WGEP-48/68/69; SND-39 |
| WGP-56 | Mission-added faction SFX is tied to an admitted triggered story event with nonempty story-dialog data. After adding the dialog goal, an already active summary only refreshes; a requested popup launches the summary. In the remaining nonpopup branch, notify the local faction's transmission text and request `SFXEvent_Mission_Added` **only when the event's dialog-SFX flag is true**. A generic objective insertion alone does not establish this cue (SND-43). Technology-change new-options SFX additionally requires the caller's unlock and notify flags, at least one actual qualifying type unlock and exact local-player identity. Independent slicer/black-market unlock producers retain their own guards. Build underway/completed/stopped speech and fallback/ping precedence remain WGEP-95..98, not a sound for every production-state mutation. | debug build GPR-06; SND-43/68; WGEP-55/95..98 |
| WGP-57 | The inspected galactic planet/corruption/approach/population/mission/new-options producers submit ordinary unforced, unattached SFX with zero requested fade. Shared SND-01..07/16..18 controls authored priority, probability, overlap, instance counts, category/HUD-speech/dialog gates and the **16 2D / 32 spatial** sample pools; no separate GC-wide FIFO, dedup policy or priority cap is established here. Default event priority is **3**, allowed authored range **1..5**, with lower numbers more important. A 2D event at its instance cap refuses admission; full-pool replacement favors least important then oldest, allowing equal-priority interruption. Event admission precedes decoding and allocation, so a successful notification is not proof of sound. SND-61's speech gain ceiling and dialog exception also remain shared. Per-event XML/presets must be resolved through the validated loader rather than assigning one priority or cap to all GC cues. | debug build GPR-06; [audio walk](audio.md), SND-01..07/16..18/61; stock XML GPR-D01 |
| WGP-58 | The direct reader inventory for own/allied/enemy starbase-upgraded faction cues leads to a station replacement ability: after successful next-type creation, state/queue transfer and old-object destruction, choose the own, ally or enemy tag by upgraded owner's relation to local player. All three lookups use the **upgraded owner's faction**, not the local faction; a neutral relation selects none. Requests are unforced, unattached, zero-fade. This reader includes tactical queue replacement and does not establish a cue for a galactic planet base-level mutation. Galactic credit addition/cap refresh and the inspected general production-admission predicate contain no direct SFX request either. Retain those GC producer seams as U-12 instead of deriving triggers from WGP-27's tag inventory. | debug build GPR-07/11; SND-36; WGEP-16/17 |

### Music and loading checklist

Ambient faction/no-restart guards are already sourced by WGP-23/24; exact-local
win/lose and first-match/empty-event fallback by WGP-25/44; pending/load reuse,
tutorial and low-memory guards by WGP-26/46. The three stock galactic playlists
author volume **60**, **2 s** fade-in/out-previous and looping. Shared pending/load
event authors volume **55**, fade-in **0**, fade-out-previous **2 s**, looping and
internal looping. These are data, not hardcoded constants. SND-53/72 owns
sequential event-file cursors and the active/ending stream transition, including
current-level-based fade-out. WGP-27/45 owns the galactic listener. Stock Lua
cinematic and music calls remain script-owned overrides; their existence does
not change the ordinary galactic selector (GPR-D02).

At comparison base `d011385aa199311cc4707d8514befd103cd8ad43`,
`src/presentation/audio/galactic.cpp` and
`apps/viewer/src/galactic/galactic_audio.cpp` already supply semantic cue mapping,
music state and playback adapters; `tests/galactic/audio_tests.cpp`,
`audio_feed_tests.cpp` and the installed-content `test_galactic_audio.py` exercise
parts of that integration. Ownership, discovery and pending alerts have feed
consumers. Other semantic cues still need their native producer metadata and
guards; an adapter or partial tag-registry row is not a producer receipt. The
loading clone remains the separate viewport/model-upload seam (legacy EAWR-2476).
This research changes no game code or tag status.

| Implementer needs | Evidence/status | Provider seam and proposed check |
| --- | --- | --- |
| Ambient/outcome/pending/loading music and transitions | sourced WGP-23..26/44/46, SND-53/72; audible qualification U-07 | Local seat/faction, exact winner/loser, strategic/demo/scripted/superweapon modes and load/tutorial/low-memory flags; table-test each guard, same-event reuse, missing/matching-empty event and a mid-fade replacement. |
| Galactic listener | sourced WGP-27/45; audible qualification U-07/SND-U02 | Live pose and cinematic flag; test requested Z plane and capped projection separately from ear capture. |
| Independent loading clone/model/orientation/framing | sourced WGP-32/51 | Conflict planet's current presentation model or location type, scale, animation, bounds and validated loading constants; GPU case with a marked alternate-1 model, nonunit scale and nonunit direction, proving an independent viewport and changing animation frames. |
| Loading lifetime and clock inputs | sourced local WGP-52; outer cadence/visible timing U-11 | Loading lifecycle plus presentation wall clock and render admission; fake-clock activation→skipped/admitted render→deactivate→reopen checks, followed by a real loading clip. |
| Ownership/capture/corruption/reveal/approach feedback | sourced WGP-53..55, WGUI-05/93/114/122 | Publish previous/new owner and new-faction playability, first-award fact, completed corruption type/local recipients, discovery-pass result and eligible approach notification. Test hidden/nonlocal/null/nonplayable cases and refused sound without retrying semantic events. |
| Population, mission, construction and build feedback | sourced WGP-55/56, WGEP-95..98 | Previous/current population including queued work and capacity; story-dialog flags/summary state; actual unlock and caller flags; production speech/fallback metadata. Test initial zero, unchanged cap, capacity shrink and silent mission/popup branches. |
| Shared SFX limits and music fades | sourced WGP-57, SND-01..07/16..18/53/61/72 | Reuse shared registry/admission/stream execution; test capped duplicates, equal-priority full-pool replacement, speech/dialog gating and decoded-start counters separately from requests. |

### Remaining capture and producer qualifications

| ID | Open question | Capture/read recipe |
| --- | --- | --- |
| U-11 | Exact loading-clone animation phase and visible progression during sparse or blocked loading render callbacks; absent model handling and alternate bone-spin behavior are not proved by local setup reads. | Record two consecutive land/space loads and a reopen, with a planet whose alternate animation has a visible marker. Log activation, render invocation/admission and animation sample times while holding progress; compare ordinary versus delayed callbacks and paused GC. Verify transform, first-type animation, looping, current-model clone and cleanup. Retain WGP U-10 for alternate-spin parity; delegate full callback cadence to the frame programme. |
| U-12 | WGP-59..64 now source local population observations, bounded failed-request paths, queued starbase interruption and ordinary campaign base/tech completion. Credit-cap cue predicate/recipient/repeat, any additional blocked-build sound reader and end-to-end campaign versus tactical upgrade audibility remain unverified. Authored tags alone do not authorize notification on every clamp, rejection, blockade change or base-level mutation. | For credits, cross/reach/remain at cap, spend then regain, and shrink cap through planet loss; distinguish account addition, cap refresh and later observer reads. For builds, distinguish failed UI check, submitted request rejected by general capability, later queue/population refusal, interrupted queued station and admitted base/tech completion. Repeat each with local, allied and enemy builders and a separate observer; log attempted operation, account/queue state, native notification, advisor text, requested event and decoded start separately. Use script and UI controls, then save/load between operations. Follow any additional observed sound to its caller before defining its recipient or cooldown. WGP-63's type completion and WGP-58's tactical replacement are separate controls. |
| U-13 | Where the zoomed-view station translation comes from (the offset constant or the station model's attachment bone), what an out-of-range `Zoomed_Terrain_Index` draws, and whether index 8 is a blank cell | Read the station set-up and the organisation display update; render one planet per index 0..13 in the zoomed ground view and one with a high index |

U-07 continues to qualify audible outcome/pending/load transitions and listener
placement; it is not reopened as missing static music-selection evidence.

## Producer-condition readiness

Read-only **debug build** evidence GPC-E02/05..14 and effective **XML/Lua**
GPC-X01..04, inspected 2026-10-10. Raw reads and their evidence map remain
outside the committed tree. These rows refine WGP-55/56/58 and U-12; no new
runtime recording or audible result is claimed. Outer player/production
admission stays with [the galactic frame schedule](../galactic-frame-service-schedule.md),
GFS-01/09/11/12/14 and GFD-11/13. The predicates below do not introduce a new
service clock.

| ID | Rule and bounded qualification | Source |
| --- | --- | --- |
| WGP-59 | The strategic population observer compares the serviced player's **ID with the local player ID**, then requires an active strategic mode. With previous stored usage **>0**, current usage **greater than previous** and current usage **>= current capacity**, request the player's faction `SFXEvent_Strategic_Pop_Cap_Reached` if present. Usage includes actual production. Store current usage after the check even with missing/refused sound. There is no cooldown or cap-reached latch in this branch: successive increases while already over cap can each request; an observed decrease becomes the next comparison baseline. Initial zero→full, equal usage and capacity-only loss do not qualify. A later increase from an observed zero also does not qualify. This confirms WGP-55's comparison direction; it is neither purchase rejection feedback nor a broadcast to observing allies. | debug build GPC-E05; WGP-55; WGEP-48/49/68/69 |
| WGP-60 | `SFXEvent_Max_Credits_Limit_Reached` is authored faction data and has a parsed event slot. The checked account-add/cap-refresh paths have WGEP-126's bounded absence of a cap-specific request; the inspected ordinary player service has population feedback but no credit-cap comparison/request. These reads do **not** source a separate reader's equality/crossing condition, recipient, repeat guard or throttle. Stock Rebel/Empire author `RHD_Credit_Capacity` / `EHD_Credit_Capacity`; available data is not proof of an emitted cue. Keep the credit producer seam qualified by U-12. | debug build GPC-E05 and tag-table read; XML GPC-X01; WGEP-126; WGP-58 |
| WGP-61 | The inspected human build-button path calls general capability with local player, selected planet and funds checking; failure does not enter its underway feedback or synchronized request branch, and has no rejection SFX/advisor branch. The later strategic request distinguishes general capability rejection (WGEP-85's entry-shaped cancellation) from queue/population refusal after capability passed (WGEP-134). Neither checked refusal branch chooses a population/tech/locked/blockade sound. A UI underway request may therefore precede a later authoritative refusal. Preserve admission stages; a generic failed-build semantic event does not supply an authored cue or a retry policy. | debug build GPC-E06/09/12/13; WGEP-19/22/24..27/85/97/134/135 |
| WGP-62 | For a queued starbase front, the inspected interruption branch runs only without a pending space conflict at the planet. It finds an orbiting fleet that is **not deletion-pending**, has an owner **different from the entry's recorded builder**, does not skip space combat and declares that its presence cancels starbase production. It sends native cancellation while the entry is present, removes the front and starts the successor. Only for a **local recorded builder**, it formats `TEXT_TOOLTIP_INTERRUPTED_PRODUCTION` with the planet display name and submits urgent advisor text in RGBA **0,193,91,255**, with **no attached SFX**. This branch does not call ordinary stopped feedback or select `SFXEvent_Build_Impossible_Location_Blockaded`. Removal prevents repeat for that entry; no separate time throttle is present here. This is interrupted queued production, distinct from a failed purchase or every fleet-arrival notice. | debug build GPC-E07; WGUI-113/120; WGEP-24/35/127 |
| WGP-63 | Admitted campaign starbase completion first uses ordinary type completion feedback: authored music, then authored completion speech, both without a producer-side local-builder check. When speech is absent and the **recorded builder is local**, request the type's `SFXEvent_Build_Complete` and strategic planet ping. This is not WGP-58's own/ally/enemy station-replacement cue selection. The inspected stock Rebel/Empire campaign base types author `RHD_Construction_Complete` / `EHD_Construction_Complete` on that type tag, whereas shared upgrade cues are faction tags. Placement then sends story construction and initializes campaign station state (WGEP-136); scripted level assignment follows WGEP-131 instead. No universal cue on every level mutation, allied/enemy completion broadcast or producer cooldown is established. | debug build GPC-E07/08/11/14; XML/Lua GPC-X03/04; WGP-58; WGEP-47/96/131/136 |
| WGP-64 | A completed `Build_Advances_Tech_Level` entry takes the **same ordinary completion music/speech/local fallback/ping selection** before assigning its authored target. Completion calls the tech setter with unlock processing and sound request enabled. The setter's new-options cue is a **second, conditional request**: at least one newly unlocked affiliated, initially locked, slicer-unlockable type with `Tech_Level` strictly below the stored level, exact local-player identity and a present faction event. It is not a replacement for the completion cue or guaranteed by a higher tech number. The four active stock Empire campaign tech types author `SFXEvent_Build_Complete` as `EHD_Tech_Upgrade_Complete`, with targets **2/3/4/5**; this is type data, not a faction-global tech-complete selector. Setter-only script changes do not take the production completion branch. | debug build GPC-E02/07; XML GPC-X02; WGEP-42/47/96/130/137 |

At comparison base `03ea7f28033efa3908b448683e1cc8d6c5a038e2`, the semantic
mapping in `src/presentation/audio/galactic.cpp` still does not establish these
producers. Implementers need previous/current local population observations,
separate UI/general-capability/actual-queue admission outcomes, recorded
builder, pending-conflict and blocker facts, advisor text metadata, ordinary
type completion metadata and actual unlock results. Keep credit and any
additional rejection cue as named unsourced seams, rather than assigning the
fleet-approach 5000-ms throttle to them.

Proposed checks: population **0→cap**, **cap→cap+1→cap+2**, equal count,
capacity shrink and **positive→0→cap**; failed UI versus submitted rejection
versus full queue/population refusal; interruption with pending conflict,
other-owner allied fleet, deletion/skip/cancellation-flag controls and nonlocal
builder; base/tech completion with and without speech and with/without an
actual unlock. Count semantic operations, advisor submissions, SFX/speech
requests, accepted output and decoded starts separately. Playback and restore
remain U-07/U-12 and WGEP U-09/U-14.

## Campaign front-end implementation boundaries

Read-only **debug build** FMN-E05..09 and effective stock **XML/text/Lua**
FMN-X01, inspected 2026-10-10. Private queries, stock-file hashes and the
evidence map stay in ignored research storage. This section refines WGP-28..30/40,
U-08 and WGS-U02. Resource existence is distinguished from enabled/visible
controls and actual selection handlers; no new retail layout or runtime
qualification is claimed.

| ID | Rule and bounded qualification | Source |
| --- | --- | --- |
| WGP-65 | Campaign staging builds the list from definitions with `Is_Listed` true and `Is_Multiplayer` equal to the requested mode. Its solo branch keeps the first admitted representative of each `Campaign_Set` in internal catalog-key traversal, not XML-file order. The debug list-all switch bypasses multiplayer matching only; it does not bypass the listed gate. Staging subsequently excludes `Tutorial` entries. It does not separately test `Is_Story_Campaign`: stock story is excluded by `Is_Listed` false, while the listed Underworld tutorial is excluded by its tutorial flag. The stock ordinary solo result has WGP-30's six families, not all 18 faction variants. | debug build FMN-E05; XML FMN-X01; WGP-30 |
| WGP-66 | Each admitted staging row retains its pre-sort campaign identity, localized display name and `Sort_Order`. Sort ascending by that numeric order; equal keys compare localized display names case-insensitively after their narrow-string conversion. Resolve the profile's last scenario for solo, or last internet campaign for multiplayer; select it only if that **same representative** reaches the admitted list. An unresolved profile name asks for the first listed multiplayer default, clears the saved-name text, and does not itself force a first solo row. The default helper falls back to its first catalog entry if no requested match exists. Do not replace these identity checks with unconditional first-visible selection or XML order. | debug build FMN-E05 |
| WGP-67 | A staging list selection changes its description and preview, stores the scenario definition name in the profile and writes that profile. Faction changes and list selection both reevaluate Play/Options and clear the custom-options switch first. With no campaign, Options is hidden/disabled and Play disabled; missing faction or missing same-set faction variant disables both. With a resolvable variant, Play is enabled, and `Supports_Custom_Settings` controls Options visibility/enabling. Normal advanced entry passes that resolved variant; the advanced handler checks its support with debug assertions, not a new simulation-side refusal. This extends WGS-75 without claiming injected/network requests obey the same UI gates. | debug build FMN-E06/07; WGS-75; WGP-29 |
| WGP-68 | Advanced campaign credits use the selected campaign's custom-credit lower/upper bounds. Dialog refresh clamps its working credit value into those bounds before its later noncustom-default branch (WGP-70). Slider steps are **(upper−lower)/1000 + 1**; a read maps normalized position `p` to **lower + 1000×((trunc(p×(upper−lower)+0.5)+999)/1000)** with integer division. Slider display first quantizes the current percentage by integer division and clamps normalized position to **0..1**. The checked campaign constructor supplies bounds **0..10000**; the six stock GC families have no authored range overrides. These are UI limits, not a replacement for WGS-74's signed startup-credit domain. | debug build FMN-E07; XML FMN-X01; WGS-74 |
| WGP-69 | Advanced tech lower/upper bounds are the campaign limits minus the selected faction's full `Displayed_Tech_Level_Adjustment`. The constructor limits are **1..5**; stock Rebel adjustment is **+1**, giving working bounds **0..4**. Refresh clamps starting tech to those bounds, then maximum tech to **starting..upper**. Each slider has **upper−lower+1** steps; its read rounds `p×(upper−lower)` with `+0.5` then clamps starting to **lower..current maximum**, or maximum to **current starting..upper**. WGP-29 owns the later launch addition of the full adjustment; WGS-13 owns the distinct downstream nonzero/subtract-one rule. Do not merge these stages into one magnitude subtraction. | debug build FMN-E07; XML FMN-X01; WGP-29; WGS-13/76/77 |
| WGP-70 | Advanced entry copies staging's working options and refreshes the dialog. For a selected campaign with custom options **off**, refresh reloads that faction's authored credits/current/max tech and updates their sliders; this is its later default branch after WGP-68/69's initial clamps. Defaults clears the custom switch, reloads those campaign values, sets auto-resolve to **Ask (2)** and random events false, then refreshes. Slider position can be clamped while the noncustom working value remains authored: stock human credits such as **12000** exceed the constructor's **10000** slider ceiling. Without a faction, its default helper uses the shared game defaults instead. Checkbox/slider/combo changes set custom options on; Accept copies working options back to staging and closes, while Escape/Cancel closes without that copy. Opening or accepting defaults is not itself enabling custom settings. | debug build FMN-E07; XML FMN-X01; WGS-11/12/73 |
| WGP-71 | Advanced GC auto-resolve offers only **Always (0)** and **Ask (2)**. Display chooses Always for zero, Ask for any nonzero value; the combo event stores its selected value. The GC refresh explicitly sets the random-events checkbox unchecked, **hides it and disables it**. Defaults also sets the stored random-events option false. Thus the random-events control in WGP-28's resource inventory is not an exposed GC toggle on this checked route. Preserve an admitted startup-option provider; these reads do not by themselves prove every option's downstream gameplay consumer. | debug build FMN-E07; stock text FMN-X01 |
| WGP-72 | Main menu's Single Player and Load entries open their separate routes; main-menu Load records that caller for later cancellation. Single Player's New Campaign route uses the profile's tutorial-played flag: an already-set flag opens story selection; otherwise set/write that profile flag and ask `TEXT_SUGGEST_TUTORIAL` with Yes/No. Yes opens tutorial selection, No opens story selection. Custom Campaign separately opens solo staging. Story selection opens full-screen, hides close/progress and defaults to **normal** difficulty. The checked start handler responds to the primary faction/start control, resolves the configured primary story campaign, disables Cancel/Start and input during the admitted loading transition, then starts it solo at the chosen difficulty. It contains no second-faction start branch. Escape/Cancel before admission returns to Single Player. Stock story XML is separate from the listed custom-GC families and disables custom settings; the actual first-use/tutorial/story presentation remains U-08's runtime recipe. | debug build FMN-E08; XML FMN-X01; WGP-28 |
| WGP-73 | A campaign preview uses the shared `Galactic_Back.tga` backdrop, not a per-scenario tactical-map thumbnail. With a resolved campaign it delegates planet blips from authored `Locations` and `Galactic_Position`, using the neutral faction color and the galactic-radar blip component. Normalize X by **(X+220)/440**, Y by **(Y+260)/520**, clamp each to **0..1**, invert Y and centre each icon at the mapped point. Nonplanet art does not become a planet blip. An empty name clears the backdrop's visible alpha; an unresolved nonempty name retains the plain backdrop. These preview branches do not consume campaign `Camera_Distance` or `Camera_Shift_X/Y`; their game-camera consumers remain U-08. | debug build FMN-E09; WGS-01a; XML FMN-X01 |
| WGP-74 | Campaign construction initializes `Sort_Order` **1000**, listed true, multiplayer false, tutorial false, story false and custom support true before authored parsing. Custom-credit bounds begin **0/10000** and custom-tech bounds **1/5**. The checked stock ordinary families author sort **1..6** and custom support true; stock story authors listed false/story true/custom support false, while the Underworld tutorial authors listed true/tutorial true/custom support false. A missing tag is not automatically false or zero. Nearby camera defaults **40/0/1200** are stored metadata, not proof of preview or live camera behavior. | debug build FMN-E05/07; XML FMN-X01 |

### Ready to implement: campaign staging

For the front-end work (legacy EAWR-2009), combine WGP-28..30/40 and WGP-65..74
with WGS-12/13/73..77 and the save/load boundary WGS-152..164. Reuse the
mounted dialog parser/catalog and the skirmish setup toolkit. Providers must
supply localized campaign rows with stable definition/set identities,
admitted faction variants, profile defaults/tutorial route, custom-support and
campaign range defaults, working versus accepted options, preview planet facts
and validated campaign start. Keep profile persistence and dialog editing out
of the canonical simulation; hand admitted options to the campaign/session owner.

At comparison base `967ca47313475e2cc6f38b129f58b4c477f39f2f`, the galactic
data catalog already has campaign list/support/tutorial fields, and
`include/eawr/campaign/setup_plan.hpp` supplies credits/current/max-tech custom
setup. The catalog does not yet expose custom-credit/custom-tech range fields,
and that setup struct alone is not an auto-resolve/random-event consumer.
The composed FoC GC front-end route, profile admission, option editing,
same-set/faction choice and resource preview remain implementation gaps.
The save/load owner is partial composition, as detailed in the session walk;
do not invent a second checkpoint implementation for the frontend.

Proposed feature tests: six stock solo families; listed/story/tutorial and
multiplayer controls; duplicate-set representatives and equal-sort localized
names; stale/missing profile identity without forced first-row selection;
missing faction variant; supported→unsupported selection clears custom options;
credit endpoints/1000-step rounding and authored value versus clamped display;
shifted tech bounds and starting≤maximum; Defaults/Accept/Cancel; only Always/Ask
and hidden random events; normal difficulty/profile tutorial route; campaign
start maps chosen set/faction/difficulty with WGP-29 and WGS-13's separate tech
stages. The preview test checks planet/art admission and authored positions.

U-08 and WGS-U02 are now narrowed to runtime/profile/mod/network admission,
actual first-use tutorial/story presentation, localized non-ASCII/tie behavior
and the campaign camera consumers. GHUD-C17 should retain the actual FoC
staging/advanced/story controls and apply/return flow, including the hidden
random-event control, alongside a lit implementation sheet. Save/load static
handlers are WGS-152..164; their remaining visible failure/restore qualifications
are WGS-U04/U05 and the bounded matrix in the session walk. Stock Lua/XML
searches do not source a general story-save refusal; use WGS-152's actual
handler predicates and qualify live story locks separately.

## Camera parameter readiness continuation

Read-only debug-build continuation and stock XML/Lua audit, 2026-10-10.
GCAM evidence is private. No new retail observation or complete campaign
camera-consumer census is claimed.

| ID | Rule | Source |
| --- | --- | --- |
| WGP-91 | The ordinary galactic transform has the overview and radius-derived focused endpoints of WGP-41 and WGUI-63; it interpolates their authored pullback/pitch/offset without a further minimum/maximum distance clamp in the checked transform or scroll-plane clamp. Keyboard/wheel dispatch does not add a continuous GC distance accumulator. Nearby stock `Max_Galactic_Zoom_Distance` **66**, `Min_Galactic_Zoom_Speed` **400**, `Max_Galactic_Zoom_Speed` **3000** and `Galactic_Zoom_Acceleration` **3000** are authored data, but the checked ordinary camera paths do not consume them. No verified reader here licenses these tags as planet-flight speed or a maximum distance of 66. Battle override and cinematic camera requests remain separate. | debug build GCAM-01/05; XML GCAM-06; WGUI-60..65/123..129 |
| WGP-92 | The checked galactic camera initializer binds the galactic mode camera, recalculates its existing scroll, reads `Galactic_Scroll_Plane`, clamps and enters overview; it does not read its campaign argument for camera shift or distance. Ordinary overview then reads global `GMC_InitialPitchAngleDegrees` and `GMC_InitialPullbackDistance`; local campaign home selection is the separate WGP-42 route. Campaign `Camera_Shift_X/Y` and `Camera_Distance` remain stored metadata (WGP-74), not substitutes for that home/global pose; preview uses WGP-73's independent mapping. Stock Lua contains explicit cinematic start/end and camera-key transitions, so authored story/cinematic overrides require their own provider rather than treating every campaign position as a planet flight. Readers outside these inspected initialization/overview/preview paths remain U-08; this is not a whole-image ignored-tag verdict. | debug build GCAM-05; effective XML/Lua GCAM-06; WGP-41/42/73/74 |

### Ready to implement camera parameters

Combine WGP-41..43/91/92 with WGUI-57..65/78/90/123..129 and the
[controls/radar continuation](gc-ui-sensors.md#camera-controls-and-radar-continuation),
WGUI-138..147. Keep validated mod parameters, planet world bounds/renderability
and radius, seat-local clock/input/lighting, observer discovery/ownership,
display-index mapping, dialog/tutorial/profile and conflict state as distinct
providers. Preserve ADR-021's presentation boundary.

Test altered global overview/zoom parameters while campaign metadata differs;
assert the ordinary pose still follows the verified global/home route. Test
0/25/50/75/100% flights, explicit versus camera-level reversal/refocus,
four-state pan/input guards and advanced-map transitions. Test radar glyph
discovery alpha, ownership-gated faction colour, hit order and authored guide
geometry independently from the fixed mapping. WGP-91/92 refine the existing
overview contract rather than introducing another zoom law. U-08 retains a
controlled fresh campaign with distinct metadata and home coordinates, before
and after authored story camera overrides, to qualify remaining consumers and
the first rendered frame; U-05 retains target loss and complete battle return.

## Campaign battle-screen implementation readiness

This appended walk rechecks loading and results on 2026-10-10 for the campaign
battle-screen work and HUD slices GHUD-S13/S14/S15 (legacy EAWR-2010, EAWR-2298, EAWR-2299,
EAWR-2300). EBS evidence identifies ignored, read-only debug-build receipts; EBS-X01
is effective stock XML/dialog data and EBS-L01 is stock scoring Lua. No new
retail run is claimed. WGP-31..35, WGP-51/52, WGT-03/41 and WAR-69/70 remain
the starting contracts, with the explicit qualifications below taking precedence
over their abbreviated loss-provider and hidden-bounty descriptions.

### Audit of the existing contracts

| Implementer needs | Audit before this walk | Added resolution |
|---|---|---|
| Planet identity, actual owner, basic income, descriptions and icons | Sourced WGP-31; preview WGP-32/51; lifetime WGP-52 | WGP-75/76 distinguish location, map and mission inputs; no live-force or random-tip provider was found in the inspected builder/resources. |
| Normal space/land versus mission dialog and text-key admission | Broadly sourced WGP-31; precise admission/key transformation missing | WGP-75/76. |
| Progress, successful completion, Begin and failure | Milestones/failure sourced WGP-33 and WGT-03; callback/control details missing; visible cadence qualified U-11 | WGP-77..80; WGP-BS-U01/U02/U06 retain runtime qualification. |
| Winner, picture, time and summary admission | Sourced WGP-34, WBF-42..44 and WAR-69/70; tutorial suppression details missing | WGP-81/82. |
| Ordinary/hero losses and their provider | WBF-45 sourced grouping; WAR-69's direct-record shorthand needs qualification | WGP-83/84 source scoring-provider reads and slots; WGP-BS-U03/U04 bound remaining visible correspondence. |
| Credits/bounty, efficiency and buttons | Sourced WGP-34; rounding/zero-power and final bounty visibility missing | WGP-85..87 qualify the shared builder after activation hides. |
| Post-space-win invasion and return | Sourced WGP-35 and WGT-41; visible offer unqualified | WGP-88/89 separate prepared transport IDs, accepted action and return; WGP-BS-U05. |
| Land auto-resolve policy and provider integration | M3 project scope, not an original-game engine rule | WGP-90 and the checklist below; no land engine is requested. |

### Loading and summary rules

| ID | Rule and boundary | Source |
|---|---|---|
| WGP-75 | A fresh campaign loading dialog selects **space/land × ordinary/mission**, using `IDD_SPACE_BATTLE_LOAD_DIALOG`, `IDD_LAND_BATTLE_LOAD_DIALOG`, `IDD_SPACE_MISSION_LOAD_DIALOG` or `IDD_LAND_MISSION_LOAD_DIALOG`. The ordinary entry notifies story for the planet and requested layer, then reads that layer's one-shot tutorial map; a nonempty override replaces the dialog map and enables mission presentation. The retained-transition entry is solo, non-skirmish and treats a nonempty retained map as mission, an empty one as ordinary. Do not infer mission from a corruption faction, map filename or planet description alone. Both entries initially hide Begin and request loading; non-skirmish entry requests campaign pause unless restoring an autosave. | debug build EBS-01/16; dialog data EBS-X01 |
| WGP-76 | The shared information builder requires a planet object or location type; a supplied nonplanet object aborts it. With an object its current type and actual owner override the fallback location type. Descriptions/icons and the preview are WGP-31/51; basic income is distinct from current net income or treasury. Ordinary title appends the layer's prepare text; mission title omits that suffix. Mission keys copy the dialog map, uppercase it and truncate at the **first `.TED` substring**, then prepend `MT` for heading and `MB` for body; a missing translation displays its key. No basename/path stripping is performed in this key-building body. Thumbnail selection is separate: use the dialog map when nonempty, otherwise location-type `Land_Tactical_Map` or `Space_Tactical_Map`; the latter is not necessarily the chosen generic child map. The thumbnail path replaces the first `.ted`, or otherwise the first `.TED`, with `.dds`, checks under the textures directory and hides the gadget when absent. Lists are cleared, wrapped to gadget width, populated in substring order and scrolled to top; ordinary tactical/advantage/history entries are non-selectable. The inspected campaign builder and four stock resources have no force-roster list or randomly selected hint/tip control. Their tactical/advantage/history and mission texts are authored descriptions, not intelligence queries; `GUI_Text_Hint_SFX` is completion feedback. This is a bounded inspection, not a claim about every mod. | debug build EBS-02/04; XML/dialog data EBS-X01 |
| WGP-77 | Starting load hides Begin, resets displayed progress to zero, sets its target to the configured start and marks work pending. Solo non-skirmish progress maps callback fraction **p** to **start + p × (end − start)**; this accessor does not clamp or force monotonicity. Network/skirmish progress instead updates the local participant target and network contexts publish it. Fresh solo campaign UI uses the solo bar and hides unused participant names/bars. Tactical entry reports WGT-03's force-creation values, then **0.75**, **0.9**, **1.0** under WGP-33; these callback values are not measured duration shares. Preserve raw stage observations separately from any documented viewer smoothing policy. | debug build EBS-01/03/04/14; WGT-03 |
| WGP-78 | Completion sets displayed and target progress to the configured end, fills the solo/override bar, shows/enables Begin and requests `GUI_Text_Hint_SFX` in solo campaign context. It clears work-pending and marks loading safe. It then asks the active mode to pause; no active mode or a mode that remains unpaused deactivates the dialog. Both inspected space and land children set their paused flag in single player, so normal solo completion retains the screen. The outer loading callback deactivates for multiplayer; in solo, absent Begin automatically resumes a non-skirmish active mode, resumes synchronized gameplay and deactivates. Thus successful load is distinct from accepted Begin; a resource without Begin is an explicit auto-start route, not a fabricated timer. | debug build EBS-04/05/15/17 |
| WGP-79 | Only the top loading dialog handles its callbacks. Begin requests active-mode resume, resumes synchronized gameplay and deactivates; Enter, Space and Escape also request resume/deactivation. Escape is not a campaign-cancel command. The key handlers themselves have no progress-complete check; actual delivery during blocking load remains WGP-BS-U06. Begin's pushbutton path additionally handles pending story-cinematic music suppression; do not make keyboard parity of that audio branch an assumed fact. The loading callback removes the progress callback after its post-load work on normal return. Deactivation releases preview ownership under WGP-52. | debug build EBS-05/17; WGP-52 |
| WGP-80 | Campaign transition passes the selected map, current base level and defender/map-owner context to its loader after command-bar reset. A false load result requests a popup naming the **selected map and base level**, then a debug assertion. Starting-force creation and the **0.75/0.9** reports belong to the success branch. The debug assertion and later control flow do not establish a recoverable retail retry, restored parent, successful ready publication or safe completion after failure. Supply an explicit failed outcome; runtime recovery remains WGP-BS-U02. | debug build EBS-14; WGP-33; WGT-03/55/56 |
| WGP-81 | Campaign tactical summary activation suppresses a duplicate live dialog, consumes the tutorial skip flag when set and defers battle-end story handling on that skip path. Otherwise a queued next campaign tactical conflict suppresses summary activation. Auto-resolve entry requires active galactic mode and no queued next tactical conflict; single-player involved/non-overrun admission remains WAR-69. Solo activation pauses campaign gameplay when needed. There is no fixed minimum display interval or automatic solo-dismiss timer in the inspected callback; Continue/keyboard dismissal is WGP-87. Upstream tactical victory maturity remains the battle-flow/session owner's responsibility. | debug build EBS-06/07/10; WAR-69/70; WBF-42 |
| WGP-82 | Tactical campaign summary reads retained last battle **layer, winner and planet**, while auto-resolve selects the current solver winner and supplied battle location. Local win means the winner is allied to the local player; use `TEXT_WIN_BATTLE` or `TEXT_LOST_BATTLE`, in white. The image comes from the **local faction**, choosing `Land_Win_Image`/`Land_Lose_Image` only for a land layer and space images otherwise. The location is the location type's display name, empty without a type. Tactical time uses accumulated milliseconds under WBF-44; auto-resolve explicitly zeros time before formatting **00:00:00**. Local faction, planet owner, winner and opposing scoring player are separate facts. | debug build EBS-06..08; XML EBS-X01 |
| WGP-83 | The shared summary obtains its displayed destroyed-type/owner entries from the **scoring provider**, not by iterating the supplied auto-resolve record. The inspected provider chooses tactical deaths while tactical is active and tactical **team** deaths while galactic is active, expanding each stored death count into entries. Stock Lua's galactic death callback feeds both campaign and tactical-team tables; tactical death feeds the tactical table, and tactical-stat reset clears both tactical tables. Filter exact local-owner entries into own losses, enemy-owner entries into enemy losses; omit allied/neutral others and any type with `Score_Cost_Credits` below **1**, before cost/power totals and hero separation. WAR-54's battle-history record remains a separate ledger; WAR-69's record-fed shorthand must not imply direct record-entry-to-icon counts. Lifecycle/team correspondence is WGP-BS-U03. | debug build EBS-08/12; stock Lua EBS-L01; WBF-45; WAR-18/54 |
| WGP-84 | Accepted losses group separately for named heroes and ordinary units, using a map comparator that orders **localized display names lexically**. Distinct types with equal display names therefore share a group; the first inserted type remains its icon/name representative, while every admitted death already contributes its own cost/power before grouping. Each side displays **12 ordinary groups** and only the **first 5 hero groups**; no hero-overflow control is supplied by these bodies. Ordinary overflow enables scrolling over group count minus 12; a changed scroll index rebuilds its pane. A grouped count greater than one receives `x<count>`; a singleton has no count caption. Ordinary rows use the representative type icon and display name; hero slots use its type icon. Unused slots clear images/captions and hide; hero slots also disable. Exact rendered non-ASCII text/overflow appearance remains WGP-BS-U04, not an unknown comparator. | debug build EBS-08..10/18; dialog data EBS-X01; WBF-45 |
| WGP-85 | Accepted losses accumulate authored `Score_Cost_Credits` and `AI_Combat_Power_Metric`; the scoring-scale input scales their totals. The loss-value controls separately request stock Lua `Get_Game_Stat_For_Control_ID`: those returned scores are not a credit refund, treasury delta or raw destroyed-cost label. The summary efficiency pair is **100 × enemy lost power / total lost power**, or **0** when total power is not positive, clamped to **0..100**; the other is **100 − that unrounded value**. Each display independently rounds with **floor(value + 0.5)** and appends `%`. Thus zero-power displays **0% / 100%**, and exact half-percent cases may round both sides upward; do not derive the second printed integer by subtracting the first from 100. Stock Lua's military/kill/rank scores remain distinct from this pair. | debug build EBS-08; stock Lua EBS-L01; WGP-34; WBF-45/46 |
| WGP-86 | Both activation routes first hide bounty, but the subsequently invoked shared information builder explicitly shows it when local collected bounty is positive **or** the local faction `Benefits_From_Corruption`; it writes `TEXT_SUMMARY_BOUNTY_COLLECTED` with the local collected amount. Otherwise it remains hidden. This qualifies WAR-70: its initial hide does not prove a final unconditional auto-resolve hide. In particular a corruption-benefiting faction can show zero bounty. The screen reads an existing tally; it does not pay credits. Actual retail appearance is WGP-BS-U03. | debug build EBS-06..08; WGP-34; WAR-70 |
| WGP-87 | Outside skirmish, Continue reads `TEXT_BUTTON_RETURN_TO_GALACTIC`. Continue, Enter, Space or Escape deactivates a solo/nonvoting summary; a voting summary routes those inputs to its close-vote request instead. Land-invade sets an invasion intent then deactivates; save-replay opens its own dialog. Auto-resolve activation hides replay, window close and land-invade, and its shared builder does not make those controls visible again. A prepared invasion candidate is not proof of a shown invasion button. Local summary controls must request session-owned transitions rather than apply deaths, ownership or solver results again. | debug build EBS-07/08/10/11; WAR-70 |
| WGP-88 | The shared builder resets prepared invasion planet, transport IDs and intent, then prepares them only for a local-allied win, a supplied location type, a **space battle** and an active galactic parent. It selects the first matching planet, visits its attached fleets owned by the **winner**, and stores each alive transport with nonempty cargo in traversal order. Inaccessible surface or planet already owned by the local player clears the planet candidate. These are preparation predicates under WGP-35, not independent proof that the visible offer is enabled; the inspected activation initially hides its button and the builder does not show it. WGP-BS-U05 retains the visible-offer seam. | debug build EBS-08; WGP-35 |
| WGP-89 | Deactivation clears dialog/active/music-queue state. For the solo campaign path, an accepted invasion with a retained planet resolves stored transport IDs again, skips missing objects and requests an invasion only with a nonempty resolved list. Otherwise ordinary Continue requests no invasion. The campaign resume path preserves its pending-tactical pause gate, restores strategic ambient music, triggers pending speech completion and sends tutorial event `battle_end_closed`. Session-owned pause restoration and return remain WGM/WGS responsibilities; do not translate the native resume loop into an unconditional viewer unpause or a second outcome application. | debug build EBS-11; WGP-35; WGT-41 |
| WGP-90 | Normal land and land-mission dialogs use the same inspected information/progress/Begin contracts as their space counterparts, with the layer-specific resource and mission route. Stock XML supplies the planet descriptions/model/scale, faction display colour/name and four result-image tags; stock scoring Lua supplies separate score and death-table inputs. None of those reads supplies a new M3 playable land engine. The project admits land auto-resolve through its conflict/outcome owner, and admitted missions require a story/mission descriptor; an unavailable land child or mission provider must not publish a fabricated successful load. | debug build EBS-01..05; XML/dialog data EBS-X01; stock Lua EBS-L01; project M3 scope |

### Provider seams and current implementation comparison

Comparison base: `e78f27b57550c8a3b1a89cf29c62d5119342c701`. These are integration
findings, not assertions that the original gap census still describes current code.

| Slice | Required read-only inputs / accepted intents | Existing support and remaining integration | Proposed feature test |
|---|---|---|---|
| GHUD-S13 summary | Battle kind/layer; retained winner and local/alliance identity; planet type; scoring loss entries/type metadata, score-control outputs, scale and tactical elapsed milliseconds; bounty tally/faction benefit; queued-conflict/tutorial admission; return/vote/invasion permissions. | `src/presentation/ui/battle_results.cpp` already groups filtered snapshot losses by type identity, separates heroes and exposes time/cost/power; WGP-84 requires a localized-name-equivalence adapter. `include/eawr/campaign/auto_resolve_outcome.hpp` already retains planet type and record entries; these are not by themselves the scoring projection in WGP-83. Campaign summary composition, final control visibility, score/bounty/efficiency binding and allowed return need integration. | Tactical space/land and AR space/land × win/loss; allied winner but allied casualties excluded; cost 0/1; equal-name distinct types; 12/13 ordinary types and 5/6 heroes; zero power, 12.5% and 87.5%; bounty 0/positive for ordinary/corruption-benefiting factions; queued conflict suppresses summary; Continue cannot reapply outcome. |
| GHUD-S14 normal space loading | Real conflict/planet identity, location type/current model/owner/basic income; selected and original map identities, base level and defender; authored descriptions/icons/preview constants; work callbacks and terminal success/failure; accepted Begin/session pause barrier. | `apps/viewer/src/galactic/galactic_handoff.cpp` already binds the real conflict map, planet, owner name/colour and basic income. Shared loading content supports ordinary campaign lists. The current handoff sets `loading_begin_barrier = false` and dismisses on ready, so solo native Begin retention must be reconciled under WGP-78/79 (legacy EAWR-2699). Work-weighted viewer progress is BLD-10's project policy, not WGT-03's native milestones. Preview parity retains WGP-51/52 and U-11. | Real descriptor with planet context differing from map metadata; actual owner versus observer apparent faction; basic versus net income; custom progress start/end; 0.85 then 0.75 callback observations; success waits for admitted Begin; controlled missing map publishes failure and its selected name/base, never ready. Lit preview sheet. |
| GHUD-S15 land/mission loading | Admitted layer and explicit mission/transition kind; story/tutorial-selected dialog map, retained location and owner; MT/MB text lookup and existence; conflict owner's land-AR policy. | Shared content has a land selector, but campaign handoff admits space only and has no mission descriptor/key adapter. Add read-only descriptors/resource adapters and explicit unsupported-child outcome; keep land-AR dispatch owned by campaign services. | All four resources; empty/nonempty mission override; uppercase `.TED` truncation and missing key fallback; selected map distinct from location; unsupported land child cannot become ready; admitted land-AR reaches the shared result adapter with zero time. |
| Invasion/return boundary | Surviving winner-owned transport IDs/cargo, location/surface/local ownership, accepted invasion/continue and session pause ownership. | Existing return/outcome owners retain identities; summary must consume their permitted projection. WGP-88's preparation does not close WGP-BS-U05's visible-offer qualification. | Accessible hostile surface with cargo versus own/inaccessible/empty/dead/missing transport; return then resolve IDs once; preserve independently owned pause. M3 accepted offer routes to land auto-resolve. |

### Ready to implement checklists

- Campaign battle-screen umbrella (legacy EAWR-2010): compose WGP-31..35/51/52 and
  WGP-75..90 with WGT-03/41, WBF-42..47 and WAR-69/70. Own the adapter only;
  source real loading, result/scoring and return permissions from their session
  owners. Run the three slice matrices above plus the qualified invasion recipe.
- Summary slice (legacy EAWR-2298): WGP-81..89; separate AR record/history from
  scoring losses and keep bounty's final shared-builder predicate. Supply
  local/alliance/result/scoring/time/bounty and allowed-return facts; test
  four layer/kind combinations, grouping/rounding edges and once-only return.
- Space loading slice (legacy EAWR-2299): WGP-75..80 and WGP-31..33/51/52;
  bind the real handoff descriptor, native stage observations, terminal failure
  and accepted Begin barrier. Test context mismatch, work regression, failed map
  and explicit completion-to-Begin; retain U-11 for animation/cadence.
- Land/mission slice (legacy EAWR-2300): WGP-75/76/90 and WGP-77..80;
  use admitted conflict/story descriptors, all four mounted resources and
  MT/MB lookup. Test missing translation/override and unsupported child versus
  admitted land auto-resolve; do not expand M3 into playable tactical land.

### Remaining qualified capture seams

These extend GHUD-C14; R05/R06/R07/R11 remain retained references, not evidence
of every branch below. Frame-order/cadence questions remain with the frame-service
programme; this walk introduces no ad-hoc schedule rule.

| ID | What remains qualified | Capture recipe |
|---|---|---|
| WGP-BS-U01 | Visible clone phase, cold/blocked progress cadence and actual loaded-model fallback; existing U-11. | Record consecutive normal space/land loads and reopen with a visible animation marker; log invocation/admission/stages and actual model identity. Compare cold/warm and delayed callbacks; use lit sheets and retain pause state. |
| WGP-BS-U02 | Retail failed-map modal/localization/acknowledgement and parent recovery after the debug assertion path. | Isolated invalid-map fixture, record selected map/base and failure publication, popup, acknowledgement, active child/parent and pause ownership. Demonstrate recovery or bounded failure without a success/ready marker. Never damage an ordinary campaign save. |
| WGP-BS-U03 | Actual AR casualty-to-scoring projection and bounty visibility for space/land, especially companies, hero cargo and transport death. | Start with reset score tables; record solver record, confirmed deaths, team-scoring entries, local tally/faction and final control visibility. Capture E/R/U win/loss plus zero/positive bounty and compare displayed rows to both ledgers; repeat a second battle to detect stale entries. |
| WGP-BS-U04 | Actual rendered localized text/clipping, five-slot hero truncation and first-representative icon after equal-name aggregation; the comparator and slot limits are sourced WGP-84. | Controlled 13 distinct-name ordinary and 6 distinct-name hero losses, plus equal-name distinct types with different icons/costs; capture both scroll endpoints, hero slots and second summary rebuild. Record death order, localized names, grouped count and representative type; compare the sourced lexical order, aggregation and first-five policy to the rendered result. |
| WGP-BS-U05 | Whether/when native invasion preparation becomes a visible offer; the inspected activation/builder keep land-invade hidden. | Win space above accessible hostile surface with living loaded transports; record prepared planet/IDs, final button visibility and later callbacks. Repeat own/inaccessible/no-cargo and AR control; accept/decline only an actually admitted offer, checking ordinary invasion/return once. |
| WGP-BS-U06 | Premature Enter/Space/Escape delivery during blocking loading, story-cinematic audio parity and retained pause after dismissal. | Capture normal and stalled load, send each key before/after completion, log delivered callback/start/deactivate state and pending cinematic. Repeat ordinary/mission and pre-paused campaign; keep UI input admission distinct from raw handler branches. |

## Campaign-end implementation readiness

Read-only walk on 2026-10-10; comparison base
`efa2285d6ab60fedae3be2261f9c494b0afd921c`. ECE evidence resolves to private
research: ECE-01 score activation/input/exit; ECE-L01 effective stock scoring
Lua; ECE-L02 execution of that unchanged Lua with synthetic tables in Lua 5.0.2;
ECE-X01 effective faction/movie/story XML. Later rows qualify the movie boundary.
No original code or asset is distributed with this note.

### Score controls, producers and formulas

These campaign-end figures differ from the tactical battle-summary power share
in WGP-85. Counter events belong to the scoring/session provider; the viewer
reads their retained result before WGS-118 tears down live player state.

| Rule | Behaviour | Evidence |
|---|---|---|
| WGP-97 | The score requests `IDC_TITLE_STATIC`, `IDC_MILITARY_EFFICIENCY_STATIC`, `IDC_CONQUEST_EFFICIENCY_STATIC` and `IDC_KILL_EFFICIENCY_STATIC` for the **local player** through `Get_Game_Stat_For_Control_ID`. A string title is a localization key; absent/non-string title supplies an empty key. A numeric efficiency is multiplied by **100**, cast to integer (truncation toward zero), clamped **0..100**, and printed as a decimal integer **without `%`**. Absent/non-numeric efficiency instead writes the literal **`0%`**. Thus a present zero and an absent stat are visibly different. Stock Lua's non-tactical route uses the campaign tables. | debug build ECE-01; stock Lua ECE-L01; present-zero runtime WGS-150/151 |
| WGP-98 | A stock scoring death update requires both victim and killer to be valid. It uses the victim's **game-scoring type**, increments one frag for the killer's player ID and one death for the victim owner's ID, and accumulates that type's combat rating, build cost and `Score_Cost_Credits` on both entries. The inspected helper has no enemy/alliance or positive-score-cost filter. A galactic death updates the campaign and tactical-team ledgers; a tactical death updates the tactical ledger. A starbase change contributes a campaign death only when an old type exists and the new type is absent, attributing its final-blow player. Tactical/campaign forwarding and validity at the native producer boundary remain ECE-U01. | stock Lua ECE-L01 |
| WGP-99 | Completed galactic production increments count and accumulated combat rating, build cost and score value by player, planet type and scoring type. A supplied type uses the planet owner's identity and that type's values; a supplied object uses its game-scoring type. Production begin/cancel callbacks add **no** stock scoring entry. Galactic mode start resets all score/player/quit tables; entry into a campaign tactical child resets only tactical and tactical-team tables. Player-list reset resets all tables. Preserve a copied end result before shutdown; tactical-stat reset alone must not erase campaign counters. | stock Lua ECE-L01; debug build ECE-01 for reads before shutdown |
| WGP-100 | Let **B** be completed campaign build count, **D** campaign death count, **K** total credited victim combat power and **L** total lost combat power. Military efficiency is **0** for B=0,D=0, **−1** for B=0,D>0, otherwise **(B−D)/B**, including negative results. Kill efficiency is **0** when K=0; otherwise **K** when L=0, else **K/L**. Build cost, built power and score-cost sums are visited by the stock calculation but do not weight either returned efficiency. These are raw rank inputs; apply WGP-97's display clamp separately. | stock Lua ECE-L01; synthetic unchanged-Lua execution ECE-L02 |
| WGP-101 | Each planet faction-change callback records one acquisition for the new owner and one loss for the previous owner, indexed by player ID and planet type. Reacquisition therefore counts again; this is neither final planet count nor distinct conquered planets. Let **A** and **Z** be the summed acquisition/loss counts. Conquest efficiency is **0** without an entry or when A=0, **A** when Z=0, otherwise **A/Z**. Hero-neutralization counts have their own ledger and do not enter these three efficiency formulas or the title score. | stock Lua ECE-L01; synthetic unchanged-Lua execution ECE-L02 |
| WGP-102 | Stock rank score adds one bucket award for each **raw** military, kill and conquest efficiency. Threshold comparison is strictly **greater than**: descending threshold → award is **1→40000; .98→30000; .94→25000; .91→20000; .88→10000; .84→9000; .80→8000; .78→4000; .74→3000; .70→2000; .60→1000; .50→500; .40→400; .30→300; .20→200; .10→100**, else zero. Exactly 1 gives 30000, exactly .98 gives 25000 and exactly .10 gives zero. Clamping raw values before scoring would change rank. The screen's title route forces total score to zero for a recorded quitter, without replacing the separately returned efficiencies. | stock Lua ECE-L01; boundary execution ECE-L02 |
| WGP-103 | Rank walks descending thresholds and selects the first with **score > threshold**, using `TEXT_REBEL_TITLE<n>` only for faction name **REBEL**, and `TEXT_EMPIRE_TITLE<n>` for every other faction, including **UNDERWORLD**. Thresholds for suffix **19 down to 0** are **145000, 125000, 115000, 100000, 90000, 85000, 80000, 75000, 70000, 60000, 55000, 50000, 45000, 40000, 25000, 20000, 15000, 10000, 5000, 0**; falling through still returns suffix 0. Exactly 5000 is suffix 0; 5001 is suffix 1. With nonnegative stock counts, military efficiency is at most 1, so its award is at most 30000; the other two award at most 40000 each. The resulting **110000** upper bound cannot reach suffix 17, 18 or 19. This title selection does not choose the victory winner. | stock Lua ECE-L01; boundary execution ECE-L02; arithmetic bound from WGP-100/102 |
| WGP-104 | The inspected native health-zero receiver omits a victim whose type has `EJECT_VEHICLE_THIEF` in either ability slot; otherwise the victim's owning mode chooses the galactic or tactical death sender. Both senders choose the final-blow player: the missing-ID sentinel selects the neutral player, whereas an unresolved non-sentinel ID supplies no killer argument and stock Lua rejects that update. They call their respective public Lua event. Completed galactic production unwraps a produced fleet to its **first contained object** before listening/scoring; the sender uses an object when present, otherwise the produced type. Planet changes are forwarded only when old/new owner pointers differ; unresolved owner IDs use the neutral player. These are source admission rules, not a blanket assertion that every tactical death is already in the campaign ledger. | debug build ECE-04; stock Lua ECE-L01 |
| WGP-105 | The victory-movie enabled flag starts **false** in the inspected constructor and is reset to **false** by galactic UI initialization's reset helper. Setting story-campaign identity does not itself enable it. `ENABLE_CAMPAIGN_VICTORY_MOVIE` sets it true with absent/empty param 1, otherwise according to whether integer conversion is nonzero. At score activation, disabled movies **or multiplayer** clear the initial win and first-finale names; that branch does not populate faction movie names. Otherwise the winner's faction and victory kind supply WGP-39's initial/first/second/post-credits names, even when the local score says Defeat. Movie selection does not separately test story versus conquest identity or exact-local victory. | debug build ECE-01/02/04; effective story XML ECE-X01 |
| WGP-106 | Effective stock faction authoring is: **Empire** generic `Empire_Win_Land`, superweapon `Empire_Win_Deathstar`, first finale `Empire_Win_Space`; **Rebel** generic `Rebel_Win_Space`, superweapon `Rebel_Win_Deathstar`, first finale `Rebel_Win_Land`; **Underworld** empty generic/superweapon, first finale `Underworld_Win_Story_Campaign`, second `Underworld_Win_Story_Campaign_Part2`. Both Underworld finale movie definitions author `Cannot_Skip=true`; their faction post-credits name is empty. Empire/Rebel stock definitions author no second/post-credits name. Local text uses `TEXT_EMPIRE_VICTORY`/`TEXT_EMPIRE_DEFEAT`, `TEXT_REBEL_VICTORY`/`TEXT_REBEL_DEFEAT` or `TEXT_UNDERWORLD_VICTORY`/`TEXT_UNDERWORLD_DEFEAT`. These are data names, not a licence to hardcode the mounted profile's choices. | effective faction/movie XML ECE-X01; debug selection ECE-01 |
| WGP-107 | After the initial movie, the local continuation stops active music and speech. Outside multiplayer it requests the retained first finale, selecting a continuation to the second finale only if that second name is nonempty; otherwise it continues directly to **`Credits`**. Second finale likewise continues to Credits. Credits is requested **looping**, game-hidden, with **no** supplied continuation. Consequently a disabled/empty initial and first-finale path can still reach credits through WGP-108; victory-movie suppression does not by itself suppress credits. This extends WGS-119's local callback graph. | debug build ECE-02; WGS-119 |
| WGP-108 | With an initialized movie scene, an unknown movie name, a known definition with empty `Movie_File`, or failed file/decoder opening invokes its supplied continuation **synchronously**, if any, and returns failure. Successful opening instead retains the continuation for stopping. The uninitialized-scene path returns failure without invoking it. Thus empty Underworld win names do not terminate an enabled finale chain; a missing Credits definition has no continuation that would invent a menu transition. Runtime failure appearance and reentrant score activation remain ECE-U02. | debug build ECE-02 |
| WGP-109 | A non-looping movie reaching its end requests stop. Stop releases movie resources and hiding/credit presentation, restores a paused active mode only when the movie's pause flag requires it, clears the retained continuation **before invoking** it, then invokes the saved callback. With a game-hiding movie playing and the main menu inactive, keyboard **Escape or Space** requests that same stop only when `Cannot_Skip` is false; the movie branch consumes the key, so it does not also dismiss the score on that key delivery. Underworld's two stock finale definitions block that skip. Visible input/teardown continuity stays ECE-U02. | debug build ECE-02/04; effective movie XML ECE-X01 |
| WGP-110 | The outer credits-completion consumer, guarded by an operational rendering device, checks credits-present and credits-done, stops the movie, then requests a nonempty retained `Post_Credits_Movie` as non-looping/game-hidden with **no** continuation. This is distinct from the local finale callbacks and from the Escape/Space skip path; the inspected skip path does not request post-credits. None of these consumers deactivates the score or activates a menu. Stock Underworld's empty post-credits name makes this branch silent. No cadence or frame-order claim is added here. | debug build ECE-03/04; ECE-X01; WGS-119 |
| WGP-111 | End data is consumed **before** targeting HUD, modes, players, sidebar and command-bar shutdown under WGS-118. Native player-list reset reloads the authored `Game_Scoring_Script_Name` unless its ignore-reset gate is set; stock authoring selects `GameScoring`. Retain copied local score/title/text/picture facts and winner-faction movie choices through that boundary rather than resolving players or counters afterward. Ordinary multiplayer results bypass the score; explicit quit remains WGS-118's exception. Score Escape/quit while topmost follows WGS-116/117: ordinary result → main menu; explicit LAN quit → LAN lobby; other explicit quit → main menu unless a tutorial is queued. The queued-tutorial helper activates neither destination, and this score handler supplies no retry action. | debug build ECE-01/04; effective global XML ECE-X01; WGS-116..118 |
| WGP-176 | **Campaign scoring ledger (project rule, GHUD-S61).** The galaxy keeps the raw counters the authored scoring module reads, as saved and hashed simulation state: completed builds per **(planet owner, planet, type)** (WGP-99), galactic deaths per **(victim owner, final-blow player, type)** (WGP-98), planet faction changes per **(planet, old owner, new owner)** (WGP-101) and the quit table (WGP-102). Counts are integers under ordered keys and only ordered commits write them, so the section is equal for 1, 2, 4 and 8 workers. The state section exists only when a counter exists, so a campaign without scoring activity keeps its earlier hash. Nothing is converted to an efficiency or a title in simulation. | project rule; WGP-98..102 |
| WGP-177 | **Ledger producers.** A completed production adds a build for the planet's **current** owner at the production commit (WGP-99; every completion kind counts, including technology and station levels, because the stock end handler has no kind filter). A planet ownership commit adds one change record, in political-control service and in the debug owner command, never at campaign construction (WGP-101). A death is recorded at the galactic loss boundary just before the identity is removed: the tactical-return and auto-resolve returns supply each loss's final-blow player, ability consumption supplies the defeated hero's killer, and a loss with no supplied final blow keeps the missing-id sentinel that the scoring boundary credits to the neutral player (WGP-104). A loss removes the identity, so it is recorded once. Seams (ECE-U01, WGP-117): the scoring type is the persistent object's original type, not the dummy company/squadron type chain; a station destroyed in a battle does not yet record the starbase-removal death; the death producer for every tactical path is still the return boundary rather than a verified native receiver; tactical-only counters are not kept. | project rule; WGP-98/99/101/104/117; ECE-U01 |
| WGP-178 | **Evaluation at the campaign end.** The provider copies the type names, planet types, faction names and the effective scoring module's files once, then at the end boundary (WGP-111) mounts the module named by the effective `Game_Scoring_Script_Name` in the script sandbox, runs its definitions and mode-start handler for `Galactic`, replays the ledger through the module's galactic event handlers, and reads the four score controls for the local player through its **non-tactical** control query. Every equation (WGP-100..103) therefore stays in the mounted module: a mod that replaces the module or its type facts changes the numbers. A string result is the title key; a numeric result is a raw efficiency (WGP-97 converts at display). The type facts are the authored scoring-type values (combat power from `AI_Combat_Power`, `Score_Cost_Credits`, build cost) with the debug-build zero defaults. No per-tick script runs. | project rule; WGP-97/100..103/111; WBF-45/46 |
| WGP-179 | **Absent and failing module.** A missing constants file, an absent module name or a named module that is not authored supplies no scoring half: the dialog shows WGP-97's absent `0%` and the end view names the reason. A module that fails to run does the same, never failing the end boundary. An empty ledger with a mounted module yields **present** zero efficiencies and the module's lowest rank, which WGP-97 prints as `0`. | project rule; WGP-97 |
| WGP-180 | **Quit and save.** The quit table is written only by the trusted forced-player-quit producer (`GalacticSession::record_player_quit`, WGS-177); a seat leaving, an ordinary multiplayer result and the score screen's own button write nothing, and no production producer is wired for an explicit quit yet (WGS-118, GHUD-S48). A recorded quit reaches the module as its quit event, so its rank score is zero while the efficiencies stay (WGP-102). The ledger rides the checkpoint's campaign-scoring section; a load reconstructs it from the replayed inputs and verifies the saved bytes. | project rule; WGP-102/111; WGS-118/177 |

### Story versus conquest qualification

The effective XML search finds `ENABLE_CAMPAIGN_VICTORY_MOVIE` only in the
Underworld story's final mission-completion branch. That branch separately
requests story `VICTORY` and enables campaign victory movies with empty param 1;
WST-45 owns the forced result/delay and ordinary-victory wait. Its authored
`Is_Story_Campaign=true` identifies that campaign, but is not the movie gate
in WGP-105. Story and conquest share the inspected score control formulas;
there is no campaign-type or faction override in stock scoring Lua.

Implement movie selection from the published enabled state, winner identity,
victory kind and mounted faction data. The reset-disabled path requests empty
win/first-finale names and can continue to Credits; the enabled Underworld
path skips its absent initial movie and requests both authored finale parts.
Do not equate a movie's existence with its admission in every ordinary conquest,
or equate a local Defeat headline with a separate lose movie. Fresh-session,
loaded-session and successive-session gate persistence remain ECE-U03.

### Counters and retained provider seams

| Consumer | Track during play / publish at end | Proposed feature test |
|---|---|---|
| Local military/kill | Completed campaign builds; victim death count; credited and lost game-scoring-type combat power, per player; full type/planet build and frag/death entries when mounting stock scoring Lua. Preserve campaign ledger across tactical resets. WGP-98..100/104. | B=0/D=0; B=0/D=1; B=2/D=3; K=0; K=3/L=0; K=6/L=2; neutral/missing killer; ejected thief; completed fleet/type/upgrade versus begin/cancel; campaign child reset and return cannot double count. |
| Conquest/rank | Acquisition/loss event counts by player and planet type; quit flags; faction-name and localization provider; raw efficiencies separate from printed values. WGP-101..103. | Reacquire the same planet; no acquisition/no loss; ratio above one; strict .10/.98/1 efficiency and 5000/10000 rank boundaries; E/R/U keys; quitter retains efficiency but forces title score zero. |
| Full-screen score/movie/exit | Matured winner/kind, exact local identity, multiplayer/explicit quit, ending mode, movie-enabled state, authored faction text/images/movie names, typed score outputs and authorized exit/tutorial intent. WGP-36..39/97/105..111; WGS-82/116..119. | Allied winner is local Defeat; losing local faction supplies score text/image while winning faction supplies movies; both presentation image draws preserve simulation hashes; present 0 versus missing stat and 12.9 → 12; enabled/disabled E/R/U ordinary/superweapon; empty/failing first/second movie; once-only continuation; natural credits versus skip; topmost/non-topmost score input; ordinary multiplayer versus explicit LAN quit. |

Ready-to-implement checklist for campaign end (legacy EAWR-2011):

- Consume WGS-82's matured result and WGS-118's explicit-quit boundary; mount
  the score dialog from WGP-37 using WGP-97..103's typed control contract.
  Start retaining the campaign counters in the first row of the table now;
  final treasury, surviving unit count and battle-history rows cannot recover
  those event totals.
- Publish copied end facts before WGP-111 teardown. Apply WGP-105..110 through
  the full-screen movie owner, with reentrant empty/failure continuations and
  separate credits/post-credits/score-exit intents. Keep picture RNG solely
  in presentation. Reuse WGS-116/117's topmost and destination contracts.
- Run the three feature matrices above and lit E/R/U win/loss score sheets.
  Label forced-enabled movie tests separately from stock ordinary-conquest
  admission. Existing WGS-121/122/150/151 qualify bounded local screens,
  not every story/LAN/skip branch.

### Remaining retail qualifications

| ID | Qualified unknown | Capture recipe |
|---|---|---|
| ECE-U01 | Native child-return death forwarding, company/transport/scoring-type multiplicity and the campaign ledger's once-only lifetime through tactical, auto-resolve and loaded-session paths. WGP-98..104 source the inspected receivers and formulas, not every death producer. | Reset/stage known builds and typed victims, including company members, hero cargo, transport/starbase and ejected thief; inspect campaign/tactical/team Lua tables with the structured debugger before/after death and parent return, then a second battle and save/load. Record producer identities and final-blow owner. Compare the actual end controls to those ledgers; retain WGP-BS-U03's battle-summary projection distinction. |
| ECE-U02 | Actual missing/unopenable movie appearance, synchronous callback reentrancy, permitted/blocked skip delivery, shutdown continuity and credits/post-credits/score layering. | Isolated authored empty/missing/bad-file movie definitions; enabled E/R/U ordinary/superweapon win and AI/allied-winner loss; trace callback requests/results and score-active state through shutdown. Try Escape/Space on ordinary and unskippable movies, wait for natural credits completion, then dismiss the topmost score. Add a nonempty test post-credits name and compare natural completion with skip. Exercise uninitialized presentation separately; never label it the ordinary missing-file route. |
| ECE-U03 | Fresh/loaded/successive story versus conquest movie-enabled persistence and queued-tutorial handoff at score exit; no full runtime parity claimed. | Record the enabled flag at galactic initialization, story reward, victory and load; complete Underworld story and ordinary E/R/U conquest with fresh and successive sessions. Trace the story-forced result separately from movie enablement. Include explicit LAN quit and queued tutorial; record final destination and ownership after score closure. Reuse GHUD-C18 and WGS-U07 rather than inventing a tutorial retry button. |

## Begin, loss grouping and undiscovered radar implementation readiness

Read-only debug-build continuation, 2026-10-10, for the Begin barrier
(legacy EAWR-2699), loss grouping (legacy EAWR-2704) and dim radar planets
(legacy EAWR-2705). This section adds narrowly scoped rules and implementation tests; it
does not claim a new retail capture. EBL-01..05 are private evidence receipts
for loading input, summary grouping, scoring-type selection, planet drawing
and stock Lua/XML/resource cross-checks. Existing WGP-75..90 and
[WGUI-130/131/142..145](gc-ui-sensors.md) remain the broader contracts.

### Short audit and settled boundaries

| Issue / implementer needs | Audit result | Resolution here |
|---|---|---|
| Begin entry, completion, pause, resume, resource absence and multiplayer/skirmish | Sourced WGP-75/77..79; premature input delivery qualified WGP-BS-U06 | WGP-112..114 source the window-input gate and ready-to-Begin distinction. Input still queued across gate reopening remains WGP-BL-U01. |
| Grouping key/order/representative, count caption, hero split/limits | Sourced WGP-83/84; rendered non-ASCII/overflow qualified WGP-BS-U04 | WGP-115/116 make exact comparison, named-hero-only admission and provider traversal explicit. |
| Squadron versus fighter loss identity and multiplicity | Missing precise scoring-type projection; ledger correspondence qualified WGP-BS-U03 and ECE-U01 | WGP-117/118 source type resolution and the consumer boundary. Native death multiplicity remains WGP-BL-U02. |
| Undiscovered alpha, click and ownership tint | Sourced WGUI-142..145; no separate radar walk file is needed | WGP-119/120 retain local discovery independently of intelligence and give exact alpha and collision rules. |
| Planet icon versus orbit/route/viewport channels | Guide hiding sourced WGUI-127; world routes sourced WGP-10..17 | WGP-121 limits the half-alpha rule to planet glyphs. Rendered texture/guide appearance remains WGP-BL-U03. |

### Rules

| ID | Rule and implementation boundary | Source |
|---|---|---|
| WGP-112 | Ordinary solo campaign loading starts with Begin hidden and progress pending. Successful completion fills the configured end, exposes/enables Begin, requests the completion hint sound, clears work-pending and requests pause on the active child. Normal solo space and land children accept that pause, retaining the dialog. All four stock campaign resources, including missions, contain a Begin button captioned `TEXT_BUTTON_BEGIN`. **Progress reaching its end is not player acceptance** and supplies no timed auto-start. Failed load must stay WGP-80's failed route. | debug build EBL-01; stock resources EBL-05; WGP-75/77/78/80/90 |
| WGP-113 | Before starting blocking work, loading clears its window-input-safe flag. The ordinary window dispatcher checks that flag for key down, key up, character input, mouse-button down/up/double-click and wheel; when false it returns without forwarding those events to their normal input handlers. Mouse movement has its foreground gate but does not consult this flag. Loading rendering still removes/translates/dispatches pending window messages. Completion sets the safe flag true. This settles the checked premature-delivery branch of WGP-BS-U06: a key/button event dispatched while unsafe cannot reach Begin/resume through these routes. It does not prove that every event physically generated during load was dispatched before reopening. | debug build EBL-01; WGP-79; WGP-BL-U01 |
| WGP-114 | After input admission, only the top loading dialog accepts Begin, Enter, Space or Escape; each requests active-mode resume, synchronized-gameplay resume and dialog deactivation. Escape therefore starts/continues through this loading barrier rather than cancelling the campaign conflict. The callback has no additional completed-progress check; the dispatcher gate in WGP-113 is separate. Begin's button path also handles pending story-cinematic music suppression; keyboard audio parity remains qualified. Multiplayer completion dismisses automatically. A solo resource without Begin is the explicit auto-start fallback in WGP-78; solo skirmish has its own force-build callback and must keep its existing admission policy. | debug build EBL-01; WGP-78/79; WGP-BL-U01 |
| WGP-115 | Within each side and each ordinary/named-hero pane, group keys are the **resolved localized display-name strings**, compared lexically by wide-string character units. The checked comparator has no locale collator, case folding, natural-number ordering or type-ID tie-break. Equal strings compare equivalent and merge distinct types; different localization keys can thus merge after translation. The first type inserted from the **scoring-provider traversal** remains the icon/name representative; later equivalent entries only increment its count. This is not a claim of chronological death ordering. Every admitted death contributes its own authored cost/power before this grouping. | debug build EBL-02; stock scoring Lua EBL-05; WGP-83/84 |
| WGP-116 | The summary hero split tests **named hero** only; a generic hero without named-hero identity enters the ordinary pane. Ordinary and hero groups remain separate even with equal localized names, as do the two sides. Each occupied group has an empty count caption for count **1**, and ASCII lowercase `x` immediately followed by the decimal count for count **greater than 1**, including hero groups. Ordinary panes show **12** groups with scroll maximum `max(groups − 12, 0)`; hero panes render only the first **5** lexical groups, with no hero scroll control. Omitted hero groups still contributed to loss totals. | debug build EBL-02; WGP-83/84 |
| WGP-117 | Stock scoring Lua records each valid victim/killer callback under the victim owner's ID and the result of `Get_Game_Scoring_Type()`. That public query first uses a nonempty team company type; otherwise a transport dummy-company type, then an eligible resolved parent-mode original type; otherwise the victim's original type. Original-type resolution can return a fleet dummy-squadron type, transport dummy-company type or team dummy-squad type before falling back to current type. Thus a scoring type is not universally the currently drawn craft type. The summary consumes these already-resolved type/owner entries and WGP-83's score-cost filter; it does not resolve craft identity anew. | debug build EBL-03; stock scoring Lua EBL-05; WGP-83/104 |
| WGP-118 | A squadron-type entry counts the deaths supplied for that type; the loss builder neither multiplies it by `Squadron_Units` nor adds child-fighter rows. A separately supplied fighter-type entry takes exactly the same owner/cost/named-hero/name-group rules as any other entry: no hardcoded fighter blacklist or squadron-name alias exists in the inspected builder. Stock X-wing/Y-wing/TIE-fighter squadron definitions author distinct squadron text keys and score costs **600/700/420**, while the checked individual craft definitions author different text keys and omit that score-cost tag. Omission alone is not evidence of a resolved zero default. Preserve mounted data and provider projection; native member/container event multiplicity and parent-return deduplication remain WGP-BL-U02. | debug build EBL-02/03; stock XML/Lua EBL-05; WGP-83/117 |
| WGP-119 | An admitted planet-display entry with shell rendering enabled, planet/planetary state present and not behind-camera draws its radar glyph when the galactic radar is enabled, with code alpha **128** if local discovery is false and **255** if true. For an 8-bit-alpha adapter the dim factor is exactly **128/255**, not **0.5**. Undiscovered is the local discovery flag, not unknown ownership, no spy, hidden contents, stealth, hostile allegiance or focused camera state. Discovery can be explicitly cleared by `REVEAL_PLANET` flag zero; ordinary discovery's monotonic service does not prevent that authored change. | debug build EBL-04; WGUI-01..06/130/131/143 |
| WGP-120 | Radar tint uses the **actual owner's authored faction `Color`** only when local discovery and ownership knowledge both hold; otherwise it uses the authored Neutral faction `Color`. Alpha remains WGP-119's independent choice: discovered/unknown-owner is opaque Neutral, undiscovered/known-owner is dim Neutral. Glyph collision skips locally undiscovered planets, while admitted background radar positioning has its own input/scroll gates. Do not hide the glyph to disable selection, and do not grant actual ownership knowledge merely because a glyph is drawn. | debug build EBL-04; effective XML EBL-05; WGUI-12/38/143/144 |
| WGP-121 | WGP-119's 128/255 override is applied to the **planet radar glyph**. World planet text, orbit ring, income/status and other overlay gadgets use a separate camera/cinematic fade value in this draw path; discovered-only force/status overlays have their own admission. The checked radar glyph draw does not draw trade-route or orbit lines, or inherit their alpha. Keep trade-route style/relation/zoom rules WGP-10..15, travelling-fleet line visibility WGP-17 and radar viewport/guide visibility WGUI-127 separate. In particular, focused hold's hidden viewport guide/zero world-label fade does not turn off the planet glyph or change its discovery alpha. | debug build EBL-04; WGP-10..17; WGUI-127/143/145 |

### Ready to implement

Comparison base: `dc95f4c6839bb4ee46c7a98112908ddd7b3f12d2`. These are
presentation fixes; authoritative conflict, scoring, observer and pause owners
retain their transitions and data. No simulation or game-code change is made
by this walk.

| Issue | Rule IDs and provider seams | Proposed feature assertions |
|---|---|---|
| Begin barrier (legacy EAWR-2699) | WGP-75/77..80/90 and WGP-112..114. `apps/viewer/src/galactic/galactic_handoff.cpp` currently sets `loading_begin_barrier = false` and clears loading on ready. Bind real terminal success/failure, admitted child/session pause and topmost accepted Begin/keys; retain this conflict's identity across ready. | With a delayed solo campaign load, ready retains the screen and paused child over subsequent updates; a fresh Begin click resumes and dismisses once, without reloading or applying a result. Separately assert Enter/Space/Escape after ready, dispatched key/button events while unsafe, failure never ready, non-topmost callback rejection, no-Begin fallback and multiplayer/solo-skirmish controls. M3's unavailable playable land child still fails explicitly or uses its admitted land-auto-resolve route. Runtime GPU case checks no tactical advancement before Begin and positive advancement afterward. |
| Name grouping (legacy EAWR-2704) | WGP-83/84/115..118. `src/presentation/ui/battle_results.cpp` currently groups by type identity. Supply resolved scoring entries in provider order plus localized name, named-hero identity, icon, cost and power; use the scoring owner's campaign/team projection rather than inferring it from AR history. | Feed ordinary types B then A with the same translated name and different icons/costs/power; assert one `x2` row represented by B, full per-entry totals, then reverse the provider order and assert A represents it. Include names `A`, `Z`, `a` and numeric suffixes to reject case folding/natural sort, equal ordinary/hero names, generic versus named hero, singleton/`x2`, 13 ordinary groups with scroll maximum 1, and 6 heroes with only first 5 shown but all totals retained. Feed a squadron count 2 and zero-cost craft entries: assert `x2`, no member-count multiplication and no zero-cost rows; separately admit a positive-cost modded fighter and verify its ordinary rules. |
| Dim radar planets (legacy EAWR-2705) | WGP-119..121 and WGUI-127/142..145. `apps/viewer/src/galactic/galactic_radar_hud.cpp` currently hides undiscovered blips. Bind observer-specific discovery and ownership permission independently, mounted faction colors, display-index/planet identity and authored radar components. | Assert the four discovery/ownership-knowledge combinations: false/false and false/true → visible Neutral, alpha 128, no glyph hit; true/false → Neutral, alpha 255; true/true → actual faction Color, alpha 255. Toggle reveal→hide and spy knowledge independently. Repeat normal/advanced radar and overview/focus: glyph alpha persists while the viewport guide follows WGUI-127. A lit GPU capture checks visible dim blip versus opaque control; adapter alpha assertions carry the exact 128 value because texture/background pixels need not equal it. Background positioning remains possible under its own admitted gates. |

The stock-data cross-check introduces no new XML application. `Text_ID`,
`Icon_Name`, `Score_Cost_Credits`, `AI_Combat_Power_Metric`, faction `Color`
and radar component texture/geometry remain mounted-data inputs. `Squadron_Units`
is formation/provider data, not a summary count multiplier; world route styles
and world-label fades are outside the radar alpha fix. Completion and discovery
alpha constants above are code-sourced, not invented data defaults.

### Retail-only qualifications

| ID | Qualified unknown | Capture recipe |
|---|---|---|
| WGP-BL-U01 | Physical input still queued when loading reopens its safe gate, repeated/held-key boundary behavior, pending cinematic keyboard audio parity and independently owned pause continuity. WGP-113 settles events dispatched while unsafe, narrowing WGP-BS-U06. | Normal and deliberately stalled ordinary/mission load; log safe-gate changes and actual key/button dispatch, inject Enter/Space/Escape/click before and across completion, release then press afresh after ready. Record topmost dialog, active child/pause owners, resume/deactivate and pending cinematic audio; distinguish dispatched-before from generated-before events. |
| WGP-BL-U02 | Native individual-fighter versus squadron/container death multiplicity, tactical-to-team forwarding and repeated-battle/restore deduplication. The consumer rule is settled WGP-117/118; ECE-U01 and WGP-BS-U03 still own producer lifetime. | Reset scoring tables; partially destroy then eliminate one stock purchased squadron, compare with spawned fighters and a positive-cost modded craft control. Log victim/member/container identities, each supplied scoring type/owner, tactical/team counts and summary before/after return. Repeat a second battle and save/load, including named-hero cargo. Never infer deaths from squadron size or the AR record alone. |
| WGP-BL-U03 | Rendered non-ASCII text, first-representative icon/clipping/hero truncation and exact radar texture/guide appearance. Comparator, slot limits and glyph alpha are already sourced. | Reuse WGP-BS-U04's equal-name/13-unit/6-hero sheet; record provider traversal and translations, including non-ASCII strings. For radar, capture same background/texture with discovery off/on and ownership unknown/known, normal/advanced/focused, with separate guide and world orbit/route controls. Record adapter alpha and tint as well as lit images; pixel intensity alone is not the 128/255 contract. |

## Advisor and event-history implementation readiness

Read-only debug-build and effective stock XML/Lua continuation, 2026-10-10,
comparison base `3c3e1d19c8abd5264b3fc329a96210e5d87d372b`. EGA-01 covers
archive dialog construction, controls and direct callers; EGA-02 covers archive
submission, advisor feedback, hint setting and the Lua story/screen-text
parameter adapters; EGA-03 covers both overlay insertion overloads and removal;
EGA-04 is a bounded instruction/string check of the screen-text categories.
EGA-X01 identifies the stock faction, GUI audio, component, constants and
story/Lua cross-check. Raw evidence remains ignored. No retail recording,
new frame-order trace or implementation is claimed.

### Short audit against the existing walks

The advisor/history work (legacy EAWR-1158) already has its core rules in the
[UI/sensors walk](gc-ui-sensors.md). This appendix adds consumer and channel
boundaries rather than another queue policy. "Sourced" below describes the
original behavior, not an implemented GC feature.

| Implementer needs | Audit disposition | Contract or residual seam |
|---|---|---|
| Localized text, color, urgent/ordinary insertion, explicit/default sound | Sourced | WGUI-66/113: duplicates accepted, ordinary FIFO, urgent prepend and removal-delay reset; no generic queue cap, expiry, priority sort or cooldown in admission. The approach producer alone has the 5000 ms throttle, WGUI-114. |
| Popup gates, duration, fades, visibility and archival side effects | Sourced; audible/rendered output qualified | WGUI-67..70/113: blocked creation retains entries; admitted sound/history precede wrap success; duration is original length times 0.1 s stock, fade changes by 8 per update. U-06/U-08 remain for runtime continuity. WGP-127 refines the channel boundary. |
| Fiscal/production, travel, combat, alliance and progress producers | Sourced at their respective owners | WGUI-114/116/117/120/121, WGM-03, WGEP-47/95..98 and WGP-53..57. Their recipient/admission differences are retained; a generic session-state change does not authorize an announcement. |
| Corruption and AI/story delivery | Sourced, with missing channel detail closed here | WGP-54 and WGCH-75/128 separate corruption completion sound from direct-action speech/hologram. WST-37/55/70/72..74 separate story evaluation, transmission, speech and movies. WGP-128..130 refine screen text and AI notification. |
| Sequential hints, defaults, disable/re-enable and clocks | Sourced; runtime qualification retained | WGUI-71/104 and GFC-28/29/GFD-29: initial enabled, frame 90, index 0; active-mode hint clock versus outer wall clock. WGP-125 adds the all-mode checkbox consumer. |
| Shared transient expiry, cap, motion, ordering, fonts and icon hit | Sourced | WGUI-82..86/118: 30000 ms event expiry, effective 23 entries under max 24, newest-to-top, fixed per-service interpolation and strict icon edges. These are not advisor or archive limits. |
| Cycle unit, sorting, repeated date headers and retention | Sourced at append/consumer boundaries | WGUI-87/94/106/119 plus WGP-122/123: current fiscal cycle at submission, stored text/color triplets in append order; no dedup or cap in these paths. Menu/load/process continuity remains qualified. |
| History dialog portrait, hint controls, reopening and refresh | Previously missing detail; now sourced | WGP-123..126. Visible wrapping, scrolling and layered-dialog behavior still need WGP-AD-U01. |
| Saved state and fresh-process continuation | Qualified unknown | WGUI-119 bounds HUD/archive nonserialization; UI/sensors U-08 and WGP-AD-U02 retain in-process versus fresh-process/menu controls. Do not infer a durable campaign archive. |

### Rules

| ID | Rule | Source |
|---|---|---|
| WGP-122 | History submission resolves `TEXT_LOG_CYCLE_01` and substitutes the **current fiscal cycle number at submission**, then stores that formatted text/color, the supplied message/color and `TEXT_HELP_SEPERATOR`/separator color. The inspected append/population paths neither supply nor consume a separate numeric event timestamp, source identity or priority. Delaying an advisor entry behind a popup gate can therefore stamp a later cycle when its admitted popup attempt calls history; the producer's original occurrence cycle must not be silently substituted. This is the submission path, not a rule that every gameplay event is archived. | debug build EGA-02; WGUI-87/94/106/113 |
| WGP-123 | Archive dialog population clears the **display list**, then traverses every stored entry from first to last. Entry index modulo 3 selects date, message or separator: date/separator each add one row; only the message is split by the listbox wrapping provider into zero or more rows. Each displayed row retains that entry's supplied color, and a separate sequential row index advances across wrapped lines. Finally refresh the list graphics and disable entry selection. This consumer has no date sort, coalescing, archive pruning or fixed row cap. The checked direct caller inventory rebuilds it at activation; submission itself does not refresh an open list. Generic listbox clipping/scroll behavior remains WGP-AD-U01. | debug build EGA-01/02; WGUI-106 |
| WGP-124 | When a local player exists, archive population sets `IDC_DROID_PICTURE_` to that player's faction `Helper_Icon_Name`, tinted white **255,255,255,255**, and disables image frames. Stock Rebel, Empire and Underworld author `i_rebel_help_droid.tga`, `i_empire_help_droid.tga` and `i_underworld_help_droid.tga`. This is a faction helper portrait for the dialog, not a per-message speaker portrait. The dialog's separate planet-ability/weather legend is static help content, not additional archived events. | debug build EGA-01; effective XML EGA-X01 |
| WGP-125 | Archive population initializes `IDC_HINTS_CHECK` to the OR of the local player's three mode hint-enabled flags. The dialog callback processes events only when its dialog is topmost. An admitted checkbox action reads that named checkbox and sets **all three** mode flags to its value, even if their previous values differed. The setters only assign enabled state, retaining each deadline and index; the checkbox does not purge pending advisor entries. Story's separate subtype-specific setters remain WGUI-115. | debug build EGA-01/02; WGUI-104/115; GFC-28 |
| WGP-126 | Ordinary archive activation requires an inactive dialog and active galactic mode, creates `IDD_DROID_LOG`, hides `IDC_DIALOG_CLOSE_BUTTON`, selects the log radio in the shared log/chart/tech/production-summary group and builds its information. Topmost Escape or `IDC_BUTTON_CLOSE` deactivates it. A true log-radio action deactivates/reactivates; other true radio actions deactivate before routing to their sibling dialog; a false radio action does nothing. Deactivation destroys the dialog and clears its active dialog identity, **without clearing the stored archive**. A separate reopen dispatcher chooses the remembered log/chart/tech/summary surface, subject to its active-mode eligibility; it does not always open the log. Existing solo pause calls and composition with other pause owners remain the frame/dialog provider seam, not a new cadence rule. | debug build EGA-01; WGUI-106/119; WGP-AD-U01 |
| WGP-127 | Advisor queue entries copy supplied localized text, RGBA and the explicit or faction-default **SFX event**. The checked popup builder starts advisor light-loop state, requests the current galactic/tactical shell animation, requests that SFX and submits history before wrapping. It creates backdrop/border/shadow/text graphics, with no per-message portrait field or automatic localization-key-to-speech conversion in these paths. Stock ordinary/hint defaults resolve to `GUI_Droid_Hint_SFX`, urgent to `GUI_R2_Urgent_Hint_SFX`; the authored GUI events use sample playback. Speech streams, conversation replacement, speech-done callbacks and command-bar movies stay separate SND/WST providers. An SFX request alone proves neither audible output nor which sound a mod authors. | debug build EGA-01/02; effective XML EGA-X01; WGUI-66/68/69/113; WST-72..74; SND-44/48/69 |
| WGP-128 | Story `SCREEN_TEXT` requires nonempty reward parameter 1 (localization key). Parameter 2 is float display seconds, default code **5**; parameter 3 optionally resolves an object type for display-name substitution. Any **nonempty** parameter 4 takes removal, including the string `0`; it is not parsed as a Boolean. Parameter 5 is teletype enabled by default, disabled when its integer conversion is zero. Parameter 6 overrides RGB from three space-separated integer tokens. Parameter 7 is upper-cased: `HINT` selects `Hint_Text_Color`, requests `GUI_Text_Hint_SFX` and bypasses the subtitle preference; `SYSTEM`/`TASK` select their respective colors and also bypass it; `ALIEN_SPEECH` bypasses it without changing the default message color. `SPEECH`, empty and unrecognized categories retain the subtitle gate. The removal/add decision is taken after these parameter/color/sound operations. This is a tutorial-text provider request, not advisor enqueue or history submission. | debug build EGA-02/03; bounded instruction/string check EGA-04; effective XML EGA-X01; WST-48/74 |
| WGP-129 | The string-key tutorial overlay adapter requires a nonnull key and, when subtitle-gated, the profile's subtitle option. It then removes existing overlay lines with that **exact case-sensitive raw key**, localizes and optionally formats the supplied type's display name, wraps using the tutorial component (cinematic width substitutes the camera width), and appends each resulting line with its original key and line index. Removal walks backwards and deletes all matching lines. The line adapter treats duration **−1** as perpetual; it retains duration, teletype, final text and color, and displays full text immediately when teletype is false. These routines call neither advisor enqueue nor archive submission. Overlay replacement must not be imported into WGUI-113's duplicate-preserving advisor queue. Visible teletype/expiry and subtitle/sound combinations remain WGP-AD-U01; the overlay clock/cadence provider awaits the frame programme (legacy EAWR-2741), rather than borrowing the advisor clock. | debug build EGA-03; WGP-128; WST-48/72 |
| WGP-130 | Lua `Story_Event` accepts one string and optionally one game object, then sends `STORY_AI_NOTIFICATION` with those values and no triggering player to the **active mode's story**. It makes no direct advisor, transient-message or archive call. Human-visible output requires an eligible story event and its reward/dialog branch; WST-13/20/37/55/70 own those gates and ordering. `Game_Message` instead has WGUI-115's direct localized urgent advisor route. An AI/story notification name must not become a generic visible log line. Stock Lua uses these APIs for different purposes. | debug build EGA-02; stock Lua EGA-X01; WST-37/55; WGUI-115 |

### Ready to implement: advisor/history

The GC HUD inventory's GHUD-S35/S36 surfaces require these provider seams
(legacy EAWR-1158). At the comparison base, `src/presentation/ui/battle_messages.cpp`
provides battle result/pause text, and `apps/viewer/src/galactic/battle_dialog.cpp`
displays auto-resolve advice; neither supplies the per-mode advisor queue or
cycle-stamped archive. `src/sim/galactic/story_adapters.cpp` supplies named
AI-notification matching, not a presentation announcement. The live save
adapter retains selected-map/battle-briefing hints, not this archive.

| Provider seam | Ready rules | Proposed focused test |
|---|---|---|
| Semantic producers → localized text/color/urgent/optional SFX | WGUI-114..117/120/121; WGP-54/127/130; WGEP-95..98; WST-70/74 | Same state change with local/nonlocal/hidden recipients; duplicate ordinary and successive urgent requests; approach at 4999/5000 ms; AI notification with no eligible story output produces no text. Corruption completion sound alone produces no invented advisor entry. |
| Per-mode queue + outer clock + popup/wrapping/audio providers | WGUI-66..70/113/122; GFC-29/GFD-29; WGP-122/127 | FIFO and urgent reversal; movie/map/menu gates retain entries; failed wrap retains front but requests sound/history; a gate spanning cycle N to N+1 stamps N+1 at submission; fixed 8-alpha steps and original-length duration. No implicit speech or speaker portrait. |
| Local hint state + active-mode clock + dialog checkbox | WGUI-71/104/115; GFC-28; WGP-125 | Independent mode deadlines/indices; mixed true/false flags display checked; topmost toggle writes all modes without resetting indices/deadlines or removing queued messages. Keep hint enqueue and later popup display distinct. |
| Shared transient messages + texture/callback providers | WGUI-82..86/118 | Effective 23 entries, 30000 ms equality expiry, setup font overrides, duplicate same-cycle triplets, null/sentinel/missing icons, exact rectangle edge rejection and per-call interpolation. Never impose this cap/expiry on the archive or advisor. |
| Process archive + fiscal/localization providers + read-only dialog view | WGUI-87/94/106/119; WGP-122..126 | Repeated cycle headers, explicit append order rather than date sort, retained submitted text/color, multiline message-only wrapping and disabled selection. Append while open then explicitly rebuild/reopen; dialog close retains storage while explicit clear removes it. Large-list display is a runtime qualification. |
| Story tutorial overlay + subtitle/type/localization providers | WGP-128/129; WST-48/74 | Default 5 s, −1 perpetual, nonempty `0` removal, teletype zero, RGB/category precedence, subtitle-off `HINT` versus `SPEECH`, same-key replacement and case-distinct keys. Verify separate requests for screen text, speech and movie; no advisor/archive entry from the overlay adapter alone. |
| Session/front-end owner + explicit clear/restore seams | WGUI-119 and WGP-AD-U02 | Separate in-process load, menu return/new campaign and fresh-process load; preserve independent capture controls. Do not claim archive serialization from story/player persistence or our presentation-hints container. |

The data registry remains unchanged: this walk applies no tags. Implementation
must resolve faction `Helper_Icon_Name` and advisor `SFXEvent_HUD_Advisor_*`,
hint lists, `Advisor_Hint_Interval/Duration`, date/separator and tutorial category
colors, fonts/components and speech/movie definitions through mounted data.
Archive message color is the submitted color; authored `Droid_Text_Color` is
not read by the inspected archive append/consumer. This bounded unused-tag
finding does not declare that tag ignored by every subsystem. Display duration
5, perpetual sentinel −1 and the all-three-mode checkbox loop are code facts.

### Remaining runtime qualifications

| ID | Qualified unknown | Capture recipe |
|---|---|---|
| WGP-AD-U01 | Visible popup/helper portrait, light/shell/audio, dialog wrapping/scroll limits, open-list refresh and tutorial teletype/expiry. Static source boundaries above are settled. | Submit duplicate/urgent, long multiline and non-ASCII text with all popup gates, missing SFX and failed-wrap controls. Open the log before/after submissions; compare reopen/radio rebuild with a still-open list, and exceed the transient cap independently from a large archive. Check helper portrait for each faction, topmost versus covered checkbox/Escape actions and log/chart/tech/summary reopening. Stage subtitle-off `HINT`, `SPEECH`, `ALIEN_SPEECH`, RGB overrides and same-key replacement. Record text, light/shell, audio and overlay independently. Existing GFC/GFSC cases own clocks/pause; route any new cadence question to the frame programme. |
| WGP-AD-U02 | In-process load/menu/fresh-process archive retention and pending queue/hint continuation. WGUI-119's bounded nonserialization does not decide every lifecycle route. | In cycle N submit distinct ordinary/urgent/transient messages with hints in mixed states, save while a popup is blocked, submit control text in N+1, then load without leaving the process. Separately return to each front-end path/new campaign and restart the process before loading the same save. Capture ordered triplets, pending versus displayed text, all mode flags/deadlines/indices and archive clears; compare an uninterrupted control. Treat missing/retained old presentation as observed lifecycle behavior, not proof of a numeric saved event time. UI/sensors U-06/U-08 and WST-U11 keep their unsampled branches. |

## Post-space-win invasion-offer implementation readiness

Research dated 2026-10-10, compared against
`6d73fe514fa82ff72154184620b11b164c8cdee7`. EIO-01..08 identify fresh,
read-only **debug build** queries; EIO-X01 and EIO-L01 identify effective
**XML/resource** and **stock Lua** cross-checks. Raw evidence and its digest
index remain in ignored `out/research/invasion-offer/`. No retail recording
or new frame-order trace is claimed. Existing GFS-02/21 and GFC-20/24 in the
[galactic schedule](../galactic-frame-service-schedule.md) retain service,
pause and resolver ownership; the close-vote timeout below is a separate
local consumer, not another galactic deadline.

This appendix qualifies the invasion-offer HUD and tactical-return work
(legacy EAWR-2302, EAWR-1116). **Prepared candidate**, **visible offer**, **accepted
command**, **queued conflict** and **landed survivors** are separate facts.
WGP-35's shorthand must not turn preparation into an admitted visible offer.

### Short readiness audit

| Implementer needs | Starting disposition | Settled contract or retained seam |
|---|---|---|
| Recipient: local, attacker, human and multiplayer | Sourced preparation WGP-88; visible recipient qualified WGP-BS-U05 | WGP-131/134: local-allied winner, winner-owned fleet scan; no explicit attacker/human predicate in preparation. Actual visibility for each multiplayer observer remains WGP-INV-U01. |
| Planet identity, accessibility and own-planet suppression | Sourced WGP-35/88; accessibility detail missing | WGP-131/133: first matching type, destroyed-state-aware authored accessibility, exact local ownership. |
| Transport versus cargo identity, liveness and capability | Sourced WGP-88; provider equivalence missing | WGP-132: direct transport ID, nonempty cargo and live transport; not WGM-106's ground-combat-capability predicate. |
| Already invaded planet and reinforcement/landing caps | Missing offer-specific qualification; landing sourced WGM-39/40/49/60 | WGP-132/136/137: no such suppression in inspected candidate/request paths; command has code limit 127, ordinary landing/return retains authored capacity 10. |
| Accept, decline and stale retained identities | Sourced WGP-89; command admission detail missing | WGP-135..137: local intent, resolve IDs again, ordinary queued command, ownership and story/pause gates; no second outcome application. |
| Timeout and lifetime | Missing offer-specific qualification | WGP-138: no solo candidate deadline in these consumers; multiplayer close voting uses strict wall elapsed > authored 15 s. Lifecycle/load remains qualified. |
| Space auto-resolve versus fought-space return | Qualified WGP-BS-U05; caller-layer detail missing | WGP-139: inspected AR activation passes galactic layer, skipping space-only candidate preparation, and hides the button. |
| AI decision and retained invasion queue | Sourced WGA-232/233/235/236/239/240 | WGP-140 cross-checks stock conquest/landing adapters; no automatic AI acceptance of a summary offer. |

### Rules

| ID | Rule | Source |
|---|---|---|
| WGP-131 | Each shared summary build clears the candidate planet, transport IDs and invasion intent. Preparation requires a winner **allied to the local player**, a supplied location type, a completed **space** layer and an active galactic parent. Select the first world object matching that location type, then scan its attached fleets owned by the **winner**. This body neither compares winner to the conflict attacker nor checks human status. Local-allied preparation therefore is not a sourced attacker-only/human-only visible-offer rule, nor permission for each multiplayer owner to command the winner's transports. | debug build EIO-01; qualifies WGP-35/88 |
| WGP-132 | Traverse attached fleets and their direct contents in their existing order. Retain a member's **transport identity** only when it has transport behavior, a present nonempty cargo list and is not dead. The inspected scan does not flatten payloads, require positive payload combat power, retest payload death, apply a reinforcement/landing count limit or query an existing land invasion. A nonempty candidate planet can remain even with zero retained transports. WGM-106's ground-capability predicate is a later admission fact, not this roster filter. | debug build EIO-01; WGP-88; WGM-106/107 |
| WGP-133 | After collecting IDs, inaccessible surface clears the candidate planet; exact planet-owner equality with the **local player** also clears it. Neither branch clears the collected ID vector. Surface accessibility returns false for a destroyed planet, otherwise reads its type's `Planet_Surface_Accessible` Boolean. This is not suppression for every allied-owned planet. Stock authors accessible and inaccessible planets separately. | debug build EIO-01/06/08; effective XML EIO-X01; WGP-35/88 |
| WGP-134 | Both fought-battle and auto-resolve summary activation explicitly hide `IDC_LAND_INVADE_BUTTON`. The shared builder does not restore visibility. The checked defined-string references contain only those two activations and the button-action callback; candidate-planet/intent direct references add no independent visibility writer. Stock resources define `TEXT_BUTTON_INVADE`, but resource existence does not override the hide. This closes the inspected static paths, **not** every possible indirect/runtime writer or a retail executable variant. WGP-BS-U05 remains visible-offer qualified through WGP-INV-U01. | debug build EIO-01/02/03; resource data EIO-X01; WGP-87/88 |
| WGP-135 | Only the topmost summary handles these actions. An invasion button action sets intent true and deactivates directly; it does not request a close vote. Continue, Escape, Enter and Space leave the initially false intent unchanged and either deactivate or request the existing close vote. In the ordinary campaign return branch, accepted intent plus a retained planet resolves stored IDs again, skips missing objects and queues an invasion command only if any resolve. This close path does not repeat cargo/liveness/capability checks. Declining dispatches no invasion, does not destroy or relocate transports and does not undo the already applied space result. Preserve WGP-89's session-owned pause/return boundary. | debug build EIO-01/04; WGP-87/89 |
| WGP-136 | The accepted ordinary invasion command stores the **first at most 127** resolved object IDs, a code command limit, not a ground landing cap. The summary leaves issuer unspecified, so the inherited command constructor retains the **local player**, not the allied winner. Execution rejects a paused active galaxy; in single player, queued tactical work or active speech also rejects when a local conflict is queued. Otherwise resolve command IDs again and retain those still present and owned by that local command player; no later ID backfills one rejected after truncation. Local-allied preparation does not grant control of allied transports. Story land-force/special-map/landing callbacks can divert or bounce the ordinary request; only a nonempty accepted list with no special-map diversion reaches ordinary land admission. The checked execution body adds no dead/cargo/cap/duplicate-invasion filter. These bounded checks do not authorize bypassing the campaign service's validated command boundary. | debug build EIO-04/05/08; WGP-135; WGM-39/42 |
| WGP-137 | Ordinary land admission copies the explicit transport-ID list and derives an unspecified defender from nonzero ground-base ownership, otherwise target allegiance, with the first landed transport as fallback for absent/neutral allegiance. Distinct invader/defender appends the record on this accepted-summary command path; equal identities add no conflict. The inspected request body does not reject another invasion at the same planet or cap this list to `Max_Ground_Forces_On_Planet`. Tactical initial/reserve selection, capable-opponents arbitration and actual landing/return retain WGM-40/42/49/60 and WGA-235/236/239/240. Stock capacity **10** is authored; it is not the candidate or command-ID limit. | debug build EIO-05; effective XML EIO-X01; WGA-233; WGM-39/40/60 |
| WGP-138 | No solo invasion-candidate expiry is read or serviced by the inspected summary paths. Rebuilding resets its candidate/IDs/intent; closing destroys the dialog, but that body does not itself clear those retained fields. The voting summary's timeout runs only while close-voting is enabled and active: wall elapsed **strictly greater than** `Battle_Pending_Timeout_Seconds` × 1000 requests resume with auto-resolve false and resets voting. Stock is **15 s**, so equality at 15000 ms does not trigger it. All-human yes votes request the same resume. Neither timeout nor ordinary close sets invasion intent. This is close voting, not a solo offer TTL, a tactical solver delay or evidence of persistence across save/menu/restart. | debug build EIO-01/04/07; effective XML EIO-X01; GFC-20; WGP-89 |
| WGP-139 | Auto-resolve summary activation requires an active galactic mode and no next queued campaign tactical conflict, stores resolver data and disables close voting. It invokes the shared builder with the **active galactic layer**, not the completed space layer; resolver data supplies the winner but does not replace that layer argument. Thus this inspected call skips WGP-131's space-only candidate preparation, besides WGP-134's explicit button hide. Do not infer an AR invasion offer from alive cargo or shared builder reuse. An explicitly admitted M3 land action still routes to the existing land auto-resolve owner as project policy under WGP-35. | debug build EIO-01; WGP-87/88; WAR-70 |
| WGP-140 | Stock AI conquest does not click or accept the local summary offer. The opponent ground thread waits for its authored space-secured condition, successfully launches/reforms, retests `Is_Good_Ground_Grab_Target`, blocks on movement and calls its retained-force `Invade`; the pirate mixed force moves then calls `Invade`. The stock adapter requires a returned invasion block and successful blocked result, prevents maintenance removal, then blocks on `Land_Units` before reporting success. Native retained-fleet transport gathering, queue append, arbitration, undefended completion, initial/reserve identities and capacity remain WGA-232..240. A space win alone supplies neither a land conflict nor invasion success. | stock Lua EIO-L01; WGA-232/233/235/236/239/240 |

### Ready to implement: offer and return providers

At the comparison base, `include/eawr/campaign/conflict.hpp` exposes an
`InvasionOffer` as planet/fleet/player, while
`src/campaign/tactical_return.cpp` collects ground-capable winner fleets only
for invader wins, with raid/corruption exclusions. This is **partial** for
WGP-131..137: it lacks the prepared explicit transport subset and local-owned
planet qualification, and its capability filter is stricter than nonempty
cargo. `src/campaign/game_session.cpp` publishes that result after committed
return; publication does not independently prove native button admission.
`LandFleet` in `src/sim/galactic/control.cpp` is an ordinary command owner,
but selecting a whole live fleet is not equivalent to replaying retained
transport IDs. These are provider gaps, not new implementation in this walk.

| Consumer/provider seam | Ready contract | Proposed focused test |
|---|---|---|
| Return identity/cargo + local/alliance/location projection → summary candidate | WGP-131..133 | Winner local/allied/enemy and attacker/defender controls; two winner fleets plus other-owner fleet; alive loaded, empty, dead and missing transports; cargo without ground capability; own/allied/hostile/destroyed/inaccessible surfaces; zero candidate IDs. Retain traversal order and transport-to-payload identity. |
| Explicit admitted action permission + immutable observer view → HUD intent | WGP-134/135/139; WGP-INV-U01 | Candidate-present but permission-absent has no invasion action. AR call has no prepared candidate. Inject an admitted project offer separately; accept/decline/topmost/keys request one session-owned transition and never apply the space result again. Actual native visible permission remains capture-qualified. |
| Retained ID list + command issuer + story/conflict providers → land request | WGP-135..137; WGM-42/49/60 | Delete/change owner after preparation and after close; add a new fleet member without adding its retained ID. Test 126/127/128 IDs, story diversion/bounce, paused and queued-speech rejection, existing same-planet invasion, equal/derived defender and later full capacity. No fabricated acceptance success or automatic prep suppression. |
| Summary close-vote wall input + session pause/return owner | WGP-138; WGP-89 | Inject elapsed 14999/15000/15001 ms in active/inactive voting and solo; only active voting after strict threshold requests resume, never invasion. Decline preserves orbital identities and independently owned pause. Lifecycle continuation remains WGP-INV-U02. |
| Retained AI plan + movement/invasion block + land arbitration | WGP-140; WGA-232..240 | Surviving space fleet without a qualifying ground thread produces no automatic land request. Distinguish command queued, block returned, defended/undefended completion, successful action and final landing; M3 land auto-resolve consumes the ordinary admitted conflict once. |

No tag registry changes are made by this research. Implementation reads
`Planet_Surface_Accessible`, `Max_Ground_Forces_On_Planet` and
`Battle_Pending_Timeout_Seconds` through mounted data at their respective
providers. `Land_Auto_Resolve_Delay_Seconds` remains GFC-24's solver clock;
it is not offer expiry. Reinforcement capacity and arrival mechanics remain
the land handoff/reinforcement owner. GUI resource/localization keys select
controls and text, not visibility permission. The **127** command-ID limit
is code-backed, independently of the stock **10** and **15 s** XML values.

### Remaining runtime qualifications

| ID | Qualified unknown | Capture recipe |
|---|---|---|
| WGP-INV-U01 | Visible offer admission, any indirect writer/retail variant, and actual multiplayer recipient/button behavior. Narrows but does not close WGP-BS-U05. | Extend GHUD-C14: fought-space win above hostile accessible surface with living loaded transports; log candidate planet/IDs, dialog create/hide/later visibility requests and actual pixels. Repeat local attacker, local defender winner, allied winner, each human multiplayer observer, own/allied/inaccessible/destroyed surface, empty/dead/no-ground-capability cargo, already invaded planet and more than 10 candidates. Run space AR as the no-preparation control under WGP-139. Accept/decline only a visible admitted action; hidden-action injection tests command plumbing, not offer visibility. |
| WGP-INV-U02 | Full runtime command/close-vote/lifecycle continuation, including stale identities and story diversion across retained pause, save/menu/restart and multiplayer clients. Static local guards and timeout are sourced above. | Record prepared list, resolved close list, capped command list, executing issuer/ownership, story callbacks, appended conflict and final landed/orbital IDs separately. Delete/change owner between boundaries; use 127/128 candidates, a pending same-planet invasion, full surface capacity, special-map bounce and two independent pause owners. For multiplayer record all close votes at 15000/15001 ms and resume/close on each client without asserting first-service cadence. Compare uninterrupted solo with in-process load, menu return and fresh-process load; never infer persistence from the campaign result record alone. |

## Information-page data owners and archives

Focused provider walk, 2026-10-10. [Reference-page rules](../gc-reference-pages.md)
GREF-01..23 already source the page bodies, axes, faction colours, dimming,
Summary aggregation and remembered-tab help. These appended rows settle their
remaining local writers/readers; they do not repeat the retail qualification.
Evidence EIP-01..06 is read-only **debug build** research; EIP-D01 is effective
stock faction/global/dialog data and EIP-L01 is stock Lua. Raw evidence and its
mapping remain private. No new retail capture is claimed.

| ID | Rule and provider boundary | Evidence |
|---|---|---|
| WGP-141 | Each player owns its own chart vector. Both inspected fresh-player constructors initialize an empty vector; they do not seed a synthetic frame-zero sample. The chart getter returns that player's archive. Recording is in player service and is not conditioned on opening History or on the player being local. | debug build EIP-01/02/03; GREF-01/11 |
| WGP-142 | Inside admitted player service, record only when the currently active subtype is galactic and its current mode frame is divisible by **30**, or is **less than 3**. The 30 is code, not `Fiscal_Cycle_Time_In_Secs` or a chart XML interval. Sample current credits/raw technology and the current owned-object list's planet count and summed ground/space combat power, then append one record. This local branch has no last-recorded-frame check, pause check, catch-up loop or positive elapsed-time requirement. Repeated calls at the same qualifying frame therefore each append locally; whether the outer loop admits those calls is a separate schedule qualification. | debug build EIP-01; GREF-11; GREF-U01 |
| WGP-143 | After the optional append, on every admitted galactic player turn, test the resulting archive size. Size at least **100** and even triggers GREF-12's adjacent-pair averaging and replaces the vector. This is not a change to the 30-frame recording gate, not a sliding window, and not restricted to a turn that appended. Size 99 stays 99 without an append; appending to 99 compacts 100 to 50. A restored even-size archive at or above 100 can compact on a nonsampling turn. | debug build EIP-01; GREF-12 |
| WGP-144 | The player save writer stores archive count and every retained sample in vector order: integer mode frame, floating credit balance, integer planets, integer technology and floating military power. It saves the compacted values actually present, not reconstructed observations or just current statistics. The reader clears the chart vector, reads that count and appends those saved fields in the same order. It neither samples current objects nor compacts locally while reading. | debug build EIP-02; GREF-U02 |
| WGP-145 | The inspected player post-object-load hook forwards to its AI only; it does not reseed or rebuild the chart. Fresh construction, chart deserialization and first subsequent admitted player turn are separate boundaries. WGP-142/143 can change restored samples on that later turn. Complete first-turn clock/mode admission remains the session/frame programme, rather than a new chart continuation promise. | debug build EIP-02/03; GREF-U02 |
| WGP-146 | Caller-position note for the frame programme: the player list invokes player service in ascending ID. Within one player, AI and tactical queue work precede chart recording/compaction; local hint generation follows it. Under the existing GFS-12/14/15/21 outer schedule this is after mode/fiscal work and before global scripts, story and final active-mode timer advance. The recorder reads the active mode at its own turn, not a captured parent-galaxy clock. Mode switches, retained qualifying frames and first post-load admission require the named schedule seam. | debug build EIP-01/04; [GC frame schedule](../galactic-frame-service-schedule.md), GFS-12/14/15/21; GREF-U01/02 |
| WGP-147 | Tech Tree reads the faction's `Tech_Tree_Dialog_Name`; the Space variant appends `_SPACE`. If dialog creation fails, the inspected path falls back to the corresponding Rebel dialog. Node population enumerates registered types, admits faction affiliation, replaces hyphens in a type name with underscores and populates an existing enabled `IDC_TECHTREE_` slot for that resulting name. Types without a matching slot or faction affiliation do not become nodes. It does not enumerate owned units or all currently producible types. | debug build EIP-05; effective data EIP-D01; GREF-04 |
| WGP-148 | The authored-node pass reads raw player technology and the unlocked-type list for GREF-05's dimming. It does not query the explicitly locked/disabled lists, a separate stolen-tech history, owned black-market ability list, purchase offers or local planet prerequisites. Theft changes this page through its already-owned raw-tech/unlock results (WGCH-32/34), not an added enemy node or stolen badge. The dialog named for black market is the authored Consortium unit/building diagram; its name does not establish a black-market item inventory. A bought item's effect can affect another provider, but no separate purchase badge/list is constructed here. | debug build EIP-05; EIP-D01; WGCH-32/34/37; WGEP-55/58; GREF-U03 |
| WGP-149 | Player serialization preserves raw technology and the unlocked-type identities separately from purchased black-market ability identities. The local unlocked-list reader clears then resolves saved type identities. A restored Tech Tree consumes those providers and mounted authored slots; it does not derive its nodes from the saved chart's technology samples. Full purchase/restore effect ordering belongs to the action/session owners. | debug build EIP-02/03/05; GREF-U02/03 |
| WGP-150 | The inspected Summary Production rebuild enumerates current planetary objects and admits rows by exact local-player ownership. Its category data is rebuilt from those planets, sorted initially by name and totalled over the whole list; it is not read from the chart archive, cumulative battle score or advisor messages. Production capacity comes from planet/current starbase/special-structure type contributions; fronts come from planetary production and persistent upgrades from the planet's retained built-upgrade list. Economy reads the planet's basic/trade income and typed income-modifier sources, retaining GREF-13's independent rounding. Fleet breakdown stays GREF-15's separate allied-content admission. | debug build EIP-06; GREF-07..18 |
| WGP-151 | Summary Production activation builds immediately. Each of the inspected Production, Economy and Fleets service handlers requires its page active and a nonnull galactic mode; a different subtype deactivates the page. When supplied service frame minus retained last-service frame is at least logical FPS, store the supplied frame and rebuild once. No local catch-up loop is present. Production rebuild reclamps offset and invokes the complete data/name-sort/totals path. This refresh input is the supplied service frame, distinct from WGP-142's active-mode timer; actual paused/first-refresh admission is not established by the local handler. | debug build EIP-06; GREF-16/22; GREF-U04 |
| WGP-152 | Chart samples and Droid Log text have different owners and persistence evidence. Chart fields are explicitly player-serialized under WGP-144. WGUI-119's shared text archive has no save/load path in its bounded reference inventory; its first loaded display remains qualified. Three mode hint flags/deadlines/indices are player-serialized under GFC-41. GREF-19's authored legend is reference data and GREF-21's help route remembers a tab; neither creates an extra campaign aggregate or separate help overlay. | debug build EIP-02; WGUI-106/119; GFC-41; GREF-19..21 |

The frame schedule owns full admission and ordering. WGP-146 is a bounded
writer/reader note for that programme to reconcile, not another world-service
phase. A provider implementation keeps partitioned inputs/staging and ordered
commits; it must not copy the original serial owned-object traversal.

### Cross-check and remaining providers

Effective stock factions select Rebel/Empire diagrams and the Consortium
combined diagram through `Tech_Tree_Dialog_Name`. Stock dialog slots supply the
type membership and arrows. `Affiliation`, `Tech_Level`,
`Build_Initially_Locked`, `Icon_Name` and `Text_ID` feed existing type facts;
the tree has no separate chart/theft/market tag to invent. Stock global data
sets fiscal cycles to 45 seconds, which supplies chart labels through GREF-03
and does not replace WGP-142's code gate. Stock Lua's black-market AI action
moves its buyer, activates the ability and reports the plan result; it supplies
no chart recorder or reference-page node enumeration. Action outcomes remain
WGCH-32..38/WGEP-55/58. No tag-registry changes are made by this walk.

Against `d1bad14fdac53ce2c0d5c4a028343a693a5b8c53`, the existing
`reference_pages` model covers History layout, tree dimming and Summary
aggregation. `reference_pages_widget.cpp` publishes tree raw-tech/unlocks and
observer-authorized Summary facts, but explicitly leaves History unavailable.
There is no chart archive owner/recorder/checkpoint/publication in the inspected
GC session/provider files. The widget refreshes on changed copied snapshots;
native Summary service admission and name-sort reset need the explicit seam
above rather than an unsupported parity claim. Persistent upgrades, fleet
metadata and log/hints/legend providers retain the existing readiness audit in
[the reference-page note](../gc-reference-pages.md).

| Provider gap | Rule contract | Focused acceptance input |
|---|---|---|
| Player-owned chart archive, admitted append and compaction | WGP-141..146; GREF-01..03/11/12 | Fresh empty archive; frames 0/1/2/3/29/30/31/60; two calls at 30; active tactical child; nonlocal player; 99/100/101/102 rows and integer/fractional pair means. Frame programme supplies admitted-call sequence; compare resulting archives on 1/2/4/8 workers. |
| Save owner and immutable History publication | WGP-144/145/152; GREF-01..03 | Save compacted fractional values and integer fields, restore order exactly before service; then inject nonsampling and repeated qualifying first turns separately. Publish authorized lines plus global axis facts, not inferred current totals; distinguish absent from empty. |
| Authored complete type registry and unlock publication | WGP-147..149; GREF-04..06 | Affiliated/unaffiliated and hyphenated names; absent/disabled slots; tech below/equal; initial lock released/unreleased; credits and explicit disable changes alone; purchased market ability without an authored node; save/load unlock identity. Mounted-data lookup must include authored nodes absent from a build palette. |
| Summary current-state publication and refresh input | WGP-150/151; GREF-07..23 | Change owner, front, income modifier, upgrade and fleet membership between copied publications. Service deltas FPS-1/FPS/FPS+1, inactive/null/tactical controls and one large jump; keep callback sort/offset distinct from complete name-sort rebuild. Retain six/ten rows and existing observer filters. |

Runtime qualification recipes GREF-U01..04 are defined in
[the reference-page note](../gc-reference-pages.md). The bounded static rules
make providers reviewable; actual pause/load continuity and changed-state
display still need those captures.

## Conflict-loading provider readiness

Focused read-only debug-build walk on 2026-10-10, comparison base
`9938fb8275ddccd7599660f38230791cb39b5c91`. This appendix extends the earlier
battle-screen and Begin walks; it does not supersede their preview or input
contracts. Private ECL evidence includes dialog admission/information/input
(01..03), loading worker/transition/forces/map reader (04..07), space load
(08), land map reader (09), local/remote progress (10/11), error popup (12),
corruption admission/mission queue/transition (13..15), progress caller inventory
(16), and effective XML, resources, text database and stock Lua (X01).
No retail run or new frame-order claim is made.

### Audit and remaining implementation inputs

| Required behavior | Status before this focused walk | Resolution or qualification |
|---|---|---|
| Planet display name, actual owner/name/color, basic income and static descriptions | Sourced WGP-31/75/76; live intelligence is a separate provider | WGP-161/162 source construction and participant boundaries. |
| Independent current-model clone and location-type fallback, framing and teardown | Sourced WGP-32/51/52 | Keep those inputs; actual animation phase/cold-load appearance remain WGP-BS-U01 and U-11. |
| Resource selection, ordinary versus mission title and MT/MB transformation | Sourced WGP-75/76/90 | WGP-168..170 link corruption admission to the queued mission resource and distinguish briefing/script from loading text. |
| Native stages and precise force progress | WGP-33/77 and WGT-03 source the late milestones; earlier producers and exact per-list denominator were missing | WGP-163..166 settle them, including regressions and completion boundaries. |
| Selected failure map, message, acknowledgement and continuation | WGP-80 sources selected map/base and assertion; WGP-BS-U02 qualifies recovery | WGP-167 settles the debug popup and post-acknowledgement structure; retail recovery remains WGP-CL-U02. |
| Solo Begin and premature dispatched input | Sourced WGP-78/79/112..114 | The current handoff implements the solo Begin barrier; physically queued/held input and cinematic audio remain WGP-BL-U01. |
| Admitted M3 land auto-resolve versus authored tactical missions | Project scope WGP-90; WGCH-08/10 and WST-44/50/51 own mission boundaries | Publish explicit admission and unsupported-child outcome; a prohibited tactical mission cannot become a successful land auto-resolve by UI policy. |

### Additional loading contracts

| ID | Rule and boundary | Source |
|---|---|---|
| WGP-161 | Fresh dialog entry retains the planet, layer, defender/map-owner identity, dialog map, optional location type and original-map identity **before** building its information and requesting load. Ordinary campaign entry first delivers the planet/layer story notification and reads the corresponding one-shot tutorial map; queued entry instead receives a retained location/layer/map-owner/map bundle. The dialog resource, authored texts and independent planet clone are built during activation, before the later admitted loading callback constructs the worker. The worker carries the retained planet/layer/map-owner/map into the child transition. This is a load boundary, not a service-frame delay. | debug build ECL-01..04; WGP-75/76/51 |
| WGP-162 | The inspected information builder uses the supplied planet's **current type and actual owner**, its basic income and authored `Describe_*` keys; it performs no observer discovery, apparent-owner or force-intelligence query. Those loading facts are not the galactic sensor projection. On each local UI, participant context instead controls progress: solo campaign uses one bar; network/skirmish enumerates human participants, using the saved connection list in network context and the current player list otherwise, with retained participant IDs, player names and player colors. Network/local callbacks and received participant progress have separate rules; do not manufacture different planet descriptions from progress-seat identity. | debug build ECL-01/02/10/11; WGP-31/76/77 |
| WGP-163 | The inspected space loader reports raw **0.25** after the shared mode-load call, then **0.5** after enabling grid culling and partitioning the scene. These reports are not conditional on that shared call's returned success value; the loader returns that value later. Camera setup follows the 0.5 report on the ordinary map path. The caller inventory identifies these two early space reports, the per-roster producer and the transition's late reports; save-operation progress is a separate consumer. There is no authored duration percentage or translated stage-label list in these bodies. | debug build ECL-08/16; WGP-77 |
| WGP-164 | The inspected land map reader reports after each closed top-level chunk: **min(0.95, 0.95 × current file position / file size)**. Its work includes scene, terrain, camera and passability input; file position is not terrain/object count or elapsed time. After boundary/camera/environment postprocessing it reports **0.95**, then returns its accumulated load result. That final fraction is therefore not independently a successful-load assertion. The inspected outer land loader has no matching space-style 0.25/0.5 reports. | debug build ECL-09/16/17; WGT-03 |
| WGP-165 | Each admitted starting-force roster reports **0.65 + 0.2 × i/N before processing entry i**, where `i` starts at zero and `N` is that invocation's current roster count. An empty roster emits no per-entry report. Its final entry is below 0.85, and the next roster invocation restarts at 0.65; these are not fractions over both sides combined. Land file progress can precede a lower force fraction; a large roster can precede the transition's lower 0.75. Raw callbacks must retain their identity/order independently of any viewer smoothing. This narrows WGT-03's “0.65 to 0.85” shorthand. | debug build ECL-06; WGT-03; WGP-77 |
| WGP-166 | On map-load success, child force construction precedes **0.75**; reveal recomputation, capture-point ownership transition and starting-camera placement precede **0.9**. Later transition work includes indigenous bonuses, victory conditions and destroyed-planet replacement; its common active-child tail reports **1.0** and removes that child's progress callback before the remaining odds/script tail. The outer loading callback waits for worker completion, performs its completion/pause operation and post-load fixups, then follows WGP-78/112..114's Begin or admitted automatic-dismiss route. The worker does not carry a checked transition-result value back to this UI callback. A numeric 1.0, worker exit or shown Begin cannot substitute for an explicit verified terminal outcome in our provider. | debug build ECL-03..05; WGP-33/78/80/112..114; WGT-03 |
| WGP-167 | A false selected-map load produces the fixed English message **“Error: Could not find/load map named '%s' for %d level base.”**, substituting the selected logical map and layer's base level, then calls the debug assertion path. The popup helper uses an OS error message box with OK acknowledgement unless quiet assertions divert the message to a warning file. It is not the mounted default in-game message resource or an inspected localization-key lookup. Acknowledgement returns to the assertion; the function has no failure-branch retry, parent restoration or early failure return there. If execution continues past the assertion, common later transition work can still reach the 1.0/true tail. Retail assertion policy and usable recovery remain qualified; never publish ready merely from that tail. | debug build ECL-05/12; WGP-80; WGP-CL-U02 |
| WGP-168 | WGCH-08's tactical corruption route reads the selected planet slot's `Corruption_N_Tactical_Game_Mode`, `Corruption_N_Tactical_Map_Name` and `Corruption_N_Tactical_Script_Name`. A nonempty map and land/space mode are necessary; the actor must be non-AI in single-player campaign, and fast-corruption bypasses the tactical route. Accepted queueing requests mission transition and installs invader-success corruption with **5 s** duration; acting-hero removal/respawn and failed-queue consequences remain WGCH-05..10. AI, multiplayer and direct corruption do not supply a tactical loading descriptor through this path. These are mission-admission facts, not an ordinary invasion inferred from the planet's faction. | debug build ECL-13; effective XML ECL-X01; WGCH-08/09/10 |
| WGP-169 | Mission loading heading/body are **MT/MB + uppercased dialog map truncated at its first `.TED`**, with no path stripping, and missing translations display the constructed key. The heading is white `IDC_MISSION_STATIC`; the white body is wrapped into `IDC_MISSION_LIST`. Neither uses `Corruption_N_Mission_Briefing` or the tactical script name as its loading text key. Effective stock XML has **29** nonempty tactical corruption slot-map bindings across land/space, all with both derived keys in the English text database; that census is evidence, never a runtime allowlist. The named plot's stock Lua owns tactical objectives/results, separately from these UI keys. Both mission resources retain Begin and the ordinary location/model facts. | debug build ECL-02; effective XML/resources/text/stock Lua ECL-X01; WGP-75/76/90 |
| WGP-170 | The inspected queued mission transition resolves the retained planet and authored defender faction separately from the attacker. With a loading callback it publishes a bundle of **layer, resolved map-owner ID, planet and authored override map**; without one it directly transitions. The queued dialog entry fixes solo, non-skirmish context and selects mission presentation for a nonempty retained map. Queue preparation retains the tactical script and marks linked-tactical/skip-summary state; transition sets the script override and disables auto-resolve. Thus ordinary land conquest, tutorial override, linked story and corruption mission share loading presentation but have different admission, script, persistence and return owners. Keep WST-44/50/51 and WGCH-10/WGM-81 boundaries instead of applying normal conquest reconciliation twice. | debug build ECL-01/14/15; WGP-75/90; WGCH-08/10; WST-44/50/51 |

Land failure uses the current **ground-base** level and space uses the current
starbase level under the inspected child transition. Dialog/thumbnail map,
selected child map and original-map identity are separate fields: WGP-76's
location-authored thumbnail fallback does not reveal the generic map chosen
later by WGT-55/56. Static land weather/terrain/population descriptions likewise
do not identify the chosen child environment or map geometry.

### Ready-to-implement provider checklists

These extend the earlier checklists for the same three consumers.

| Consumer | Rule IDs and provider seam | Proposed covering test |
|---|---|---|
| Real space loading (legacy EAWR-2299) | WGP-31/32/51/52/75..80/112..114/161..167. Publish copied real conflict/planet/current-model/actual-owner/basic-income/static-text facts before work, separate map identities and selected base/defender, raw stage events, verified terminal outcome and session-owned Begin intent. | Two local contexts with identical planet facts but distinct progress IDs; actual/apparent owner and basic/net-income mismatch; 0.25/0.5, N=0/1/2 per-list fractions and repeated 0.65; delayed success retains Begin; map failure cannot become ready even after a synthetic terminal fraction. Lit independent clone plus custom nonunit framing inputs. |
| Land and mission loading (legacy EAWR-2300) | WGP-75/76/90/161/164..170; WGCH-08/10 and WST-44/50/51. Publish explicit ordinary/tutorial/queued-story/corruption admission, layer, dialog/selected/original maps, owner/location and script separately from loading keys. Retain campaign-owned ordinary land-auto-resolve policy and explicit unavailable mission/child outcome. | Four resources; mixed-case/path-qualified map and first `.TED`; missing MT/MB versus valid unrelated corruption briefing; land byte-position cap then lower force fraction; AI/multiplayer/fast-corruption direct controls; prohibited land mission never silently auto-resolves. |
| Conflict/handoff provider (legacy EAWR-1115) | WGP-161/162/166/167/168/170 with WGM-48..64, WGT-03/55/56 and WST-44/50/51. Retain conflict ID/planet/current type, actual owner separately from attacker/defender, layer base, admitted map/mission/script and terminal failure across the handoff; loading consumes these through a read-only facade. | Authored planet map versus generic/tutorial/explicit override; changed map owner without changed planet owner; ground/starbase levels differ; controlled failed reader followed by callbacks never starts a child; queued corruption result returns through WGCH-10 without ordinary conquest mutation. |

At this comparison base, `apps/viewer/src/galactic/galactic_handoff.cpp` already
binds a real conflict map, planet display context, owner and basic income and
enables the loading Begin barrier for exactly one active human. Its returned
preparation failure is explicit. Earlier WGP-75..90 and WGP-112..121 comparison
tables describe their older bases, including their then-disabled barrier.
The shared `LoadingScreenInput` still provides ordinary space/land selection
and a campaign flag, with no explicit mission kind, script or MT/MB fields.
Native stage publication, distinct dialog/selected/original map inputs and the
complete current-model descriptor remain integration work, not a game-code
change delivered by this walk. Work-weighted BLD-10 remains documented viewer
policy, not a claim of native milestone equivalence.

### Runtime qualifications

| ID | Qualified unknown | Capture recipe |
|---|---|---|
| WGP-CL-U01 | Visible progress and clone phase across the newly sourced space/land producers; WGP-BS-U01/U-11 still apply. | Record one cold and one warm ordinary and queued mission load per layer, retaining raw callback values/list IDs, rendered bar values, current model/animation and terminal result. Use N=0/1/2 and uneven larger lists, a delayed reader and lit consecutive frames; distinguish raw regressions from displayed smoothing. |
| WGP-CL-U02 | Retail failed-map assertion policy, modal presentation and safe post-acknowledgement parent/child recovery; WGP-BS-U02 remains open. | Isolated invalid selected map and corrupt-but-openable map fixtures, never an ordinary save: retain selected path/layer/base, loader return, popup versus quiet log, acknowledgement/assertion continuation, later callbacks, active mode and pause owners. Verify usable restored parent or bounded explicit failure; a 1.0 callback is insufficient. |
| WGP-CL-U03 | Actual ordinary land versus linked story/corruption mission layout and admitted continuation, including script/key mismatch and unavailable land-child behavior. | Stage an admitted solo space corruption and a land corruption/linked-story fixture, record slot/layer/attacker/actual planet owner/resolved map owner/script/MT/MB keys and four-resource choice before load; capture Begin, objectives and invader win/loss return. Add AI/multiplayer direct-corruption controls. Keep runtime land capture separate from M3's auto-resolve policy; do not force an unavailable land mission through the ordinary conquest route. |

The existing WGP-BL-U01 recipe retains physical input queued across reopening,
held keys and cinematic audio. WGP-113 already settles ordinary key/button
events **dispatched** while unsafe; this appendix does not reopen that sourced
branch or claim full retail input qualification.


### Starting technology in campaign setup

| ID | Rule | Source |
| --- | --- | --- |
| WGP-190 | The advanced campaign dialog labels the integer sliders `TEXT_SKIRMISH_STARTING_TECH_LEVEL` and `TEXT_SKIRMISH_MAX_TECH_LEVEL`. Their numeric captions add the selected faction's full `Displayed_Tech_Level_Adjustment` to the WGP-69 working coordinates. Thus the visible stock choices are **1..5** for every playable faction. Defaults reload the selected campaign's first faction start/max records and leave custom settings off; accepting an untouched dialog keeps authored per-player values. An edit enables custom settings; Accept retains edits, Cancel/Escape discards them, and changing the campaign/faction clears accepted custom settings. Launch adds the full selected-faction adjustment to the working coordinates, then WGS-13 subtracts exactly one for each player's own nonzero adjustment, including AI players. For stock chosen visible L this gives Rebel internal L−1 and Empire/Underworld internal L, all displayed as L. | debug build; effective XML/resource; WGP-29/67/69/70; WGS-13/39/76/77; WGEP-99 |

Stock Defaults are campaign values, not a universal level one. Gateways,
Equal Footing and Rim Worlds display **2**, Origin **1** (Rebel-human **2**),
Heart of the Maw **3** (Rebel-human **4**), and Clusters **3**. Maximum is
visible **5**. Only the stock Rebel faction has adjustment **+1**; Underworld
explicitly authors zero and Empire leaves the constructor zero. The six families
have no custom-tech limit overrides; mods may author `Min_Custom_Tech_Level`
and `Max_Custom_Tech_Level`.

Creation reuses WGS-77 and WGEP-42/130/147: current tech is committed before
maximum and the initial pass unlocks affiliated, initially locked,
slicer-unlockable types with required tech **strictly below** current. Planet
choices still use WGEP-19/22 and ordinary owner, base, factory, placement and
prerequisite gates. A higher level neither upgrades bases nor clears explicit
locks. Research targets and technology-price modifiers use the existing model.

The native screen builds the retail advanced resource and drives both technology
sliders plus Defaults/Accept/Cancel. Starting credits remain displayed and read
only; editing tech uses the selected faction's authored credits in the existing
all-player custom option assignment (WGS-12), as retail does. The auto-resolve
selector is disabled until its own accepted-option consumer exists. Random
events stay hidden per WGP-71; these neighboring controls are outside this task.

Current/max tech and unlock lists remain in the ordinary checkpoint state.
Campaign metadata section version **2** additionally retains optional accepted
custom startup credits/current/max/hero values. Main-menu restore reconstructs
that startup before validating and loading the saved campaign identity; it does
not recompute current research state from the initial option. This is native
save composition, not a claim of retail byte-format compatibility.

### Approach, population and story notice integration

WGUI-122's accepted departure assigns the final enroute destination with its
notification flag enabled; the bounded debug-build caller read confirms this
input. Clearing enroute passes false. The native movement owner now publishes
WGUI-114 feedback after that committed assignment, with the actual local
recipient and destination. It reads current political allegiance, explicit
enemy relations from the live creation inputs, current combat-skip facts and
local orbital presence. Missing authoritative inputs stay silent. Ordinary
arrival, rejected redirection and changed snapshot membership do not synthesize
another approach notification.

The existing runtime feedback sink stages this value until successful
publication. A per-player runtime wall clock applies the 5000 ms guard before
retention, stamps only admitted warnings and remains outside simulation,
checkpoint and replay state. A new live runtime starts with a zero clock
baseline; this is a project lifetime boundary, not a promise of retail popup
continuity across loading. The exclusive cursor retains the qualified notice
when presentation skips frames. Missing or refused sound never retries the
semantic notice. Local presentation requests the authored faction approach cue
separately from urgent advisor feedback. The minimal advisor adapter localizes
the planet text, prepends urgent entries, uses the sourced green text and
authored per-character duration, and advances its fade in outer updates during
strategic pause (WGUI-66..70/113/114; GFC-29). Full shared advisor graphics,
history, story-summary suppression and load continuity remain the shared popup
provider seam; this adapter does not claim their visual qualification.
Missing or invalid `Advisor_Hint_Duration` leaves the HUD usable and disables
only this text adapter as a named unsourced-default seam. It does not substitute
a stock duration or suppress the independently delivered faction approach cue.

`galactic_population_warning` implements only WGP-55/59's qualified local
strategic predicate. Its caller must supply current usage including queued
production (`used + reserved`, with checked arithmetic) and store that current
usage after every admitted observation, including absent/refused SFX. The frame
programme still owns baseline initialization, reset, load and admitted service
sequence; no HUD polling or fresh snapshot delta is connected to this API.

WGP-56 mission delivery awaits the story output publication owner (legacy EAWR-2042).
Its API must consume actual `Goals::dialog` `PresentationEvent` values
with the exact owner and `SoundRequest`, retain them after successful triggered
event delivery through the campaign/live observer boundary and expose an
exclusive cursor to audio. The caller supplies the real summary-open state,
dialog popup/SFX flags and local faction mission event. An open-summary refresh,
popup, generic objective insertion, icon announce or teletype reveal cannot
substitute for that request. No synthetic mission notice is published here.
