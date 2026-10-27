# R4: land base and economy

## Scope and evidence

This walk supplies the retail contract for **L-B1**, the land base/economy work in
phase P2. It covers production buildings and player queues, construction, base
power, base shields, income and the prebuilt-base option. It also identifies inputs
for L3 (land start), L6 (land HUD), L1/L4/L5 (terrain and pathing), P3 (land AI) and
P6 (campaign land entry and return).

Evidence is read-only inspection of the **debug build** and the effective stock
XML. Opaque R4-E and R4-D IDs identify the private evidence index; binary names,
addresses and raw output are not part of this note. No retail capture was made.
Authored stock examples are neither engine limits nor a fixed roster for mods.

The [production walk](../../behaviour/walks/production.md) owns shared queue,
credit and income arithmetic; [build pads](../../behaviour/walks/build-pads.md)
owns pad construction; [land combat](../../behaviour/walks/land-combat.md) owns
reinforcement and support admission; [land AI](../../behaviour/walks/land-ai.md)
owns construction plans and campaign funding. The
[skirmish setup walk](../../behaviour/walks/skirmish-setup.md) owns the options
transaction. This note qualifies their land consumers rather than replacing them.

Frame order and cadence belong to the
[frame service schedule](../../behaviour/frame-service-schedule.md) and land
frame-map research R1. WPR-15, FS-17 and FC-37 establish the shared income/queue
boundary; they do not settle every land power, shield, capture or deletion race.
No new frame traversal or universal callback order is asserted here.

## Questions and closure boundary

There was no earlier base/economy walk and no inherited U-* row specifically for
this R4 scope. The following local questions make each rule's closure explicit:

| Question | Static question answered by this walk |
|---|---|
| U-R4-01 | Which producer/menu, player queue, cost and completion paths apply on land? |
| U-R4-02 | What supplies base power and which checked consumers require it? |
| U-R4-03 | What decides base-shield state, recovery and terrain contribution? |
| U-R4-04 | Which land income sources use the shared economy, and what is the campaign boundary? |
| U-R4-05 | What does prebuilt bases control, independently of free starting forces? |

An answer below closes only the stated static question. The runtime and lifecycle
remainders have separate U-R4-06 onward rows. In particular **U-LC-6** and
**U-WLAI-1** remain open for R1; **U-LC-8**, **WGT-U12/WGT-U13** and
**U-WLAI-5** retain their campaign persistence and interrupted-construction work.

## Rules

### Production and construction

