# FoC tactical space minimap (EAWR-455)

## Applicability

The minimap in the Forces of Corruption space tactical command bar on the pinned FoC build: what it
draws, how it maps the battle, how often it updates and what the pointer does on it. The rules were
read in the FoC debug build (the radar map, its renderer, the game mode's per-frame radar update and
radar icon submission, the command bar's mouse handling and component action table) unless marked
as project policy. Evidence IDs MME-1 to MME-11 map to research notes kept outside the repository.
Values are from the FoC `RadarMap.xml`, `GameConstants.xml` and object XML.

The remake implements it in `presentation/ui/minimap.hpp` (settings, mapping, blips, guide, fog), the
Godot `EawrMinimap` control (drawing, pointer) and the viewer's live battle glue. It is presentation
only: it reads the newest snapshot, the selection and the camera; a left press or drag moves the
camera, a right click becomes the same move order as a right click on empty space.

## Interface

Input: the units the local player sees (type, owner, position, facing), the selection, the local
player's fog cells (or, in a battle without fog rules, the local team's sensor revealers), the
camera's frame and target bounds, and the shell's `radar` mesh.
Output: the minimap image inside the radar mesh; camera moves and move orders from the pointer.

## Rules

- MM-01 (MME-1). The space minimap reads `RadarMapSettings` in `RadarMap.xml`: the backdrop
  `Space_Backdrop_Texture_Name` (`i_radar_map_grid.tga`), the fog colour `Space_FOW_Color`
  (25, 66, 120, 100) and `Space_Is_Guide_Rectangle` (No: the camera outline is a trapezoid).
- MM-02 (MME-9). The minimap spans a world square: the camera's target bounds, widened on their
  shorter axis to the larger half extent around the same centre. The bounds themselves are the
  playable rectangle. The remake's bounds are the project's space camera config (project policy
  until the retail bounds are read from map data).
- MM-03 (MME-11). The minimap frame runs from -1 to 1 across the square, +x right and +y up (world
  +Y up on screen). The engine can flip the map but resets it at the start and never flips it.
- MM-04 (MME-1, MME-8). The minimap is rebuilt every rendered frame: every blip and the camera
  outline. The fog layer is rewritten nine texel rows a frame into a second buffer, which replaces
  the shown one when all rows are done; a new fog source rewrites every row at once.
- MM-05 (MME-3). A selected unit takes `Radar_Selected_Units_Color` (209, 255, 209, 255) while
  `Radar_Colorize_Selected_Units` is Yes. `Radar_Colorize_Multiplayer_Enemy` (No) does not apply.
- MM-06 (MME-4, MME-5). Per type: `Is_Visible_On_Radar` (default No), `Is_Visible_On_Enemy_Radar`
  (default Yes), `Radar_Icon_Name` (default `i_radar_default_blip.tga`), `Radar_Icon_Size` (default
  0.05 0.05, the quad's half extents in minimap units), `Radar_Show_Facing` (default Yes) and
  `Radar_Rotate_Icon` (default No). Icons come from the command bar's mega texture.
- MM-07 (MME-3). In skirmish a blip's colour is its owner's player colour (the lobby colour). An
  owner without a lobby slot (a map's Neutral or Pirates objects) takes its faction's `Color` from
  Factions.xml: grey for Neutral, orange for Pirates, as the retail Coruscant still shows.
- MM-08 (MME-4, MME-6). A blip is its icon on a quad centred on the unit, half extents from
  `Radar_Icon_Size`, turned by the unit's facing less a quarter turn when the type shows facing; a
  facing of exactly zero is not turned. The icon list is submitted from its end, so the first unit
  is drawn last.
- MM-09 (MME-7). The camera outline: the rays through the viewport's four corners meet the plane at
  the reference height (the mean height of the playable factions' radar-visible objects at the
  start); a ray that misses it gives the view's far end instead. The four points are joined by white
  lines, cut where they leave the minimap. A rectangle guide (MM-01) joins their bounding box
  instead.
- MM-10 (MME-8). The fog layer has one texel per minimap pixel (the radar's screen size, rounded
  up). A texel in the playable rectangle whose world point is fogged for the local player takes the
  fog colour; the rest is clear.
- MM-11 (MME-10). Contemporary controls: a left press on the minimap points the camera at the world
  point under the pointer; while the left button stays down over the minimap, a move of more than 12
  pixels from the press starts a drag, and from then every move re-points the camera. A right release
  on the minimap orders the selection to move there (with the attack-move and guard modifiers of a
  right click). A left double click also points the camera and ends a tethered camera.
- MM-12 (MME-2). A unit shows only when its type is visible on the radar; an enemy also needs its
  type visible on enemy radars and must be seen by the local player (not jammed, stealthed, in a
  nebula or fogged). Asteroid fields, ion storms, nebulae and dead units never show as blips.
- MM-13 (MME-6). Draw order: the background layer (MM-14), the fog layer, the backdrop grid, the
  point layer, the unit icons, the camera outline, all alpha-blended. The backdrop quad's texture coordinates run 0 to 25 with
  wrapping, so the 32-texel grid tile repeats 25 times on each axis (the retail still shows a line
  about every 6.5 pixels on a 164-pixel minimap).
- MM-14 (MME-6). The background layer: in space the renderer fills it with the `<Color name="space">`
  entry under `RadarMapSettings` (12, 30, 51, 255), opaque, over the whole square. It is the navy seen
  in revealed areas of the retail stills; fogged areas add the fog colour on top (16, 47, 87 in the
  fog-on still).

## Cases

- K-1. The M2 Coruscant start, Rebel local player: the Rebel station and fleet blips in the Rebel
  lobby colour top left, the camera trapezoid around the start view, the rest of the square fogged
  blue; enemy units show only when a Rebel sensor sees them.
- K-2. A left press at the minimap's centre moves the camera to the square's centre; a slow drag
  shorter than 12 pixels moves it once, a longer one follows the pointer.
- K-3. `--eawr-live-reveal on` (project policy, matching [space-fog-presentation.md](space-fog-presentation.md)
  FW-14): the minimap's fog layer draws nothing, agreeing with the world's fog plane, while the fog
  cells and revealers it reads are unchanged (the live session's sensor content stays untouched, EAWR-507).

## Unverified / fidelity list

- The retail fog-on still shows some Neutral objects (grey icons, the planet star) through the fog;
  ours shows only what the snapshot's visibility shows the local player.
- Fog reads the local player's fog cells that the live session publishes with each tick (EAWR-494, the
  grid of [space-visibility.md](space-visibility.md) V-11 to V-19): a texel is fogged where its cell
  is zero, so it lingers and is quantised as FoC's is, and it matches the fog drawn in the world
  ([space-fog-presentation.md](space-fog-presentation.md) FW-08). A battle without fog rules falls
  back to the local team's current sensor circles.
- The opaque background (MM-14) covers the shell's radar scan lines, as in the retail stills.
- `Radar_Rotate_Icon`'s quarter turn direction is unverified (no M2 type uses it).
- The point layer (`Radar_Blip_Size` for types without an icon), damage flashes, reinforcement and
  shield range icons, background objects (the planet icon), asteroid field fill, radar events
  (click, death and beacon models) and the double click's tether release are not drawn.
- The world square follows the project's camera bounds, not FoC's own space camera bounds.
