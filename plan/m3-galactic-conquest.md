# M3 galactic conquest: dependency plan

Written 2026-10-07 with the [galactic architecture](../docs/galactic-architecture.md)
(ADR-012 to ADR-021) for the M3 phase epic (legacy EAWR-18) and its tracks E3-0 to E3-10
(legacy EAWR-1948, EAWR-1949, EAWR-1950, EAWR-1951, EAWR-1952, EAWR-1953, EAWR-1954, EAWR-1955, EAWR-1956, EAWR-1957, EAWR-2013). Slices: S1 galaxy on screen (legacy EAWR-1964), S2 conquer
(legacy EAWR-1965), S3 space battles and save/load (legacy EAWR-1966), S4 galactic AI opponent
(legacy EAWR-1967), S5 full vanilla GC (legacy EAWR-1968), sign-off (legacy EAWR-1969).

This is a planning document. Dates are forecasts under the stated assumptions, not
commitments; the coordinator turns it into the schedule and re-estimates from the first
delivered slices, as [the estimate](../docs/estimate.md#measured-recalibration-2026-10-02)
asks.

## Foundation tickets

The walks filed behaviour packets; nothing owned the shared skeleton they all plug into.
These foundation tickets (FT) do, each filed as a sub-issue of its track epic:

| FT | Ticket | Track |
|---|---|---|
| FT-01 | Galactic simulation skeleton: `GalacticSession`, storage, sectioned hash, snapshot, events | E3-1 |
| FT-02 | Galactic clock: session tick and galactic frame, nested pause, speed settings, fiscal window | E3-1 |
| FT-03 | Command envelope, galactic opcode block and the GC replay format | E3-1 |
| FT-04 | `GameSession` skeleton, `sim_headless` GC runner and synthetic campaigns | E3-1 |
| FT-05 | GC replay corpus, pinned hashes and the worker-parity gate | E3-1 |
| FT-06 | Two-peer lockstep loopback gate (tactical first, then GC; second PR FT-06b) | E3-1 |
| FT-07 | Checkpoint container, validate-then-swap restore, child-log section, restore-and-continue harness: the first PR of the existing save-and-load ticket (legacy EAWR-681) | E3-6 |
| FT-07b | Galaxy checkpoint sections and restore-and-continue over the GC corpus | E3-6 |
| FT-08 | Tactical nesting: child lifecycle, conflict record, result record, auto-resolve seam | E3-7 |
| FT-09 | Conflict entry builder: tactical setup from a conflict record, sparse persistent IDs | E3-7 |
| FT-10 | Galactic Lua host: `ScriptedGalacticSession`, scheduling, routing, galactic Lua API inventory | E3-3 |
| FT-11 | `LiveGameSession` on the simulation thread, pacing and the presentation boundary | E3-4 |
| FT-12 | Viewer galactic mode skeleton (second PR FT-12b on `LiveGameSession`) | E3-4 |
| FT-13 | Viewer battle hand-off and return in one process | E3-4 |
| FT-14 | GC benchmark and deterministic work budgets for the galactic phases | E3-1 |
| FT-15 | AI-vs-AI GC soak harness | E3-5 |
| FT-16 | AI engine mode-neutral world interface | E3-5 |
| FT-17 | Tag registry galactic status shard | E3-1 |
| FT-18 | Galactic type table beside the M2 unit tables | E3-1 |
| FT-19 | Coop/versus seats: up to eight humans, one coop side or two versus sides, faction authorization, drop-out, side arbitration and battle forwarding | E3-10 |
| FT-20 | Coop presentation: my seat and my faction, per-seat selection, control groups and camera | E3-10 |
| FT-21 | Two-seat shared-control space skirmish on the lockstep loopback | E3-10 |

Issue numbers for the FT rows are in [the ticket map](#ticket-map). Existing tickets whose
scope this design changes carry one comment each saying how.

## Dependency graph

Calibrated hours use the planning-rate table of
[the estimate](../docs/estimate.md#measured-recalibration-2026-10-02) (area by size), times
**1.25** for foundation tickets (our own design, novel lifecycle) and **1.5** for
walk-driven behaviour (the GC discovery allowance). The area mapping is: simulation C++ at the
weapons row, AI, UI/HUD, economy, abilities, rendering, tooling and docs at their rows.
Rates include the reviews in the run records. Large existing tickets are split where the
graph needs it: the galaxy model into planet graph (a), fleets (b) and locomotion (c); the
economy into the fiscal ledger (a) and planet queues (b); the space battle scene build
(legacy EAWR-2006) into three parts (a: forces split, spawning and identity; b: station, garrison,
planetary weapons and orbital structures; c: upgrades, escorts and entry markers) so b and c run
beside a. The wave-3 gap tickets are in: space battles from the galaxy (legacy EAWR-2006, EAWR-2007,
EAWR-2008), presentation (legacy EAWR-2000, EAWR-2002, EAWR-2003, EAWR-2004, EAWR-2009, EAWR-2010, EAWR-2011) and auto-resolve
(legacy EAWR-2022, EAWR-2023, EAWR-2024, EAWR-2025, EAWR-2026), with their walks' calibrated hours times the 1.5
allowance. Story packets below come from the published story walk (legacy EAWR-1960): notifications
(legacy EAWR-2042), rewards (legacy EAWR-2043), and presentation (legacy EAWR-2044). Core remains the existing story ticket
(legacy EAWR-683); stock GC hero arrivals and unlocks are required by S5, while the full land-led
FoC story campaign is M4.

Lane: **now** can start while the M2 tail runs (new directories, no shared-file contention);
**full fleet** after M2 sign-off; **after M2 AI tail** waits for the M2 AI files to settle.
Slice is the first slice that needs the work.

| ID | Work | Track | Depends on | Size | Area | Calibrated h | Lane | Slice |
|---|---|---|---|---|---|---:|---|---|
| FT-01 | Galactic sim skeleton | E3-1 | - | L | sim | 8.75 | now | S1 |
| FT-03 | Command envelope + GC replay format | E3-1 | FT-01 | L | sim | 8.75 | now | S1 |
| FT-02 | Galactic clock: pause, speed, fiscal window | E3-1 | FT-03 | S | sim | 4.38 | now | S1 |
| FT-04 | GameSession + sim_headless GC runner | E3-1 | FT-03 | M | tooling | 2.81 | now | S1 |
| FT-05 | GC replay corpus + pins | E3-1 | FT-04, FT-02 | M | tooling | 2.81 | now | S1 |
| FT-06 | Lockstep loopback gate | E3-1 | - | M | tooling | 2.81 | now | S1 |
| FT-06b | Loopback on GameSession | E3-1 | FT-06, FT-04 | S | tooling | 2.81 | now | S1 |
| FT-07 | Checkpoint container + restore harness | E3-6 | - | L | sim | 8.75 | now | S3 |
| FT-07b | Galaxy checkpoint sections wiring | E3-6 | FT-07, FT-04, part c (legacy EAWR-680), part b (legacy EAWR-682), FT-10 | M | sim | 4.38 | full fleet | S3 |
| FT-08 | Tactical nesting: child, conflict + result records | E3-7 | FT-04, FT-02 | L | sim | 8.75 | full fleet | S2 |
| FT-09 | Conflict entry builder | E3-7 | FT-08, part b (legacy EAWR-680), FT-18 | L | sim | 10.5 | full fleet | S3 |
| FT-10 | Galactic Lua host + API inventory | E3-3 | FT-04 | L | AI | 6.88 | full fleet | S4 |
| FT-11 | LiveGameSession on the sim thread | E3-4 | FT-04 | M | sim | 4.38 | now | S1 |
| FT-12 | Viewer galactic mode skeleton | E3-4 | FT-01 | L | UI/HUD | 8.12 | now | S1 |
| FT-12b | Viewer galactic mode on LiveGameSession | E3-4 | FT-12, FT-11 | S | UI/HUD | 2.81 | full fleet | S1 |
| FT-13 | Viewer battle hand-off and return | E3-4 | FT-12b, FT-09, loading-screen return loop (legacy EAWR-1923) | L | UI/HUD | 8.12 | full fleet | S3 |
| FT-14 | GC benchmark + work budgets | E3-1 | FT-04, part c (legacy EAWR-680) | M | tooling | 2.81 | full fleet | S2 |
| FT-15 | AI-vs-AI GC soak | E3-5 | FT-04, FT-07 | M | tooling | 2.81 | full fleet | S4 |
| FT-16 | AI engine mode-neutral world interface | E3-5 | - | M | AI | 3.44 | after M2 AI tail | S4 |
| FT-17 | Tag registry galactic shard | E3-1 | tag overview (legacy EAWR-1863) | S | tooling | 2.81 | now | S1 |
| FT-18 | Galactic type table | E3-1 | (legacy EAWR-273), catalog diagnostics (legacy EAWR-793) | M | economy | 3.75 | now | S1 |
| FT-19 | Eight-human seats and whole-team battle roles, observers, dropout, forwarding | E3-10 | FT-03, FT-08 | L | sim | 8.75 | full fleet | S2 |
| FT-20 | Coop/versus UI: seat/faction, team-lead battle choice, role-aware controls and observer view | E3-10 | FT-12b, FT-19 | L | UI/HUD | 8.12 | full fleet | S3 |
| FT-21 | Two-seat shared-control space skirmish on the loopback | E3-10 | FT-06 | S | tooling | 2.81 | after M2 tail | S1 |
| (legacy EAWR-273) | Retail list/empty/boolean parse | M2 | - | S | economy | 3.0 | now | S1 |
| (legacy EAWR-679) | GC data loaders | E3-1 | (legacy EAWR-273), catalog diagnostics (legacy EAWR-793) | M | economy | 4.5 | now | S1 |
| part a (legacy EAWR-680) | Galaxy: planet graph + paths | E3-1 | FT-01, (legacy EAWR-679) | L | sim | 10.5 | now | S1 |
| part b (legacy EAWR-680) | Galaxy: fleets, membership, commands | E3-1 | part a (legacy EAWR-680), FT-03, FT-18 | L | sim | 10.5 | full fleet | S1 |
| part c (legacy EAWR-680) | Galaxy: locomotion + arrival | E3-1 | part b (legacy EAWR-680), FT-02 | L | sim | 10.5 | full fleet | S2 |
| (legacy EAWR-1149) | Campaign session setup | E3-1 | part b (legacy EAWR-680), FT-04, FT-18 | L | sim | 10.5 | full fleet | S1 |
| part a (legacy EAWR-682) | Fiscal ledger + income | E3-2 | (legacy EAWR-1149), FT-02 | M | economy | 4.5 | full fleet | S1 |
| part b (legacy EAWR-682) | Planet queues + admission | E3-2 | part a (legacy EAWR-682) | L | economy | 9.0 | full fleet | S2 |
| (legacy EAWR-1114) | Political control | E3-1 | part c (legacy EAWR-680), part a (legacy EAWR-682) | M | sim | 5.25 | full fleet | S2 |
| (legacy EAWR-1115) | Conflict admission + hand-off | E3-7 | part c (legacy EAWR-680), FT-08 | L | sim | 10.5 | full fleet | S2 |
| (legacy EAWR-2022) | Auto-resolve: side gathering and strength | E3-7 | (legacy EAWR-679), FT-01 | L | sim | 10.5 | full fleet | S2 |
| (legacy EAWR-2023) | Auto-resolve: round, winner, loss allocation | E3-7 | (legacy EAWR-2022) | L | sim | 10.5 | full fleet | S2 |
| (legacy EAWR-2024) | Auto-resolve: outcome, retreat results, battle record | E3-7 | (legacy EAWR-2023), (legacy EAWR-1116) | M | sim | 5.25 | full fleet | S2 |
| (legacy EAWR-2025) | Auto-resolve: dialogs, advisor text, result screens | E3-4 | (legacy EAWR-2024), FT-12b | L | UI/HUD | 9.75 | full fleet | S2 |
| (legacy EAWR-2026) | Mid-battle auto-resolve from the retreat dialog | E3-7 | part a (legacy EAWR-2006), (legacy EAWR-2023), (legacy EAWR-2008) | M | sim | 5.25 | full fleet | S3 |
| (legacy EAWR-1116) | Tactical return + retreat | E3-7 | (legacy EAWR-1115), FT-08 | L | sim | 10.5 | full fleet | S2 |
| (legacy EAWR-1150) | Victory + defeat | E3-1 | (legacy EAWR-1114), (legacy EAWR-1149) | M | sim | 5.25 | full fleet | S2 |
| (legacy EAWR-1129) | Population census | E3-2 | part b (legacy EAWR-682) | M | economy | 4.5 | full fleet | S2 |
| (legacy EAWR-1131) | Fiscal/production events | E3-2 | part b (legacy EAWR-682) | M | economy | 4.5 | full fleet | S2 |
| (legacy EAWR-1157) | GC camera | E3-4 | FT-12 | M | UI/HUD | 4.88 | full fleet | S1 |
| (legacy EAWR-1155) | Planet HUD + production | E3-4 | FT-12b, part b (legacy EAWR-682) | L | UI/HUD | 9.75 | full fleet | S2 |
| (legacy EAWR-1156) | Map input + selection | E3-4 | FT-12b, part c (legacy EAWR-680) | M | UI/HUD | 4.88 | full fleet | S2 |
| (legacy EAWR-2000) | Galaxy scene, planets, planet colours | E3-4 | FT-12, part a (legacy EAWR-680) | L | rendering | 6.75 | full fleet | S1 |
| (legacy EAWR-2002) | Trade-route and fleet route lines | E3-4 | (legacy EAWR-2000) | M | rendering | 3.38 | full fleet | S1 |
| (legacy EAWR-2003) | Corruption and galactic one-shot effects | E3-4 | (legacy EAWR-2002), (legacy EAWR-1141) | M | rendering | 3.38 | full fleet | S5 |
| (legacy EAWR-2004) | Galactic music and event sounds | E3-4 | (legacy EAWR-1149), FT-12b | M | UI/HUD | 4.88 | full fleet | S2 |
| (legacy EAWR-2009) | Custom-campaign front end and save/load menus | E3-4 | (legacy EAWR-1149), FT-12b | L | UI/HUD | 9.75 | full fleet | S2 |
| (legacy EAWR-2010) | Campaign battle loading and summary screens | E3-4 | FT-13, (legacy EAWR-1116) | M | UI/HUD | 4.88 | full fleet | S3 |
| (legacy EAWR-2011) | Campaign end: score screen and win movies | E3-4 | (legacy EAWR-1150), FT-12b | M | UI/HUD | 4.88 | full fleet | S2 |
| part a (legacy EAWR-2006) | Scene build: forces split, spawning, identity (WGT-03..08, 19..24) | E3-7 | FT-09, (legacy EAWR-679), part b (legacy EAWR-680), (legacy EAWR-1115) | L/3 | station upgrades | 15.0 | full fleet | S3 |
| part b (legacy EAWR-2006) | Scene build: station, garrison, planetary weapons, orbital structures (WGT-09..13) | E3-7 | FT-09, (legacy EAWR-679) | L/3 | station upgrades | 9.37 | full fleet | S3 |
| part c (legacy EAWR-2006) | Scene build: upgrades, escorts, entry markers (WGT-14..18) | E3-7 | FT-09, (legacy EAWR-679) | L/3 | station upgrades | 9.37 | full fleet | S3 |
| (legacy EAWR-2007) | GC battle rules that differ from skirmish | E3-7 | part a (legacy EAWR-2006), part b (legacy EAWR-682) | L | economy | 9.0 | full fleet | S3 |
| (legacy EAWR-2008) | GC tactical space retreat | E3-7 | part a (legacy EAWR-2006), (legacy EAWR-1116) | XL | abilities | 20.62 | full fleet | S3 |
| part b (legacy EAWR-681) | Tactical full-state checkpoint (second PR) | E3-6 | FT-07 | L | sim | 10.5 | now | S3 |
| (legacy EAWR-252) | GC checkpoint capacity | E3-6 | FT-07b | M | tooling | 3.38 | full fleet | S3 |
| (legacy EAWR-1120) | GC AI lifecycle + cadence | E3-5 | FT-16, FT-10 | L | AI | 8.25 | full fleet | S4 |
| (legacy EAWR-1121) | GC AI perception | E3-5 | (legacy EAWR-1120), part c (legacy EAWR-680) | L | AI | 8.25 | full fleet | S4 |
| (legacy EAWR-1122) | GC AI funding ledger | E3-5 | (legacy EAWR-1120), part a (legacy EAWR-682) | L | AI | 8.25 | full fleet | S4 |
| (legacy EAWR-1123) | GC AI goals + force selection | E3-5 | (legacy EAWR-1121), (legacy EAWR-1122), part b (legacy EAWR-682) | L | AI | 8.25 | full fleet | S4 |
| (legacy EAWR-1124) | GC AI freestore | E3-5 | (legacy EAWR-1123) | L | AI | 8.25 | full fleet | S4 |
| (legacy EAWR-1125) | GC AI build + attack plans | E3-5 | (legacy EAWR-1124), (legacy EAWR-1115) | L | AI | 8.25 | full fleet | S4 |
| (legacy EAWR-1126) | GC AI magic defense | E3-5 | (legacy EAWR-1125) | M | AI | 4.12 | full fleet | S4 |
| (legacy EAWR-1127) | GC AI battle return + checkpoint | E3-5 | (legacy EAWR-1116), (legacy EAWR-1122), FT-07b | L | AI | 8.25 | full fleet | S4 |
| (legacy EAWR-683) | Story engine: graphs, ordered reduction, flags, persistence | E3-3 | FT-10, (legacy EAWR-679), (legacy EAWR-1149) | L | AI | 8.25 | full fleet | S5 |
| (legacy EAWR-2042) | Story galactic notifiers (WST-30..39, 52) | E3-3 | (legacy EAWR-683), part c (legacy EAWR-680), (legacy EAWR-1114), part b (legacy EAWR-682), (legacy EAWR-1116), (legacy EAWR-1140) | L | AI | 8.25 | full fleet | S5 |
| (legacy EAWR-2043) | Galactic rewards: spawn/unlock first (WST-40..46) | E3-3 | (legacy EAWR-683), (legacy EAWR-2042), (legacy EAWR-1150), (legacy EAWR-679), part c (legacy EAWR-680), part b (legacy EAWR-682) | L | AI | 8.25 | full fleet | S5 |
| (legacy EAWR-2044) | Story goals, objectives, speech-done and tutorial presentation | E3-3 | (legacy EAWR-683), (legacy EAWR-2043), FT-12b, (legacy EAWR-236), (legacy EAWR-1669), (legacy EAWR-1158) | L | UI/HUD | 9.75 | full fleet | S5 |
| story bindings (legacy EAWR-1991) | Story bridge extension inside FT-10 allowance (WST-53..57) | E3-3 | FT-10, (legacy EAWR-683) | included | AI | 0.0 | full fleet | S5 |
| story conflict adapter (legacy EAWR-1115) | Space story overrides inside conflict allowance (WST-44, 50/51) | E3-7 | (legacy EAWR-1115), (legacy EAWR-683), (legacy EAWR-2043), FT-09, part a (legacy EAWR-2006) | included | sim | 0.0 | full fleet | S5 |
| (legacy EAWR-1153) | Observer intelligence | E3-4 | part c (legacy EAWR-680) | L | sim | 10.5 | full fleet | S5 |
| (legacy EAWR-1154) | Fleet intelligence presentation | E3-4 | (legacy EAWR-1153), FT-12b | M | UI/HUD | 4.88 | full fleet | S5 |
| (legacy EAWR-1158) | Advisor + history | E3-4 | (legacy EAWR-1131), FT-12b | M | UI/HUD | 4.88 | full fleet | S5 |
| (legacy EAWR-1128) | Income + production modifiers | E3-2 | part b (legacy EAWR-682) | L | economy | 9.0 | full fleet | S5 |
| (legacy EAWR-1141) | Corruption transitions | E3-8 | (legacy EAWR-1114) | L | abilities | 10.5 | full fleet | S5 |
| (legacy EAWR-1140) | Corruption actions | E3-8 | (legacy EAWR-1141), part c (legacy EAWR-680) | L | abilities | 10.5 | full fleet | S5 |
| (legacy EAWR-1130) | Corruption + black-market economy | E3-2 | part b (legacy EAWR-682), (legacy EAWR-1141) | L | economy | 9.0 | full fleet | S5 |
| (legacy EAWR-1142) | Neutralization + sabotage | E3-8 | (legacy EAWR-1140) | L | abilities | 10.5 | full fleet | S5 |
| (legacy EAWR-1143) | Theft, siphon, spy | E3-8 | (legacy EAWR-1140), (legacy EAWR-1153) | L | abilities | 10.5 | full fleet | S5 |
| (legacy EAWR-1144) | Superweapon hack + planet destruction | E3-8 | (legacy EAWR-1140), (legacy EAWR-1150) | M | abilities | 5.25 | full fleet | S5 |
| (legacy EAWR-236) | Lua HUD bridge | E3-4 | FT-10, FT-12b | M | UI/HUD | 4.88 | full fleet | S5 |
| (legacy EAWR-237) | HUD movies | E3-4 | FT-12b | M | rendering | 3.38 | full fleet | S5 |
| (legacy EAWR-1669) | Speech + hologram movies | E3-4 | (legacy EAWR-683), FT-12b | M | UI/HUD | 4.88 | full fleet | S5 |
| (legacy EAWR-1644) | Construction-unlock notification | E3-4 | part b (legacy EAWR-682) | S | UI/HUD | 3.38 | full fleet | S5 |
| (legacy EAWR-560) | Data rosters, role/type-based tests and scoped content identities | E3-1 | (legacy EAWR-679), FT-18, FT-05 | XL | tooling | 13.5 | after M2 tail | S5 |
| (legacy EAWR-232) | Mod HUD parity parent reconciliation (children priced separately) | E3-4 | (legacy EAWR-236), (legacy EAWR-237) | tracker | docs | 0.5 | full fleet | S5 |
| (legacy EAWR-1963) | Settle unverified items (wave 4) | E3-0 | - | L | docs | 5.25 | now | S2 |
| (legacy EAWR-1964) | S1 acceptance | E3-9 | FT-21, (legacy EAWR-2002), FT-05, FT-06b, part a (legacy EAWR-682), FT-12b, (legacy EAWR-1157), (legacy EAWR-1149) | S | docs | 2.62 | full fleet | S1 |
| (legacy EAWR-1965) | S2 acceptance | E3-9 | (legacy EAWR-1964), FT-19, (legacy EAWR-2024), (legacy EAWR-2025), (legacy EAWR-2004), (legacy EAWR-2009), (legacy EAWR-2011), (legacy EAWR-1116), (legacy EAWR-1150), (legacy EAWR-1129), (legacy EAWR-1155), (legacy EAWR-1156), (legacy EAWR-1131), FT-14 | S | docs | 2.62 | full fleet | S2 |
| (legacy EAWR-1966) | S3 acceptance | E3-9 | (legacy EAWR-1965), FT-20, FT-13, part b (legacy EAWR-2006), part c (legacy EAWR-2006), (legacy EAWR-2007), (legacy EAWR-2008), (legacy EAWR-2010), (legacy EAWR-2026), FT-07b | S | docs | 2.62 | full fleet | S3 |
| (legacy EAWR-1967) | S4 acceptance | E3-9 | (legacy EAWR-1966), (legacy EAWR-1126), (legacy EAWR-1127), FT-15 | S | docs | 2.62 | full fleet | S4 |
| (legacy EAWR-1968) | S5 acceptance | E3-9 | (legacy EAWR-1967), (legacy EAWR-2042), (legacy EAWR-2043), story bindings (legacy EAWR-1991), (legacy EAWR-2044), story conflict adapter (legacy EAWR-1115), (legacy EAWR-2003), (legacy EAWR-1142), (legacy EAWR-1143), (legacy EAWR-1144), (legacy EAWR-1130), (legacy EAWR-1128), (legacy EAWR-1154), (legacy EAWR-1158), (legacy EAWR-236), (legacy EAWR-237), (legacy EAWR-1669), (legacy EAWR-1644), (legacy EAWR-560), (legacy EAWR-232) | S | docs | 2.62 | full fleet | S5 |
| (legacy EAWR-1969) | M3 sign-off | E3-9 | (legacy EAWR-1968) | M | docs | 2.62 | full fleet | S5 |

Totals: **643.9 calibrated worker-hours** (26.8 agent-days), of which the 24 foundation
rows are 131.1 h. By first slice: S1 118.9 h, S2 158.3 h, S3 129.9 h, S4 77.6 h, S5 159.2 h.
The zero-hour story milestones are scope extensions already included in FT-10 and conflict
admission, not free implementations or duplicate estimates. Story replaces its 16.5 h allowance
with 26.25 h of new adapters; the new whole-team battle roles
add 8.4 h to FT-19/20. These are worker occupancy estimates, not elapsed delivery dates.

## Critical paths

Pure dependency chains at central latency (section below), from a cold start:

| Slice | Chain | Hours |
|---|---|---:|
| S1 | parse (legacy EAWR-273) -> data (legacy EAWR-679) -> graph part a (legacy EAWR-680) -> fleets part b (legacy EAWR-680) -> setup (legacy EAWR-1149) -> fiscal ledger part a (legacy EAWR-682) -> S1 | 79 |
| S2 | S1 spine to fleets -> locomotion part c (legacy EAWR-680) -> hand-off (legacy EAWR-1115) -> return (legacy EAWR-1116) -> outcome (legacy EAWR-2024) -> screens (legacy EAWR-2025) -> S2 | 129 |
| S3 | S1 spine to fleets -> locomotion part c (legacy EAWR-680) -> hand-off (legacy EAWR-1115) -> scene build part a (legacy EAWR-2006) -> retreat (legacy EAWR-2008) -> mid-battle auto-resolve (legacy EAWR-2026) -> S3 | 150 |
| S4 | S3 path -> S4 acceptance; the parallel AI chain is spine -> queues -> goals (legacy EAWR-1123) -> freestore (legacy EAWR-1124) -> plans (legacy EAWR-1125) -> defence (legacy EAWR-1126) | 156 |
| S5 | spine -> political control (legacy EAWR-1114) -> corruption (legacy EAWR-1141) -> actions (legacy EAWR-1140) -> story notifications (legacy EAWR-2042) -> rewards (legacy EAWR-2043) -> presentation (legacy EAWR-2044) -> S5 | 164 |

The spine of M3 is **data -> planet graph -> fleets -> campaign start -> economy**; every
slice hangs off it, S2 and S3 through fleet movement, the conflict hand-off and the return.
Start the spine first and keep it staffed. The scene build is the largest single ticket on the
S3 path: split it, and start parts b and c on fixture planet records as soon as the entry
builder (FT-09) lands. The AI chain (S4) is the longest single chain after the spine: start the
AI world interface (FT-16) as soon as the M2 AI files settle and the galactic Lua host (FT-10)
right after the `GameSession` skeleton.

## Calendar at 10-15 workers

The schedule uses every dependency row, including sequential slice acceptance. It is a
cold-start forecast from 2026-10-08 09:00, not a claim that the already-started foundations
need doing again. Continuous agent availability is assumed; owner availability is a gate.

- **Calibrated central:** five now-lane workers, then twelve from 10-11; ticket wall latency
  1.4 x calibrated hours + 2 h per PR; owner gates 12 h per slice, 24 h S5, 48 h sign-off.
- **Slow:** four now-lane workers, then ten from 10-13; 2.2 x hours + 4 h per PR; owner
  gates 24 h per slice, 48 h S5, 96 h sign-off.
- **Realistic shared-host forecast:** slow capacity and owner gates, plus a 4 h congestion
  allowance per compiled/viewer packet and at most twelve M3 merges/day. These are explicit
  conservative allowances, not measured queue-to-build durations; the historical worker
  rates already include ordinary reviews/queues. Ready work is scheduled by downstream
  critical-path weight, and merge admission is in completion order. Split rows are PRs;
  a packet needing extra fix PRs is a reason to reforecast, not free capacity.

Fresh evidence on 2026-10-07 informs the realistic column:

- `main`'s first-parent PR merges on the four complete days 10-03 to 10-06 were 87, 110,
  52 and 70: median 78.5/day, range 52–110. These include M2, tools and docs; they are not
  a demonstrated GC rate. Twelve M3 merges/day reserves less than a quarter of the lowest
  observed total for GC while the M2 tail shares the fleet.
- The build journal at 18:06 UTC reported 18 waiting Linux clients for two slots, and
  6–13 waiting Windows clients per candidate lane in the seven-slot primary pool. Pool
  clients can appear on several candidates; do not add those counts. Host queue coverage
  was marked incomplete and health/occupancy observations were mixed, so these indicate
  congestion rather than a guaranteed current FIFO depth. GPU lanes showed no queued work
  in that sample. Re-read queue telemetry before increasing the fleet.
- Owner turnaround in the estimate includes evening/overnight waits and about one acceptance
  evening per gate. The realistic column books at least a day for each slice, two for S5,
  four for sign-off. These are allowances until M3 supplies measured acceptance latencies.

| Slice | Calibrated central | Slow | Realistic shared hosts | Board forecast |
|---|---|---|---|---|
| S1 galaxy on screen | 2026-10-14 | 2026-10-18 | 2026-10-18 | 2026-10-16 |
| S2 conquer | 2026-10-15 | 2026-10-21 | 2026-10-22 | 2026-10-23 |
| S3 space battles, save/load | 2026-10-16 | 2026-10-22 | 2026-10-24 | 2026-10-30 |
| S4 galactic AI | 2026-10-17 | 2026-10-23 | 2026-10-25 | 2026-11-06 |
| S5 full vanilla GC | 2026-10-18 | 2026-10-26 | 2026-10-27 | 2026-11-16 |
| Sign-off | 2026-10-20 | 2026-10-28 | 2026-10-30 | 2026-11-20 |

The realistic S1 date exceeds the board's 10-16 forecast; revisit that booking with actual
foundation receipts and M2 capacity, rather than treating the calibrated central column as
a commitment. Later board dates retain contingency. The walks still contain unverified
items and greenfield GC has no demonstrated delivery rate. Re-measure after the first ten
M3 PRs and every owner preview; additional review rounds, missing evidence, slower service
or a changed full-fleet date move the calendar. Future option features remain outside M3.

## Branches and sync

Owner decision 2026-10-07: GC is built on its own branch.

- **The cut is complete.** The coordinator cut `Thom-Ernst/m3-galactic` from `main` at
  `78bee775` on 2026-10-07 after the pre-GC refactor check. Foundation lanes A to E have
  started; the calendar's full-fleet date is a capacity increase after the M2 tail, not the
  branch cut. GC PRs target this integration branch now. Docs-only planning PRs may target
  `main`; changes to existing M2 behaviour retail parser (legacy EAWR-273) land on `main` and
  sync into GC. New-directory foundation PRs may stack before their dependencies merge.
- **After the cut.** Every M3 PR targets `Thom-Ernst/m3-galactic`. Validation v2 applies per PR against
  the GC branch head; the batch checks (installed-game, five targets, GPU, pin regeneration)
  run on the GC branch as well as on `main`.
- **Sync.** `main` is merged into the GC branch at least daily and after every batch on
  `main`, by one coordinator-owned sync PR that resolves conflicts and reruns the Linux job
  and the M2 replay comparison. The M2 game pins come from `main`; GC pins live only on the GC
  branch. GC does not merge into `main` until M3 sign-off, or an earlier slice boundary the
  owner chooses.
- **Tactical changes GC needs.** Behaviour-neutral changes to M2 files (the AI world interface
  FT-16, the pre-GC refactors) land on `main` first so both branches share them and syncs stay
  small. GC-only tactical additions (the sparse-ID header extension of FT-09, the retreat
  opcode, the coop seat forwarding of FT-19) land on the GC branch as opt-in additions that
  leave M2 replays byte-identical.
- **Leases.** A shared-file lease covers both branches: an M2 PR and a GC PR that edit the same
  shared file conflict at the next sync, so the GC side keeps its edits to the one-line
  includes listed below.

## Owner scope seams and future work

The evidence/design tracking tickets carry no extra implementation hours: architecture
(legacy EAWR-1958), movement/control walk (legacy EAWR-1113), AI walk (legacy EAWR-1119), economy walk
(legacy EAWR-1132), corruption/heroes walk (legacy EAWR-1145), session/save walk (legacy EAWR-1148),
UI/sensors walk (legacy EAWR-1152), auto-resolve walk (legacy EAWR-1959), story walk (legacy EAWR-1960),
tactical walk (legacy EAWR-1961), presentation walk (legacy EAWR-1962). Their behaviour packets
are priced in the graph; research follow-ups belong to the sweep (legacy EAWR-1963).
The phase/track epics are roll-ups, never added to child effort. Data-roster/scoped-identity
work (legacy EAWR-560) uses the current tooling XL rate with the 1.5 discovery allowance;
the mod HUD parent (legacy EAWR-232) retains only 0.5 h reconciliation, avoiding child double count.

These boundaries are in the existing M3 tickets' estimates; the future feature tickets do
not add replacement algorithms, camera options or diplomacy to M3.

| Future ticket | M3 contract / existing owners | Dependencies for future work | Size / hours / lane / slice |
|---|---|---|---|
| (legacy EAWR-2033) | `AutoResolveModel`: forces, strengths, planet, modifiers, conflict RNG; typed result; setup model identity and canonical restore. FT-08, (legacy EAWR-2022), (legacy EAWR-2023) | those M3 model contracts | future sizing deferred; after M3; no M3 slice |
| (legacy EAWR-2034) | `GalacticCameraBehaviour`, data/mod parameters; seat-local pose outside sim. FT-12, (legacy EAWR-1157) | FoC camera | future sizing deferred; after M3; no M3 slice |
| (legacy EAWR-2035) | Arbitrary faction count, non-playable owners, `FactionController`; versus AI controls every non-player faction. (legacy EAWR-679), (legacy EAWR-1149), (legacy EAWR-1120), FT-19 | data/setup/controller contracts | future autonomy sizing deferred; after M3; no M3 slice |
| (legacy EAWR-2038) | Per-command-kind pause admission policy in setup/replay/checkpoint; FoC table default. FT-02/03/07b, (legacy EAWR-680) | command admission and save contracts | future option sizing deferred; after M3; no M3 slice |
| (legacy EAWR-2039) | Slot count N in model, slot commands, conflicts, save, snapshots and HUD; FoC default three. (legacy EAWR-680), (legacy EAWR-1115), (legacy EAWR-2000), (legacy EAWR-2002), (legacy EAWR-1155) | orbit/conflict/viewer contracts | future option sizing deferred; after M3; no M3 slice |

Tests exercise N=5 orbit slots, a non-default pause policy and a synthetic replacement
resolver without exposing those future options in the M3 product. The seat test matrix is
two-seat shared control, eight-seat coop, 4v4 and 3v1 versus, third-side rejection, dropout
with/without remaining human seats, and two human sides plus multiple AI-controlled factions.
Human-versus-AI conflicts in versus offer fight or auto-resolve; the non-fighting team's
lead assigns its whole team to control the AI units, join as temporary allies, or spectate.
The session command and effective mapping are replay/checkpoint state; observer commands,
partial-team assignments, non-lead choices and stale conflict IDs are refused. All peers seal
inputs and compare hashes, including observers. Any tactical child freezes all uninvolved
factions (WGT-01); return applies results to original owners and clears temporary roles.
FT-19 grows M -> L (8.75 h) and FT-20 M -> L (8.12 h) for these owner requirements;
the two-seat FT-21 smoke test stays small, with the role matrix in FT-19/20.

## Lanes that can start now

New directories, no contention with the M2 tail:

| Lane | Work | Touches |
|---|---|---|
| A galaxy core | FT-01, then FT-03, FT-02, FT-04 (stacked) | `src/sim/galactic/`, `include/eawr/sim/galactic/`, `tests/galactic/`, one `add_subdirectory` line each; `src/campaign/` from FT-04 |
| B galactic data | retail parse semantics (legacy EAWR-273), then GC data loaders (legacy EAWR-679), FT-18 | `src/data/galactic/` (own `sources.cmake`), `src/units/galactic_types.cpp` |
| C checkpoint | FT-07 on tactical sessions (child-log form), then tactical full state (legacy EAWR-681) | `src/checkpoint/` (new library), `tests/galactic/` |
| D lockstep gate | FT-06 on the tactical corpus first | `tests/galactic/lockstep*`, a harness header |
| E viewer | FT-12 against FT-01's synthetic snapshot | `apps/viewer/src/galactic/` (own `sources.cmake`), one include line in the viewer CMake, one entry in `viewer_mode_sources.py` |
| F registry | FT-17 | `tools/inventory/tag_registry.py`, the new shard |
| G research | wave-4 unverified items (legacy EAWR-1963) | `out/research/`, walk docs |

FT-16 waits for the M2 AI tail (it touches `src/script/foc`), FT-21 for the M2 skirmish tail.
Lanes A and B feed the spine and should get the first workers.

## Shared files and leases

| Shared file | Contention | Plan |
|---|---|---|
| Root `CMakeLists.txt`, `tests` CMake lists | one line per new library or test directory | FT-01 adds `src/sim/galactic` and `tests/galactic`; FT-04 adds `src/campaign`; FT-07 adds `src/checkpoint`; nothing else touches them |
| `src/sim/sources.cmake` | M2 owns it | not touched by M3: the galaxy has `src/sim/galactic/sources.cmake` |
| `src/data/sources.cmake`, `src/units/sources.cmake`, `src/platform/sources.cmake`, `src/script/sources.cmake` | one line each | the first M3 PR in each adds a galactic source list once (data, script and platform); give each list its own direct `include()` in viewer CMake, not a nested include, so `viewer_mode_sources.py` discovers it; later PRs edit only the galactic list |
| `apps/viewer/CMakeLists.txt` | hand-listed viewer sources | FT-12 includes `apps/viewer/src/galactic/sources.cmake` once; galactic viewer PRs edit only that list; a viewer build is still required |
| `tests/presentation/renderer/viewer_mode_sources.py` | one entry per mode | FT-12 adds the galactic mode once |
| `docs/tag-coverage/statuses.json` | every applied tag | FT-17 moves GC classes (Campaign, Planet, TradeRoute and the galactic GameConstants and Faction rows) into a galactic shard; GC PRs edit the shard |
| Census and pins | generated | GC census and pins are new files (`data/census/galactic-*`, `tests/galactic/fixtures`); the coordinator batch on the GC branch regenerates them, never a PR |
| `docs/simulation.md` phase map, `docs/replay-format.md` | M2 docs | GC writes `docs/galactic-architecture.md` (phase map) and a new `docs/galactic-replay-format.md`; the tactical phase-map test is not touched |
| Opcode registry in the common spec | coordinator | FT-03 reserves the whole galactic block and one tactical retreat opcode at once; FT-09 needs one tactical replay header extension number |
| `apps/sim_headless/cli_options.cpp`, `main.cpp` | M2 tooling | FT-04 adds the GC dispatch once; GC options live in `cli_galactic.cpp` |
| `src/script/foc/ai_*` | M2 AI tail | FT-16 after the tail; galactic AI code in new `galactic_*` files |

With these, two M3 PRs contend only when they implement the same subsystem, which the graph
already orders.

## The first PRs in detail

Each is ready to become a worker spec. "Tests" names what the PR adds; validation follows
validation v2 (galactic C++ adds the GC corpus on 1/2/4/8 workers in the Linux job once FT-05
exists).

### 1. FT-01 galactic simulation skeleton (L, sim, 8.75 h)

- **Scope.** `eawr_sim_galactic` with its own `sources.cmake`; `GalacticSession` with
  `create`, `submit` (stub command type until FT-03), `step`, `snapshot`, `canonical_state_bytes`,
  `state_sha256`, `set_state_hasher`, `scramble_storage_for_testing`; EnTT storage for planets,
  routes and fleets as plain components; sectioned state hash `eawr-galactic-state-v1` with dirty
  tracking; `GalacticSnapshot` and an ordered event list; the `galactic-gather` phase; a synthetic
  galaxy builder for tests (no game data). Diagnostics block `EAWR-SIM-05xx`.
- **Depends.** Nothing.
- **Files.** `include/eawr/sim/galactic/{session,snapshot,types,events}.hpp`, `src/sim/galactic/*`,
  `tests/galactic/` (new CMake), one root CMake line each.
- **Tests.** Sectioned hash equals a from-scratch hash every tick; 1/2/4/8 workers and scrambled
  storage give identical hashes; the phase reaches the executor with its name and 64 partitions.

### 2. FT-03 command envelope and GC replay format (L, sim, 8.75 h)

- **Scope.** `GameCommand` (session, galactic, tactical layers) issued by a **seat**, with
  the seat table in the setup, canonical `(tick, seat, sequence)` order, per-seat sequencing
  and faction authorization (ADR-020, coop from the start); the galactic opcode block reserved for every command in the architecture
  table; `EAWRGCR` v1 (header: identities, campaign identity, seed, players and options; commands;
  final tick) with parse, write, validate and `record()`; `docs/galactic-replay-format.md`.
- **Depends.** FT-01. Stack on its branch.
- **Tests.** Byte round trip, every refusal diagnostic, out-of-order submission, late command,
  live-equals-replay; eight-human all-coop and 4v4 setup, third human side refused, arbitrary
  faction count with separate AI controllers. Setup pins pause policy, orbit slots and resolver ID.

### 3. FT-02 galactic clock (S, sim, 4.4 h)

- **Scope.** Session tick, galactic frame and tactical frame counters; nested pause with the pause
  and resume commands (WGM-05/06); speed settings as state with their commands (pacing only,
  WGM-U02); fiscal window in frames with one cycle per service and an empty payment hook
  (WGM-02/03, WGEP-01/02); the 30-frame periodic gate (WGM-04).
- **Depends.** FT-03.
- **Tests.** Fiscal boundary at 1350 frames from the stock value through a fixture constant
  table, no catch-up loop, pause nesting, paused galaxy does not advance, speed changes pacing without moving
  world outcomes (the speed field itself is hashed); FoC pause table refuses deployment swaps.

### 4. FT-04 `GameSession` skeleton and `sim_headless` GC runner (M, tooling, 2.8 h)

- **Scope.** `eawr_campaign` library with `GameSession` wrapping the galaxy (no child yet);
  `sim_headless --galactic-replay <file> --hash-out` and `--galactic <campaign> --game-root`
  (synthetic campaigns until (legacy EAWR-1149) builds the real start); two synthetic campaign
  fixtures under `tests/galactic/fixtures/`.
- **Depends.** FT-03.
- **Tests.** CLI tests: hash CSV header and rows, nonzero exit on bad input, no partial output.

### 5. FT-06 two-peer lockstep loopback gate (M, tooling, 2.8 h, then FT-06b S)

- **Scope.** A header-only harness: two sessions in one process; a scripted command generator
  per session type; sealed per-tick batches delivered with input delay D (default 3 ticks); the
  hash compared every N ticks (default 30) and at the end; peer B on 4 workers with scrambled
  storage; first divergence reported with both encodings. PR 1 runs it over tactical sessions and
  the M2 replay corpus; PR 2 (FT-06b) adds `GameSession`.
- **Depends.** Nothing (PR 1); FT-04 (PR 2).
- **Tests.** The gate itself as a CTest (`gc_lockstep_loopback`), plus a negative test that an
  injected divergence is caught at the right tick.

### 6. FT-07 checkpoint container (L, sim, 8.75 h)

- **Scope.** `EAWRCKP` v1 header and section registry; identities; validate-then-swap restore;
  `EAWR-CKP-01xx` diagnostics; the `child-log` section (conflict record placeholder, tactical setup,
  commands); a generic restore-and-continue harness. Its first user is a tactical session saved as
  `child-log` at sampled ticks of M2 replays and continued with identical hashes.
- **Depends.** Nothing for the container and the tactical child-log; galaxy sections follow in
  FT-07b.
- **Files.** `include/eawr/checkpoint/`, `src/checkpoint/` (new `eawr_checkpoint` library, one root
  CMake line), `tests/galactic/`.
- **Tests.** Round trip, truncated, oversize, wrong identity and corrupted section all refused
  with the running session untouched; restore-and-continue on M2 replays at three ticks.

### 7. FT-17 tag registry galactic shard (S, tooling, 2.8 h)

- **Scope.** `tag_registry.py` reads a list of status shards; the GC classes move to
  `docs/tag-coverage/statuses-galactic.json` with no status change; the gate and `check` work on
  both.
- **Depends.** Nothing.
- **Tests.** The registry tests over the split files; the gate result is unchanged.

### 8. Retail parse semantics (S, economy, 3 h)

The existing retail list, empty-value and boolean parse ticket (legacy EAWR-273).

Existing ticket; a prerequisite for tolerant GC loaders. No scope change.

### 9. GC data loaders (M, economy, 4.5 h)

The existing GC data ticket (legacy EAWR-679).

- **Scope change.** Loaders live in `src/data/galactic/`; tolerant diagnostics rows (ADR-017);
  every stock campaign definition loads in the installed-game test, the fixture campaign is
  proposed for the owner; the galactic type fields move to FT-18.
- **Depends.** Retail parse semantics (legacy EAWR-273).
- **Tests.** Loader tests on synthetic XML (unknown tags, dangling references, duplicate faction
  records per WGS-11/U01), installed-game listing of all 33 definitions with diagnostics.

### 10. FT-18 galactic type table (M, economy, 3.75 h)

- **Scope.** GC fields of unit, company, squadron, transport, hero and structure types from the
  shared object registry into `units::galactic_type_table`, with its own content identity; M2
  tables and pins untouched. Registry rows in the galactic shard.
- **Depends.** Retail parse semantics (legacy EAWR-273); FT-17 for the shard (or edit the main file before it lands).
- **Tests.** Table values for a synthetic fixture; installed-game table census.

### 11. Planet graph and paths (part a (legacy EAWR-680), L, sim, 10.5 h)

- **Scope.** Planets and routes in galactic storage from the loaded tables; the graph rule
  (WGM-16) and path search with its flags and costs (WGM-17..22, WGM-86), pure and deterministic;
  ties resolved by ascending planet ID until WGM-U03 is settled (listed unverified).
- **Depends.** FT-01, GC data loaders (legacy EAWR-679).
- **Tests.** Graph on synthetic and stock campaigns, path costs, gates and the corruption bypass
  penalty; work counts pinned.

### 12. FT-12 viewer galactic mode skeleton (L, UI, 8.1 h)

- **Scope.** `--eawr-galactic`; `apps/viewer/src/galactic/` with its own source list; planets at
  their positions with their models, route lines, fleet markers, ownership colours, a basic orbit
  camera; report and capture hooks; one GPU case. It reads "my seat" and "my faction"
  separately from the start (coop). Driven by FT-01's synthetic snapshot until
  FT-11 lands (FT-12b switches to `LiveGameSession`).
- **Depends.** FT-01.
- **Tests.** Viewer build; one GPU case on a GPU host; source-reading contract entry.

### 13. FT-11 `LiveGameSession` (M, sim, 4.4 h)

- **Scope.** The game session on its simulation thread, pacing by speed and pause with
  command-driven steps while paused, snapshot publication (two newest), the command sink; a header
  that names no session type; boundary checks extended for GC.
- **Depends.** FT-04.
- **Tests.** Live run recorded and replayed headless with equal hashes; paused session produces no
  empty ticks; failure isolation like the tactical live session.

### 14. FT-08 tactical nesting (L, sim, 8.75 h)

- **Scope.** `GameSession` child lifecycle (one layer per session tick, frozen galaxy), conflict
  and result record types, combined game hash, conflict-scoped tactical commands, the auto-resolve
  seam (a test `AutoResolveModel` until the FoC solver), result application hook for the
  return ticket. A synthetic child battle from a hand-built record.
- **Depends.** FT-04, FT-02.
- **Tests.** Enter, fight, return on a synthetic galaxy; galaxy hash unchanged across the battle
  except the reconciliation; late tactical command after the battle refused; 1/2/4/8 workers.

FT-13's first step extracts a `LiveSession`-shaped feed and adapters for standalone M2 and
the retained `GameSession` child (A2); only `GameSession` owns the child and its thread.
The source-list additions follow the direct viewer include precedent (G1).

### 15. FT-05 GC replay corpus and pins (M, tooling, 2.8 h)

- **Scope.** A corpus of synthetic GC replays with pinned per-tick hashes, run on 1/2/4/8 workers
  in the Linux job; the compare tool extended for `EAWRGCR`; the batch regeneration recipe; the
  "GC pins stale" rule in the worker spec.
- **Depends.** FT-04, FT-02.
- **Tests.** The corpus CTest; a deliberately perturbed rule makes it fail with the first tick.

## Risks

| Risk | Impact | Mitigation and early experiment |
|---|---|---|
| Tactical state restore is incomplete (collection-tree history, sliced searches, fog rows are state, not caches) | save during battle, M6 resync | The `child-log` section needs no tactical serializer; FT-07's first experiment saves M2 replays at three ticks and continues, which shows any missing field as a hash divergence. Full state (legacy EAWR-681) is not on a slice's critical path. |
| Speed settings and pause semantics (WGM-U02, WGEP U-01) | travel times, fiscal timing | Speed is state and pacing only; the wave-4 captures (legacy EAWR-1963) settle it early; only the reader changes. |
| Galactic AI and story Lua cost with all data-defined AI factions | hitches at fast speed | Deterministic work budgets per service; FT-14 runs the AI evaluator over a synthetic largest galaxy before the AI plans land; the soft-float budget from multiplayer readiness is the gate. |
| The AI engine refactor collides with the M2 AI tail | merge pain, regressions | FT-16 waits for the tail and is interface-only with identical tactical hashes; galactic AI code lives in new files. |
| Persistent identity across the battle boundary changes tactical setup rules | M2 pins move | The sparse-ID setup is an opt-in tactical header extension; M2 replays keep their bytes, checked by the replay corpus. |
| Wall-clock gates in FoC (speech end, 15 s vote timeout) | desync in multiplayer | They become commands injected by the session service; the simulation reads no clock (boundary check). |
| Story graph order and flag lifecycle remain unverified (WST-U01/U04) | S5 slips | Core loads stock GC plots early; callback/reward/recursive order has causal replay tests; the sweep settles U01/U04; the land-led full story campaign remains M4. |
| Unverified walk items (dated counts in the walk results) | fidelity, rework | Least-visible choices recorded as unverified; the wave-4 work (legacy EAWR-1963) settles them in parallel; the tick order is designed to be adjusted, not rebuilt. |
| Checkpoint size and hash cost at GC scale | multiplayer join, per-tick cost | Sectioned hashing from FT-01; capacity measurement (legacy EAWR-252) after S3; no truncation. |
| Shared-file contention at 10-15 workers | stalls | Per-track source lists, the registry shard, the reserved opcode block (shared files table). |
| Coop seats retrofitted late | rework of every command and UI | The envelope is seat-keyed from FT-03; FT-21 proves shared control in a space skirmish on the loopback before galactic plumbing; the UI rule "my seat, my faction" is in FT-12 from its first PR. |
| GC branch drifts from `main` | painful syncs, late M2 fixes missing in GC | Daily sync PR; M2-file refactors land on `main` first; GC edits to shared files limited to one-line includes. |
| Viewer mode switch memory and load time (galaxy scene resident while a tactical map loads) | owner-visible stalls | FT-13's first experiment loads the M2 map with the galaxy scene alive and measures memory and load time on the rig. |
| Measured rates do not transfer to greenfield GC | calendar | Re-estimate after the first ten M3 PRs; the slow scenario is the planning floor. |

## Ticket map

| FT | Issue |
|---|---|
| FT-01 | (legacy EAWR-1976) |
| FT-02 | (legacy EAWR-1979) |
| FT-03 | (legacy EAWR-1978) |
| FT-04 | (legacy EAWR-1980) |
| FT-05 | (legacy EAWR-1981) |
| FT-06, FT-06b | (legacy EAWR-1982) (two PRs) |
| FT-07 | the existing save-and-load ticket, first PR (legacy EAWR-681); no new ticket |
| FT-07b | (legacy EAWR-1983) |
| FT-08 | (legacy EAWR-1986) |
| FT-09 | (legacy EAWR-1989) |
| FT-10 | (legacy EAWR-1991) |
| FT-11 | (legacy EAWR-1993) |
| FT-12, FT-12b | (legacy EAWR-1994) (two PRs) |
| FT-13 | (legacy EAWR-1995) |
| FT-14 | (legacy EAWR-1996) |
| FT-15 | (legacy EAWR-1997) |
| FT-16 | (legacy EAWR-1998) |
| FT-17 | (legacy EAWR-1999) |
| FT-18 | (legacy EAWR-2001) |
| FT-19 | (legacy EAWR-2016) |
| FT-20 | (legacy EAWR-2017) |
| FT-21 | (legacy EAWR-2018) |
