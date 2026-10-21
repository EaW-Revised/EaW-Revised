# Galactic conquest data

The galactic loaders (`include/eawr/data/galactic/galactic_data.hpp`, `src/data/galactic/`) read
the data a galactic conquest campaign starts from into immutable tables: campaigns, planets, trade
routes, factions and the galactic GameConstants. As the M3 architecture decides for galactic data,
loading is tolerant, every problem is an issue row and nothing vanilla is hard-coded. No
simulation reads the tables yet; the consumers are the galaxy model, session setup, economy,
victory, AI and viewer tickets named in the [tag registry](tag-coverage/statuses.json) rows
(`todo` with that ticket).

## Sources

| Table | Read from | Order |
|---|---|---|
| Campaigns | the catalog's `campaign` category (`data/xml/campaignfiles.xml` and its files) | registry order, then definition order: the authored order of the 33 stock definitions (WGS-01) |
| Planets | `Planet` catalog records with `PLANET` in common `Behavior` or `GalacticBehavior` (WGS-01a) | catalog definition order |
| Galaxy art objects | the remaining `Planet`-typed catalog records, retaining their position/model and authored background metadata | catalog definition order |
| Factions | the catalog's `faction` category (`factionfiles.xml`) | catalog definition order |
| Trade routes | `data/xml/traderoutefiles.xml` and the files it lists, read like a registry include | file order, then document order |
| Constants | `data/xml/gameconstants.xml`, the tags of `galactic_constant_tags()` | that list |

Variants (`Variant_Of_Existing_Type`) resolve through the catalog, so a planet variant inherits
its base's values.

## Value rules

