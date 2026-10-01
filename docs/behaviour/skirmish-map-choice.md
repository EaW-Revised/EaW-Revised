# Space skirmish selection

Project lobby and camera policy requested by the owner in EAWR-908. These rules
describe project choices; existing starts still apply SK-01 to SK-24 and
space-movement PL-01/PL-03. No new claim about original-game behaviour is made.

- **SC-01:** The selectable lobby has exactly two players. Maps with more spawn
  slots may host those two players; unused markers remain unused. The default
  map, factions, teams, controllers, fleets and seed are the unchanged M2 fixture.
  Explicit options replace the requested values and bind the selected map's
  actual SHA-256. The pinned Coruscant map still requires its original hash.
  Lobby factions must author `Is_Playable`; the existing station and spawn
  selection validates their team markers and station candidates.
  Selectable sessions extend the pinned unit-table inputs only for missing
  selected fleet, faction free-force and station-candidate types, using the
  existing validated loader and its craft/hangar/projectile dependency loading.
- **SC-02:** Without a camera XML, the live camera uses the map's declared
  extents centred on the origin as project target bounds. Its target is the
  mean XY position of the local player's starting free units and fleet placed
  from that player's selected spawn by PL-01/PL-03 (the spawn itself for an empty
  fleet), clamped to those bounds, at battle-plane height zero. It starts at
  distance 1200 and XML `Yaw_Default`, with the same owner overrides as the
  Coruscant live camera. Explicit camera XML and fixed capture poses retain
  precedence. This is project framing, not an original-game opening-camera rule.

The map census resolves TED placement CRCs through the effective FoC XML.
`Is_Asteroid_Field`, `Is_Nebula` and `Is_Ion_Storm` identify authored hazards;
`Space_Obstacle_Radius` and `Space_Obstacle_Offset` describe their volumes.
Selection does not wire these simulation behaviours: asteroid fields are
drawn by the existing map population, without field collision or damage;
nebulas and ion storms are classified but not drawn or simulated. Mine
effects remain outside this change; no placed mine type occurs in the checked
space skirmish maps. `Mineral_Extractor_Pad` is an economic object, not a mine.
The map-by-map list is in the viewer README.