| Rule | Retail rule | Evidence summary | Question closed | Decision / consumer |
|---|---|---|---|---|
| LR4-01 | Construction and production are different paths. Stock land pads offer unfinished products through `Tactical_Buildable_Objects_Multiplayer`; the unfinished type names its completed structure with `Tactical_Buildable_Constructed`. Completed barracks, factories and command buildings offer company or research types through their own tactical menus. Do not treat a unit purchase as a new pad occupant. | effective XML R4-D01; LC-54..57; WBP-09..19 | U-R4-01: content chain | L-B1/P2; land structures and pads (legacy EAWR-1458); L2 type closure |
| LR4-02 | Tactical production uses the buyer's units and upgrades queues. A purchase selects `Tactical_Production_Queue` from the product and stores the producer identity in the entry. A human is refused when that queue reaches `Max_Build_Queue`; the inspected capacity branch does not apply that human limit to an AI. The shared queue is not a new independent queue for every factory. | debug build R4-E04; WPR-30 | U-R4-01: queue ownership | L-B1/P2; L6 queue display; P3 AI purchasing |
| LR4-03 | Ordinary non-pay-as-you-go purchasing checks available credits, debits at acceptance and appends the entry. The producer's tactical build-time bonuses are sampled for that entry. Explicit cancellation and loss-of-validity refunds retain WPR-20/31 and positive AI credit adjustment WPR-12; an automatic invalidation is not equivalent to the player's Cancel command. | debug build R4-E04; WPR-20/30/31 | U-R4-01: transaction boundary | L-B1/P2; L6 cancel/refund feedback |
| LR4-04 | The shared duration query selects `Land_Build_Time_Multiplier` for an AI in land mode, rather than the space multiplier. Normal duration combines `Tactical_Build_Time_Seconds`, `Tactical_Build_Time_Multiplier`, the selected AI adjustment, per-product time modifiers and, when a campaign parent exists, the faction's corruption tactical-production time multiplier. The producer bonus is then applied at purchase. The tutorial-player branch returns five seconds before those normal factors. | debug build R4-E04; WPR-33 | U-R4-01: land duration | L-B1/P2; P3 difficulty; P6 campaign context |
| LR4-05 | Queue completion sends ordinary company/unit products to the reinforcement pool; upgrade products create a held object at the producer. `Tactical_Build_Increments_Tech_Level`, allied prerequisites and authored build limits remain the shared production rules. Stock land tech upgrades can deliberately use the units queue even though they are research products. Production completion therefore does not itself land a company beside its factory. | WPR-20..22/33; effective XML R4-D01; LC-42..51 | U-R4-01: output and tech boundary | L-B1/P2; L2 companies; landing work (legacy EAWR-1457); L6 pool |
| LR4-06 | The checked tactical queue requires a non-null allied producer and finds the product in that producer's tactical menu using the **producer owner's faction**. It also requires a tactical queue kind, passing build limits, positive valid price/time, and ownership of each tactical prerequisite. In land mode it rejects a producer whose `ION_STUN_EFFECT` behavior is service-enabled. The same predicate validates queued entries before completion, so this is an invalidation/refund path, not a demonstrated pause. Validation resolves the stored producer ID and does not search for another factory if it is gone. This predicate contains no base-power query; indirect outage effects remain U-R4-06. | debug build R4-E05/10 | U-R4-01: producer validity and land ion gate | L-B1/P2; P3; L6 refusal/removal |
| LR4-07 | Land unfinished-child initialization uses `Tactical_Build_Time_Seconds`, multiplied for an AI owner by its **land** build-time adjustment, then truncated to unsigned whole seconds; positive duration is required. It starts at **0.01 absolute hull**, stores a fixed finish frame and a per-frame hull increment from maximum hull divided by duration and logical FPS. This path does not apply the player queue's global or producer time-bonus factors. Damage and completion retain WBP-16's distinct rules. | debug build R4-E06; WBP-14..16, qualified to land | U-R4-01: construction timing | L-B1/P2; land pads (legacy EAWR-1458); P3 difficulty |

### Power and shield state

