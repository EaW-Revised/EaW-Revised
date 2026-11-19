# Object class of a mod's own element names

**SG-CLS-01.** Evidence: debug build, the game-object type manager's create-type routine. Every element of a game-object file
creates the same single type object and reads only its `Name` attribute; no class table is looked up from the element name.
So an element name is a label. A mod may name its objects anything (`Space_Unit`, `Props_Stargate`, `Template`) and rely on
`Variant_Of_Existing_Type` plus tags.

Remake rule (data-driven, never silent):
- An element name that FoC's own game-object files use is the class (unchanged for stock data).
- Otherwise the class is the nearest FoC class named along the variant chain (`class_origin` = `ancestor:<Class>`).
- Otherwise it is implied by the effective tags (`tags:<Class>`): `Company_Units` (HeroCompany with a hero flag, else
  GroundCompany), `Squadron_Units` (Squadron), a `DUMMY_STAR_BASE` behavior (StarBase), an `Upgrade` category (UpgradeObject),
  `Is_Named_Hero`/`Is_Generic_Hero` (HeroUnit).
- Last, for squadrons only: whatever the class so far, an object whose effective `Squadron_Units` is non-empty is a
  Squadron (`class_origin` = `tags:Squadron`), unless it also lists `Company_Units` (one list is used per mode and a company
  keeps its land path). Evidence: debug build, the object creator in space mode takes the squadron-unit count of the type
  and places each craft; it reads neither the element name nor an ancestor. So a mod's `SpaceUnit` variant of a squadron
  template has no hull of its own, its craft do. Stock data has no such object. Hero and starbase classes stay unverified.
- Otherwise the label stays (`unclassed`). The unit loader records a note row for every object that took any of these paths.

Presentation draw ownership uses the original map record. Once the environment view claims a planet or nebula model,
neither static population nor the live session draws that record again, including when live composition assigns a new
synthetic record. This applies to inherited classes and unclassed labels alike; units spawned without a map record
keep their ordinary live draw policy. Simulation admission is separate and unchanged by this presentation guard.

The fixed table names the retail class vocabulary; it does not bound mod element names or values. Remake's `Template`
and `Dummy` element names are mod labels under this rule. A directly resolved template, or a variant chain containing
only mod labels, therefore receives a tag-derived class or remains `unclassed`; a chain naming a FoC class keeps that
ancestor's class, except that squadron units always make a Squadron (last rule above). Remake runtime loading has not been verified here.

**SG-CLS-02.** `Tactical_Health` and `Armor_Type` belong to the effective object, including when a
mod labels the definition `Template`. For an admitted space body, inherited values and a child's
overrides feed the ordinary hull maximum (HD-01) and projectile armor multiplier (DG-10). The
label neither suppresses those consumers nor creates a body by itself. A definition resolved as
a `Squadron` still describes its craft and team metadata; it does not acquire a pooled hull from
an inherited template health value (SG-CLS-01). Evidence: debug build's common type and damage
paths; synthetic custom-label body, inheritance, override and squadron contracts.
An admitted `TransportUnit` body uses the same health and damage path. Reading a carried or
unselected transport definition as metadata does not deploy such a body; its absent deployment
consumer remains separate from the health reader.

Unverified for hero and starbase (squadrons are settled above): the retail hero/starbase predicates are stored flags set by the type constructor; the tag set above is
read off FoC data (the tags every object of that class carries), not decompiled. Full parity (tags only for every object,
element name ignored) is a follow-up.


**SG-CLS-02.** Registry scope for inherited mod labels: the shared catalog resolver
follows `Variant_Of_Existing_Type` for every selected registered game-object
definition, independently of its XML element label. The resolved chain supplies
effective values to the space loaders; a new label does not require another
inheritance reader. Evidence: SG-CLS-01 and ML-23; current source audit of
`Catalog::resolve_in`, `trace_chain` and the unit-table resolver, supported by the
existing inheritance and mod-class synthetic contracts.

This is partial application coverage, not complete original loading parity.
ML-20/22's completed-base requirement, duplicate registration sequence and
maximum ten parsing passes are distinct from on-demand catalog resolution.
Consumer-specific copied-state/list behavior (ML-24/26/27), physical/logical
loader admission and the loaded space closure remain separate qualifications.
In particular, an inherited definition is not proof that a map-only object,
particle, hero or abstract template is deployed into a battle.

**SG-CLS-03.** Existing mask/target consumers are shared by the selected loaded
space type, rather than an XML element-label allowlist. The shared decoder reads
`CategoryMask`, `Property_Flags` and `Is_Valid_Target`; physical target profiles
and priority rows consume the resulting values (WCC-25; R-09), and tactical
object queries consume the copied masks (FH-20; EX-56). This source audit
qualifies mod-label registry rows for those existing consumers only.

It does not admit every authored object to the space closure. Logical squadron
purchase masks and their independently authored runtime container masks remain
distinct (PL-13); the present adapter's craft union is not evidence that container
masks are applied. Generic projectile categories and props outside live admission
also keep their separate interfaces. Land and galactic consumers are unchanged.

