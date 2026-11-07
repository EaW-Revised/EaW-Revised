# Galactic planet detail screen

The advanced planetary map is distinct from the focused planet's ground row. The rules below describe source-read behaviour from effective XML and the debug build; the indicated capture questions remain open (legacy EAWR-5725).

| Rule | Source-read behaviour | Evidence |
|---|---|---|
| GPD-01 | The advanced-planet button opens the initially hidden map shell and its overlay; the back button closes it. The shell has its own radar and zoom control. | XML |
| GPD-02 | The 512 by 512 map window hosts the pad layout. Land maps have matching preview texture names, but whether these are the displayed images still needs a capture. | XML; capture needed |
| GPD-03 | The screen defines 80 draggable build pads and 10 draggable unit pads, with eight-frame alternate textures for a free-target pulse. Their unit and building art names are crossed in the data. | XML |
| GPD-04 | The 20-slot ground fleet tray supports dragging and stack counts. The shell also defines the header, capacities, help, weather, bonus and four filter tabs. | XML |
| GPD-05 | A map object is eligible for the header marker list when its type enables `Include_In_UI_Map_Header`. | XML; debug build |
| GPD-06 | A campaign keeps each planet type's base land map marker list and dimensions. Entries carry a type, map record identity and normalised position; corruption map variants are not used for this list. | debug build |
| GPD-07 | Marker types identify in-base, out-base and defending-force positions; specific-object markers, reinforcement points, bunkers and tactical build pads form further groups. | debug build |
| GPD-08 | Build pads take in-base markers first, ordered by increasing squared distance from their mean, then specific and out-base markers, up to 80. In and out share a base-pad class; unused pads hide. | debug build |
| GPD-09 | Defender markers fill at most ten unit pads in list order. Reinforcements and bunker/build-pad markers have separate display-only groups, for which stock XML supplies no components; capture is needed to confirm the appearance. | debug build; XML; capture needed |
| GPD-10 | Each normalised coordinate is transformed with `(coordinate - 0.5) * 2` inside the map window. | debug build |
| GPD-11 | Opening the screen builds the layout; changes in landed-company or ground-structure counts trigger a rebuild. | debug build |
| GPD-12 | Owners can edit. A non-owner with ground intelligence, or whose faction benefits from corruption on a corrupted planet, can view with pads and tray disabled; otherwise the screen closes. | debug build; capture needed |
| GPD-13 | Unit and building capacities show current count and maximum as `current/max`: landed forces against the global ground-force maximum, and land special structures against the planet-type maximum. | debug build; XML |
| GPD-14 | The tray groups the local player's orbiting ground units at all three positions by type and count. Entries are disabled at the landed cap or in view-only mode; unused slots hide. | debug build |
| GPD-15 | Separate filter bits control unit and build pads. The four tabs' mapping to those bits remains unresolved. | debug build; capture needed |
| GPD-16 | Every-specific-marker and companion structures, and structures matching specific markers, appear dimmed on their specific positions and cannot be dragged. A positive shield radius adds a scaled shield-range image. | debug build; capture needed |
| GPD-17 | Other structures reuse a remembered base marker, searching the preferred base-position list before the other. Missing markers are cleared; unassigned structures take the first free preferred pad, then a free pad in the other list, and remember the choice. | debug build |
| GPD-18 | Dropping a structure on a base pad moves it or swaps with its occupant, updating both markers. Either base list accepts a manual move; specific pads do not. | debug build; capture needed |
| GPD-19 | During a structure drag, free base pads pulse and unit pads disable; a unit drag pulses free unit pads and disables build pads. | debug build |
| GPD-20 | Unit pads contain only the planet owner's landed companies. Remembered positions are reused; remaining companies fill free pads in landed order and remember their assignments. Excess companies have no pad. | debug build |
| GPD-21 | A unit can move or swap between unit pads. | debug build; capture needed |
| GPD-22 | Dropping a tray unit on a unit pad remembers that marker and lands it if capacity permits; an occupant is launched to orbit. | debug build; capture needed |
| GPD-23 | Launching a unit from a pad creates a fleet at the first orbit position and clears its marker. The drop target that invokes this remains unconfirmed. | debug build; capture needed |
| GPD-24 | A dedicated network event updates each object's remembered marker, with no execution-time ownership or legality check. Manual changes and automatic default placement emit events, including in view-only mode. | debug build |
| GPD-25 | At land battle start, a defender uses the start marker matching its remembered identity, falling back to round-robin when none matches. Space battles ignore that marker. | debug build |

The project's planned implementation validates layout commands for the owning seat and commits swaps atomically. Capture is also needed for visual fidelity, filter mapping, launch gesture and persistence; the land-battle consumer is separate work.