| Rule | Retail rule | Evidence summary | Question closed | Decision / consumer |
|---|---|---|---|---|
| LR4-08 | An active `Base_Power_Ability` adds a true, uniquely source-identified contribution to each allied player's battlefield power state. Removal removes that source's contribution; the relationship-change callback invokes removal and application again. This is source-tracked battlefield state, not an authored wattage/capacity budget. Complete capture/relationship callback ordering remains U-R4-07. | debug build R4-E01 | U-R4-02: supply representation | L-B1/P2; P3 allied context; P7 deterministic state |
| LR4-09 | The data chooses power suppliers. Stock Rebel/Empire generators and the Underworld palace author `Base_Power_Ability` with `Ground_Automatic` activation. Multiplayer generator variants inherit their supplier ability. A supplier must be discovered through its content/ability, rather than a hard-coded generator-name test or an assumption that every faction uses a separate plant. | effective XML R4-D01; debug build R4-E01 | U-R4-02: content source | L-B1/P2; L2 ability activation; P6 imported structures |
| LR4-10 | A base shield tests `Shield_Requires_Base_Power` before querying its owner's battlefield power. Dead or nonpositive-health owners fail. Outside the editor, neutral ownership and `Base_Shield_Always_Off` suppress it; forced-off state also suppresses it. `Requires_Base_Power` is a different consumer flag: the checked turret service uses it to clear targets and power down (FC-61). Do not apply one blanket outage rule to every building or every service. | debug build R4-E02; FC-61 | U-R4-02/03: consumer gates | L-B1/P2; land combat/targeting; L6 state feedback |
| LR4-11 | The public shield-up query reads the last committed shield state. An eligibility calculation and the actual raised state are different observations while recovery is pending. Bombing and orbital target checks consume active shield state using their existing geometry/relationship rules, LC-62/65. | debug build R4-E02; LC-62/65 | U-R4-03: observable state | L-B1/P2; support work (legacy EAWR-1459); L6 |
| LR4-12 | Every failed shield-status evaluation records the current logical frame as its last-down frame. An ordinary down-to-up transition waits while elapsed frames are **less than** logical FPS times `Base_Shield_Delay_Time`; equality passes that local test. A forced state refresh bypasses the wait. Stock delay is 10 seconds. This is a transition policy, not proof of the service interval or of a ten-second timer starting only at the original outage. | debug build R4-E02; effective XML R4-D01 | U-R4-03: recovery test | L-B1/P2; R1 owns poll/reset schedule; U-R4-08 capture |
| LR4-13 | Shield initialization seeds the remembered down frame to zero and performs a normal status update. State changes update the remembered up/down and upgrade flags, visual shield state, authored animations/colours and local relationship-qualified online/offline audio. A forced refresh or a changed up/down state recalculates the terrain contribution; an upgrade-colour change alone does not force that up/down recalculation. Initial runtime visibility and restored-state timing remain U-R4-08. | debug build R4-E02/03 | U-R4-03: state publication | L-B1/P2; L6 presentation; L1/L4 dynamic terrain |
| LR4-14 | The shield terrain contribution is rasterized from `Base_Shield_Radius` on the 20-unit land grid, with radius rounded by adding 0.5 before integer conversion. Circular boundary strips are clipped to the map. Up state adds secondary occupancy by **this shield** only when absent; down state removes only this shield's existing contribution. Do not mark the entire disk impassable or erase another overlapping shield's identity. Preserve the land movement-class shield distinctions in LC-08. | debug build R4-E03/09; LC-08 | U-R4-03: terrain input | L1 terrain; land path service (legacy EAWR-1451), L4/L5; L-B1 |

### Income and campaign boundary

| Rule | Retail rule | Evidence summary | Question closed | Decision / consumer |
|---|---|---|---|---|
| LR4-15 | Land income uses the shared source/recipient stream contract: active sources pay continuously per logical frame, not a lump sum only when their displayed interval closes. `Base_Income_Value`, `Base_Interval_In_Secs` and the separate income/interval modifier categories determine the rate; the bookkeeping close does not add the same money again. Setup suppresses payment. | WPR-10..16; effective XML R4-D01 | U-R4-04: arithmetic | L-B1/P2; L6 income display |
| LR4-16 | Stock mining-facility sources author 125 credits per 5 seconds; mineral processors 75 per 5 seconds; Rebel/Empire command centres 45 per 5 seconds; the Underworld palace 50 per 5 seconds. These are per-source authored examples, not a faction-independent passive stipend. Each inspected source sets `Split_Income_With_Allies=Yes`, `Split_Favors_Owner=No`, `Full_Amount_To_Everyone=Yes`. The reader gives each allied **playable** recipient the full amount; without ally splitting it registers only the source owner. If both split-style flags are true, owner-favouring style takes precedence over full-amount style. | debug build R4-E07; effective XML R4-D01; WPR-11 | U-R4-04: source and recipient data | L-B1/P2; P3 economic context; P7 allied ledgers |
| LR4-17 | The shared credit path scales positive AI additions, including refunds, by selected `Credit_Multiplier`; debits are not scaled. Multiplayer tactical balances have no skirmish-earned-credit ceiling. `Min_Skirmish_Credits`/`Max_Skirmish_Credits` bound the starting option, not later income. | WPR-01/12 | U-R4-04: ledger | L-B1/P2; L3 starting credits; P3 difficulty |
| LR4-18 | Campaign land funding uses its parent-account contract; WLAI-42's budget release and campaign funding work must not be replaced by minting skirmish money. Static standalone income/menu rules do not close campaign tactical-built persistence, survivor return or child checkpoint continuation. | WLAI-42; WGA-196..201; WGT-84; WGT-U12/U13 | U-R4-04: scope boundary only | campaign account switch (legacy EAWR-2641); P6 land entry/return |
| LR4-19 | Income activation requires the active mode to expose an income manager; the checked ability otherwise returns failure. Its own target predicate only requires a non-null source and no separate target. Neither the inspected application nor stream-allocation helper tests for a campaign parent. The mode getter returns its stored manager; the full mode installation/activation policy was not traced. Therefore the stock XML comment about campaign income is not, by itself, proof of a universal campaign suppression rule. Keep that gate unverified in U-R4-10. | debug build R4-E07/10/11; effective XML R4-D01 | U-R4-04: checked activation boundary only | L-B1/P2 standalone income; P6 manager/funding boundary |

