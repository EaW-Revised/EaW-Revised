# Starting a mod campaign headless

`sim_headless --galactic <campaign name> --game-root <install> --mod-root "<leaf>;<parent>;..." --census-out report.json`
starts an installed campaign (stock or mod) without a viewer, one stage at a time:

| Stage | What it does |
|---|---|
| mount, catalog, galactic-data | Mounts the layers (leaf first, then FoC expansion, then base), loads the XML catalog and the galactic tables. Every loader issue is a row of the report, grouped by kind and tag. |
| campaign, types | Finds the campaign and builds the galactic type table, including the types the campaign names that carry no tracked galactic tag. |
| plan, services | Plans the campaign (galaxy, locations, forces, credits, tech, victory) and prepares the stock providers. Each diagnostic that the setup went on without is listed with its code. |
| placement, session | Places the starting forces and creates the session; prints the tick-zero hash. |
| ticks | Steps `--ticks` ticks. |
| play-move, play-build, play-days | With `--play n`: orders one fleet move and one purchase for the human player and lets three fiscal cycles pass; each order may run `n` ticks. |
| live-prepare, live-run | With `--live n`: the viewer's own start (prepared campaign, native AI) run by the live session in driven mode for `n` ticks. |

The first failing stage stops the run and prints its diagnostic. Nothing is skipped silently: data that names
things which do not exist follows the debug build's load rules (`docs/behaviour/campaign-setup.md`, CGS-05..12) and
is reported.

## Native AI on a mod chain (`--live n --live-ai on`)

The AI start follows the same rule as the rest of the setup: what the data names and nothing defines is a null reference,
reported and left out, and what a mod authors under its own names is found where it is.

- Selection content: a name in `Required_Orbiting_Units`, `Planet_Restricted_Unit_Types` or `Potential_Indigenous_Power` that no
  bound type carries is skipped and listed (CGS-07).
- AI files: the stock-named files are read first. Only when that closure does not load (a mod's templates name equations that
  live in a file with another name) are the mounted `templates`, `goalfunctions`, `goals` and `perceptualequations` files added,
  one at a time; a file that does not load is skipped and listed. A stock closure keeps its exact bytes. Reading every mounted
  file, as the debug build does, would move the stock content identity and waits for the batch pin regeneration.
- Difficulty: the level is one setting for all AI players; a player type that authors another adjustment name for it plays the
  chosen level.
- Goal lines (`--live n --live-ai on --live-goals k`): the live run samples fleets at most 60 ticks apart; at multiples of `k` and the end it notes, per AI
  player, the goals by goal function (count, active, planned, running plan, largest desire), the retained movement rows and the
  engine's refusal counters (goal unavailable, allocation blocked / desire / no plan / units / time / funds, unavailable budget
  equation). Each retained goal also lists its target, desire, selected plan, running plan ID and retained movement requests for that plan.
  Movement requests are current retained rows, not a history of every `Move_To` call. The flag enables observation counters;
  these are excluded from saved state and decisions. Use it to see where an AI that builds but never moves stops.
- Plans: a plan whose pause library differs from the first plan's is listed; each plan sleeps by its own library at run time.

- Plan files that do not load: the leaf layer's conquer plan script has a table constructor with an empty field
  (`"a",` newline `, "b"`). The debug build's table-constructor parser accepts one separator between fields (and a leading
  `;`), so it rejects that script with the same "unexpected symbol near ','" as ours: the original game does not load that plan
  either. Its plan definition manager logs a file whose script does not load (or that names no task force or no goal type) and
  goes on with the next file; the goal never gets that plan. The catalog does the same: such a file is skipped and listed
  (`galactic AI plan skipped: ...`). A catalog in which no plan loads is still refused.

## Submod chains

Pass every layer, leaf first. A leaf can list files that only one of its parents ships (the Remake Clone Wars
holdouts submod lists Republic building files that its own folder does not contain), so a missing layer shows up as
dangling `Starting_Forces` names in the report. Compare the count with and without a layer before trusting a chain.
