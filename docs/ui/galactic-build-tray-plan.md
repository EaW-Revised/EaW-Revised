# Galactic build tray: implementation plan

Order of work for [the galactic build tray spec](galactic-build-tray.md) (legacy EAWR-7830). One pull
request per slice, in this order; a slice may start before the one before it merges by stacking
on its branch, since each needs the earlier code.

Rules that hold for every slice:

- **Retail fidelity first.** The bar's own mapping (GHUD-S90 to GHUD-S92, `production_choice_slots`)
  and its tests are not edited. Every slice that touches the viewer adds a case showing that a
  planet whose choices all fit gets no handle, no tray and an unchanged bar, with the feature on.
- **Data first.** Roles come from `Behavior`, category names from `CategoryMask` and the mod's
  own category enum, members from the company and squadron lists. No mod or retail names in
  code; every list is the layer XML's.
- **Presentation only.** No slice touches the simulation, replay hashes, saves or the choice
  list itself.
- Clean room, no retail names in committed text, behaviour rule IDs cited (GBT-nn).

| Slice | Covers | What it delivers | Checks beyond the light ones |
|---|---|---|---|
| 1 | GBT-04, GBT-05, GBT-06 (rules), GBT-12 (rule order) | An engine-free grouping and row-packing model in the UI library. Input: the choice list as facts (type name, roles, category names, member categories, lane) plus a group definition (priority list, building categories, captions, extra groups, assignments) and the mod's category enum order; output: ordered groups and rows for a panel width, a row budget and a size step. Unit tests with synthetic data that reproduces each real-data pitfall: a hero ship with a targeting name, a composite category, a company whose member is a structure dummy, a placeholder with only a marker name, a name the enum does not define, a squadron with no categories of its own, `All` and `None`, one-choice groups folding into misc, the 6-row budget, the height-driven size step, and "no earlier row is backfilled". | Plain build; the new unit test. No viewer build. |
| 2 | GBT-11, GBT-12 (files and diagnostics) | Parsers, validators and the layer rule for `RevisedBuildTray.xml` and `RevisedBuildTrayGroups.xml` (the highest valid file wins whole, an invalid file keeps the last valid one and is reported once), the checks that need the mod's category enum, the stale-assignment count, and tests with synthetic layers. Nothing ships yet. | Plain build; the loader test. |
| 3 | GBT-13 | The Options > Tweaks switch **Galactic build tray**, stored as `gameplay.galactic_build_tray`, with the untouched-versus-explicit record of the selection hotkeys, `Enabled_By_Default` from the layout file, Apply, Cancel and Defaults, the deviation entry, and a gate function the later slices consult. Lands before anything draws, so the default-on tray is switchable from its first release. | Plain build; settings tests; the viewer build only if the Tweaks page is in the viewer. |
| 4 | GBT-11, GBT-12 (shipping) | The default `RevisedBuildTray.xml` and `RevisedBuildTrayGroups.xml` under `tools/revised_build_tray/`, installed into the Revised layer like the planet search file, listed in the Revised layer manifest and in the presentation-only XML list that keeps them out of the content digests, with the tag inventory and registry rows for the new tags and the tuned default values. No pixel changes. | Plain build; installer and manifest tests; the registry check; a skirmish digest case showing the files do not change the identity. |
| 5a | GBT-04, GBT-05 (data) | The facts the tray groups by: category names, members and Behavior values in the production art, a type index built from it, and the loader for the mod's category enum. No drawing. | Viewer build; the scene and files unit tests. |
| 5b | GBT-01, GBT-02, GBT-03, GBT-10, GBT-06 (layout in the galaxy view) | The tray in the galaxy view: the `+N` handle, panel, headers, dividers and entries copied from the bar's production button (art, price, disabled and under-production state, tooltip, click action), laid out from the layer XML with the model of slice 1. Choice facts come from slice 5a. Tray entries are their own buttons beside the bar's, hover and click like a bar slot (the hover and encyclopedia lookup learns the tray's entries; the bar's component maps are not extended). Handle placement by ordered candidates, restyle and disable through the file. Opened by the handle only, to prove drawing and clicking. | Viewer build; GPU cases for a fitting planet (nothing drawn, bar pixels unchanged), an overflowing planet, a mod-restyled bar and an entry click matching a bar click; lit eye-check captures beside the stock bar. |
| 6 | GBT-07 | Open and close rules: stays open across planets, purchases and view changes; only on a planet the local player owns and that overflows; session state not saved; the Tab binding in Options > Controls (default Tab, rebindable, suspended while a text field has focus, while no handle is shown and while the HUD is hidden); Esc and pointer movement never close it. | Viewer build; GPU case for the open-state sequence; input routing unit tests. |
| 7 | GBT-08 | The planet view overlay: capped at the named cap components, the smaller size step then fewer unit rows when short, never covering the cap components, closing on a unit drag in the planet view's UI. | Viewer build; GPU cases in the planet view; eye-check captures. |
| 8 | GBT-12 (offline tooling) | The export tool that lists every buildable choice of a mod chain with its facts and the group the generic rules give it, and the checker that validates a groups file against that export and prints what moved. No model in the game; the tool only reads and reports. | Python tests; cleanroom and doc_refs. |
| 9 | GBT-12 (compat patches) | One `RevisedBuildTrayGroups.xml` each for Thrawn's Revenge and Remake 4.0, made with slice 8 and reviewed by the owner, published as separate compat data, not in the main repository's game files. | The checker's report; a GPU eye-check of a large mod's tray. |

Carried into slice 5 from the review of slices 1 to 3: (1) the tray recomputes its layout only when the choice list, the layout or the panel size changes, never per frame (`fit_to_height` packs up to once per row and allocates); (2) `Packed::omitted`, the entries the capacity limit cuts, reaches an EAWR-UI diagnostic, not only the report; (3) the first real call site of `check_against_categories` and `stale_assignments` emits the one EAWR-UI diagnostic GBT-11 and GBT-12 require for a rejected or stale file (the Options build already reports a rejected layout file).

Slices 1 to 4 change no pixel. Slice 5 is the first visible change and is behind the switch of
slice 3. Slices 8 and 9 can run beside 6 and 7 once slices 1 and 2 have merged.