### Prebuilt bases and structure lifetime

| Rule | Retail rule | Evidence summary | Question closed | Decision / consumer |
|---|---|---|---|---|
| LR4-20 | Prebuilt bases and free starting units are separate options. `MP_Default_Pre_Built_Base` and `MP_Default_Free_Starting_Units` both author yes in stock defaults, but neither implies the other. The shared options dialog edits a copy and commits on Accept; Defaults and Cancel have distinct effects. Space hiding the prebuilt control does not clear the stored flag and is not evidence for a land start policy. | WSS-22/24/27; effective XML R4-D01 | U-R4-05: option identity | L3/P1; L-B1/P2; L6 options; P7 setup |
| LR4-21 | In multiplayer tactical startup the pending automatic child requires **both** prebuilt bases enabled and `Respects_Skirmish_Pre_Build_Bases_Option` true on the producer. Either false leaves that automatic child uncreated and requests the pad's sell animation. The stock ordinary faction build pads explicitly set the respect flag false, preventing automatic filling even with prebuilt enabled. This gate is not a universal instruction to remove already existing buildings. Complete layouts and absent-tag defaults remain U-R4-11; space-station ambiguity SS-U3 remains separate. | debug build R4-E11; WSS-27; effective XML R4-D01 | U-R4-05: exact per-type gate | L3/P1; L-B1/P2; land pads (legacy EAWR-1458) |
| LR4-22 | Land skirmish setup uses team/start and structure-position marker data; map-object ownership remap and player-queue creation are part of startup, not a hard-coded list of completed bases. WLEN-54/55 establish that boundary. The fresh remap read creates missing per-player tactical queues and sets actual tech to 1, then recomputes tactical capacity. Preserve faction/owner identity and let the data resolve the structures; prebuilt/free-force choices feed their respective creation paths. | debug build R4-E08; WLEN-54/55; WSS-28 | U-R4-05: setup boundary | L3/P1; L2 content; L-B1/P2; P7 seats |
| LR4-23 | A pad construction request/execution has its own occupancy, allied ownership, cooldown, affordability and child-creation rules, WBP-09..19. It does not consume the player's unit/upgrade queue or reinforcement population. Human execution debits before creating the unfinished child; AI reservation/debit/failure refund follows WBP-47/48 and WLAI-39. Do not promise player-queue cancellation/refund semantics for a destroyed unfinished structure. | debug build summarized by WBP-09..19/47/48 and WLAI-39; effective XML R4-D01 | U-R4-01: construction transaction | L-B1/P2; land pads (legacy EAWR-1458); P3 AI |
| LR4-24 | A held research object is distinct from the building holding it and from historical build counts. The shared combat holder-loss path searches for a live eligible allied holder whose mode-selected build list for the upgrade owner's faction offers that research; if none exists it destroys the research and withdraws its ability sources. WPR-57 does not establish every land ownership-change/direct-removal path or holder-search tie. Retain those limits rather than universally keeping or universally deleting research when one land factory dies. | WPR-57 | U-R4-01: held-object boundary | L-B1/P2; L2 upgrade types; P6 persistence; U-R4-06 |
| LR4-25 | Outside the editor, automatic construction requires a pending starting-child selection and no unfinished or completed child. After the multiplayer gate, an unfinished selected type resolves directly to its `Tactical_Buildable_Constructed` type. The completed object is created at the pad's built-object position/facing for the pad owner, with parent/sale/detachment links and the pad's garrison-spawn permission; success clears the pending selection. This path performs no player-queue purchase, credit debit or timed unfinished construction. The producer of the pending starting selection remains U-R4-11; do not infer that it is always the first menu item. | debug build R4-E11 | U-R4-05: automatic child creation | L3/P1; L-B1/P2; land pads (legacy EAWR-1458); L6 |
| LR4-26 | Before that automatic-child test, a fresh non-multiplayer tactical load can reconstruct a persisted built object from its strategic parent. The inspected route queries the conflict planet at the pad's position, creates the recorded type for the recorded player, and links it to the pad. Saved-game loading skips this fresh-parent reconstruction route. The multiplayer prebuilt checkbox is therefore not a substitute for importing campaign base state. This narrows the reconstruction boundary of WGT-U12 without closing its writer/return/save questions. | debug build R4-E11 | U-R4-05: campaign reconstruction boundary; WGT-U12 partly narrowed | P6 land entry/return; L-B1 structure identity |

