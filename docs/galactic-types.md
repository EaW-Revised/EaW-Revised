# Galactic type data

`resolve_initial_galactic_type` applies the [WGS-92](behaviour/walks/gc-session-save.md)
shared constructor defaults after resolved inheritance: zero political control,
squadron capacity and hyperspace speed, with false hyperspace and orbit-spawn flags.
Invalid authored consumed numeric values refuse. Authored `Hyperspace`,
`Always_Spawn_In_Orbit` and `Create_Additional_Object` are retained by the
validated type table; the additional link is reference-checked. Planet political
faction and residual-influence authoring remain in GalacticData. Movement geometry,
access and active ability state still require their owning runtime services.

The resolver's technology target bound is the same nonnegative signed-32 bound
as the validated galactic production model; larger authored values refuse.

The starting-force allocator retains a squadron's logical original template as
the persistent fleet member type. Its ordered commissioned craft are separate
movement contents, so travel reads the live craft while packing and build cost
retain the template. A company's carrier keeps its actual transport type and
records the exact dummy company original type in census metadata. The census
binding admits that override only for the authored carrier with authored payload;
an arbitrary transport or child type refuses. Both representations preserve
existing movement, production and census checkpoint/replay sections.

The galactic type table (`include/eawr/units/galactic_types.hpp`) reads GC values from active game-object definitions with relevant fields, excluding planets (owned by GalacticData). It uses the catalog variant chain and AU-80..83 retail value rules. Loading has no tactical table, model or simulation side effects.

Movement and carrier packing use hyperspace speed and squadron holds (WGM-24/26/87); political control uses original-type values and faction (WGM-67/69/70); population uses the original type (WGEP-49). Bribe prices are movement admission inputs (WGM-85). GC production reads cost, duration, locks, prerequisites, base progression, population capacity and limits (WGEP-18..35/48..52). Each remains authored data, including fractional population values. The consumer applies the rule; parsing does not mark a registry row applied.

Absent and malformed numeric values stay absent. Diagnostics name the file, line, type, tag, value and reason. Empty/TBD values preserve inherited fields; single values use the most-derived non-placeholder occurrence. Squadron and company membership append ordinary occurrences; the first nonempty variant occurrence clears inherited members, then subsequent occurrences append. Starting spawn rows append across layers. Ordinary ability blocks append, while each authored variant block replaces prior abilities (WAR-84). The authored company tag is `Company_Units`. The special-structure prerequisite expression remains raw text so a later production consumer can interpret its AND/OR groups without losing them. Other list merge rules remain unverified and use the most-derived single occurrence.

Auto-resolve reads combat power, category masks, direct unique space/ground references, bombing types, hero flags, participation and strength ability data (WAR-15..26, WAR-76..84). Company power is the sum of its member powers and its mask their union; squadron membership then performs the same replacement (WAR-82). These effective values replace authored parent power/mask. Starting spawn rows retain occurrence order and duplicates at tech levels 0..5. The consumer selects the latest nonempty row at or below the owner's tech, and uses the first matching type's count (WAR-77). Explicit `None` and unresolved unique links resolve to null, with diagnostics for unresolved names (WAR-83). Missing power and ability percentages have verified zero defaults, missing categories no bits, missing named-hero/killer flags false and missing space participation true (WAR-76/78/80/83).

The pure functions in `include/eawr/campaign/auto_resolve_strength.hpp` take an FT-08 conflict and an explicit roster. Space fleets use the GC snapshot's ordered `Fleet.members`. Landed objects, ground specials, persistent built structures, transport contents, own-fleet starships and runtime layer/ability predicates remain explicit integration inputs. The roster supplies owner tech, and the caller supplies contrast order plus validated category enum values. Storage order does not determine gathering order. Per-combatant strength work uses the partition executor; totals, weakest ties and last qualifying heroes commit in gather order (ADR-009).

Prepare/cleanup retain the eight-entry history ring and initiate publishes both sides' strength. The conflict integration consumer owns score/bounty resets and the decision to clean up and return to arbitration when initiation fails (WAR-18). Round timing, attacking, winner/loss allocation, live `AutoResolveModel` binding, and the mid-tactical gatherer and team scaling remain with their respective rule owners. This library does not advance a round or modify the galaxy.

The length-framed little-endian `eawr-galactic-types-v5` identity encodes type/class names and every field in table order. Diagnostics and file provenance are excluded. Version 2 adds authored initial stealth; version 3 adds the transport-stay flag consumed by ground capability; version 4 retains the numeric technology target instead of a boolean; version 5 adds both derived current-type member flags, including availability. Empty stealth contributes no extension bytes, and omission defaults apply at consumers rather than entering the authored digest. Each version boundary intentionally prevents older GC content identities from loading. No compatibility reader is supplied before 1.0. Checkpoint and replay wire formats are unchanged. The existing load diagnostic reports a content-identity mismatch because the domain tag is hashed, not serialized separately. This identity is independent of the M2 UnitTables identity: tactical tables and their replay pins are unchanged.

`galactic_type_contracts` exercises synthetic carriers, squadrons, companies, variants and a base. With `EAWR_EAW_GAME_ROOT` it prints the installed-game type count, class census and identity, and checks the walk's stock transport speed. `galactic_auto_resolve_strength` covers ordered space/land gathering, bombing, type selection, category gating, garrison selection, bonuses, history lifecycle, errors and 1/2/4/8-worker equality; its `_stock` case uses the installed stock XML through the same validated loaders. Planet data, difficulty adjustments, production expressions and faction controller implementations remain with their owning GC subsystems.


The loader records `ground_company_member` and `squadron_member` under
[WGA-363/364](behaviour/walks/gc-ai.md#live-fleet-stealth-and-freestore-input-readiness).
Fresh bits are false. A variant copies its loading-complete base's current bits
and rosters, then applies its authored roster overrides. After every parse pass,
all current rosters mark referenced types true without resetting other bits.
Later passes replay a file for each incomplete type in it, including complete
variants in that file. At most ten passes run. An earlier variant can remain
false after its base is marked, while a later copy inherits a prior-pass mark.
The authored ground roster tag is `Company_Units`; the member bits refer to the
current type, independently of behavior names, runtime parents and original-type
resolution. Work runs only during catalog loading or a perturbation overlay.

`Catalog::member_flags` returns the completed pair. Duplicate/empty game-object
identities, an unresolved roster reference or an incomplete ten-pass load refuse
with `EAWR-XML-0013` and the named `member-parse-timeline` seam. Such a catalog's
galactic fields remain absent, so consumers cannot turn a failed parse timeline
into ordinary false bits. Bare referenced member types receive galactic rows.
Perturbation overlays replay the timeline independently of the shared catalog.

The stock contract compares both flags for all 3,345 stock game-object types
against an independent XML audit fingerprint: 186 ground-company members and
40 squadron members. It includes early-copy variant chains that stay unmarked
when their final bases are marked. Synthetic contracts cover later-pass copies,
complete sibling replays, repeated fix-ups, unresolved members and the pass cap.
The content identity contracts also cover sensitivity to both flags and their
availability, along with deterministic repeated loads.
