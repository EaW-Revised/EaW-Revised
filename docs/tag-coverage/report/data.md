# data tag coverage

[All areas and legend](README.md)

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 74 | 74 |
| partial | 0 | 0 |
| todo | 0 | 0 |
| presentation-later | 0 | 0 |
| foc-ignores | 0 | 0 |
| deferred | 0 | 0 |
| land-or-galactic | 0 | 0 |
| multiplayer | 0 | 0 |
| **Total** | **74** | **74** |

Tables group the exact object class families listed together in the registry. Object kinds
are station, ship, squadron or craft when specified; an empty kind list means the whole
listed class. Each consumer retains its own kinds and rule IDs. Rule IDs are plain text:
the registry does not supply public link targets. Tickets refer to the private tracker
and are plain numbers. Code locations are repository paths without identifier anchors.

## Campaign, CIN_GroundInfantry, Cin_GroundProp, Cin_GroundStructure, Cin_GroundVehicle, Cin_Projectile, CIN_SpaceProp, CIN_SpaceUnit, Cin_TransportUnit, CommandBarComponent, Container, Decal, Difficulty_Adjustment, Faction, GenericHeroUnit, GroundBase, GroundBuildable, GroundCompany, GroundInfantry, GroundStructure, GroundVehicle, HardPoint, Hero_Clash, HeroCompany, HeroUnit, Indigenous_Unit, LandBombingUnit, LandPrimarySkydome, LandSecondarySkydome, LightSource, Marker, MiscObject, Mobile_Defense_Unit, MOV_Cinematic, Movie, MultiplayerStructureMarker, MusicEvent, Particle, Planet, Priority_Set, Projectile, Prop_Desert, Prop_Felucia, Prop_Forest, Props_Generic, Props_Snow, Props_Story, Props_Swamp, Props_Temperate, Props_Urban, Props_Volcanic, ScriptMarker, SecondaryStructure, SFXEvent, Slave_Unit, SlaveCompany, SpaceBuildable, SpacePrimarySkydome, SpaceProp, SpaceSecondarySkydome, SpaceStructure, SpaceUnit, SpecialEffect, SpecialStructure, SpeechEvent, Squadron, StarBase, TacticalCamera, TechBuilding, TradeRoute, TradeRouteLine, TransportUnit, UniqueUnit, UpgradeObject

| Tag | Status | Object kinds / missing kinds | Rule IDs | Ticket | Code paths | Note / evidence |
|---|---|---|---|---|---|---|
| @Name | applied | whole class (src/data/xml_registry.cpp) | R-08, R-11 | — | src/data/xml_registry.cpp | basis: reviewed |