## Unverified and exact later captures

The following are retained evidence tasks, not invented defaults. Run them later
on an original-game host through the existing in-battle Lua/debugger harness.
Use controlled content for boundary cases, suspend unrelated AI/spending, record
logical frames and object/producer/owner identities, and keep an uninterrupted
control beside each save/load case. Use fog off except when testing fog admission.
No rig/GPU run is part of this walk.

| Question | What remains unverified | Capture/read that settles it | Consumer |
|---|---|---|---|
| U-R4-06 | Complete producer-loss, ownership-change, queue cancellation and held-upgrade lifecycle on land, including alternate allied holders and AI positive refunds. Shared queue/holder rules do not prove every land callback. | Two allied completed factories with the same offer, another with a different offer, human and AI buyers. Fill both player queues; destroy, transfer or directly remove the recorded producer just before a front's completion, then repeat with only power lost. Record entry producer, validity, credit debit/refund, held-upgrade owner/holder, pool and tech before/after and at the next two admitted frames. Test ordinary Cancel separately. | L-B1/P2; P3 AI; P6 persistence |
| U-R4-07 | All base-power removal, transfer, alliance-change, disable and restore producers. A relationship callback is not proof of every notification's ordering. | Controlled land map with two independent allied suppliers and a third enemy supplier. Remove one, then the last; transfer the surviving supplier and change the alliance in separate runs. Repeat ordinary ability disable, isolated ion stun and pre-transition save/load. Record each supplier's active contribution, each player's aggregate power and an independently flagged turret and shield. Distinguish a source that is disabled from one whose contribution was actually removed. | L-B1/P2; P7 state/restore |
| U-R4-08 | Full initial, forced-off and restored shield timing; absent-tag defaults; power/state publication relative to other same-frame services. The local recovery comparison is sourced, its runtime observation phase is not. | Build a shield with power already present and restore power after a long outage. Log eligibility, actual up/down state and last-down frame at every admitted poll, then at last-failed-poll +299/+300/+301 stock frames. Remove power one frame before versus after the candidate raise. Repeat forced off/on, construction completion, neutral/owned transfer and save/load just before equality. A controlled type matrix omits then explicitly sets `Shield_Requires_Base_Power` and `Base_Shield_Always_Off`. If the harness cannot expose the reference frame, retain that part as unknown. | L-B1/P2; R1; L6 |
| U-R4-09 | Full shield projectile interception, inner/outer direction, upgraded permeability and movement/vulnerability consequences; overlaps, height and raster-edge identity. A state/terrain routine alone does not prove all these outcomes. | One active stock shield and paired hostile/allied direct and arcing shots, source/target inside and outside, then shield upgrade on/off and power off/on. Log hit object/mesh, generator and target hull, projectile lifetime and actual shield state. Walk infantry, vehicle and no-shield movement classes across the same radius from both directions; record position/speed, passability cells and damage. Repeat overlapping shields and points on either side of one boundary cell. Coordinate collision with R2 and pathing with R3 rather than inventing a force-field rule. | L-B1/P2; L1/L4/L5; combat/support (legacy EAWR-1454, EAWR-1459) |
| U-R4-10 | Runtime stream activation/termination and recipient rebuilding at construction, setup, transfer and alliance changes; complete mode income-manager installation/activation in campaign and forced-skirmish variants. Stock XML rates and comments do not settle those mode gates. | Trace the mode's manager initialization and automatic-ability activation selector, then in standalone land use one command building, one mine and one processor, first separately then together; run human and Easy/Normal/Hard AI recipients with one ally. Record exact ledger deltas each frame before Begin, for the first admitted second and across source completion/death, owner transfer and alliance change. Repeat in an ordinary campaign child and a controlled forced-skirmish case, keeping parent and tactical accounts distinct. Log manager presence, ability activation, actual recipients and modifiers, not only rounded HUD income. | L-B1/P2; P3; campaign account switch (legacy EAWR-2641) |
| U-R4-11 | Producer of the pending starting-child selection, complete visible layouts and marker/ownership behavior for each faction, local/LAN starts, custom maps and omitted content fields. Space-station ambiguity SS-U3 is outside a land-only result. | Trace writes of the pending starting selection back to marker/map loading. On one fixed stock land map, run Rebel, Empire and Underworld starts for all four combinations of prebuilt bases and free starting units. Record copied options, selected markers, pending selection, pad/unfinished/completed identities and owners, credits/tech, queues and power before Begin and first admitted frame. Repeat a LAN start. A controlled mod uses otherwise identical pads with the prebuild-respect flag omitted/true/false and two distinct starting selections; verify the resulting completed type and absence of an automatic purchase debit. | L3/P1; L-B1/P2; L6; P7 |
| U-R4-12 | Campaign land base reconstruction, tactical-built persistence, survivor/upgrade return and child checkpoint continuation. This walk does not close the existing campaign questions. | One campaign land invasion: construct a pad structure and research, save/reload the live child, then complete separate attacker win, defender win and retreat runs. Compare parent base inventory, tactical-built records, held upgrade ownership, returned companies and restored strategic/tactical accounts. Use WGT-U12/U13, U-LC-8 and U-WLAI-5 as the owning rows rather than declaring them closed here. | P6 land entry/return; campaign funding (legacy EAWR-2641) |

## Implementation handoff

L-B1 can reuse the player ledger, product queue, reinforcement pool, modifier and
pad-construction contracts. It needs land producer and source registration,
data-selected power abilities, shield state with terrain effects, and the land
start's selected prebuilt option. L3 supplies marker/owner setup; L2 supplies land
types and ability activation; the structures/pads work supplies physical children
(legacy EAWR-1458). L6 displays these authoritative states and queues.

Keep per-entity source/consumer work in partitioned phases over immutable inputs
with disjoint outputs and ordered commits. Preserve the shared serial ledger and
queue transaction boundary; do not introduce a second economy owner. New land
power/shield/producer/held-upgrade state needs replay/digest/checkpoint coverage and
1/2/4/8-worker equality when implemented. R1 determines the phase dependencies;
this documentation change supplies no new phase ordering.

The existing tag registry is not changed by research. Loading or applying the
land tags in a later implementation must update the relevant class/tag rows with
the actual consumer and these rules; a parsed tag or this note alone is not an
`applied` status.
