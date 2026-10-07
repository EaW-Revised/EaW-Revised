# Walk 12: space build pads and pad structures

## Scope and evidence

This walk covers multiplayer space pads, their construction children, completed structures, mining income, merchant docks, rebuilding and selling. Concrete maps are the stock two-player Bespin, Endor, Geonosis, Kessel and Polus. Campaign menus are identified as a separate data branch, not substituted for multiplayer menus. Missile pads and the capturable construction-pod variant are included although these five maps do not place them.

Sources are **data** (the effective FoC XML selected by `GameObjectFiles.xml`, faction, difficulty, goal, perception and Lua files) and the **debug build**, cited by opaque EBP evidence IDs. The targeted economy follow-up adds opaque EEC IDs for additional read-only debug-build reads. The private evidence index and raw reads remain outside committed files. No retail run was made for either pass. An authored value establishes the data interface; it does not establish an untraced ability consumer. Explicit unknowns are in [Unverified](#unverified).

The capture algorithm and map census are interfaces to [hazards](hazards.md), WHZ-40..47 and WHZ-50..52. Queue purchasing, population and income arithmetic are interfaces to [production](production.md), WPR-10..14, WPR-20..33 and WPR-41/50..52. Picking, fog and shared selection belong to [sensors and UI](sensors-ui.md), WSU-10/16. Combat, targeting, shield regeneration and ability effects belong to their respective walks. AI scheduling belongs to [tactical AI](tactical-ai.md), WTA-44/45. This walk establishes the pad-side calls and inputs, not those subsystems' internals.

“Existing” compares those walks and [space purchasing](../space-purchasing.md), [skirmish start](../../skirmish-start.md), [minimap](../foc-minimap.md), and [battle flow](battle-flow.md). “Ours” compares this branch's code at the start of the walk. A **same** verdict covers the stated interface only; it does not imply that a pad is already implemented. Every missing or differing rule links to a gap below.

## Entry points and ordering

Use WBF-20..26 and WPR-15 for economy nesting: frame bookkeeping precedes object services; due object services and deferred creations/deletions precede collision/setup/retreat and victory-countdown work; the income-stream service follows that countdown; outer player work visits players in ascending ID, with AI before their units and upgrades queues; global Lua follows. Production's U-1 is now settled. WBP-43 establishes first UC eligibility and the delayed-damage branch; command delivery and the exact first traversal of a newly inserted UC remain U-BP-1 and the frame-order walk's boundary.

Capture has its own four-logical-frame service (WHZ-41), not a rendered-frame timer. A construction child services once per logical frame. The pad's nominal service interval is one frame, but its own “requires service” result is false: its service checks link exclusivity, rather than advancing construction. Completion is called by the child. Selling and building enter through scheduled commands; AI can invoke the build request directly. Income pays continuously through the income manager. Income-modifier objects periodically discover eligible streams every round(5 × logical FPS) frames, 150 at 30 FPS. The command bar and minimap refresh in their presentation loops. Sources: debug build EBP-04/05/08/10/11/21/22/38/39; WBF-20..26; WHZ-41; MM-04.

## Stock objects and menus

Placement counts below reproduce WHZ-50; they are empty objects, not prebuilt mines or satellites.

| Two-player map | Laser pads | Mining pads | Merchant docks |
|---|---:|---:|---:|
| Bespin | 4 | 5 | 2 |
| Endor | 4 | 5 | 0 |
| Geonosis | 0 | 5 | 1 |
| Kessel | 5 | 4 | 1 |
| Polus | 11 | 5 | 0 |

| Type | Hull / shields | Capture radius / seconds | Respawn seconds | Reveal / scale | Other authored inputs |
|---|---|---|---:|---|---|
| `Defense_Satellite_Laser_Pad` | 1200 / none | 350 / 8 | 45 | 200 / 1 | Hides when built on; visible to enemies when empty; destroy when child dies |
| `Mineral_Extractor_Pad` | 1700 / none | 550 / 10 | 38 | 275 / 0.75 | Same three flags; mining-facility radar icon |
| `Skirmish_Merchant_Dock` | 600 / 500 | 800 / 15 | 80 | 850 / 1.75 | Ownership sticks; community property; ordinary shield/power/production interfaces |
| `Defense_Satellite_Missile_Pad` | inherited laser-pad inputs | inherited | inherited | inherited | Multiplayer menu has Empire/Rebel missile only; no Underworld group |
| `N_Orbital_Construction_Pod` | 10000 / none | 800 / 5 | no positive value authored | 850 / 1.75 | Ownership sticks; community property; merchant menu; does not use pad construction |

Laser/missile/mining pads and the capturable pod author `Influences_Capture_Point=No`; the merchant has no authored override, so its flag uses the true default. Candidate admission still belongs to WHZ-41. The ordinary `Orbital_Construction_Pod` has neither this variant's capture behavior nor the pad-construction behavior; its model/name does not make it a build pad. Merchant shields refresh at 10, energy is 5000 with refresh 1000, and reinforcement prevention radius is 800. These are ordinary combat/reinforcement inputs. Laser/mining pads and the capturable pod reject living projectiles; the merchant accepts them. No merchant income, repair or purchase discount is authored. Sources: data; WHZ-50..52.

Menu selection reads `Tactical_Buildable_Objects_Multiplayer` for a multiplayer battle (also the map editor) and `Tactical_Buildable_Objects_Campaign` otherwise, choosing the requesting player's faction group. Each pad entry names an **under-construction** type; `Tactical_Buildable_Constructed` names its completed result. Merchant entries instead name ordinary produced units.

| Pad / faction | Under-construction type | Base credits | XML build seconds | Completed hull / shields |
|---|---|---:|---:|---|
| Laser / Empire | `UC_Empire_Defense_Satellite_Laser` | 875 | 25 | 1200 / 500 |
| Laser / Empire | `UC_Empire_Defense_Satellite_Missile` | 700 | 28 | 1000 / 500 |
| Laser / Empire | `UC_Empire_Defense_Satellite_Repair` | 750 | 28 | 1100 / 700 |
| Laser / Rebel | `UC_Rebel_Defense_Satellite_Laser` | 875 | 25 | 1200 / 500 |
| Laser / Rebel | `UC_Rebel_Defense_Satellite_Missile` | 700 | 28 | 1000 / 500 |
| Laser / Rebel | `UC_Rebel_Defense_Satellite_Repair` | 750 | 28 | 1100 / 700 |
| Laser / Underworld | `UC_Underworld_Defense_Satellite_Plasma` | 1000 | 20 | 1200 / 500 |
| Laser / Underworld | `UC_Underworld_Defense_Satellite_DBM` | 1200 | 20 | 1000 / 600 |
| Laser / Underworld | `UC_Underworld_Defense_Satellite_Sensor` | 875 | 18 | 1000 / 800 |
| Mining / Empire | `UC_Empire_Mineral_Extractor` | 850 | 35 | 1700 / none |
| Mining / Rebel | `UC_Rebel_Mineral_Extractor` | 850 | 35 | 1700 / none |
| Mining / Underworld | `UC_Underworld_Mineral_Extractor` | 1000 | 30 | 1700 / none |

Construction hull is 1200 for these satellite children except Underworld DBM (1000); extractor children have 1700. Preserve **health fraction**, not absolute hull, on replacement. `UC_Empire_Defense_Satellite_Tractor` also exists (800 credits, 28 seconds; result 1400/800) and appears in the AI plan, but is absent from the stock laser-pad multiplayer menu. AI allocation also filters against the faction menu (WBP-48); the plan token does not create a stock tractor button or compatible pad reservation. Sources: data; EBP-24/33; debug build EEC-11.

## Rules in evaluation order

### Admission, capture and opening a build menu

| ID | Rule, branches, constants and XML reads | Source | Existing | Ours / gap |
|---|---|---|---|---|
| WBP-01 | Admit map placements and variants through the effective object registry, retaining type, owner, position and facing. Read `Variant_Of_Existing_Type`, `Behavior`/`SpaceBehavior`, `Affiliation`, `Scale_Factor`, `Custom_Soft_Footprint_Radius`, `Space_Obstacle_Offset`, `Space_Layer`; apply the stock census above. The merchant/pod producer branch is distinct from pad construction. | data; WHZ-50 | same WHZ-50; differs from SK-32/PU-G3's inert-pad limit | **differs**, `start.cpp::add_map_objects` and `unit_tables_decode.cpp::load_obstacle` retain obstacles but omit live pad profiles; G1/G7 |
| WBP-02 | Read capture radius/transition, `Influences_Capture_Point`, `Ownership_Sticks`, `Is_Community_Property` and the profiles above. The default influence flag is **true**, so absence of the tag does not exclude fighters/squadrons or require a corvette. Ownership-sticks defaults **false**; stock empty laser/mining pads can neutralize after friendly presence leaves. Capture influence is false on the pads and their UC/completed types. | data; debug build EBP-43/44/46 | same WHZ-41; default influence was missing there | **missing**, scene placement only supplies a presentation capture-point flag; G1 |
| WBP-03 | Schedule capture at the WHZ four-frame cadence, UC every logical frame, and respawn through deferred object creation; do not replace those with the player's queue cadence. Income-manager and queue ordering is the WBF interface described above. Pad service itself is not a timer. | EBP-05/08/10/21/22; WBF-20..26 | same WBF; differs from WPR's unresolved older global-order note | **differs**, `TacticalSession::step` has no pad services; preserve its existing queue work and add the object-side stages; G2/G6, battle frame-order dependency review (legacy EAWR-949) |
| WBP-04 | Interface to WHZ-40..47: query raw, unscaled `Capture_Point_Radius`, require 3D distance² **≤ radius²**, not self, alive/not deleting or in limbo, capture influence, owning faction and matching point `Affiliation`. Neutral chooses first eligible query owner; allies retain it, a nonally contests to neutral and ends the scan. Owned plus hostile also targets neutral; no majority or direct enemy-to-enemy transfer. With no candidates, sticky/reinforcement-point interface retains owner, otherwise neutral. Same wanted/current owner drains progress by 4/(transition seconds × FPS) to 0. Different owner advances by 4/(`Capture_Point_Transition_Time_Seconds` × modifier × FPS) to 1; changing target halfway does not reset progress. Modifier = 1 plus applicable positive capture-time adjustments (`Abilities/Battlefield_Modifier_Ability/Capture_Point_Time_Multiplier`), nonpositive falls back to 1; zero modified duration advances immediately. At 1 notify control/story interfaces, change owner, reset progress; no second claim that service. Multiplayer has **no opening delay**; single-player space interface uses `Space_Capture_Allowed_Countdown_Seconds` 30, neutral bypasses. Stock radius/time/sticky values are above; zero-duration decay remains WHZ U-04. | data; WHZ-40..47 | same WHZ; missing in PU/SK | **missing**, no simulation capture state/service; G1 |
| WBP-05 | While either UC or completed child exists, the pad-side capture interface keeps its current owner. The child belongs to the **building player**, who may be an ally of the pad owner; do not merge these identities. Clearing a child reopens the ordinary capture rules, subject to the distinct death/sale paths below. | EBP-04/03/06; WHZ-41 | same WHZ-41; child owner detail missing there | **missing**, no parent/child ownership state; G2 |
| WBP-06 | Choose the faction and multiplayer/campaign list as above. `Tactical_Buildable_Objects_Multiplayer`, `Tactical_Buildable_Objects_Campaign`, `Tactical_Buildable_Constructed` define offered UC types and replacement types; laser offers three entries/faction, mining one, missile only E/R. Do not treat all declared UC types as offered. | data; EBP-24/33 | missing there; PU-G3 recorded inert pads | **missing**, station menus in `economy_rules` are not pad menus; G2 |
| WBP-07 | “New construction allowed here” requires a live, empty pad. Query the collidable spatial tree over XY bounds enlarged by `Capture_Point_Radius`; ignore dead/deleting/death-clone objects and base-shield/dummy-ground-structure behavior. A nonallied, capture-influencing object blocks only when **3D squared distance < radius²**. No spatial tree means allowed. This differs from capture's inclusive boundary and candidate filter. | EBP-02 | missing there | **missing**, no pad eligibility query; G1/G9 |
| WBP-08 | For a visible allied pad, the UI's action resolver offers build when WBP-07 succeeds and the selected faction menu is nonempty, before ordinary selection. AI open-pad enumeration also uses WBP-07 and cooldown. A request from an already-open menu and its later execution do **not** repeat that proximity helper; a hostile entrant between menu opening and execution is therefore not a proven rejection. | EBP-20/47 | missing there; WSU covers selection only | **missing**, `production.cpp::layout_build_buttons` only models producer queues; G8/G9; U-BP-3 |

### Commands and creating the construction child

| ID | Rule, branches, constants and XML reads | Source | Existing | Ours / gap |
|---|---|---|---|---|
| WBP-09 | A build request rejects an existing UC/completed child, pad fogged for the builder, nonallied ownership, cooldown not due, type without UC behavior, or missing player. Human requests also check affordable effective price and queue a pad-build event; rejection supplies local negative feedback. AI requests call execution directly with the local-debit/start-feedback flags false; its reservation debit and failure refund are WBP-47/48. | EBP-01/11; debug build EEC-20/21/24 | missing there | **missing**, no pad build command; G2 |
| WBP-10 | Human event execution enables debit/feedback and rechecks empty links, cooldown, valid player, allied owner and affordability. It deducts the whole effective price once **before** creating UC. It does not repeat request fog or WBP-07 proximity tests. The inspected failure-after-debit path has no refund. Do not promise a station-queue transaction or cancellation refund for this path. | EBP-04/11 | missing there | **missing**, structure enum is only a future completion ledger; G2; U-BP-1/9 |
| WBP-11 | Read UC `Tactical_Build_Cost_Multiplayer` in multiplayer, without corruption adjustment. Outside multiplayer use positive `Tactical_Build_Cost_Campaign`, otherwise multiplayer cost; at a corrupted conflict location apply the player faction's `Corruption_Tactical_Build_Pad_Price_Multipliers` value and truncate to integer. Stock multiplayer prices are the menu table. Generic sale cost uses the same campaign fallback but no corruption adjustment (WBP-31). Campaign corruption is outside this walk's runtime scope, not a speculative multiplayer cost bonus (U-BP-10). | data; EBP-12/35 | missing there; WPR owns producer cost rules | **missing**, no UC/effective pad price profile; G2 |
| WBP-12 | Pad execution checks occupancy rather than the station's queue, population, tech, build-limit or production-prerequisite gates. No such checks appear in the inspected pad request/execution. Stock UC entries author no extra limits/prerequisites; do not import station `Tech_Level` gates or consume reinforcement population for this creation. Menu availability remains WBP-06. | data; EBP-01/04 | differs from applying PU-G23/ordinary queues to pads | **missing**, `BuildKind::structure` has no live pad path; G2 |
| WBP-13 | Create UC at the pad's `Tactical_Build_Attachment_Bone_Name` transform when available, otherwise pad transform, with the pad's facing and building player's ownership. Link UC to its parent and install detach notification; clear pad selection, refresh its local visibility, start pad build animation, and clear cooldown start/end. | EBP-04/15/16 | missing there | **missing**, no attachment-backed construction child; G2 |
| WBP-14 | UC starts at **0.01 absolute hull**, not full health and not zero; retain its type's `Tactical_Health` maximum. Set the parent link, positive healing increment and a fixed completion deadline. Occupancy starts immediately, not on queue completion. | EBP-04/09/16 | missing there | **missing**, generic production completion emits no damageable UC; G2 |
| WBP-15 | Read integer `Tactical_Build_Time_Seconds`. For AI builders multiply by difficulty `Space_Build_Time_Multiplier` (Easy 1.5, Normal 1, Hard 0.9), then **truncate to unsigned whole seconds**; positive duration is required. Finish frame = current logical frame + seconds × logical FPS. Per-frame hull increment = max hull ÷ seconds ÷ FPS. This initializer does not read the queue's global `Tactical_Build_Time_Multiplier` or station build-time bonuses. | data; EBP-09 | missing there; WPR queue time is a different path | **missing**, `economy_rules` multiplies queue time globally; needs separate UC time reader; G2 |

### Object services, completion and economy

| ID | Rule, branches, constants and XML reads | Source | Existing | Ours / gap |
|---|---|---|---|---|
| WBP-16 | Each UC service adds its fixed increment, clamps to max hull, updates visual damage, then tests current frame ≥ fixed finish frame and notifies parent. Setup forces full hull but does not move the deadline. Damage lowers hull and can change visuals, but does **not** delay completion; destruction before the notification takes the death path. Due delayed damage on the UC precedes its completion service (WBP-43); a live projectile race remains U-BP-1. | EBP-08/10; debug build EEC-34/40 | missing there | **missing**, no UC tick/deadline; G2 |
| WBP-17 | If construction animation exists with nonzero length, rate = animation duration ÷ build seconds, otherwise 1. With A model damage alternates, visual alternate = clamp(integer((A+1) × (1−health fraction) + 0.5), 0, A); initial update also hides meshes flagged to stay hidden on alternate decrease. Damage may regress the visible alternate independently of elapsed time. A stored vertical increment is not proof of a `Tactical_Build_Start_Lower_Z` consumer (U-BP-7). | EBP-09/19 | missing there; unit animation notes cover generic clips | **missing**, no UC visual state; G8 |
| WBP-18 | On completion validate the UC link and its `Tactical_Buildable_Constructed` target. Save building player and UC health fraction, clear building-player state and detach listener, and create final type at attachment position **plus 0.1 code units Z**, with pad facing. Creation failure destroys UC and clears its link; no refund is in this path. | EBP-03/15 | missing there | **missing**, no UC replacement transaction; G2 |
| WBP-19 | Link completed child and its detach listener; register shield interface if the result needs it, set sale parent when it has tactical-sale behavior, set container/garrison transfer and the `Tactically_Built_Child_Object_Persists` interface (default true). Send pad completion notification **before** copying UC health fraction to the final type; then send UC replacement notification and remove UC, clearing its pointer. If final `Previous_Upgrade_Level_Type` exists, delete allied previous-type objects; stock satellites do not author it. No reinforcement-pool step occurs. Campaign persistence is an interface, not a multiplayer claim. | data; EBP-03 | missing there; WPR upgrade replacement is a separate entry | **missing**, no parent/link replacement or signal order; G2 |
| WBP-20 | Human local start/completion uses `SFXEvent_Tactical_Build_Started`/`SFXEvent_Tactical_Build_Complete`, falling back to `SFXEvent_Build_Started`/`SFXEvent_Build_Complete`; faction spatial start/loop/complete sounds are separate. Completion stops the construction loop and emits the construction story interface. The AI false flags suppress the local start path. | EBP-03/04 | missing there | **missing**, no pad feedback lifecycle; G8 |
| WBP-21 | Completed satellites hand ordinary targeting/weapons/turrets/power/shields their authored profiles; no bespoke pad-damage formula is in the construction code. Use hull/shields above. Repair satellite data supplies `Force_Healing_Ability` with `Space_Automatic`, range 550, heal percent 0.10, interval 2 seconds, single target, categories Fighter/Transport/Bomber/Corvette/Frigate/Capital/Hero. Underworld sensor supplies `Sensor_Jamming_Ability`, `Combat_Automatic`, duration 60 and blob appearance. Tractor supplies user input, range 5..1200, speed-decrease 0.95 and Corvette/Frigate/Capital plus named Falcon/Heart inclusion, Buzz-Droids exclusion. These ability consumer semantics are U-BP-8, not established effects. | data; EBP-03; combat/ability interfaces | same combat interface; these pad ability inputs missing in M2 ability scope | **same** ordinary combat interface: UC results load through `load_body`, `combat_table`, `durability_table` and `new_unit`; ability consumers remain G11 |
| WBP-22 | Only the completed extractor is an income source, not the empty pad or UC. `Income_Stream_Ability/Base_Income_Value` is 200 E/R, 205 Underworld; `Base_Interval_In_Secs` is 10. Unmodified rates are therefore 20, 20, 20.5 credits/sec. Ordinary completed-mine creation activates its income ability during initialization, so it pays in that frame's later income pass while setup is inactive; there is no ten-second start delay (WPR-16). Extractors have hull 1700 and no authored weapon/shield. | data; WPR-10..13/16; debug build EEC-04/10/31/50 | same arithmetic WPR; source creation missing there | **same**, completed-child admission precedes the partitioned `income-sources` phase in `TacticalSession::step` |
| WBP-23 | Hand the income manager source owner, allied recipients and flags: `Split_Income_With_Allies=Yes`, `Split_Favors_Owner=No`, `Full_Amount_To_Everyone=Yes`, no authored owner percentage. Each allied recipient receives the full stream, rather than dividing 200 between teammates. WPR pays each logical frame, applies percentage/additive/interval modifiers, and applies AI `Credit_Multiplier` (0.5/1/1.2) to positive credits. The 10-second interval is bookkeeping, not a lump-payment wait. | data; WPR-10..13 | same WPR-10..13 | **same** base allied-payment interface in `TacticalSession::step`; modifier/AI-credit gaps remain G4/skirmish tech and AI credit rules (legacy EAWR-727), source admission G3 |
| WBP-24 | A completed mine offers its faction's two income upgrades through the ordinary **upgrades queue**: E `ES_Increased_Supplies_L1/L2_Upgrade`, R `RS_Increased_Supplies_L1/L2_Upgrade`, U `UL_Extort_Cash_L1/L2_Upgrade`. E/R costs/times are 250/40 and 500/65, multiplier 1.20/1.40; U 400/20 and 700/30, multiplier 1.15/1.45. L1 has no prerequisite/previous type; L2 requires L1, names L1 as previous type and destroys it by default. Each level has all-allies current limit 1. Read `Tactical_Production_Queue`, `Tactical_Build_Prerequisites`, `Previous_Upgrade_Level_Type`, `Destroy_Previous_Upgrade_Level`, `Build_Limit_Current_For_All_Allies`; WPR owns queue gates/replacement. Do not multiply L1 and L2 blindly. | data; WPR-20..33/50..52 | same WPR upgrade interface; mine menu missing there | **implemented**, completed mine menus admit the existing upgrades queue with authored gates and previous-level replacement |
| WBP-25 | `Income_Stream_Mod_Ability` reads `Target_Stream_Source`, `Affects_All_Allied_Sources`, `Stacking_Category`, `Income_Multiplier`, `Income_Additive_Value`, `Interval_Multiplier`, `Reverse_Application_Logic`. Allied source-type match is required; false allied flag restricts exact owner. Register source/category once. Add multiplier−1 to percentage mods, additive value to absolute mods, interval multiplier−1 to interval mods; skip neutral contributions. Normal application scans immediately and periodically at **5 seconds**; reverse removes instead, with service interval 0. Removal deletes only this source/category's three contributions. Stock mine upgrades use category 1, allied=true, additive=0, interval=1, reverse=false. Category reduction, scan phase and contribution lifetime are WBP-44..46. | data; EBP-29/38/39/41/42; debug build EEC-01/02/03/07/08/13/29..31 | same WPR-11 formula; registration cadence missing there | **implemented**, loaded space-target modifiers attach through the partitioned discovery service and reduce independent category lists |
| WBP-26 | Remove a mine's stream when its source ceases to exist; recipient identity is live source ownership/alliance, not a stale pad placement owner. Ability termination removes its stream and its hosted modifier contributions (WBP-46). A surviving mine is not recaptured through the occupied-pad capture path. Destruction completed before the income pass gives no subsequent payment in that pass; direct parent-pad destruction is a separate unresolved route (U-BP-5). | WPR-14/15; EBP-29/42; debug build EEC-03/16/35/40/47; WHZ-41 | same WPR teardown; pad identity missing there | **implemented**, dead streams prune before payment and removed modifier objects withdraw only their own contributions; mine-host deletion remains U-BP-6 |

### Child death, rebuilding and selling

| ID | Rule, branches, constants and XML reads | Source | Existing | Ours / gap |
|---|---|---|---|---|
| WBP-27 | Child-killed notification clears the matching UC or completed link. Completed-child death also clears the contents lock and removes the persistence interface when enabled. If `Destroy_When_Child_Dies=Yes`, destroy the pad: this is the stock laser/mining route. No construction refund is in this callback. Bare detachment only clears matching links; detaching the parent from a UC clears its parent reference and its later services cannot grow hull or complete. Detachment does not itself run death, refund, cooldown or sale cleanup (WBP-49). Direct pad destruction remains U-BP-5. | data; EBP-06/14/27; debug build EEC-39; EBP-08 | same WHZ-51 interface; callback detail missing there | **same**, `TacticalSession::step` clears matching child links and applies the authored destroy flag without refund; direct-parent cleanup remains U-BP-5 |
| WBP-28 | With destroy-child flag false, force the pad's capture state neutral, refresh visibility for all players, and play empty/sold animation. If `Minimum_Time_Before_Pad_Can_Build_Again` > 0, start a cooldown at now and end at now + round(seconds × FPS); otherwise no timer (default 0). Build is due when frame ≥ end. Progress clamps (now−start)/(end−start) to 0..1 and returns 1 for equal endpoints. Stock destroy-on-child-death pads normally take WBP-27 instead. | EBP-06/07/30 | missing there; WHZ notes surviving-pad interface | **partial**, surviving pads neutralize and start their authored rounded cooldown; `pad_cooldown_progress` supplies clamped progress; empty animation remains G8 |
| WBP-29 | WHZ-52 destruction schedules a **neutral**, same-type/position/facing replacement at death frame + round(`Tactical_Respawn_Time_In_Secs` × FPS), for positive respawn values. Laser 45 seconds, mine 38, merchant 80; no child or old owner is copied into the new object. Nonpositive means no authored respawn schedule. Due creation occurs at the first playing object-manager pass with frame ≥ deadline, after existing object services/camera alignment and before queued deletion; it does not wait an additional service interval. Rendered visibility and direct-pad-death child cleanup remain U-BP-5. | data; WHZ-52; debug build EEC-12/14 | same WHZ-52; due comparison now settled | **same**, ordered deferred creation in `TacticalSession::step` recreates neutral capture points at the authored deadline after object services; rendered visibility remains U-BP-5 |
| WBP-30 | A final object with tactical-sale behavior exposes sale only to its **exact local owner**, not allied players. Alt+Ctrl/guard override supplies the action. Single-step mode refuses the request; otherwise queue a sale event. Execution rejects a parent whose contents are locked. UC and mine types have no tactical-sale behavior; this is not a generic cancel button. | EBP-17/26/45/47; data | missing there | **same** exact-owner sale request through guard override and recorded opcode 15; contents locks and single-step refusal are explicit interfaces |
| WBP-31 | Find the UC entry in the parent type's current-mode menu for the **sold child's owning faction**, whose `Tactical_Buildable_Constructed` matches the sold type. Refund = round(UC **generic tactical cost** × final `Tactical_Sell_Percentage`, default **0.5**), not health-scaled and not the adjusted pad-debit reader. Missing parent/type/menu match returns 0; negative computed refund asserts and returns 0. Stock E/R laser/missile/repair refunds 438/350/375; U plasma/DBM/sensor 500/600/438. Owner credit addition uses WPR's AI-credit rule. Optional `SFXEvent_Tactical_Sold` and faction spatial sold sound follow. | data; EBP-17/25/35/40 | missing there; station cancel refunds differ | **same**, current-mode owning-faction UC menu matching and health-independent round-half refund in `execute_economy`; AI credit adjustment remains separate |
| WBP-32 | Sale notification clears completed link **before** destroying the sold child, removes persistence, refreshes all-player visibility and plays pad sold/empty animation. It does not invoke the killed-child route, destroy the pad, force neutral, or start the death cooldown. Unregister the shield interface then destroy sold child. A victory-relevant sold child uses ordinary tactical elimination evaluation after removal (WBF-31/35); selling the last relevant object can therefore decide defeat under the selected all-enemy-units condition. Later capture service may neutralize a now-empty nonsticky pad if its friendly ships left. | EBP-17/18; WHZ-41; debug build PSV-01..03 | missing there | **same**, clear link before child removal, no killed notification or respawn, retained owner and cooldown; ordinary capture can service the empty pad later |

### Merchant production and presentation interfaces

| ID | Rule, branches, constants and XML reads | Source | Existing | Ours / gap |
|---|---|---|---|---|
| WBP-33 | Captured merchant dock and neutral construction-pod variant are ordinary producers, not UC-building pads. They share faction menus and community-property selection but retain sticky capture ownership; buy permission remains the producer/alliance interface. Merchant has no authored income/discount/repair stream. Read `Tactical_Buildable_Objects_Multiplayer`, `Is_Community_Property`, `Ownership_Sticks` and production behavior. | data; WHZ-43; WPR-30/33; WSU-16 | same existing interfaces; concrete menu missing there | **missing**, merchant is only a map obstacle, not a live producer; G7 |
| WBP-34 | Merchant E/R menu uses `Empire_`/`Rebel_` variants of `Pirate_IPV` (1200/10s/pop2), `Pirate_Frigate` (2200/14s/pop3), `Pirate_Fighter_Squadron` (350/8s/pop1), and `Z95_Headhunter_Empire_Squadron`/`Z95_Headhunter_Rebel_Squadron` (200/8s/pop1). U uses `Underworld_Pirate_IPV` 1200/8s/pop2, `Jedi_Cruiser_U` 2100/20s/pop1, `Underworld_Pirate_Fighter_Squadron` 350/6s/pop1, `Z95_Headhunter_Underworld_Squadron` 250/5s/pop1. Pass `Tactical_Build_Cost_Multiplayer`, `Tactical_Build_Time_Seconds`, queue and population inputs to WPR's normal producer queue → pool → arrival path. Galactic `Tech_Level`/`Required_Star_Base_Level` data does not add a tactical gate to WPR-33. These are not pad-child population rules. | data; WPR-20..33/41 | same WPR producer/pool interface | **same** generic queue/pop interface in `economy_rules`/session; missing producer/menu admission is G7 |
| WBP-35 | Pad visibility behavior returns true for nonlocal-player queries and local map-editor queries; for the local player, empty pad is visible when `Visible_To_Enemies_When_Empty` is true, otherwise only when not enemy. Occupied pad returns inverse `Hides_When_Built_On`; missing state returns false. Ordinary fog/reveal (`Space_FOW_Reveal_Range`, initial/last-state visibility) is a separate visibility gate. WSU-10 picks UC while construction exists, then completed child; do not pick only the hidden pad. Community selection uses WSU-16, not ownership transfer. | data; EBP-13; WSU-10/16 | same WSU interfaces; pad flags missing there | **differs**, generic obstacle rendering/picking has no live child/hidden-pad substitution; G8 |
| WBP-36 | Opening a pad menu resolves at most **six** command-bar entry slots from the faction list and UC type; refresh checks effective price (or pay-as-you-go permission) and cooldown for enabled/disabled state; loss of pad behavior closes the menu. Enabled tint is white (255 channels), disabled is gray (128 RGB, 255 alpha); cooldown supplies its progress. Occupancy is rechecked by the request, not by this refresh. Button click dispatches pad request, deselects the pad and closes build mode rather than submitting a station queue purchase. Land-only unique support-structure filtering in menu activation is outside the stock space-type scope. Entries use UC `Icon_Name`, `GUI_Row`, `Text_ID`, `Encyclopedia_Text`, `MP_Encyclopedia_Text`; generic tooltip/layout belongs to sensors/UI. Exact retail arrangement and click feedback need U-BP-7/9. | data; EBP-23/32/33/34/36 | missing there | **missing**, production UI has only station queue snapshots/buttons; G8 |
| WBP-37 | Supply capture owner/progress/contest from WHZ-41/47 to the world UI: target-faction capture animation (old faction when neutralizing), target idle on finish, tint/radar/hologram progress with inverted neutralization fill, local allied begin/end radar events and friendly/enemy capture/loss sounds. UC progress/deadline and hull are separate values; damage can regress hull alternate without moving deadline. Existing bar rendering belongs to sensors/UI; exact stock pad overlay is U-BP-7. | WHZ-41/47; EBP-08/19; WSU UI interface | same capture-state interface; retail visual not established there | **missing**, no live capture/UC snapshot; G8 |
| WBP-38 | Feed `Has_Space_Evaluator`, `Is_Visible_On_Radar`, `Is_Visible_On_Enemy_Radar`, `Radar_Icon_Name`/size/facing/rotation and live owner/faction into MM-06/07's renderer; mining uses `mini_map_mining_facilities.tga`, merchant `mini_map_structure.tga`. Fog/exclusion is MM-12, minimap rebuilding is rendered-frame MM-04. Authoring capture behavior alone does not override radar flags. | data; MM-04/06/07/12 | same MM renderer interface | **differs**, icons can render but map-object owner is static and no capture transition updates it; G8/G1 |

### AI and Lua interfaces

| ID | Rule, branches, constants and XML reads | Source | Existing | Ours / gap |
|---|---|---|---|---|
| WBP-39 | XML goals `Secure_Build_Pad_Space` (Enemy_Build_Pad, Map_Control, limit30, tracking20), `Build_Structure_Space` (Friendly_Build_Pad, Med_Priority, limit0, tracking60), `Build_Refinery_Space` (same, High_Priority) use Space/Any_Threat and failure-desire −15. Fields are `AIGoalApplicationFlags`, `GameMode`, `Category`, `Reachability`, `Time_Limit`, `Activation_Tracking_Duration`, `Per_Activation_Failure_Desire_Adjust`; perception `Function`/`Literal`/`Operator` trees supply the expressions below. Perception counts completed faction extractors. Save for mines when open mineral pads > 0 and credits < **1500**. Structure desire = 10 × not-saving × not-needs-new-station × (enemy/(enemy+friendly)<0.2); refinery desire = 50 × (ratio<0.2) × (enemy-base-distance>**2500**). General contestable score = contestable × [5×(enemy force=0)+5×(1−enemy force)+20×clamp(10000/(nearest-friendly-capital-distance+1),0,2)−5×has-built-object+1]; Secure desire = (not firesale) × allowed-defender × general score × (enemy-base-distance>2500) × [campaign + noncampaign × (is-refinery + (1−is-refinery) × completed-resource-count)]. Perception evaluator/zero-denominator behavior belongs to WTA, not inferred here. | data: goal/perception XML; WTA-44/45 | same WTA goal/evaluator interface; pad formulas missing there | **same** generic XML/Lua import interface; live predicates are G9, not proven by import alone |
| WBP-40 | Secure-pad Lua requests main Fighter/Corvette force 1 plus Fighter/Corvette/Frigate escort 0..4, avoids engaged forces, reinforces and moves to target. With empty or enemy-owned child it attacks existing contents, guards up to 60 seconds until any valid child exists, and exits if pad owner still differs. Friendly captured pad waits for a build opportunity 30 seconds; mineral pads use indefinite wait (−1). Child predicate accepts UC as well as completed child. Nonpad path guard/sleeps10; target-owner-change handling is overridden. Movement, abilities and reinforcement calls are WTA/other walk interfaces. | data: `movetolocationrush.lua` | same WTA script interface; concrete pad branches missing there | **missing**, pad perceptions/contents do not provide plan inputs; G9 |
| WBP-41 | Structure plan declares UC E laser/missile/**tractor**, R laser/missile/repair and U three satellite types (0..1), ignores target, forbids free store and blocks on `Build_All`. Allocation requires a compatible faction-menu pad (WBP-48); the stock tractor token has no such menu entry. Refinery plan reserves one faction UC extractor, chooses reserved pads with enemy-base-distance>**2000** (distinct from desire's 2500), builds, marks goal nonremovable, sleeps30 then succeeds if any child exists, even a 35-second UC. No suitable pad sleeps5 and releases budget. Open-pad enumeration requires live reservation entry with no built type, WBP-07 and cooldown. Charging/refunds are WBP-47/48. | data: `buildstructurespace.lua`, `buildrefineryspace.lua`; EBP-20; debug build EEC-11/20..24/49 | same WTA budget/taskforce interface; pad plan contract missing there | **missing**, `plan_bindings.cpp::register_taskforce` lacks Build/Build_All/reserved-pad APIs; G9 |
| WBP-42 | Lua `Get_Build_Pad_Contents` returns completed child first, otherwise UC, otherwise nil; invalid/dead/nonbuilder receiver produces the invalid-receiver path. `OpenBuildPadCount`, `TacticalBuiltStructureCount`, `IsContestable`, `IsBuildPad`, `HasBuiltObject` and actual credits must reflect live pad/child/player state, not fixed zeros. Reservation enumeration must not bypass proximity or cooldown. | EBP-20/37; data Lua/perception | missing there; WTA documents fixed-predicate gaps | **differs**, `tactical_ai_bindings.cpp` returns nil; `ai_perception.cpp` returns fixed zeros and credits stub; G9 |

### Economy follow-up: timing, modifiers and AI charging

These rules refine the earlier rows without prescribing a new global frame loop. Source constants
use logical FPS; values in frames below assume stock 30 FPS.

| ID | Rule and boundary | Source | Existing / remake gap |
|---|---|---|---|
| WBP-43 | UC's one-frame behavior initializes its next-service deadline to **0** and enables service: it is eligible on the first traversal that reaches it, with no extra one-frame admission delay. Exact traversal after insertion is U-BP-1. Within its own object service, due delayed damage runs before UC healing/completion. If that damage enters deletion, serviced-behavior count becomes 0, so UC cannot then complete in that service. This does not settle a live projectile striking during another object's service or the later collision pass. Normal final-mine creation initializes its automatic income ability immediately; completion in object work therefore registers the stream before that frame's income pass (WPR-16). | debug build EEC-34/40/41/42/50; EBP-08/09/10; WPR-15/16 | missing first-service/death branch there; **missing** UC lifecycle, G2; **missing** mine admission, G3 |
| WBP-44 | Keep three modifier lists per stream, keyed by contributing upgrade/source identity and category. For each list, choose the **greatest signed value within each category**, then add category winners. Value = base × (1 + percentage total) + additive total; interval = max(**1 second**, base interval × (1 + interval total)). Repeated scans do not duplicate one source/category. Stock category-1 E/R L1/L2 bonuses are 0.20/0.40; U bonuses 0.15/0.45. If both contributions coexist, only L2 counts; after L2 removal a surviving L1 contribution can become the winner. Stock L2 normally deletes allied previous-level objects through WPR-22. E/R mine rates: 20 → 24 → 28 credits/sec; U: 20.5 → 23.575 → 29.725, before AI credit adjustment, paid in full to each stock allied recipient. | debug build EEC-01/07/08/13/53/56/58/59; EBP-29; data; WPR-11/22 | category maxima, independent lists and interval floor are **implemented** |
| WBP-45 | Normal modifier activation immediately scans streams already present. Periodic service scans again without duplicate contributions and schedules the next scan at **current service frame + round(5 × FPS)** (150 frames). For an ordinary newly initialized host, the first periodic deadline is randomized using current frame and current frame + interval as bounds; it is not necessarily activation + 150. After that first scan, cadence is relative to the actual scan frame. A newly registered mine gets an existing upgrade when that host next scans it; missed earlier income is **not repaid**. If source creation precedes the host's due service in the same object traversal, that scan can discover it before current-frame income. Creation after the host's scan waits for a later scan. Reverse logic has no periodic service and exchanges apply/remove roles. Retail confirmation of stock inheritance and observed phase is U-BP-6. | debug build EEC-01/02/04/29/30/31/37/45/52/54; EBP-38/41 | immediate discovery, randomized first periodic deadline and no back-payment are **implemented** |
| WBP-46 | Contributions are registered with **no timed expiry**; the five-second scan interval is not their lifetime. Normal removal deletes that upgrade source/category from each eligible stream's percentage, additive and interval lists, preserving other sources. Marking an ability-owning object for deletion invokes ability termination and cancels further services; termination removes its active effects. Mine income removal destroys its own stream. Thus a modifier object's ordinary deletion withdraws its contributions before a later income pass; surviving contributions remain available to category reduction. Whether destroying the mine that holds an upgrade also deletes that upgrade object is still U-BP-6, distinct from the verified modifier-object teardown. | debug build EEC-01/03/16/35/40/47; EBP-42 | source-specific withdrawal, reverse termination and contributions without expiry are **implemented**; mine-host deletion remains U-BP-6 |
| WBP-47 | AI unit allocation adds a newly built, nonmagic UC type's **effective pad price** to the plan's total cost; free-store/reinforcement allocations do not add it. First tactical resource reservation debits that total once from player credits, copies it to the refundable remainder and clears the pending total. Reserving an already reserved plan does not debit again; lifting an ignored reservation also avoids a new debit. This is earlier than Lua `Build`/`Build_All`, whose pad execution suppresses its own debit. Successful reservation matching consumes one reserved type/pad, marks building underway and subtracts **generic tactical cost** from the refundable remainder, floored at 0. Ordinary plan abandonment adds the remaining refundable amount once through the player's positive-credit rule (including AI difficulty adjustment). Do not debit again at UC creation or completion. | debug build EEC-11/22/24/49; EBP-01/04/12/35 | AI debit location missing there; **missing** G9; separate AI interface for G2/G3 |
| WBP-48 | AI `Build`/`Build_All` checks allied pad, new-construction permission and cooldown before consuming reservations. A rejection at these checks neither starts UC nor consumes the refundable price; eventual plan abandonment returns the unused remainder. After reservation consumption, if the request leaves **no UC**, the taskforce sends completion feedback to unblock its waiter and immediately adds the type's generic tactical cost to player credits, using the AI positive-credit multiplier; the same cost was already removed from the later plan refund. UC destroyed after a successful start is not this failure refund (WBP-27). `Build` rejects unmatched reservations; `Build_All` calls reservation notification without testing its return value, so forced stale/duplicate retries need U-BP-4. Allocation chooses a closest compatible open pad by XY distance to the goal, requiring the UC type in that pad's faction menu. Stock menus therefore exclude the Empire tractor from compatible allocation despite the plan token. | debug build EEC-11/20/21/22/49; data; EBP-06/24/33 | failure-refund/menu-filter detail missing there; **missing**, G9; G2 must preserve human/AI distinction |
| WBP-49 | Bare pad-side detach clears only the matching UC/final pointer. UC-side detach clears its parent pointer; parentless UC service cannot grow hull or complete. Pad behavior shutdown is empty and disposing its construction state clears references rather than explicitly destroying children. These local readers do **not** establish what the wider direct-pad deletion route does to a live child. Respawn creation is due at **frame ≥ scheduled frame** in the post-object deferred-creation pass; queued deletions follow. A new respawn created there has missed that pass's ordinary object traversal. Child-death destruction, bare detach, direct parent destruction and sale remain distinct routes. | debug build EEC-06/12/39; EBP-08/14/27; WHZ-52 | detach known there; **partial**, matching links clear and typed respawn commits at frame >= due after object services; wider direct-parent deletion remains U-BP-5 |

## Economy implementation notes

| Implementation scope | Apply these answers | Remaining capture dependency |
|---|---|---|
| G3: completed mining income | WBP-22/23/26/43 and WPR-11/12/15/16: activate the completed child's stream, pay in that frame's income pass, full amount to each ally, live builder identity, AI adjustment and no skirmish balance cap. Empty pads/UC earn nothing. | U-BP-3 allied builder/recipient observation; U-BP-1 live projectile race can decide whether a final mine exists for that payment. |
| G6: death and reclamation | WBP-27..29/49: child death without construction refund; exact neutral respawn deadline and post-object creation; detach is not sale/death. | U-BP-5 direct parent deletion, surviving child/income, and rendered due-frame visibility. |
| Allied mine-income upgrade modifiers (legacy EAWR-996), G4 | WBP-24/44..46: ordinary upgrade queue gates, source/category contributions, largest category winner, five-second discovery with randomized first periodic phase, no retrospective income, source-specific removal. Preserve WPR-22's previous-level replacement. | U-BP-6 destruction of the upgrade host and stock variant ability inheritance; do not assume the source died just because another mine did. |
| Satellite sale and empty-pad preservation (legacy EAWR-998), G10 | WBP-30..32: exact owner, parent lock and single-step guards; round-half generic-cost refund independent of hull, clear completed link before death, keep the empty pad. Queue cancellation and UC destruction are different operations. | U-BP-3 allied ownership/permission feedback and U-BP-9 stock UC action inventory; U-BP-7 sale visibility is presentation-only. |
| AI economy boundary, G9 | WBP-47/48: reserve/debit once, no second pad debit, distinguish early refusal, no-UC failure refund and later child death. Menu-filter the tractor allocation. | U-BP-4 malformed `Build_All` reservations/retries; normal reservation accounting is settled. |

Other unverified items were checked for these scopes. U-BP-8's inherited mine self-repair can change
when G6 death occurs, but supplies no additional income formula; capture it before adding that
ability. U-BP-7's lower-Z/overlay details affect presentation, and U-BP-10's corruption pricing is
campaign-only, so neither changes stock G3/G6 income or sale arithmetic. Production U-4 is relevant
to cancelling a queued mine **upgrade**: WPR-31 already establishes its refund, while the sound
remains unverified. Production U-3 (reinforcement delay) and U-5 (population sounds) do not gate pad
UC creation or income; U-6's station-level retail visuals do not settle mine modifier-host lifetime.
The capture recipes below distinguish observations still needed from rules already established in
the debug build; no retail run is a prerequisite to the traced arithmetic.

## Gap list and implementation tickets

Each row is a separately reviewable scope, with size **S** (one narrow behavior), **M** (several connected components), or **L** (new lifecycle crossing simulation/data/presentation). Existing issues are reused rather than duplicated. Tracking issue: build-pad rule walk (legacy EAWR-1000). The original 42 per-rule remake verdicts were **3 same interfaces, 7 differs, 32 missing**; these become 11 connected implementation scopes. The targeted follow-up adds WBP-43..49 (49 rules total) under the same gaps and refines existing rules, without re-auditing later implementation PRs or opening new tickets.

| Gap | Impact and exact work | Rules | Size / work |
|---|---|---|---|
| G1 | Load live capture/pad profiles, default influence and sticky/community flags; implement WHZ capture and pad eligibility/ownership snapshot for all five maps. Existing obstacle transforms remain useful. | 01/02/04/07/38 | M, space build pads and mining facilities (legacy EAWR-541) |
| G2 | Immediate damageable UC child; separate request/event and AI flags; actual debit, attachment, truncating timer, health growth, fixed finish, replacement and parent links. No station queue/pop path. | 03/05/06/09..16/18/19/43/47/48 | L, damageable pad construction lifecycle (legacy EAWR-995) under space build pads and mining facilities (legacy EAWR-541) |
| G3 | Implemented: completed mines join the shared ledger as live-owner streams, paying the full amount to allies and stopping on death. G4 modifiers and the AI credit multiplier remain separate work. | 22/23/26/43/46 | M, space build pads and mining facilities (legacy EAWR-541), skirmish tech and AI credit rules (legacy EAWR-727) for AI credits |
| G4 | Income-upgrade menus and prerequisite/limit gates; apply/remove allied source/category percentage, additive and interval contributions; five-second discovery scan. Existing parsed multiplier values currently have no effect. | 24..26/44..46 | M, allied mine-income upgrade modifiers (legacy EAWR-996); queue gates: production limits and prerequisites (legacy EAWR-724), upgrade infrastructure: data-driven station upgrades (legacy EAWR-540) |
| G5 | Implemented: completed satellites use the ordinary loaded hull/shield/power/targeting/weapon/turret profiles. Living-projectile/neutral hostility remains separate. | 21 | M, space build pads and mining facilities (legacy EAWR-541), projectile collision order and living-object gates (legacy EAWR-749) |
| G6 | Implemented: child death destroys stock pads, or neutralizes a surviving pad with its authored cooldown; deferred typed respawn returns capture points neutral at 38/45/80 seconds. Wider direct-parent deletion remains U-BP-5. | 03/27..29/49 | M, capturable-object respawning (legacy EAWR-928) |
| G7 | Captured merchant/pod become ordinary producers with faction pirate/Z95 menus and queue/pop interfaces, sticky/shared ownership, no invented economy bonus. | 01/33/34 | M, merchant-dock capture and neutral profiles (legacy EAWR-927), production foundation: station purchasing and reinforcements (legacy EAWR-530) |
| G8 | Build-menu action/buttons, local/spatial feedback, capture and construction snapshots, alternate/animation progression, hidden-pad child picking and live radar owner. Retail layout remains a capture prerequisite. | 08/17/20/35..38 | M, live pad state and UI picking (legacy EAWR-997) |
| G9 | Live pad/credit perceptions, Lua child query, reservation/build APIs and supplied secure/structure/refinery plan execution; retain both 2000/2500 distance tests. | 07/08/39..42/47/48 | M, live AI economy queries (legacy EAWR-786), skirmish AI mode: purchasing-capable skirmish AI setup (legacy EAWR-603) |
| G10 | Implemented: exact-owner satellite sale, lock guard, round-half generic-cost refund, detach before removal and preserve the live empty pad. | 30..32 | M, satellite sale and empty-pad preservation (legacy EAWR-998) |
| G11 | Repair/sensor/tractor and mine self-repair consumer verification, then implementation under the ability workstream. Do not implement data guesses as rules. | 21 | M, space ability rule walk (legacy EAWR-760); U-BP-8 |

Code locations for the verdicts: `src/skirmish/start.cpp::add_map_objects`, `src/scene/scene_build.cpp` placement, `src/units/unit_tables_decode.cpp::load_obstacle and src/units/unit_tables_profiles.cpp::load_production`, `src/skirmish/economy.cpp::economy_rules`, `src/sim/tactical/session_step.cpp::TacticalSession::step`, `include/eawr/sim/tactical/economy.hpp` structure completion ledger, `src/presentation/ui/production.cpp::layout_build_buttons`, `src/script/foc/ai_perception.cpp`, `src/script/foc/tactical_ai_bindings.cpp` child-query binding and `src/script/foc/plan_bindings.cpp::register_taskforce`. `BuildKind::structure` is a placeholder, not implemented UC behavior. Capturing a scene object visually is not simulation capture.

Top five release impacts: **G1** claiming mining sites; **G2** paying for and completing a mine while it can be attacked; **G3** actually earning allied credits from it; **G6** losing and reclaiming the site; **G9** letting the AI contest/build mines instead of receiving zero/nil answers. G4 changes the economy after upgrades, and G8 makes the preceding mechanics usable from the HUD.

## XML reads and registry gaps

The rule tables identify owned reads and boundary reads. Additional interface fields are ordinary combat `Shield_Points`, `Shield_Refresh_Rate`, `Energy_Capacity`, `Energy_Refresh_Rate`, weapon/targeting/turret definitions and `Collidable_By_Projectile_Living` (combat owns their evaluation); queue `Population_Value`, `Tactical_Production_Queue`, prerequisites/current limits, upgrade replacement and sidebar visibility (production owns their evaluation). Faction spatial feedback reads `SFXEvent_Tactical_Object_Building_Started`, `SFXEvent_Tactical_Object_Building_Loop`, `SFXEvent_Tactical_Object_Building_Complete`, `SFXEvent_Tactical_Object_Sold`; capture/loss uses `SFXEvent_Tactical_Gain_Friendly_Control`, `SFXEvent_Tactical_Lose_Friendly_Control`, `SFXEvent_Tactical_Gain_Enemy_Control`, `SFXEvent_Tactical_Lose_Enemy_Control` through WHZ's presentation interface. `Capture_Point_Time_Multiplier` is a capture-modifier interface owned by WHZ, not a new mine-upgrade effect.

Nearby tags not claimed as new pad reads: faction `Corruption_Tactical_Build_Pad_Price_Multipliers` belongs only to WBP-11's campaign branch (stock E/R 1,1); `Corruption_Tactical_Build_Pad_Time_Multipliers` is not read by the traced UC initializer. Global `Tactical_Build_Time_Multiplier`, producer bonus/gate fields and `Tech_Level`/`Required_Star_Base_Level` belong to their production/campaign consumers, rather than WBP-15. `Tactical_Build_Start_Lower_Z` has stock value 0 but an unverified consumer. Heal/jamming/tractor activation and visual effect fields are data inputs for U-BP-8 and the abilities workstream; this walk does not claim their inner behavior. Model names, material names, tooltip text and radar assets are presentation data, not simulation timers or economy constants.

The table below is an exact intersection of this walk's owned/interface tag paths and the registry's `todo`/`deferred` rows for the relevant classes. Registry status is broader than this walk and can lag generic implementation (for example a tag already parsed on station units). A registry umbrella ticket is not proof that a specific pad consumer exists. `Tactical_Build_Start_Lower_Z` is listed separately as **unverified consumer**, not claimed as an active read. Empty default fields and variant/ability-list inheritance require the retail checks below rather than an XML-only guessed merge.

| XML path | Classes in this walk | Registry status / existing work |
|---|---|---|
| `Abilities/Battlefield_Modifier_Ability/Capture_Point_Time_Multiplier` | UpgradeObject | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Force_Healing_Ability/Activation_Style` | SpaceBuildable | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Force_Healing_Ability/Applicable_Unit_Categories` | SpaceBuildable | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Force_Healing_Ability/Applicable_Unit_Types` | SpaceBuildable | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Force_Healing_Ability/Heal_Amount` | SpaceBuildable | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Force_Healing_Ability/Heal_Interval_In_Secs` | SpaceBuildable | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Force_Healing_Ability/Heal_Percent` | SpaceBuildable | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Force_Healing_Ability/Heal_Range` | SpaceBuildable | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Force_Healing_Ability/Single_Target_Heal` | SpaceBuildable | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Income_Stream_Ability/Base_Income_Value` | SpaceBuildable | applied: completed-source live-owner income, WBP-22/23/26 |
| `Abilities/Income_Stream_Ability/Base_Interval_In_Secs` | SpaceBuildable | applied: completed-source live-owner income, WBP-22/23/26 |
| `Abilities/Income_Stream_Ability/Full_Amount_To_Everyone` | SpaceBuildable | applied: completed-source live-owner income, WBP-22/23/26 |
| `Abilities/Income_Stream_Ability/Owner_Income_Percentage` | SpaceBuildable | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Income_Stream_Ability/Split_Favors_Owner` | SpaceBuildable | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Income_Stream_Ability/Split_Income_With_Allies` | SpaceBuildable | applied: completed-source live-owner income, WBP-22/23/26 |
| `Abilities/Income_Stream_Mod_Ability/Affects_All_Allied_Sources` | UpgradeObject | applied to loaded space-source modifiers; ground targets remain outside the space consumer (legacy EAWR-760) |
| `Abilities/Income_Stream_Mod_Ability/Income_Additive_Value` | UpgradeObject | applied to loaded space-source modifiers; ground targets remain outside the space consumer (legacy EAWR-760) |
| `Abilities/Income_Stream_Mod_Ability/Income_Multiplier` | UpgradeObject | applied to loaded space-source modifiers; ground targets remain outside the space consumer (legacy EAWR-760) |
| `Abilities/Income_Stream_Mod_Ability/Interval_Multiplier` | UpgradeObject | applied to loaded space-source modifiers; ground targets remain outside the space consumer (legacy EAWR-760) |
| `Abilities/Income_Stream_Mod_Ability/Reverse_Application_Logic` | UpgradeObject | applied to loaded space-source modifiers; ground targets remain outside the space consumer (legacy EAWR-760) |
| `Abilities/Income_Stream_Mod_Ability/Stacking_Category` | UpgradeObject | applied to loaded space-source modifiers; ground targets remain outside the space consumer (legacy EAWR-760) |
| `Abilities/Income_Stream_Mod_Ability/Target_Stream_Source` | UpgradeObject | applied to loaded space-source modifiers; ground targets remain outside the space consumer (legacy EAWR-760) |
| `Abilities/Sensor_Jamming_Ability/Activation_Style` | SpaceBuildable, SpaceUnit, Squadron | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Sensor_Jamming_Ability/Blob_Color` | SpaceBuildable | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Sensor_Jamming_Ability/Blob_Material_Name` | SpaceBuildable | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Sensor_Jamming_Ability/Duration_In_Secs` | SpaceBuildable | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Tractor_Beam_Attack_Ability/Activation_Max_Range` | SpaceBuildable, SpaceUnit | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Tractor_Beam_Attack_Ability/Activation_Min_Range` | SpaceBuildable, SpaceUnit | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Tractor_Beam_Attack_Ability/Activation_Style` | SpaceBuildable, SpaceUnit | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Categories` | SpaceBuildable, SpaceUnit | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Tractor_Beam_Attack_Ability/Applicable_Unit_Types` | SpaceBuildable, SpaceUnit | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Tractor_Beam_Attack_Ability/Excluded_Unit_Types` | SpaceBuildable, SpaceUnit | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Tractor_Beam_Attack_Ability/Stacking_Category` | SpaceBuildable, SpaceUnit | todo: ability tag support (legacy EAWR-760) |
| `Abilities/Tractor_Beam_Attack_Ability/Target_Speed_Decrease_Percent` | SpaceBuildable, SpaceUnit | todo: ability tag support (legacy EAWR-760) |
| `Affiliation` | SecondaryStructure, SpaceBuildable, SpaceStructure | todo: combat tag support (legacy EAWR-650) |
| `Build_Limit_Current_For_All_Allies` | Squadron, UpgradeObject | todo: economy tag support (legacy EAWR-654) |
| `Capture_Point_Radius` | SecondaryStructure, SpaceBuildable | todo: economy tag support (legacy EAWR-654) |
| `Capture_Point_Transition_Time_Seconds` | SecondaryStructure, SpaceBuildable | todo: economy tag support (legacy EAWR-654) |
| `Collidable_By_Projectile_Living` | SecondaryStructure, SpaceBuildable, SpaceStructure, SpaceUnit | partial movement tag coverage (legacy EAWR-649): live capture/UC types; other types deferred |
| `Credit_Multiplier` | Difficulty_Adjustment | todo station purchasing and reinforcements (legacy EAWR-530) |
| `Destroy_Previous_Upgrade_Level` | UpgradeObject | todo: economy tag support (legacy EAWR-654) |
| `Destroy_When_Child_Dies` | SpaceBuildable | applied: matching child-death callback, WBP-27/28 |
| `Energy_Capacity` | SecondaryStructure, SpaceBuildable, SpaceStructure | todo: economy tag support (legacy EAWR-654) |
| `Energy_Refresh_Rate` | SecondaryStructure, SpaceBuildable, SpaceStructure | todo: combat tag support (legacy EAWR-650) |
| `GUI_Row` | SecondaryStructure, SpaceBuildable, SpaceUnit, Squadron | todo: presentation tag support (legacy EAWR-653) |
| `Has_Space_Evaluator` | SecondaryStructure, SpaceBuildable, SpaceStructure | todo: AI tag support (legacy EAWR-652) |
| `Hides_When_Built_On` | SpaceBuildable | todo: presentation tag support (legacy EAWR-653) |
| `Influences_Capture_Point` | SecondaryStructure, SpaceBuildable, SpaceUnit | todo: economy tag support (legacy EAWR-654) |
| `Initial_State_Visible_Under_FOW` | SecondaryStructure, SpaceBuildable, SpaceStructure | todo: presentation tag support (legacy EAWR-653) |
| `Is_Community_Property` | SecondaryStructure, SpaceBuildable, UpgradeObject | todo: economy tag support (legacy EAWR-654) |
| `Last_State_Visible_Under_FOW` | SecondaryStructure, SpaceBuildable | todo: presentation tag support (legacy EAWR-653) |
| `Next_Upgrade_Level_Type` | UpgradeObject | todo: economy tag support (legacy EAWR-654) |
| `Ownership_Sticks` | SecondaryStructure, SpaceBuildable | todo: economy tag support (legacy EAWR-654) |
| `Previous_Upgrade_Level_Type` | UpgradeObject | todo: economy tag support (legacy EAWR-654) |
| `Reinforcement_Prevention_Radius` | SecondaryStructure | todo: economy tag support (legacy EAWR-654) |
| `SFXEvent_Build_Complete` | SecondaryStructure, SpaceBuildable, SpaceUnit, Squadron, UpgradeObject | todo: economy tag support (legacy EAWR-654) |
| `SFXEvent_Build_Started` | SecondaryStructure, SpaceBuildable, SpaceUnit, Squadron, UpgradeObject | todo: economy tag support (legacy EAWR-654) |
| `SFXEvent_Tactical_Build_Complete` | SpaceUnit, Squadron | todo: economy tag support (legacy EAWR-654) |
| `SFXEvent_Tactical_Build_Started` | SpaceUnit, Squadron | todo: economy tag support (legacy EAWR-654) |
| `SFXEvent_Tactical_Gain_Enemy_Control` | Faction | todo: presentation tag support (legacy EAWR-653) |
| `SFXEvent_Tactical_Gain_Friendly_Control` | Faction | todo: presentation tag support (legacy EAWR-653) |
| `SFXEvent_Tactical_Lose_Enemy_Control` | Faction | todo: presentation tag support (legacy EAWR-653) |
| `SFXEvent_Tactical_Lose_Friendly_Control` | Faction | todo: presentation tag support (legacy EAWR-653) |
| `SFXEvent_Tactical_Object_Building_Complete` | Faction | todo: economy tag support (legacy EAWR-654) |
| `SFXEvent_Tactical_Object_Building_Loop` | Faction | todo: economy tag support (legacy EAWR-654) |
| `SFXEvent_Tactical_Object_Building_Started` | Faction | todo: economy tag support (legacy EAWR-654) |
| `SFXEvent_Tactical_Object_Sold` | Faction | todo: presentation tag support (legacy EAWR-653) |
| `Shield_Points` | SecondaryStructure, SpaceBuildable, SpaceStructure | todo: combat tag support (legacy EAWR-650) |
| `Shield_Refresh_Rate` | SecondaryStructure, SpaceBuildable, SpaceStructure | todo: combat tag support (legacy EAWR-650) |
| `Show_In_Sidebar_When_Complete` | UpgradeObject | todo: presentation tag support (legacy EAWR-653) |
| `Show_In_Sidebar_While_Building` | UpgradeObject | todo: economy tag support (legacy EAWR-654) |
| `Space_Build_Time_Multiplier` | Difficulty_Adjustment | todo: AI difficulty tag support (legacy EAWR-737) |
| `Space_Capture_Allowed_Countdown_Seconds` | GameConstants | todo: economy tag support (legacy EAWR-654) |
| `Space_FOW_Reveal_Range` | SecondaryStructure, SpaceBuildable, SpaceStructure, Squadron | todo: presentation tag support (legacy EAWR-653) |
| `Space_Layer` | SpaceStructure | todo: movement tag support (legacy EAWR-649) |
| `Space_Obstacle_Offset` | SecondaryStructure, SpaceBuildable, SpaceStructure | todo: movement tag support (legacy EAWR-649) |
| `Tactical_Build_Cost_Multiplayer` | SpaceBuildable | todo: economy tag support (legacy EAWR-654) |
| `Tactical_Build_Cost_Multiplayer` | UpgradeObject | todo data-driven station upgrades (legacy EAWR-540) |
| `Tactical_Build_Prerequisites` | SpaceUnit, Squadron, UpgradeObject | todo: economy tag support (legacy EAWR-654) |
| `Tactical_Build_Start_Lower_Z` | SpaceBuildable | todo: economy tag support (legacy EAWR-654) |
| `Tactical_Build_Time_Seconds` | SpaceBuildable | todo: economy tag support (legacy EAWR-654) |
| `Tactical_Build_Time_Seconds` | UpgradeObject | todo data-driven station upgrades (legacy EAWR-540) |
| `Tactical_Buildable_Constructed` | SpaceBuildable | todo: economy tag support (legacy EAWR-654) |
| `Tactical_Buildable_Objects_Multiplayer` | SecondaryStructure, SpaceBuildable | todo: economy tag support (legacy EAWR-654) |
| `Tactical_Production_Queue` | UpgradeObject | todo data-driven station upgrades (legacy EAWR-540) |
| `Tactical_Respawn_Time_In_Secs` | SecondaryStructure, SpaceBuildable | applied: ordinary typed neutral respawn, WHZ-52/WBP-29 |
| `Victory_Relevant` | SpaceUnit | deferred combat tag coverage (legacy EAWR-650) |
| `Visible_To_Enemies_When_Empty` | SpaceBuildable | todo: presentation tag support (legacy EAWR-653) |

`Minimum_Time_Before_Pad_Can_Build_Again` is read for the surviving-pad WBP-28 path;
stock space pads do not author a positive value. `Split_Favors_Owner` and explicit owner
percentages remain outside this change; stock completed mines use full payment to every
recipient, so those branches do not change their stream. Loaded space-source mine-upgrade modifiers use WBP-24/25/44..46; ground targets remain outside this consumer.

## Settled questions from the unverified sweep

Question IDs are retained; these boundaries no longer require a new source read. Opaque evidence IDs identify ignored research receipts. Runtime acceptance and explicitly remaining clauses stay below.

| ID | Sourced disposition | Evidence |
|---|---|---|
| U-BP-4 | Each Build_All entry rechecks alliance, construction admission and cooldown before reserving or adding a blocking build. Stale/refused entries skip reservation. After an admitted request creates no construction child, it sends completion, refunds authored tactical cost and reports a script error; the free-store completion receiver resets the empty reservation. A repeated entry is rechecked after the preceding request; no unconditional second reservation is established. | EUS-27 |

## Unverified

These are precise rig requests for the coordinator's retail Lua staging route. Log **logical
frame**, setup/pause/single-step state, fractional player credits, ownership and object IDs; HUD
whole-credit rounding alone cannot expose a one-frame payment. Keep a continuous-play control
alongside stepped runs because single-step itself refuses sale. Settled portions of the original
questions are WBP-43..49 and WPR-01/11/12/15/16; only the residual questions remain below.

| ID | Unknown | Capture that settles it |
|---|---|---|
| U-BP-1 | Exact first traversal of a newly inserted UC, live projectile/lethal-damage vs completion, and pad-command placement relative to object traversal. First-service eligibility, due delayed damage preceding UC behavior and completion-frame mine payment are settled by WBP-43/WPR-16. | Record request frame, debit/UC creation frame, first hull-growth frame and computed deadline F from a claimed E/R mining pad (35 seconds), logging each logical frame. In separate continuous runs make the UC receive lethal delayed damage and an actual projectile at F−1, F and F+1; log UC/final IDs, hull and credits across replacement, plus collision/damage observation when available. Repeat command delivery before/after the pad's observed traversal. A surviving final mine pays 200/10/FPS that frame; confirm with fractional deltas after subtracting station income. The frame-order walk owns scheduler placement. |
| U-BP-3 | Human menu race and allied builder identity/permissions. | Two allied players claim with A, open/build with B; record pad owner, UC/final owner, credit debit, selection/menu and recipients. In a second run enter a hostile capture-influencing ship after opening palette but before clicking; compare opening anew while hostile is already inside, including exact radius and just inside/outside. |
| U-BP-5 | Direct pad destruction with UC/final child, wider exceptional detach cleanup and rendered due-frame respawn visibility. Local detach semantics and the ≥ due-frame creation test are settled by WBP-27/29/49. **Sweep:** Still unverified: Local detach only clears matching construction/final-child references. Parent destruction does not establish child destruction through that callback; exceptional routes and rendered due-frame admission require their callers and a frame capture. | Separate runs: destroy UC; destroy completed mine; destroy parent pad with each child present; detach each link without death. Use the harness to distinguish combat death from direct deletion, since stock laser/mining pads reject living projectiles. Record both IDs/links, upgrade IDs, source income and ownership until after replacement. For death frame D at 30 FPS sample D+1139/1140/1141 for mine, D+1349/1350/1351 for laser, D+2399/2400/2401 for merchant. Compare replacement ID existence at the deferred pass with rendered visibility; explicitly record whether the old child, orphan UC or income survives instead of assuming cleanup. Retained sweep boundary: EUS-31. |
| U-BP-6 | Whether destroying an upgrade's mine host deletes its modifier object; effective stock variant ability inheritance and observed scan phase. Category aggregation, no-expiry contributions, modifier-object removal, completion-frame payment and no retroactive catch-up are settled by WBP-44..46/WPR-16. **Sweep:** Still unverified: Modifier termination is sourced, but host deletion is a different relationship. The mine-host-to-held-upgrade teardown and variant subobject-list merge rule remain unresolved; scan phase also needs service-frame observations. | Two allied same-faction owners with one mine each: complete L1 then L2 and record upgrade IDs/containers plus fractional credit deltas. Delete the modifier object directly in one run; in a separate run destroy its mine host while the allied mine survives. Observe whether the modifier ID is deleted, whether surviving L1 becomes active, and whether contributions persist. Build a third mine immediately after a recorded host scan and log at least six seconds: measure base-rate frames then upgraded-rate frames, with no repayment of earlier frames. Repeat E/R rates 20/24/28 and U 20.5/23.575/29.725 before AI adjustment; log the initial periodic phase rather than assuming exactly activation+150. Retained sweep boundary: EUS-37. |
| U-BP-7 | Exact capture overlay, pad palette arrangement, minimap change, alternate/animation vs elapsed progress, and any lowered-Z construction consumer. | Record empty neutral/claimed/contested pads, faction palettes, UC selected at quarter/half/near-finish and after damage, completed/hidden pad, sale and respawn. Include world bars, command bar, fog and minimap in the same view. Inspect stock lower-Z=0 and a controlled nonzero override separately; do not ascribe its motion to the stored increment without observation. |
| U-BP-8 | Ability consumers and inherited mine self-repair, especially multiple `Abilities` lists in variants/upgrades. **Sweep:** Empire_Mineral_Extractor explicitly authors automatic self-heal: 0.003 percent, one-second interval, range/amount zero and single target. Rebel and Underworld variants each author a named income ability list. The XML does not state the parser’s replace/merge policy for inherited named subobjects. Still unverified: XML explicitly authors Empire automatic self-heal and faction variants author income lists, but XML alone does not settle whether a variant list replaces or merges inherited named abilities. The subobject parser query returned no resolved body; inherited self-heal and individual healing/tractor/jammer consumers remain unverified. | Damage isolated E/R/U mines, leave idle and log hull at one-second intervals; Empire authors automatic self-heal 0.003, interval1, range0, amount0, single-target. Stage repair satellite with damaged allied ships just inside/outside550 and two candidates; log target/amount every2 seconds. Stage sensor and tractor separately with valid/invalid target categories, duration and observable sensor/speed changes. Verify inherited effects at runtime before extending the abilities walk. Retained sweep boundary: EUS-41. |
| U-BP-9 | Still unverified: any stock UC-specific cancel action and its refund; exact disabled/negative UI feedback. Traced pad build/UC death routes have no cancel/refund, and UC lacks tactical-sale behavior (WBP-10/27/30). These are not proof of every command-bar route. | Select owned E/R mine UC at 0%, about 50% and the last precompletion frame; record command-bar actions, right-click and Alt+Ctrl/guard behavior plus credits and both child/pad IDs before/after each available action. If cancel exists, execute it at each stage and with damaged UC, recording refund and pad survival/cooldown. Repeat allied selection and continuous/pause/single-step controls. Separately cancel a queued L1 income upgrade as the WPR-31 full-refund control; a queue cancel must not be reported as UC cancellation. Record insufficient-credit, enemy, cooldown and locked-final feedback separately. |
| U-BP-10 | Campaign corruption pad-price adjustment and sale using a different cost reader (outside stock skirmish). | If campaign parity is scheduled later, stage corrupted and uncorrupted conflict locations, build/sell the same satellite and compare debit/refund/tooltip with the faction corruption multiplier; repeat damaged/full hull. Multiplayer control remains base875 → refund438. Do not add corruption pricing to multiplayer. |

The default influence flag is settled by EBP-46 and is not an outstanding retail question. Zero-duration capture and other nonstock WHZ unknowns remain in that walk rather than being silently resolved here.

## Simulation decisions for G1/G2

The implementation uses the space index's ascending-ID candidate order. This is the project's
deterministic query-order policy, not a claim about a debug build's object-storage order.
Positive capture increments round upward by at most one Q24 unit, so accumulated quantization
does not defer an authored 8- or 10-second transition by an additional four-frame service.

U-BP-1 remains unverified: human build events execute after command delivery in the same logical
frame, construction first heals on the following frame, and damage resolves before healing and
the completion check. Lethal damage therefore wins against a same-frame deadline. Completion
retains the original deadline and copies health fraction even when damage occurred earlier.
These choices keep the uncertain ordering explicit until the requested observation settles it.
Completed-source income and child-death cooldown/neutral respawn are implemented in G3/G6.
Sale and the full pad presentation remain their separate G10/G8 tasks. Multiplayer menus and prices are used; campaign corruption is not
introduced into the skirmish path.

WPR-54 supplies the verified `Shield_Default` for the nine offered satellites whose
`Shield_Armor_Type` is absent. Only the two repair satellites' absent
`Space_FOW_Reveal_Range` remains an explicit diagnostic under G11. Completed
satellites already enter ordinary combat and durability through the UC result closure.

The WHZ-51 projectile boundary now reads `Collidable_By_Projectile_Living` for the
four admitted capture types (`Defense_Satellite_Laser_Pad`, `Mineral_Extractor_Pad`,
`Skirmish_Merchant_Dock`, `N_Gravity_Well_Station`) and the twelve offered UC types
reachable through the laser/mining pad menus. False rejects ordinary projectile
contact before exact geometry tests. Completed satellites and other object types
retain the existing permission default; absent values also retain that default.
Living-projectile permissions remain partial movement coverage (legacy EAWR-649).

Capture preparation follows the project's partitioned tick policy: occupied pads
schedule no capture work. Open pads reuse the moved-world combat index; retained
partition buffers index only additions when that index exists, or prepare the
fallback index in parallel when combat is absent. Queries count every inspected
broadphase body, including bodies rejected by the exact radius test. Index and
input preparation have separate work counters. The warmed preparation, query and
object-service phases allocate nothing; retained transactional pad maps synchronize
only changed records. Station queue commands reject pad menus and construction
children before debit, so queue cancellation cannot refund a rejected pad purchase.

A stock Polus combat probe exposed deliberate targeting of its neutral mining pad
before capture completed. WHZ-51's default neutral relationship now supplies one
shared ordinary-combat gate: explicit attack orders, ship/opportunity scans, retained
weapon targets, the attack cursor and projectile contact reject either neutral player.
Combat-only sessions use the sorted faction IDs whose validated `Is_Neutral` value
is true, including scenes with no capture points or economy service. Capture changes
the object's owner, so its new owner's ordinary team relationship applies immediately.
Explicit scripted damage stays a separate interface. The debug build also has a
separate forced-friendly attack option; the remake's ordinary attack command has no
such option and does not invent it through a cursor modifier.
The real-data Polus contracts reject an explicit attack and pass a projectile even
with living collision permission enabled while the pad is neutral. Hostile ownership
alone does not override the stock bare pad's disabled collision permission: R-08
rejects it under either owner. The debug build's suitability check reads the effective
type flag without an ownership exception. An explicitly collidable hostile pad
variant supplies the projectile-contact and automatic-fire positive controls;
the collidable neutral variant remains untargeted. WBP-18/21 creates a separate
completed mine: effective XML enables living collision on `Empire_Mineral_Extractor`,
and `Rebel_Mineral_Extractor` inherits it. The stock hostile completed-mine control
is fired at while its noncollidable bare pad remains untargeted. The nearest-contact
project choice remains separate collision-order work (legacy EAWR-749).

The same WHZ-51 boundary applies to the existing GS-11 goal matcher: a neutral
capture point does not qualify as an enemy unit or structure. Promoting map pads
to known types must not consume enemy-goal proposal work for their neutral owners.
Captured points retain the existing allied/enemy goal classification; capture AI
plans themselves remain G9.

The G8 viewer now opens a visible allied empty pad through WBP-07/08, resolves
its faction's UC menu into at most six command-bar entries using `GUI_Row`, and
refreshes credit, shared availability and cooldown state (WBP-36). Every click
closes build mode and dispatches the pad command through replay opcode 13.
Portraits, localized names/descriptions and recharge art come from XML and the
text database. Hidden parent models leave picking to UC, then the completed child;
an existing selection follows that replacement (WBP-35, WSU-10/16).

Live capture owner/target/progress feed the model's masked colourisation, world bars and
owner-coloured minimap. Neutralization reverses the displayed fill (WBP-37).
Construction shows both hull and fixed-deadline progress; BUILD clip time follows
that deadline, while authored mesh alternates follow current hull (WBP-17).
The model light tint and reused world-bar geometry are presentation substitutions:
the exact faction capture clip mapping, retail overlay artwork and lowered-Z
consumer remain U-BP-7. Construction start/complete feedback resolves authored
UC/faction events; each spatial loop ends with its own child and cannot stop a
voice that was subsequently stolen (WBP-20). UI and scene contracts cover these
state mappings without adding presentation fields to canonical snapshot bytes.

The pad palette shares the station menu cache. Its six-button storage survives
snapshot refreshes; only a changed menu identity reconstructs card names. Credits,
availability and recharge change the retained card state in place. The project
regression budget is zero allocations across 10,000 unchanged pad snapshots and
10,000 advancing recharge snapshots after warm-up, with no repeated card metadata
construction on those frames.

## Simulation decisions for G3/G6

Child-death commits return the destroyed-parent result directly. Prior UC removals
form one sorted prefix for binary-search membership; later final-child removals
append outside that prefix. This keeps the pad commit's membership work within
O(pads × log(deaths + 1)) rather than scanning a growing removal list for each pad.
A deterministic 128-pad UC/final bulk-death contract bounds actual queries and
comparisons and checks identical state and work on 1/2/4/8 workers. The diagnostic
counter is excluded from canonical state.

Completed mines enter the existing ledger before the completion frame's income pass.
The partitioned `income-sources` phase reads current owner and surviving hardpoint
bonuses once per admitted source; retained outputs allocate nothing in warmed worker
phases. An ordered commit pays the few economy players. Empty pads and construction
children never register streams. Stock mine data gives each allied recipient the full
amount; percentage/additive upgrade modifiers use the G4 consumer below. AI credit adjustment remains its separate interface.

Child death clears the matching links without refund. Stock destroy-on-child-death
pads leave immediately and schedule typed neutral replacements. Surviving pads reset
capture and use their authored cooldown. The sparse respawn journal rolls back with
a failed tick. Due batches visit creation events in due-frame/death-notification order
after the existing object phases; this bounded serial creation commit follows WBP-49
rather than adding a serial traversal of the live world. New capture points first
service on a later capture pass. Death clones are presentation objects and never
enter the ordinary destruction scheduler. Direct-parent cleanup remains U-BP-5.

The viewer reserves one reusable model slot per initially placed respawnable object.
A focused replay containing a pad-build command binds the selected map's economy and
construction/replacement slots. The Polus contract checks capture, completion-frame
income, 300 full payments, source/parent death and neutral creation at the 38-second
deadline with per-frame identical hashes for 1/2/4/8 workers.

## Simulation decisions for G10

The recorded sale command rechecks exact child ownership, current parent contents
lock and sale behavior at execution. Its owning-faction multiplayer menu matches
the completed type to a UC entry. The generic UC menu price supplies the refund;
its rounded half is independent of both current hull and the construction debit.
Missing parent/menu matches and negative computed amounts give zero. Campaign
corruption pricing remains outside the multiplayer interface. AI positive-credit
adjustment remains separate.

Sale clears the parent link before deleting the child, emits a sale notification
without combat death, and retains the pad owner and death-cooldown values. A later
ordinary capture service may neutralize it. The next snapshot reveals the empty
parent to each player through the existing visibility policy. Presentation restarts
the parent's existing empty clip and plays the optional local/spatial sold sounds;
exact retail clip mapping remains U-BP-7. Parent contents locks are an explicit
container-state interface: stock skirmish has no operation that sets them. The
request API refuses an explicit single-step flag, while ordinary pause retains its
normal command behavior; no single-step UI mode is introduced by this change.


## Simulation decisions for G4

Completed mines offer their authored upgrades through the existing station queue.
Stock modifier nodes omit `Activation_Style`; their `Target_Stream_Source` selects
the loaded space income source. An explicit ground style or an unloaded ground
target does not enter the space consumer. Previous-level destruction defaults to
true when a previous type is present, and explicit false retains it.

Each hidden completed upgrade keeps its own sorted stream attachments and scan
deadline. The partitioned modifier phase discovers matching live sources immediately,
then at a deterministic randomized first deadline within 150 frames and every
150 frames from the actual scan thereafter. Newly created mines receive their
base income until discovery, without retrospective payment. Three independent
lists select the greatest signed nonzero contribution per category and sum their
winners. The interval has a one-second floor; every allied recipient gets the full
stock mine payment. Checked arithmetic fails and rolls back the tick on overflow.

Normal object removal drops only that object's contributions, exposing any surviving
lower-level winner. Reverse activation removes, and reverse termination applies
once without a periodic service. These effects persist without a timed expiry and
dead streams prune before payment. U-BP-6 remains explicit: loss of a mine host
alone does not establish deletion of its held modifier object, so the service does
not infer that deletion while allied sources survive. Stable hidden object IDs,
attachments and deadlines enter the conditional IMOD state block. Worker phases
reuse retained buffers after capacity preparation; ordinary warmed reduction and
periodic scans allocate nothing. The project's deterministic RNG stream is a
simulation policy, not a claim about the retail RNG sequence.


### Capture-query membership

- **WBP-50** (debug build, EAC-01..03): capture-owner determination and
build-palette eligibility query the object manager's projectile-collidable
spatial tree. A living object enters that tree only when its effective
`Collidable_By_Projectile_Living` flag is true; the flag defaults to false.
`Influences_Capture_Point` still defaults to true, but it is evaluated only
for objects admitted to this query domain. A hazard's movement footprint or
environmental damage behavior does not put it in the capture query. Stock
asteroid fields omit the projectile-collidable flag and consequently cannot
contest a neighboring mining pad or prevent opening its build palette.
An authored collidable neutral or hostile contender retains the ordinary
WBP-04/07 gates. This query membership does not remove the hazard from
movement, environmental damage, rendering or radar services.

`economy_rules` binds the loaded unit/obstacle flag into each capture-influence
profile; both pad helpers apply the same membership gate before influence.
The installed Polus contract retains the full map surroundings, verifies
capture, menu permission, completed construction and completion-frame income,
and compares each frame on 1/2/4/8 workers. The graphical capture contract
records a fresh selected-map setup, retains its hazards and drives movement
and mine construction through ordinary mouse input.

- **WBP-51** (debug build, ULC-01..07; effective XML): living collidable
admission uses each created object's effective type, with a false default and
no squadron, hero, team or upgrade exception. A squadron purchase template
creates its craft and then a separate team using `Create_Team_Type`; team
membership does not replace the craft in the collidable domain. Our squadron
record represents that spawned team and therefore reads the team's inherited
collision flag. Stock teams omit it, while their member craft explicitly opt
in: craft capture and contest pads, teams neither capture nor block a build
palette. Live upgrade modifier objects also remain outside these queries.
Hero companies supply deployment metadata; the deployed ship/team and any
carried hero have their own effective admission flags.

WAD-14/18 applies the same opt-in to individual blast recipients without
substituting a team or dividing damage by its roster. Ordinary projectile
contact already rejects a noncollidable recipient under WHZ-51; projectile
definitions used as shot payloads do not become unit-table recipients merely
because their type omits the flag. WSU-13 independently uses the same false
default for mouse admission, with behaviour and impassable-asteroid opt-ins,
so correcting unit collision defaults changes no mouse admission. The native
per-player projectile-collidable tree and its traversal order remain separate
work (legacy EAWR-1362).


- **WBP-53** (**debug build**, EAT-05; WBP-04/50/51): capture-owner determination
  has no fighter, corvette, frigate or capital category restriction. Each live
  object admitted to the collidable query is tested for influence, faction and
  point affiliation, presence outside limbo, and inclusive raw 3D capture radius.
  A stationary corvette can capture; a frigate or capital can also capture when
  its centre is in range. Hull overlap alone is insufficient, and a larger ship's
  authored height can put it outside the sphere even when its XY centre lies in
  the pad's circle. Ship pathfinding and destination clipping decide whether an
  order reaches that range; they are independent of capture eligibility. The
  headless class contract covers the inclusive 3D boundary on 1/2/4/8 workers.

## Ownership colourisation

- **WBP-52** (**debug build**, capture progress, ownership colour binding and
submesh parameter readers; **data**, the three mining models and their shader
channel contracts): capture service supplies its progress colour to the model's
colourisation constant. Each submesh receives it only when its shader exposes
that constant. It does not multiply ownership colour into model lighting.
The existing four-frame capture service and authored transition duration remain
WHZ-44..47; the stock mining pad uses ten seconds. The bump-colourise family
multiplies base RGB by this colour only under base-texture alpha. Normal-texture
alpha is a separate specular mask. Noncolourising shell, girders and glow
materials retain their authored appearance.

`Mineral_Extractor_Pad` uses `NB_AsteroidMining_Pad.ALO`; its construction child
uses `NB_AsteroidMining.ALO`, and the completed extractor uses
`NB_AsteroidMining_Full.ALO`. All three contain an `Asteroid` mesh using
`MeshBumpColorize.fx`, `w_asteroid00.tga` and `w_asteroid00_bc.tga`.
Their facility meshes use that same shader with `NB_CMC22Mining.TGA` and
`NB_CMC22Mining_BC.tga`. The rock base texture has zero alpha throughout,
so its bump-colourise surface is independent of ownership colour. The translucent `Asteroid_shell` uses
`MeshAlphaGloss.fx` with `NB_AMining_Rock.tga` and its gloss texture; its
colourisation declaration is unused by the selected shader and is not a
whole-rock colour mask.

`Skirmish_Merchant_Dock`, the capturable mercenary station, inherits
`EB_construction_pod.alo` from `Orbital_Construction_Pod`. Its main body uses
`MeshBumpColorize.fx` with `EB_Station.tga` and `EB_Station_bump.tga`; the
station base texture limits colourisation to a small binary-alpha trim mask.
The alpha scaffolding uses `MeshAlpha.fx` with `NB_Girder2.tga`. The capturable
`N_Orbital_Construction_Pod` variant uses the same model. Both enter the same
capture-view and per-piece binding path as mining and defense pads: masked
colourisation changes, while noncolourising scaffolding keeps its lighting.

The viewer supplies the existing WBP-37 colour fade as an instance parameter
through `GodotRenderer::set_unit_colorization`, leaving shared material uploads,
arrival light scale, shield flashes and fog opacity independent. Unbound
instances keep their uploaded colour; linear adapters decode the stored override,
while bump adapters use stored values as before. Updates reuse each piece's
cached value, skip identical values and visit the pieces already posed that frame;
there is no material duplication or new world scan. CPU contracts cover ownership
states, zero/partial/full mask coverage, specular independence and zero allocations
while advancing the fade. Shader selection and XML application are unchanged.

The existing viewer fade uses white for its neutral endpoint. The debug build's
capture-progress reader instead obtains the neutral faction's ordinary colour,
and initial ownership binding obtains its no-colourisation colour. Matching those
distinct neutral endpoints remains a presentation gap; this fix preserves the
existing fade values and timing while restricting their effect to the mask.

The merchant purchase route applies WBP-33/34 and WPR-22/30/33: a captured
ordinary producer retains its owner-faction menu, and an allied buyer pays into
its own production queue. Capture-point membership alone does not select pad
construction. Only a capture profile with the construction-pad flag uses that
route; UC types remain excluded from ordinary production. Finished mercenaries
enter the buyer's reinforcement pool and use normal player placement, arrival
and population admission. No power, upgrade or galactic tech gate is added.

The replay contract exercises neutral and hostile refusal, capture, owner and
allied payment, queued completion and delivery at chosen points with identical
per-frame hashes on 1/2/4/8 workers. The installed-data contract checks all twelve
stock faction options. The GPU contract captures the Coruscant dock, clicks its
build card, and drags the completed unit from the reinforcement pane.
