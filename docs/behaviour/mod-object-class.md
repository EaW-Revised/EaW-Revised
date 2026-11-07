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

The fixed table names the retail class vocabulary; it does not bound mod element names or values. Remake's `Template`
and `Dummy` element names are mod labels under this rule. A directly resolved template, or a variant chain containing
only mod labels, therefore receives a tag-derived class or remains `unclassed`; a chain naming a FoC class keeps that
ancestor's class, except that squadron units always make a Squadron (last rule above). Remake runtime loading has not been verified here.

Unverified for hero and starbase (squadrons are settled above): the retail hero/starbase predicates are stored flags set by the type constructor; the tag set above is
read off FoC data (the tags every object of that class carries), not decompiled. Full parity (tags only for every object,
element name ignored) is a follow-up.