Values follow FoC's database parse, `eawr::data::retail`
([unit data value rules](unit-data.md#value-rules), AU-80 to AU-83): an empty value or `TBD`
changes nothing, booleans read the first letter, numbers read their leading number. Each
occurrence of a record tag (`Starting_Forces`, `Starting_Credits`, `Home_Location`,
`Special_Case_Production`, ...) is one record, kept in authored order with its duplicates: the
credit and tech readers take the first record of a faction (WGS-11), and how FoC stores a
repeated one is unverified (WGS-U01), so both stay and the repeat is an issue row. A list-shaped
single value (`Locations`, `Trade_Routes`, the victory lists) is the last occurrence's names.

A campaign location without planetary behavior stays in `locations` and is left out of
`planets` (WGS-01a). `GalacticData::galaxy_art_object` resolves retained Planet-typed art;
`planet` resolves conquerable planets only. Common and galactic behavior lists both apply;
an authored background flag cannot remove a record that has planetary behavior. This is
independent of the still-unverified presentation layer routing (WGP U-01).

## Issues

Each issue names its kind, file, line, record, tag, value and reason. The load still succeeds;
only an unreadable trade-route registry fails it.

| Kind | When |
|---|---|
| `unknown_tag` | a tag the schema inventory does not know for the class (catalog records), or not one of the six trade-route tags |
| `bad_value` | a value without a leading number, a fraction where a count belongs, a record with the wrong number of fields |
| `trailing_text` | a number followed by text, read as its leading number |
| `dangling_reference` | a location, route, end point, faction, object type, planet or include that resolves to nothing of its kind |
| `duplicate_record` | a repeated faction record, location or trade-route name (the later route definition wins, unverified) |
| `missing_required` | a campaign without locations, a planet without a position, a route without end points, an absent galactic constant |

Stock schema-unknown tags include legacy campaign victory tags, planet `Facing_Adjust`,
`Max_Ground_Base`, `Special_Structures`, `Trade_Routes` and faction `Space_Retreat_Flight_Increment`.
They remain named issue rows; schema absence alone does not establish runtime behavior.

## Identity

`content_identity` is SHA-256 over the length-framed encoding `eawr-galactic-data-v4` of every
table field in table order, the unit tables' encoding style. Issues and input receipts are not
part of it. It joins the galactic session identity when the session is built.
Planet fields include the effective authored system-spy definitions: ability name, activation
style, enabled/despawn flags, duration, resolved influence range and requested intelligence mask.

## Galactic constants

| Constant | Walk rule | Consumer |
|---|---|---|
| `Object_Max_Speed_Multiplier_Galactic` | WGM-97 | galaxy locomotion (legacy EAWR-680) |
| `Corruption_Hyperspace_Bonus` | WGM-27 | galaxy locomotion (legacy EAWR-680) |
| `Fiscal_Cycle_Time_In_Secs` | WGS-05, WGEP-02 | galactic clock (legacy EAWR-1979) |
| `Good_Side_Name`, `Evil_Side_Name`, `Corrupt_Side_Name` | WGS-07 | session setup (legacy EAWR-1149) |
| `Credit_Cap_Per_Planet` | WGS-09 | fiscal ledger (legacy EAWR-682) |
| `MaximumPoliticalControl`, `Political_Control_Change_Time_Seconds` | WGM-65..77 | political control (legacy EAWR-1114) |
| `Max_Ground_Forces_On_Planet` | WGM-39 | surface landing capacity (legacy EAWR-1114, EAWR-1115) |
| `Raid_Force_Required_Faction`, `Raid_Force_Free_Object_Category_Mask`, `Raid_Force_Limited_Object_Category_Mask`, `Raid_Force_Max_Heros`, `Raid_Force_Max_Limited_Objects` | WGM-37 | canonical raid policy and shared fleet capability predicate |
| `TradeRouteMovementFactor`, `MaximumFleetMovementDistance` | WGM-17..38 | galaxy model (legacy EAWR-680) |
| `Strategic_Queue_Tactical_Battles`, `Battle_Pending_Timeout_Seconds` | WGM-44..47 | conflict hand-off (legacy EAWR-1115) |
| `AutoResolveDisplayTime`, `AutoResolveWinnerAttrition`, `AutoResolveLoserAttrition`, `RetreatAutoResolveWinnerAttrition`, `RetreatAutoResolveLoserAttrition`, `AutoResolveAttritionAllowanceFactor`, `AutoResolveTransportLosses` | WAR-28/29/38/39/44 | `auto_resolve_round_rules` and the pure galactic round; omitted values use only the walk's code defaults |
| `Auto_Resolve_Tactical_Multiplier` | WAR-40 | pure `auto_resolve_survival_budget` kernel; integrated mid-tactical allocation stays a seam (legacy EAWR-2026), galactic budgets remain unchanged |

## Not read here

- **Difficulty adjustments.** The galactic multipliers (`Galactic_Build_Time_Multiplier`, the
  galactic AI contrast and goal-cycle values) belong to the planet queues and the galactic AI;
  those tickets read them with the selected difficulty, as the tactical tables do.
- **Trade route lines** (`traderoutelines.xml`): colours and widths per relationship, read by the
  viewer's galactic mode.
- **Planet presentation and encyclopedia tags** (`Describe_*`, icons, `Name_Adjust`, idle
  animation), the corruption mission tags (`Corruption_N_*`) and the black-market tags: their
  registry rows stay `land-or-galactic` until the viewer and corruption tickets read them.
- **The galactic fields of unit types** (`Hyperspace_Speed`, population, bribe and galactic build
  values): the galactic type table (legacy EAWR-2001).

## Listing

`xml_scan --profile foc --game-root <install> --report <catalog.json> --galactic-report <galactic.json>`
writes the tables: each campaign's factions, locations, planets, routes and setup records, the
planets, routes, factions and constants, every issue and the input receipts.

Add `--galactic-trace-out <trace.json>` to record only the GC load. Join it with the registry using
`tools/inventory/tag_coverage.py scene --mode galactic --game-root <install> --trace <trace.json>
--statuses docs/tag-coverage/statuses.json --out <coverage-dir>`. Galactic scope checks application
in GC consumers, while the default tactical scope retains the M2 checks.

## Proposed M3 fixture

`Sandbox_Gateways_Rebel`: a stock single-player conquest of middling size (28 locations, 23
routes), all three factions, ordinary victory conditions and custom settings. The smaller Heart of
the Maw family (13 locations) is the alternative for fast runs. The owner confirms the fixture
when the M3 pins are made, as M2 pinned its skirmish map.
