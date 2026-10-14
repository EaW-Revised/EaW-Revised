# Galactic planet graph and path queries

Part a of the galaxy model (legacy EAWR-680).
The source rules are [the movement/control walk](walks/gc-movement-control.md),
WGM-16..22, WGM-86 and WGM-91; campaign membership is WGS-01 in
[the session walk](walks/gc-session-save.md). These are debug-build observations.

`build_campaign_galaxy` consumes validated campaign planets and trade routes, their positions,
endpoints and positive speed factors, and `MaximumFleetMovementDistance`. It allocates persistent
IDs in authored planet order followed by authored route order. It combines the validated galactic
data and original-type table identities (ADR-017). Initial ownership is supplied by the caller;
home locations, faction controllers and starting forces belong to campaign setup (legacy EAWR-1149).
Nonplanet galaxy art is already excluded by the validated loader. No stock table is copied.

WGM-16 connects every unordered planet pair in both directions when its center distance is at
most the authored movement limit or the pair has a named route. WGM-17 uses checked Q24 Euclidean
distance, divided by the route's `HS_Speed_Factor` on named routes. This consumer does not use
`TradeRouteMovementFactor`, influence range, political/credit route gains or corruption travel
speed: those belong to locomotion, political control and economy. Route line style and planet
models remain presentation inputs. Missing positions/factors/constants and duplicate route pairs
fail with a diagnostic; their retail fallback/duplicate selection is unverified.

The immutable graph is derived once at session creation, outside the tick. Pair enumeration is
serial at this load boundary because each pair appends to two neighbor tables; it adds no serial
entity work to simulation service. Graph work counts are exactly N(N-1)/2 pairs and twice the
accepted pairs. Limits are 4096 planets and one million directed edges, enforced before exceeding
the output budget. The stock-campaign test exercises every loaded campaign with planets.

Path queries consume a complete copied observer table. WGM-18 applies single-player human reveal
and optional AI endpoint usability before WGM-20's final-endpoint exception. Other hops check the
story-map restriction, WGM-19 neutral/hostile passability (invisible human owners classify neutral)
and WGM-86 defense: starbase allegiance first, otherwise visible nonallied fleets for humans or
AI enemy-space-force perception, conservatively defended on perception failure. WGM-21 adds the
code constant 100 only when a corruption-benefiting faction needs the corrupted planet bypass.
Actual allies and corruption pass allegiance before owner visibility and AI classification.
Failed AI classification accepts allegiance; a successful evaluation with no classified player
uses neutral passability. Current-node expansion checks also apply to the source, without the
destination reveal/story-map gates; source corruption permits expansion without charging a hop.
The AI owner supplies its classification, usability and perception; this service does not invent
the AI evaluator, diplomacy, visibility or story state. The path service returns an empty path for
unreachable destinations and a one-planet path for identical valid endpoints (WGM-22).

The remake uses Dijkstra priority `(accumulated Q24 cost, ascending planet ID)` with ascending-ID
neighbors. Only strictly cheaper paths replace predecessors; stale heap entries are discarded.
This avoids a floating estimate and remains bounded by graph adjacency work for a static query.
The largest route factor is retained as an optional for later AI estimate consumers (empty without named routes); it is not a second travel
speed formula. WGM-17's original estimate is direct distance divided by the largest factor.
This internal search choice preserves shortest costs, but equal-cost path parity is **unverified**:
WGM-91 establishes the original strict total-cost heap behavior and planet enumeration, while its
equivalence to remake ID order remains open. Ascending-ID ordering is the current plan's explicit
deterministic policy, not a claim of original heap/FIFO parity. Tests pin expansion/edge counts
and symmetric ties; fleet packing and retreat selection remain later lanes.

Graph-enabled setups opt into replay header extension tag 1 and state section 5. Legacy synthetic
setups omit both and retain their original replay/snapshot/state hashes. Planets and routes remain
the existing snapshot rows; graph-enabled snapshots also carry the authored factors and distance.
The derived adjacency is not independently serialized: the graph section hashes its complete
additional inputs, while positions/endpoints are already hashed in sections 2/3. Save/load carries
the rules in setup/replay and verifies the graph section in the planets checkpoint section;
the unreleased checkpoint sections retain version 1 by coordinator allocation.