**SG-VFX-01.** For a model-bearing object already admitted to the space scene, the element label does not
select a separate model or scale reader (SG-CLS-01). The debug build selects the space model resource in space
mode and applies the effective `Scale_Factor` through the generic object transform (R-SKY-05 in
[p1-effective-environment](p1-effective-environment.md)). The remake's `scene::build` resolves the effective model
and scale; its live population transforms use the resulting placement scale. This covers admitted station, transport
and custom `FlagshipUnit` bodies. It does not deploy a carried or unselected company transport that remains
metadata (WHE-07/49), nor implement the separate dummy-spawner gameplay lifecycle. The original's
ordinary space asset load and model refresh have no element-label or dummy-spawner exclusion for that model.

A death clone is also a separate placed object: `LiveSessionView::prepare_death_clone` resolves its own effective
type, model, `Specific_Death_Anim_Type` and `Specific_Death_Anim_Index`, then scene composition reads the clone's
scale. The death-clip rules and the documented draw/fallback policies remain UA-08, UA-P2 and UA-P6 in
[unit-animation](unit-animation.md). These existing consumers apply to a `Death_Clone` element as well as to a
clone whose label was inferred through SG-CLS-01. This coverage correction changes no presentation behavior.

Prepared hardpoint breakoff props likewise resolve their effective type without an element-label gate.
`DebrisProps::prepare` reads `Debris_Attached_Particle` and composes a separate placed prop; the ordinary scene
resolver reads that prop's model and scale. BP-33 in [battle-presentation](battle-presentation.md) supplies the
attached-fire rule. This covers `SpaceProp_Breakoff` labels as well as `SpaceProp`; authoritative debris
health/damage and other creation interfaces remain separate consumers.

**SG-CLS-04.** The following authored legacy names have no original typed game-definition
handler: `Moniker`, `Transport_Capacity`, `Surface_Bombardment_Capable`,
`Prev_Level_Default`, `Next_Level_Default`, `Is_Interdictor`, `IsBuildable`,
`Combat_Power_Value`, `Is_Sprite`,
`Place_At_Every_Specific_Marker_Position` and `Black_Sun_Laser_Cannon_Fire`.
Fresh debug-build registration, string and symbol checks find no game XML
consumer. The unrelated platform error strings containing “moniker” are not
object-tag readers. These names do not gain behavior because a mod authors them
on a familiar or custom element label.

This is not an alias declaration: the registered construction/upgrade, marker,
interdiction-behavior and combat-power controls retain their own names and
consumers. Do not wire a guessed replacement from one of these spellings.
Evidence: debug build DCL-L01/DCL-L02 and the typed game-definition catalog.


**SG-CLS-05.** Two existing selected-space presentation consumers also use resolved
type data without a mod element-label gate. `Score_Cost_Credits` is copied into
the mounted scoring provider and results-type facts (WBF-45/46); `Movie_Object`
filters otherwise eligible live battle-camera candidates (BCAM-06). Loading an
abstract template or a type outside those live/scoring closures does not deploy
it or make it a camera subject. Independent runtime-container metadata and land
or galactic consumers retain their own qualification. Evidence: the cited debug
build rules and current shared decoder/provider/candidate source audit.

**SG-CLS-06.** Existing selected-space fog and capture providers consume shared
resolved metadata independently of mod element labels. The initial/last-state
fog flags reach the live remembered-model provider under FW-25..29, including
its drawable composed model, battle-start identity and local-observer guards.
The capture reader and profile builder likewise apply authored radius and
transition values to loaded `CAPTURE_POINT` types under WNO-19/WBP-04. The current
space path requires both numeric values; the original constructor defaults for
absent radius or transition values are not established by that path. Neither
provider expands the loaded/deployed type closure or grants capture behavior
from a class name. Independent runtime-container, land and campaign metadata
remain separate. Evidence: the cited debug-build rules and current shared
reader/profile/live-provider source audit.

SG-VFX-01 also covers `DummyStructure`, `Template` and `SpaceProp_Obstacle` labels for a body already
admitted to the space scene. Effective model and scale values may be authored on that object or inherited
from a `Template` ancestor through SG-CLS-02; neither source selects another presentation reader.
This records model/transform application only. It does not make an abstract template deployable, implement
a dummy-spawner lifecycle, or claim terrain, collision or damage behavior for an obstacle.

**SG-AI-01.** GS-10 goal-target admission uses the effective `Has_Space_Evaluator` flag for an
ordinary body already present in the live space view. The shared body loader reads the flag, the
skirmish AI setup copies it into type metadata, and the initial/new target providers consume it
without another XML element-label gate (SG-CLS-01/02). This qualifies directly admitted transport
bodies and the `Props_Story` map bodies in the existing WHZ-50/51 live closure. Planetary weapon
source records remain excluded by these providers. A carried identity, unselected transport or
other map-only definition does not become a live target merely because the flag was parsed;
WHE-07/49 and the existing map admission rules keep their separate qualifications. This source
audit changes no loading, frame service, target ordering or allocation behavior.
