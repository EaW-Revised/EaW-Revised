# Galactic conquest: economy and production

## Scope and evidence

This walk covers FoC galactic conquest accounts, fiscal income, planetary ground
and space queues, production admission, prices and durations, cancellation,
population, starbase progression, and the economic effects of corruption and
black-market purchases. Campaign-specific story scripts are outside the walk;
their calls into these systems are recorded as interfaces. Tactical income,
reinforcement pools and tactical station upgrades remain in the
[production walk](production.md). Fleet movement, conflicts and the return from
battle belong to the movement/control walk; AI plan and budget decisions belong
to the GC AI walk.

Sources are **debug build**, read-only queries, and **data**, the effective FoC
XML layer. Opaque evidence IDs **GEP-E01** onward identify private research;
the symbol/address map and raw reads remain in ignored research output. Bounded
debug-build campaign captures are identified separately below.
**Unverified** entries are questions, never inferred
original-game rules. XML examples below describe effective values rather than
comments in the data, several of which disagree with those values.

Wave-4 evidence **G4-E01** is a fresh read-only debug-build pass for the political curve. Its private evidence map and raw reads remain in ignored research storage.

Capture build-list evidence **CBL-E01..03** rechecks WGEP-18 and WGM-57: the
production gate reads affiliation from the queried builder's faction before
checking current/target planetary control. An ordinary victorious land invasion
clears the galactic ground-special list; this differs from retaining the former
faction's facilities as working captured producers. The removed facilities
cannot supply `Required_Special_Structures`, and that prerequisite never bypasses
the produced type's own affiliation gate (WGEP-21). Tactical map-object capture
and winner-owned tactical persistence are separate paths (WGT-84, WGM-57).
Private reads remain in ignored research output; these are static debug-build
facts, not a retail runtime capture.

Capture implementation evidence **CPI-01..03** rechecks WGEP-05's income gate:
actual planetary income first removes expired Boolean blocking modifiers and
combines their values with logical OR. A true result returns zero before income
source refresh or siphon subtraction. The inspected source writer is the
credit-halt sabotage route: it registers true under its unique ability source
for the authored halt duration. This source ledger is distinct from orbital
blockade and political ownership; the inspected income consumer does not read
the orbital blockade flag. Wiring that flag directly to zero income would
therefore be an inference. Source-scoped blocking and its sabotage writer remain
the named WGEP-05 / WGM-PC-U02 integration seam (legacy EAWR-2804), now implemented as WGEP-163..165.
These read-only results are not an end-to-end sabotage or blockade capture.

Wave-5 evidence **G5-E01..06** covers cancellation listeners, AI reservation/payment, population census, reduction filters/ownership, political-source updates and gambling RNG/lifecycle. The private map and raw reads stay in ignored research storage. Data evidence **G5-X01** audits winning registered effective XML population/capacity occurrences and variant chains. Reused queue-lane evidence **GQP-01..11** supplies UI refund selection, recorded-builder assertion, completion-before-billing and the empty human unification hook. No capture recipe below is observed evidence.

There is no galactic simulation implementation in the inspected checkout. Every
rule below is **missing in ours** for GC, including shared concepts already used
tactically. The planned integration column identifies a reuse point, not a claim
that its tactical behavior already implements the GC rule.

## Service and interface order

The galactic mode refreshes planetary visibility and runs ordinary mode service
before checking the fiscal deadline. A due cycle pays players, moves the fiscal
window forward, increments the cycle number, rerolls black-market planetary
income, updates the credit display, and tests cycle-dependent victory conditions.
The fiscal payment is a discrete cycle operation, unlike tactical income streams.

A planet's production service first revalidates ground then space work, services
the two AI planetary build queues, then visits the ground queue followed by the
space queue. Each queue builds only its front. The global ordering of commands,
planet services, player services and paused/tactical mode time is a shared frame
loop interface; unresolved details are U-01/U-02 below. Within the inspected
production and fiscal routines, the order specified in the rules is verified.

## Rules

### Fiscal service and accounts

| ID | Rule and conditions | Source | Planned integration; GC status |
|---|---|---|---|
| WGEP-01 | After ordinary mode service, a frame timer at or beyond the fiscal end processes **one** cycle in that service. Then the previous end becomes the new start, the duration is added to the old end, the cycle number rises, and black-market influences update. There is no catch-up loop in this routine. | debug build GEP-E01/E02 | new galactic scheduler; missing |
| WGEP-02 | Fiscal duration is obtained through the frame synchronizer from `Fiscal_Cycle_Time_In_Secs` (**45.0** in stock XML). Convert seconds to logical frames by nearest-integer rounding of `seconds × logical_frames_per_second`, with half ties away from zero. The deadline uses logical frames; initial-cycle boundaries and time advancement are U-01. | debug build GEP-E02/E25; data | economy_rules, new scheduler, save/replay; missing |
| WGEP-03 | Fiscal service visits players in player-list index order. A null, neutral, nonplayable, or non-planet-controlling faction earns zero from the planet aggregation. A nonzero aggregate is added once through the account's difficulty/cap adjustment; an initialized galactic AI budget is then notified with the original aggregate. A zero aggregate skips both addition and budget notification. WGEP-112 rechecks the account-before-budget ordering. | debug build GEP-E03/E04; GER-E15 | economy_rules, AI engine; missing |
| WGEP-04 | For each planet owned by the player, aggregate its actual income. For other planets, corruption income is possible only for a faction with `Benefits_From_Corruption`. Independently add income assigned to this player's siphon and positive piracy income for a corruption-benefiting faction. Actual fiscal collection resets consumed piracy income; a preview does not. | debug build GEP-E04 | economy_rules, new corruption state; missing |
| WGEP-05 | Basic income reads the planet's current base credit value; a destroyed planet instead reads `Planet_Destroyed_Credit_Value`. Without planetary state, use `Planet_Credit_Value`. An income-blocked planet's actual income is zero. Otherwise remove expired modifiers, refresh base/alignment/political/trade contributions, sum the income modifiers, and subtract any applicable siphon. | debug build GEP-E05/E06; data | economy_rules, new planet state; missing |
| WGEP-06 | The base contribution is basic income multiplied by the stored black-market income multiplier. Alignment, political, trade and structure bonuses are separate additive contributions based on **unmultiplied** basic income. Do not multiply the complete income sum by the black-market factor. | debug build GEP-E06/E07 | economy_rules; missing |
| WGEP-07 | Planet/faction alignment adds `basic × alignment_bonus × MaxCreditIncomeAlignmentBonus` when that product is positive; otherwise a positive penalty subtracts `basic × alignment_penalty × MaxCreditIncomeAlignmentPenalty`. Zero removes the old alignment contribution. Both maximum constants are **0.50**; authored bonus and penalty are validated in the range 0..1 and are not both positive. | debug build GEP-E07; data | economy_rules; missing |
| WGEP-08 | Nonneutral planetary political control is evaluated through `Political_Income_Curve` (**0,0; 1,1; 5,1**), with piecewise-linear interpolation under WGEP-63. A positive curve value contributes `basic × (curve_value − 1)`; a zero contribution removes its old source. The inspected nonpositive-curve and neutral branches return zero without replacing that source. WGEP-75 sources the income refresh and alignment relations; complete transition cleanup and duplicate points remain U-03. | debug build GEP-E08; G4-E01; data | economy_rules, movement/control ownership interface; missing |
| WGEP-09 | For each valid trade link to an allied endpoint, add its positive `Credit_Gain_Factor` to the route factor sum; the planet's trade contribution is `basic × factor_sum`. A zero contribution removes its old source. Actual income does not use the AI's hypothetical all-links preview. | debug build GEP-E09; data | economy_rules, galaxy/trade-route model; missing |
| WGEP-10 | A planetary income bonus adds `basic × Percentage_Income_Modifier + Absolute_Income_Modifier`, multiplied by `Additional_Multiplier_On_Mining_Colonies` on `Planet_Is_Mining_Colony`. The target must be the source planet itself or the source's current planet. The reader uses WGEP-05's destroyed-aware basic income; the target predicate has no destroyed-planet rejection. Ruins source termination still applies. A nonplanet source loses its effect when its owner loses that planet, and gains it when its owner gains that planet; ending the effect removes its unique source. Stock mining facilities author **2**, **0**, and **1.5**: +200% basic income before the colony multiplier, despite stale XML comments. | debug build GEP-E10; GRI-E02..04; data | economy_rules, Lua bindings/ability lifecycle; missing |
| WGEP-11 | A gambling income service finds its current planet, draws a win/loss sample, then draws a signed factor from **[0, Max_Income_Modifier]** on a win or **[Min_Income_Modifier, 0]** on a loss. It replaces this source's contribution with `basic × factor`, using WGEP-05's destroyed-aware basic income. This service has no destroyed-planet rejection; source termination during ruins is separate. Stock arena data authors **Chance_To_Win 0.80**, **Min −0.20**, **Max 0.60**, and **Evaluation_Interval_In_Secs 30.0**; WGEP-76/77 sources synchronized randomness and callbacks; first-service qualification remains U-04 at the galactic frame schedule boundary. | debug build GEP-E11; GRI-E01/04; data | economy_rules, ability service, save/replay RNG; missing |
| WGEP-12 | A siphon percentage is validated in **0..1**; an active siphon records `income × percentage` and removes that amount from the owner's income. An absent recipient returns zero. On a noncorrupted planet it expires only when current frame is **strictly greater** than its end; corruption bypasses that expiry test. The corruption-income preview requests the total **before** siphoning. | debug build GEP-E12/E04 | economy_rules, new corruption state; missing |
| WGEP-13 | On a corrupted nonowned planet, a corruption-benefiting faction receives `pre-siphon_actual_income × Corruption_Choice_Income_Percentage[type]` only when the percentage and resulting product are positive. Stock percentages, in authored corruption order, are **0.5, 0.8, 0.4, 4.5, 0.5, 0.5, 0.8, 1.0**. These are multipliers, including racketeering's **4.5**, not percentages divided by 100. The owner still follows its ordinary income path. | debug build GEP-E04; data | economy_rules, corruption state; missing |
| WGEP-14 | After the cycle payment, each planet with `Planet_Black_Market_Influences` gets a synchronized random multiplier from `Black_Market_Income_Mult_Min` (**1.0**) to `Black_Market_Income_Mult_Max` (**3.0**); a destroyed affected planet is reset to **1.0**. This new multiplier affects the next payment. This planet-income feature is separate from buying black-market technology. | debug build GEP-E13; data | economy_rules, save/replay RNG; missing |
| WGEP-15 | The inspected fiscal calculation/payment path has **no progressive tax, redistribution, or unit-maintenance debit**; its bureaucracy-tax refresh is empty. XML nevertheless authors `Progressive_Taxation` (**1,0; 5,0.2; 10,0.5**), `Income_Redistribution` (**0.6**) and faction `Maintenance_Cost` (**0.25** for the three playable factions). These values do not establish a fiscal charge. No claim is made that unrelated mod/script credit debits are impossible. | debug build GEP-E03/E04/E14; data | economy_rules; missing GC path, preserve absence of charges |
| WGEP-16 | Positive account additions for an AI multiply by difficulty `Credit_Multiplier` (**Easy 0.5, Normal 1.0, Hard 1.2**), including refunds. Debits are unmultiplied. Clamp the balance at zero and its active credit cap, then test credit-dependent victory and notify story accumulation with the whole balance. A refund marked as a windfall notifies AI with the actual adjusted delta. | debug build GEP-E15; data | economy_rules, AI engine, Lua bindings; missing |
| WGEP-17 | GC credit-cap refresh sets `max(1, owned_planet_count) × Credit_Cap_Per_Planet` (**40000.0**) and immediately clamps an excess balance. Multiplayer tactical rules use no cap, and forced tactical skirmish rules bypass an existing cap on addition; neither exception removes the GC cap. Ownership-refresh timing is U-02. | debug build GEP-E16; data | economy_rules, movement/control events, save/replay; missing |

### Admission, price and duration

These gates are the purchase path's order. Failed admission changes no account
or queue. Deferred work is checked through WGEP-35 rather than rerunning every
gate here.

| ID | Rule and conditions | Source | Planned integration; GC status |
|---|---|---|---|
| WGEP-18 | A nonnull type and producer with production support are required. Reject cinematic-only types and types whose `Affiliation` excludes the **queried builder's faction**, a location-adjusted whole price below **1**, negative location-adjusted time, direct ground-base types, and types classified as neither ground nor space production. Affiliation is independent of the producer's previous owner; after conquest the capturer queries its own faction list. A purchase checking affordability compares the price with whole available credits unless pay-as-you-go applies. | debug build GEP-E17; CBL-E01 | `src/sim/galactic/production.cpp` reads the queried player from WGEP-147's complete registered affiliation table; unregistered primitive fixtures supply their explicit offer flag. |
| WGEP-19 | A type with `Build_Advances_Tech_Level` additionally requires current player tech **equal** to its `Tech_Level`, and its target level no greater than the player's maximum. Then reject the explicit locked list; an initially locked type requires membership in the explicit unlocked list. Explicit locking takes precedence. | debug build GEP-E17 | production, Lua bindings, save/replay locks; missing |
| WGEP-20 | The planet's current controlling player must be the builder, and target control must equal current control. Reject direct company/squadron members and directly built transport wrappers; reject a type restricted by this planet. For space work, a present starbase must have the same owner as ground control; an absent owner does not itself fail this check. | debug build GEP-E17 | production, ownership model; missing |
| WGEP-21 | `Required_Special_Structures` must pass local presence checks. The parsed list supports OR-linked alternatives within groups, with separate groups required together. Next, every `Required_Orbiting_Units` entry must be found by exact type among the attached fleets' contents, excluding pending-deletion units. This reader does not perform a second owner or ally test on those contents. | debug build GEP-E17/E18/E19; data | production, galaxy fleet index; missing |
| WGEP-22 | Require player tech at least `Tech_Level`, ground base at least `Required_Ground_Base_Level`, and starbase at least `Required_Star_Base_Level`, in that order. Base levels are validated **0..5**. These are independent gates; a high station level does not raise global tech. | debug build GEP-E17 | production, economy_rules; missing |
| WGEP-23 | Ground and orbital structures reserve separate planet slots: completed count plus queued count must be below the corresponding live land/space capacity. Planetary state supplies the capacity when present; otherwise use type `Special_Structures_Land` or `Special_Structures_Space`. Then require this planet in `Required_Planets` when that list is nonempty. Orbital structures additionally require a starbase level of at least **1**. | debug build GEP-E17/E20; data | production, new planet slot ledger; missing |
| WGEP-24 | A starbase purchase must be exactly the next buildable `Base_Level`, fit the planet's `Max_Space_Base`, and have no qualifying nonowner fleet in orbit. The blocking fleet is not pending deletion, does not skip space combat, and declares that its presence cancels starbase production. A fresh fleet constructs that declaration as true; the traced AI unit-hijack route can change it. The check uses **different owner**, not the ally relationship. | debug build GEP-E17/E21, CSP-E05; data | production admission now consumes explicit fleet blocker declarations and live fleet owner/orbit through `ProductionSetup::fleet_blockers`. Pending deletion, skip-combat and cancellation declarations remain owning-service inputs. The absent declaration table preserves the legacy setup-only `starbase_blocked` gate; a present table selects the live check. |
| WGEP-25 | Ground and orbital structures with nonnegative `Build_Max_Instances_Per_Planet` require completed plus queued instances of their exact type below that limit. `Build_Limit_Lifetime_Per_Player` rejects **0** or a positive limit already reached by historical completions. `Build_Limit_Current_Per_Player` rejects **0** or a positive limit reached by owned plus currently producing instances. Negative values are unrestricted. The inspected GC admission path does not read the two tactical all-allies limits. | debug build GEP-E17 | production, shared production_allowed concepts; missing |
| WGEP-26 | A surface-inaccessible planet rejects ordinary ground company, ground-base and ground-structure production, with the inspected generic-hero exception. `Build_Requires_Initial_Placement` requires that type in the planet's initially placed set. Ground-base purchases have already been rejected by WGEP-18; this later check does not enable them. | debug build GEP-E17 | production, validated planet tables; missing |
| WGEP-27 | After those gates, buy admission checks human queue length against `Max_Build_Queue` (**5**, the default established by WPR-30); AI has no such limit. It then checks global population room including already queued production. Either failure leaves credits and queues unchanged. | debug build GEP-E22; WPR-30/33 | production, economy_rules; missing |
| WGEP-28 | Compute the location-adjusted price, duration, builder ID, build ID and location ID for the new entry. A human without pay-as-you-go is charged the full price immediately; append the entry to the planet's selected queue, then emit production-begin. Humans use `Pay_As_You_Go` (**false** in stock); every **nonhuman player always uses pay-as-you-go**, independently of that constant. | debug build GEP-E22/E23; data | production, AI engine, save/replay; missing |
| WGEP-29 | Price before rounding is `Build_Cost_Credits × production_price_multiplier × corruption_price_multiplier × tech_purchase_multiplier`. Non-corruption-benefiting factions select their corruption multiplier by the planet's corrupted flag; Rebel/Empire author **1.0 / 0.9**. Corruption-benefiting factions use **1.0** on this route. The tech-purchase multiplier applies only to `Build_Advances_Tech_Level` types. | debug build GEP-E24; data | economy_rules, production; missing |
| WGEP-30 | The price getter raises a positive result below **1.0** to **1.0**, then rounds to the nearest integer, with half ties away from zero. The tech-purchase multiplier is **1 + sum of unexpired simple modifiers**; an invalid nonpositive multiplier is replaced with **0.0001**. | debug build GEP-E25/E26 | economy_rules; missing |
| WGEP-31 | For price and time reductions, merge the player's global per-type list with the planet's per-type list after expiry cleanup. Reduce each stacking category with the greater-value comparator and sum the category winners; return **1 − sum**. Price reduction of at least **1** is replaced by **0.999999**; the inspected time reducer has no corresponding clamp. Empty lists give multiplier **1**. | debug build GEP-E27 | economy_rules, production modifier cache; missing |
| WGEP-32 | Ordinary duration is `Build_Time_Seconds × production_time_multiplier × base_vs_tech_multiplier × AI_galactic_multiplier × corruption_time_multiplier / factory_count`. A tutorial player uses exactly **5 seconds** instead. AI difficulty `Galactic_Build_Time_Multiplier` is **Easy 1.5, Normal 0.8, Hard 0.6**. Humans use **1.0**. Rebel/Empire corruption time factors are **1.0 / 0.95**; corruption-benefiting factions use **1.0**. | debug build GEP-E28; data | economy_rules, production, AI engine; missing |
| WGEP-33 | Ground production uses no base-vs-tech speedup. For space work at a station level in **1..5**, set `d = base_level − type.Tech_Level − 1`; negative d uses **1.0**, and d **0,1,2,3,≥4** uses `Production_Speed_Mod_Base_Vs_Tech_0..4`, respectively **1.00, 0.90, 0.75, 0.55, 0.45**. These are time multipliers. | debug build GEP-E28; data | economy_rules; missing |
| WGEP-34 | Only a type with `Build_Time_Reduced_By_Multiple_Factories` counts completed instances of its `Required_Special_Structures` types at the planet. Sum those counts and divide duration by **max(1, sum)**. Unfinished factories contribute nothing to this divisor. | debug build GEP-E28; data | economy_rules, planet structure census; missing |

### Planet queue service, completion and cancellation

| ID | Rule and conditions | Source | Planned integration; GC status |
|---|---|---|---|
| WGEP-35 | Before building, traverse ground entries backwards, then space entries backwards. Cancel an entry when local required structures no longer pass, its respective required base level exceeds the live level, or an orbiting-unit requirement fails. Emit cancellation, remove it, and restart the next front if index zero was removed. This sweep does **not** rerun price, tech, lock, population or current/lifetime limit admission. No direct refund is called here; cancellation listeners are U-06. | debug build GEP-E29 | production, movement/control events; missing |
| WGEP-36 | Service the ground AI planetary queue, then space AI planetary queue; afterwards service actual ground then actual space production. Structures and units of one domain share its queue. There is no third independent structures queue. AI decisions and prequeue reservations belong to the GC AI walk. | debug build GEP-E30 | production, AI engine; missing |
| WGEP-37 | A front space squadron, orbital structure, fleet or ship requires the starbase owner to equal its recorded builder. If that owner differs, emit cancellation, remove the front, start its successor, and return from the production service, skipping later work that service. This path calls no direct refund. | debug build GEP-E30 | production, ownership events; missing |
| WGEP-38 | For a front starbase, when no space conflict is pending, any orbiting fleet satisfying WGEP-24's blocker conditions cancels it, emits cancellation, starts its successor and returns from the service. The local builder also gets interrupted-production advisor text. A pending conflict skips this fleet-cancellation check. Direct refund is absent; U-06 covers listeners. | debug build GEP-E30 | production now stages the live orbit check in its partitioned phase and commits front cancellation before completion/billing, without a direct refund. Pending conflict skips the check. Existing cancellation events feed presentation; local advisor text and cancellation listeners remain their owning-service seams. |
| WGEP-39 | Only the front completes or pays. An expired ground front is held completed in the queue if landed transports are at least `Max_Ground_Forces_On_Planet` (**10**) and a nonnull occupying orbit player differs from the occupying ground player. This condition is applied to the ground front without first distinguishing units from structures. Removing the hold permits normal completion. | debug build GEP-E30; data | production, movement/control occupancy; missing |
| WGEP-40 | On completion, remove the front, emit the completion UI/audio work, then either place its result or set tech to `Build_Advances_Tech_Level`. Increment its historical build count and emit production-finished. Human-created orbiting results call an empty unification hook; this call proves no automatic fleet merge (debug build GQP-11). Record paid build cost on the result, or the first result inside a created fleet, and start a successor if present. A failed ordinary placement calls the refund helper; structure/base results are allowed to return no ordinary object. | debug build GEP-E30 | production, Lua bindings, fleet model, save/replay; missing |
| WGEP-41 | Creation dispatches starbase data, ground structure, orbital structure, ground company, squadron and single-ship fleet to their distinct placement routes. A starbase completion records its authored level/type/owner; creation of level **1** clears orbital special structures. Base completion and successfully created ordinary results notify story construction. An additional result-type reference recursively creates an additional result; its authoring route is U-13. Fleet landing, squadron assembly and tactical persistence remain interface boundaries. | debug build GEP-E31 | production, galaxy model, Lua bindings; missing |
| WGEP-42 | `Build_Advances_Tech_Level` is an unsigned target level (fresh **0** means none), not a Boolean. Tech completion sets the **authored target level**, rather than incrementing by one, clamps it to **0..player_max**, and notifies story tech-level events. With unlock processing enabled, affiliated initially locked slicer-unlockable types whose `Tech_Level` is **strictly lower** than the new player level enter the unlocked list. New-options audio is local and requires an actual new unlock. | debug build GEP-E32 | economy_rules, production, Lua bindings; missing |
| WGEP-43 | For unfinished pay-as-you-go work, due bill is `stored_price × (last_serviced_remaining − current_remaining) / full_duration`, with no rounding here. A human pays only if credits cover it; AI delegates payment to its planetary AI queue. Failure resets the timer to last serviced remaining seconds. Successful work updates countdown notifications and the stored remaining value. WGEP-70/71 sources AI reservation/payment. Completion precedes this unfinished-work branch, so completed fronts have no final catch-up bill (debug build GQP-08..10); end-to-end ledger qualification remains U-07. | debug build GEP-E30/E48 | production, AI engine; missing |
| WGEP-44 | An entry records its duration and paid price when bought. Its construction timer is initialized then; when promoted to front, reinitialize it from the stored full duration, so time spent waiting does not build it. Entry service only updates last remaining seconds. No current-price or current-duration recalculation occurs in this service; modifier-driven queue updates remain U-08. | debug build GEP-E33/E34 | production, save/replay; missing |
| WGEP-45 | Indexed cancel requires the indexed entry's type to match the command type. If requested, refund before emitting cancellation and removing the entry. Canceling the front restarts the next front; canceling a later entry does not reset the running front. Wrong type or absent index fails without mutation. The human/UI sender requests a refund and prepares domain, type and index. Strategic execution has no current-location-owner check; refund validation is WGEP-46, and complete automatic listener effects remain U-06. | debug build GEP-E35; GQP-01..06 | production, command/Lua bindings; missing |
| WGEP-46 | Refund amount is the stored full price for prepaid work, or stored price multiplied by `clamp(1 − last_serviced_remaining / full_duration, 0, 1)` for pay-as-you-go. A missing recorded builder gives zero. The explicit refund helper asserts that the recipient ID equals the recorded builder; it has no mismatch return or zero-amount branch before adding the computed amount to the supplied recipient. Mismatched commands are debug-invalid input and remain a capture qualification, not a verified ownership-rejection rule. Refund passes through WGEP-16 and is an AI windfall; it uses the stored service value and is not recalculated from today's modifiers. | debug build GEP-E36/E48; GQP-07 | economy_rules, production, AI engine; missing |
| WGEP-47 | Completion first signals the command bar and queues authored completion music. Build-complete speech takes precedence over local `SFXEvent_Build_Complete`; local strategic fallback also activates the planet completion ping. Cancel speech similarly precedes local `SFXEvent_Build_Cancelled`. Countdown speech checks minute transitions at **60 s or more**, ten-second transitions at **10 s or more but below 60 s**, then second transitions below **10 s**. WGEP-95..98 source exact naming and producer-side ownership; playback qualification remains U-09. | debug build GEP-E30/E35/E37; GUW5-E01..03 | production events, presentation/audio; missing |

### Population, starbases, modifiers and black-market interface

| ID | Rule and conditions | Source | Planned integration; GC status |
|---|---|---|---|
| WGEP-48 | GC maximum population is faction base capacity plus `Additional_Population_Capacity` from eligible live, non-deleting owned objects. For owned planets, also add the current starbase type's capacity. Faction base capacity defaults to **0**; `Base_Population_Capacity` is parsed but absent from stock XML. An AI using the cap skips sources under a planet unusable by that AI. Humans always use the cap; AI can bypass it through `AI_Uses_Galactic_Pop_Cap`, whose false default is now verified in WGEP-62. | debug build GEP-E38/E47/E49; data | economy_rules, galaxy ownership census; missing |
| WGEP-49 | Global usage sums owned objects' **original type** GC `Population_Value`, skipping a child when its immediate container's original type already has a positive value. An AI using the cap skips objects under unusable planets. The include-production variant adds the queues' population contribution through producer objects. Round the total using integer conversion of **total + 0.5**. | debug build GEP-E39 | economy_rules, fleet/company model; missing |
| WGEP-50 | Population admission allows a type when its GC population value is at most **max(capacity − usage_including_production, 0)**. Each producer sums every entry in both actual queues and converts **sum + 0.5** to an integer. AI with the cap disabled passes. Thus a zero-population structure can still be bought when the player is over cap. WGEP-68/69 defines container and lifetime accounting. | debug build GEP-E40 | economy_rules, production; missing |
| WGEP-51 | Next starbase level is unavailable while any starbase is queued, at an invalid planet owner, at level **5**, or when the next level exceeds the planet's live station cap. Planetary state supplies that cap when present, limited to **5**; otherwise use `Max_Space_Base`. With no starbase owner the next level is **1** only if the planet permits a station; otherwise it is current level plus **1**. This progression does not change global tech. | debug build GEP-E21/E20 | production, new planet starbase state; missing |
| WGEP-52 | The inspected next-type helper chooses an existing station's `Next_Level_Base`; without one it recognizes the Rebel and Empire level-1 XML object types from target control faction. It returns the candidate only when its `Base_Level` does not exceed the available next level. It does not contain a Consortium fallback; Consortium construction discovery is U-11, not inferred from the Rebel/Empire branch. | debug build GEP-E41 | production, content discovery, AI engine; missing |
| WGEP-53 | A starbase level change to zero from a positive level emits the story destroy-base event. Emit the level-change notification, set the new type, then revalidate space production. Zero clears station ownership. These are strategic state changes, separate from WPR-52's tactical hardpoint and selection transfer. | debug build GEP-E42 | production, movement/control, Lua bindings; missing |
| WGEP-54 | A production reduction ability with `Galaxy_Wide` on a planet applies to that planet's current controlling player; other cases use a planet-local route. Destroyed planet sources fail application; global application also rejects intimidation corruption. Register each affected type's authored reduction/category under the unique source, with no expiry. A galaxy-wide time effect on ownership change removes the old player's source and applies to the new player; a local source planet keeps its local effect. Nonplanet time sources remove/apply on loss/gain by their owner. Removal uses the corresponding global/local route. WGEP-73/74 sources filters and equivalent price/time rebinding; complete already-queued adjustment qualification remains U-08. | debug build GEP-E43/E27; data | economy_rules, Lua/ability lifecycle; missing |
| WGEP-55 | A black-market purchase execution resolves the buyer object again and rejects a missing buyer or insufficient credits. Otherwise debit the prepared price, add the prepared item to the buyer player's independent black-market unlock state, emit local new-options/slice-success/production-complete feedback, and hand the buyer to the despawn route. This operation uses no ordinary planet build queue. Availability after an owned item or buyer loss remains U-12. | debug build GEP-E44 | economy_rules, Lua bindings, black-market state; missing |
| WGEP-56 | Cancellation by type searches the selected queue **backwards** and removes the last matching type, optionally refunding first, then emits cancellation and promotes a successor if needed. Bulk stop emits cancellation for each entry in forward order, clears the queue, and calls no direct refund. Indexed cancellation remains WGEP-45. | debug build GEP-E45 | production, command/Lua bindings; missing |
| WGEP-57 | Queue checkpoint data records each domain's order/count and each entry's type identifier, full duration, last serviced remaining time, current remaining time, builder/build/location identity and stored price. It also saves both AI prequeues and local per-type price/time modifier tables. Load rebuilds entries in order and sets both countdown and last serviced remaining to saved **current** remaining. Saving asserts each actual queue has fewer than **63** entries; this is not the human purchase cap. Other GC account/planet save fields are U-13. | debug build GEP-E46 | save/replay, production, AI engine; missing |
| WGEP-58 | Black-market targets must pass the opposing-planet check, belong to a playable faction, and have black-market corruption. Offer preparation skips an item whose faction differs from a present planet owner's faction. Nontutorial price is `(Base_Cost_Credits + Planet_Black_Market_Modifier) × Price_Modifier × tech_purchase_multiplier × uncommon_factor`, floored at **0**; tutorial preparation uses tutorial base cost without the planet/ability/tech multiplier step. Stock hero `Price_Modifier` values are **1.0, 1.5, 0.75**; item `Uncommon_Price_Multiplier` is **5.5**. In multiplayer GC for a human buyer, uncommon pricing applies when the highest displayed tech of all other players is below the item's tech. Otherwise it applies if any matching-item-faction player is below item tech. Preparation records already-owned status without removing that offer. Item tech numbering and menu acceptance remain U-12. | debug build GEP-E44; data | economy_rules, black-market menu, Lua bindings; missing |

## Comparison with existing behavior notes and code

Every WGEP row is missing in the GC implementation. The following comparison
is with the **existing notes**, not with implementation coverage.

| WGEP rules | Existing note | Comparison |
|---|---|---|
| 01..15 | WPR-10..16; PU-02..06 | **Differs:** GC pays discrete fiscal aggregates; tactical streams pay continuously. Planet modifiers, piracy, siphons and black-market planet factors are **missing there**. WPR-11's category aggregation is analogous but applies to a different source model. |
| 16..17 | WPR-12 | **Same** shared positive AI-credit multiplier and campaign cap formula; GC cap refresh timing and fiscal budget notification are **missing there**. |
| 18..27 | WPR-30/33; PU-11..15 | **Differs:** GC adds tech, lock, political control, facilities, local slots and population-at-buy. GC admission reads per-player limits; tactical checks also read all-allies limits. |
| 28, 43, 46 | WPR-21/30/31 | **Differs:** nonhuman GC players always use pay-as-you-go; their refunds use completed fraction. Tactical queue cancellation's full refund must not be copied to GC AI work. |
| 29..34, 54 | WPR-33; hero modifier aggregation WHE-17..19 | **Missing there** for GC price/time/base/factory formulas. **Same** greater-per-category then sum principle, with independent lists and GC price clamp. |
| 35..38 | WPR-20 | **Differs:** the GC backward sweep checks local facilities/base/orbiting requirements; it is not the tactical can-produce test and calls no direct refund. |
| 39..42 | WPR-22/52/57 | **Differs:** GC completion creates fleets/structures or changes strategic base/tech data, rather than filling a reinforcement pool or swapping a tactical station. Blockaded completed ground work is **missing there**. |
| 44..47 | WPR-21/22/31; PU-17..19 | **Same** one front, successor full countdown, stored-price accounting and notification interfaces where stated. **Differs** pay-as-you-go accounting; countdown and indexed-cancel checks are **missing there**. |
| 48..50 | WPR-04/41; PU-21 | **Differs:** GC has global original-type/queue usage and planetary/base capacity; space skirmish uses its tactical faction cap and fractional surviving craft. |
| 51..53 | WPR-50..52 | **Differs:** GC station levels are planetary state gated by planet capacity, not tactical automatic upgrade objects. |
| 55, 58 | WPR-60/61; hero black-market ability boundaries | **Missing there:** GC purchase execution, offer preparation and independent unlock state. |
| 56..57 | WPR-21/31; checkpoint infrastructure | **Missing there:** GC last-matching-type/bulk stop and the GC queue save inventory. |

Reuse candidates are `src/skirmish/economy.cpp#economy_rules`,
`include/eawr/sim/tactical/economy.hpp` and `src/sim/tactical/economy.cpp` for
validated profiles, accounts, queue entries and count concepts;
`src/script/foc/ai_engine.cpp` for budget and production consumers;
`src/script/authoritative/bindings.cpp` for script-facing commands and state reads;
and the existing replay/checkpoint infrastructure for canonical state. The current
`BuildQueue` enum and player ledger hold two **tactical player-wide** queues;
GC needs two queues **on every planet**, one shared player account, recorded
builder/location/build identity, and pay-as-you-go state. Reuse the queue
mechanics through an explicit GC profile rather than reinterpreting tactical
population, time or completion data.

## XML and GameConstants cross-check

The walk reads `gameconstants.xml`, `difficultyadjustments.xml`, `factions.xml`,
`expansion_factions.xml`, `planets.xml`, `starbases.xml`, `starbases_underworld.xml`,
the unit/company and `specialstructures*.xml` files, `techbuilding.xml`, and
`blackmarketitems.xml`, and the Consortium hero unit ability files. Type references, campaigns' initial ownership/credits/tech,
and persistent planet placement need the galactic data loaders. No tag registry
row is marked applied by this docs-only walk.

Registry statuses refer to exact class/tag pairs in `docs/tag-coverage/statuses.json`.
`applied` to tactical types and `partial` do not cover GC. Nearby presentation or
cinematic rows are outside this subsystem. The relevant uncovered rows are:

| XML field group | Registry at inspection | WGEP rules |
|---|---|---|
| `Fiscal_Cycle_Time_In_Secs`, `Pay_As_You_Go`, `Political_Income_Curve`, `Progressive_Taxation`, `Income_Redistribution`, `Black_Market_Income_Mult_Min/Max`, `MaxCreditIncomeAlignmentBonus/Penalty` | GameConstants **todo**. The tax/redistribution fields are authored but unused by the inspected fiscal path. | 01..17 |
| `Credit_Cap_Per_Planet`, `Corruption_Choice_Income_Percentage`, `Max_Ground_Forces_On_Planet` | GameConstants **land-or-galactic**. | 12..17, 39 |
| `Production_Speed_Mod_Base_Vs_Tech_0..4` | GameConstants **todo**. | 33 |
| `Planet_Credit_Value`, `Planet_Destroyed_Credit_Value`, `Planet_Black_Market_Influences`, `Planet_Is_Mining_Colony`, planet `Political_Control`, `Additional_Population_Capacity`, `Special_Structures`, `Special_Structures_Land/Space`, `Max_Space_Base`, `Required_Planets` | Relevant Planet/type rows **land-or-galactic**. `Max_Ground_Base` is **foc-ignores**; do not treat its XML occurrence as a functioning ground-base cap. | 05..14, 23..26, 48..53 |
| `Abilities/Planet_Income_Bonus_Ability/{Percentage_Income_Modifier, Absolute_Income_Modifier, Additional_Multiplier_On_Mining_Colonies, Specific_Mod_Source_Text}` and gambling `{Chance_To_Win, Evaluation_Interval_In_Secs, Min_Income_Modifier, Max_Income_Modifier}` | SpecialStructure **todo**; activation names are also **todo**. The existing registry ticket points to the space-abilities walk, so G2 supplies specific GC implementation ownership. | 10..11 |
| `Build_Cost_Credits`, `Build_Time_Seconds`, `Build_Initially_Locked` | SpaceUnit/Squadron/StarBase/SpecialStructure and related GC-producing classes **todo**; ground/company rows generally **land-or-galactic**. | 18..19, 28..34, 44..46 |
| `Required_Special_Structures`, `Required_Star_Base_Level`, `Build_Requires_Initial_Placement` | Relevant space/structure rows **todo**; ground/company prerequisites and `Required_Ground_Base_Level` **land-or-galactic**. | 21..26, 34..35 |
| `Build_Max_Instances_Per_Planet`, `Required_Orbiting_Units`, ground-company `Build_Time_Reduced_By_Multiple_Factories` | **land-or-galactic**. TechBuilding factory-speed tag **todo**. | 21, 25, 34..35 |
| `Tech_Level`, `Base_Level`, `Next_Level_Base`, `Population_Value`, `Build_Limit_Current_Per_Player`, `Build_Limit_Lifetime_Per_Player` | Existing tactical application is **applied/partial** for supported space types; Container/Squadron tech rows are **todo**, ground/company rows **land-or-galactic**, and GC application is absent. `Build_Advances_Tech_Level` on TechBuilding and starbase `Additional_Population_Capacity` remain **todo**. | 19, 22, 25, 42, 48..53 |
| `Benefits_From_Corruption`, `Corruption_Galactic_Production_Time_Multipliers`, `Corruption_Galactic_Production_Price_Multipliers`, `Galactic_Build_Time_Multiplier` | Faction/difficulty **land-or-galactic**. Difficulty `Credit_Multiplier` **todo**. `Maintenance_Cost` faction/space rows **todo**, ground-company rows **land-or-galactic**, without a charge in the inspected fiscal path. | 04, 13, 15..16, 29, 32 |
| `Abilities/Reduce_Production_Price_Ability` / `Reduce_Production_Time_Ability`: `Applicable_Unit_Types`, `Applicable_Unit_Categories`, `Excluded_Unit_Types`, `Unit_Strength_Category`, `Galaxy_Wide`, `Stacking_Category`, `Price_Reduction_Percentage` / `Time_Reduction_Percentage` | Planet rows **land-or-galactic**; supported hero price-reduction rows **todo**. Filtering/application closure is U-08. | 31, 54 |
| `SFXEvent_Build_Started`, `SFXEvent_Build_Complete`, `SFXEvent_Build_Cancelled` | Relevant space/structure classes **todo**; ground rows **land-or-galactic**. Build-pad tactical applications do not establish GC event wiring. | 47 |
| `Planet_Black_Market_Modifier`, hero `Abilities/Black_Market_Ability/Price_Modifier`, `Activated_Black_Market_Ability_Names` | Planet **land-or-galactic**, HeroUnit/GameConstants **todo**. | 55, 58 |
| `Item/{Faction, Tech_Level, Base_Cost_Credits, Uncommon_Price_Multiplier, Ability_Names, Used_By_Units, Tutorial_Tech_Level, Tutorial_Base_Cost_Credits}` | BlackMarketItems **land-or-galactic**; menu acceptance remains U-12. | 55, 58 |

Constants read or encountered are given with values in the rules, including
code constants **1**, **5**, **0.5**, **0.0001**, **0.999999**, and the **10/60**
countdown boundaries. `Max_Build_Queue`'s default is inherited from the sourced
tactical walk, not an authored stock value. Tag-table reads also confirm parsed
`Base_Population_Capacity` and `AI_Uses_Galactic_Pop_Cap`, neither authored in
stock XML nor present in the occurrence-based registry. Faction base capacity
defaults to zero; the AI flag defaults to false under WGEP-62/69. Tactical
`Tactical_Build_Time_Multiplier`, `Space_Tactical_Unit_Cap`, and
`Tactical_Production_Queue` do not drive the GC paths above.

## M3 gaps and ticket ownership

All **58 rules** are missing in GC; **0 same / 0 differs / 58 missing** against
GC code. Tactical comparison labels above are not included in that tally.
The walk tracker owns four new implementation sub-issues (legacy EAWR-1132).
Existing economy, story and checkpoint issues remain under their phase epic and
are linked by the tracker. Validated GC data and the galaxy model are shared
prerequisites (legacy EAWR-679, EAWR-680); control-transition integration belongs to the
movement/control gap (legacy EAWR-1114).

| Gap | Rules | Implementation and acceptance | Size / impact |
|---|---|---|---|
| G1 Fiscal ledger and base queues | 01..09, 15..17, 18..30, 32..47, 51..53, 56 | Existing GC economy scope (legacy EAWR-682): discrete fiscal account service; per-planet ground/space queues sharing player credits; admission/slots/tech/starbase state; recorded builder, price and time; explicit cancel and pay-as-you-go. Pin two planets buying in one frame, fiscal-boundary completion, zero/excess population, front/tail/type/bulk cancel, loss of a producer and non-Normal AI refund. | L; highest, vanilla GC progression |
| G2 Planet income and production modifiers | 10..11, 31, 54 | Source-scoped mining/gambling income, global/local price/time category aggregation, filters and ownership lifecycle through validated data (legacy EAWR-1128). Settle U-04/U-08 before encoding affected branches. Pin same/different-category effects, mining colony, source removal, ownership change and queue modifier changes. | L; high, planet bonuses and facilities |
| G3 Population accounting and reservations | 27, 48..50 | GC population census with original-type container suppression, station/planet capacity and queued reservations; optional AI cap, usable-planet exclusions, and over-cap zero-cost-in-population structures (legacy EAWR-1129). Pin company and squadron without duplicate children, two-planet simultaneous buys, cap loss and queue cancellation. | M; high, construction and fleet limits |
| G4 Corruption and black-market economy | 04, 12..14, 29, 32, 55, 58 | Persistent siphon, piracy consumption, corrupted planet income, corruption cost/time factors, synchronized planet multiplier rolls, black-market offer/purchase/unlock state (legacy EAWR-1130). Settle U-12 menu/item-tech cases and U-11 Consortium discovery. Pin owner/beneficiary payments, preview without consumption, strict expiry and replay RNG. | L; high, third playable faction |
| G5 Presentation and script/checkpoint contracts | 03, 16, 40..47, 53, 55, 57 | Production/fiscal event delivery (legacy EAWR-1131), existing story consumers (legacy EAWR-683), and checkpoint format/capacity (legacy EAWR-681, EAWR-252). Persist fiscal deadlines, all planet entries, locks/tech, modifiers and RNG state. Pin save/load before a fiscal boundary and build completion; resuming must neither repay nor recharge nor reroll. Delivery does not change authoritative state. | M for delivery; existing story/checkpoint scopes L; required for playable GC and saves |

Top five planning risks are G1's planet queues and discrete fiscal service, G1's
admission/tech/starbase progression, G3's global population reservations, G2's
source/ownership modifier lifecycle, and G4's Consortium economy. AI's always
pay-as-you-go branch and Normal GC build factor **0.8** must be explicit acceptance
cases rather than inherited tactical defaults.

## Frame schedule clarification

The [galactic frame schedule](../galactic-frame-service-schedule.md) narrows U-02:
GFS-09/11/12 separates planetary AI/actual queues inside object service from later
outer player AI. GFD-02/05 and GFSC-02 establish that fiscal collection reads
already committed control/ownership and refreshed credit caps; income refresh
belongs to the income reader. A remake fiscal phase must gather after those
commits. This does not settle control/production attachment or cancel listeners
(GFS-U02), nor U-03/U-04/U-08's source lifecycle (GFS-U05).
U-01's initial fiscal start/end/cycle and one-cycle admission are sourced by
GFC-02; this is not an unresolved global phase-order question. GFC-08 retains
production deadline/reset qualifications independently of fiscal rounding.
GFC-09/GFSC-16 narrow U-04's scheduler phase: gambling uses the object ability
manager, with eligible initialization/enable/resume randomizing its first due frame
over the current frame through current + interval, inclusive. Activation alone
does not guarantee an immediate sample or reset that deadline. Per-invocation
source/RNG and complete lifecycle qualification remain U-04/GFS-U05.

## Unverified and capture requests

These are scoped evidence gaps, not additional verified rules. They remain open
for M3 planning; the walk does not implement a guessed branch.

| ID | Unsettled detail | Targeted read or capture that settles it |
|---|---|---|
| U-01 | Settled: WGEP-59: initial window, mode clock, pause and speed boundary. | Debug-build clarification below; no required retail capture. |
| U-02 | Capture-needed: WGEP-60 and GFD-02/05 source the outer order and credit-cap refresh; WGEP-119 qualifies scripted zero/one/two-planet cap controls without a new order or restore claim. Scheduling part: see [galactic frame schedule](../galactic-frame-service-schedule.md), GFS-U01/02; individual object/attachment/listener admission requires the existing runtime probes. | Run GFSC-01/02/06 with arrival/control/production and fiscal due together, both object registration and control/production attachment orders. Record owner, cap, credits, queue mutation and listener payload before each consumer; repeat after save/load with an uninterrupted control trace.
| U-03 | Capture-needed: WGEP-63/75 sources curve interpolation, retained-list refresh, zero-return distinction and alignment binding/relations; complete transition cleanup and duplicate points remain unqualified. | Author political values 0, fractional, 1 and 5 with a prior source; change to neutral and nonpositive curve values, then transfer ownership. Include neutral/equal/opposing The_Force and faction Force_Alignment, repeated curve occurrences and duplicate-x pairs in both orders. Record source ledger, actual fiscal income and resolved alignment/strength; preserve an uninterrupted control trace. |
| U-04 | Capture-needed: WGEP-76/77 sources synchronized draws and activation/removal callbacks; first-service and external lifecycle qualification belongs to GFS-U05/08. | Complete seeded arenas on ordinary/mining planets, then remove, move, destroy or change source/planet ownership. Record frame, both RNG draws, source keys and income before/after a fiscal boundary, plus a save/load continuation. First establish the admitted service trace under GFS-19/GFC-09/GFSC-16; retain an uninterrupted control trace. |
| U-05 | Settled: WGEP-61: countdown initialization truncates, remaining seconds clamp at zero. | Debug-build clarification below; no required retail capture. |
| U-06 | Capture-needed: WGEP-45/46/64/67/72 sources explicit refund selection/recorded-builder assertion and bounded automatic listener behavior. WGEP-88/89 adds human prepaid ground head/tail refunds and scripted barracks-loss queue removal; WGEP-117/118 adds human pay-as-you-go manual head/tail refunds and automatic producer-loss cancellation retaining spent credits. AI budgets, mismatched builders, other requirements and complete listener/failure-state accounting remain unqualified. | Cancel head/tail prepaid and pay-as-you-go work for human and AI, with matching/mismatching recorded builder and command player. Repeat after facility/base/orbit requirement loss and political control crossing 1. Record builder, current owner, selected domain/type/index, refund flag, account delta, goal/global budget, listener payload and failure state; separate callback ordering under GFD-07/09/GFS-U02 from refund arithmetic and keep an uninterrupted control trace. |
| U-07 | Capture-needed: WGEP-70/71 sources prequeue time reservation, goal commitment, integer-shortfall payment and completion-before-final-billing; WGEP-116 qualifies one isolated human incremental account/debit trace through completion; stored final-bill fields and full AI task/budget ledger parity remain unqualified. | Stage equal-score candidates, local discounts, fractional resource shortfalls around integer conversion, repeated shortage/resume, taskless retained build identity and ownership change. Cancel at zero/partial progress and complete across the last countdown frame at every difficulty. Record prequeue deadline, selected build ID, stored price/remaining time, account debits and goal/global budgets separately; use the GFS-09/GFD-10 schedule boundary and an uninterrupted control trace. |
| U-08 | Capture-needed: WGEP-73/74 sources reduction filters and equivalent price/time ownership callbacks; WGEP-120/121 qualifies an explicitly included/excluded type, retained head/tail outcomes through loss of a global planet source and undiscounted outcomes for a later purchase. Other filters, removals and complete external cleanup remain unqualified. | Author category inclusions/exclusions, explicit excluded and explicit included types, faction-mismatched types and zero base cost/time. Apply local/global planet and hero sources while a head builds and a tail waits; move/remove/destroy sources and transfer ownership. Record affected type/source lists, stored and preview price/duration, front deadline and final debit, preserving a control trace and GFD-21/GFS-U05 callback seam. |
| U-09 | Capture-needed: WGEP-95..98 source exact countdown names/buckets, producer-side ownership checks, human underway submission and the inspected stock speech definitions. Event queueing does not establish audible playback. | Capture authored countdown definitions at minute/ten-second/second boundaries, local and nonlocal completions/indexed cancels, and UI admission followed by strategic rejection. Record submitted, queued, accepted and actually played events separately, including advisor text and save/load continuation. |
| U-10 | Settled: WGEP-62/68/69 defines zero population/capacity omission defaults after variant inheritance, false AI-cap default, caller bypass, capacity lifetime filters, immediate-container original-type usage and both actual queue contributions. | Read-only debug-build default, census and admission consumers; no required retail capture. List removal timing stays with the shared lifecycle/frame schedule. |
| U-11 | Capture-needed: WGEP-83 sources generic human registered-type discovery, distinct from WGEP-52’s limited next-type helper. No additional Consortium fallback is inferred; exact human/AI stock menu choices remain a runtime qualification. | For each faction, use the same planet at station levels 0..5 and global tech below/equal/above the next station requirement. Include authored station/planet caps and an already queued station. Record eligible type IDs, displayed/enabled buttons, prepared/accepted type, eventual-production result and AI proposal output separately; compare Consortium human enumeration with next-type-based AI/Lua discovery.
| U-12 | Settled: WGEP-80/81 source menu admission, ordinary/tutorial item tech versus duration indexing, repeated-owned execution and buyer-loss-before-debit. Stale prepared UI delivery remains a separate runtime case, not a new rejection rule. | Read-only menu, sender, execution, unlock insertion, item parser/getter and duration-index consumers; no retail capture required for these contracts.
| U-13 | Capture-needed: WGEP-82 audits account/planet/fiscal writer fields; WGEP-84..86 settle extra-result authoring and the inspected command/AI Lua/tactical Build returns. Full restoration remains WGS-U05/08. | Queue a primary with an additional result, execute accepted/rejected/cancelled strategic requests and wait on the AI task-force handle. Save/reload before completion and after extra placement; compare credits, queue identities/timers, primary/extra owners and containers, cancellation/completion delivery and blocking-status completion. No byte-level or full Lua restore completeness claimed. |

Captures should use the established original-game/Lua harness and stay in ignored
research output. No owner recording is required to accept this docs-only walk;
any unresolved observation remains **unverified** until evidence arrives.

## Unverified sweep: sourced clarifications

The [consolidated sweep](gc-unverified-sweep.md) records whole-item dispositions and residual questions. A clarification below closes only the stated branch; it does not turn an entire mixed question into verified behavior. Opaque GUV evidence remains in private research storage.

| ID | Rule | Source |
|---|---|---|
| WGEP-59 | A fresh galactic fiscal window starts at **0**, ends at one converted `Fiscal_Cycle_Time_In_Secs` duration, and its current cycle is **1**. Initialization performs the next-window update once. Expiry is `mode frame >= end`; payment is after shared object service, advances start/end by one duration and increments cycle once per invocation (WGM-03). Mode-frame advance occurs later and is suppressed by the mode pause predicate; only the active mode advances, so child tactical time does not advance the retained galaxy. Speed-setting target FPS and mode speed modifier are separate inputs (WGM-90). | debug build GUV-01..05; EGM-02 |
| WGEP-60 | Synchronized command execution precedes mode/object/fiscal service; player AI service and global/story scripts follow it (WGM-89). Ownership processing refreshes **every player credit cap** after its victory-condition tests and before its story-conquest notification. Root object assignment also refreshes caps. The cap refresh clamps current credits to the new cap. This fixes outer phase placement; exact order among simultaneous object completions/ownership changes and their listeners remains unverified. | debug build GUV-01/07/10/18 |
| WGEP-61 | Starting the queue front initializes its countdown with the current active mode frame and **`int(seconds × logical FPS)`**, truncating rather than adding the fiscal conversion’s half-frame rounding. It separately stores the authored float as its last-countdown value. Remaining seconds are `max(0, start + count − active mode frame) / logical FPS`. Thus positive durations shorter than one logical frame initialize to zero frames. Stored save remaining-seconds values re-enter the same seconds initializer on load; this does not prove the whole checkpoint round trip. | debug build GUV-15 |
| WGEP-62 | Omitted `AI_Uses_Galactic_Pop_Cap` defaults to **false**. In GC, a nonhuman player with the flag off receives maximum capacity **0** from the capacity getter; do not reinterpret that return as a human-style zero-cap admission rule without following its caller. With the cap enabled, capacity starts at faction base capacity and adds eligible live, non-delete-pending owned contributions; contributions on an AI-unusable enclosing planet are skipped, and a planet additionally contributes its current starbase type. WGEP-68/69 defines the separate usage and admission rules. | debug build GUV-13/18 |
| WGEP-63 | `Political_Income_Curve` loads a **piecewise-linear** evaluator from authored x/y pairs. Every occurrence replaces the previous curve before adding its pairs. For distinct control points it sorts by x and linearly interpolates the bracketing values; fewer than two points or x outside the authored domain assert and return **0**, without extrapolation. Stock **(0,0), (1,1), (5,1)** therefore yields x for **0 < x < 1** and **1** for **1..5**. WGEP-08 applies the positive value as a source-scoped contribution; its neutral/nonpositive branches still do not remove a prior source in this helper. Complete transition cleanup, alignment mapping and duplicate-x handling remain U-03. | debug build G4-E01; stock GameConstants data |
| WGEP-64 | The explicit refund helper asserts **debug-only** that the recorded builder equals the supplied recipient. In release behaviour it still obtains WGEP-46's amount from the stored entry and credits the supplied recipient as a windfall through WGEP-16; the assertion is not an early return or a zero-refund gate. Indexed cancellation validates the type before calling it. A mismatch is debug-invalid input; automatic cancellation listeners remain unverified. | debug build GQP-05..07; read-only recheck by gc-walks-w5 |
| WGEP-65 | Completion is processed before the unfinished pay-as-you-go branch and does not issue a final due bill or set the last serviced remaining to zero. The completed result's paid-cost value therefore comes from the last stored service fraction. The AI no-current-task branch with a valid build identity charges its planetary owner and spends the same amount from its general budget; a current-task branch delegates to that task's resource-spend result. Missing AI/controller state fails payment. Task reservation policy remains unverified. | debug build GQP-08..10 |
| WGEP-66 | The inspected human guard-position fleet-unification hook called after ordinary placement returns without changing state. The call in WGEP-40 therefore does not establish an actual merge in this debug build. | debug build GQP-11 |
| WGEP-67 | UI cancellation records the selected domain, index and type. Strategic execution derives the domain from the type and requires it to match the recorded domain; indexed and last-matching-type cancellation both request a refund. A missing-type cancel instead selects the current-entry-only route with refund. **Not supported by the current implementation:** commands require a nonzero type and expose no missing-type cancel route. This closes refund-flag selection for explicit strategic commands; the helper's debug assertion/release recipient behaviour is WGEP-64 and automatic cancellation listeners remain U-06. | debug build GQP-01..04 |
| WGEP-68 | Global GC usage reads the owned-object list, uses each object's **original type**, and has no independent dead/deletion-pending filter while the object remains on that list. Skip an object when its **immediate** parent's original type has positive GC population, also skipping that object's production contribution; do not recursively find the highest positive ancestor. The separate enclosing-planet traversal skips AI-unusable planets for capped AI. Include-production adds both actual queue contributions from each eligible producer, not AI prequeue candidates. Per-producer sums and the global sum use integer conversion after adding **0.5**. | debug build G5-E03; WGEP-49/50 |
| WGEP-69 | Omitted `AI_Uses_Galactic_Pop_Cap` is false: a nonhuman player receives **0 capacity and 0 usage**, while the admission caller explicitly returns **true**. Humans and capped AI use max(capacity minus selected usage, zero); zero-population types can pass while over cap. Capacity has the live/nondeleting and AI-enclosing-planet filters of WGEP-62, distinct from WGEP-68's usage list. Container category does not itself suppress usage: the immediate parent's original-type population value does. Omitted Population_Value and Additional_Population_Capacity both start at **0** in shared type construction. Variant_Of_Existing_Type copies the resolved base values before applying the variant's authored integer occurrences: local omission retains inherited values; omission throughout the chain yields zero. The GC population getter uses the base value, not tactical overrides. The winning registered effective XML audit includes both inherited positive values and complete-chain omissions, so do not reset an omitted variant value to zero. | debug build G5-E03; data G5-X01; WGEP-48/62 |
| WGEP-70 | An AI prequeue reservation initializes a countdown from `AI_Build_Task_Reservation_Seconds`; that method reserves **time, not credits**. Queue selection tests production with credit-affordability checking off and population including production on, keeps the first candidate at a tied minimum score, and installs a build identity before immediately executing its synchronized purchase. Marking the selected task under production clears its prequeue reference, records planet/build identity, registers begin/finish/cancel listeners and marks goal resources using the type price **without a planet location**, rather than today's local discounted entry price. | debug build G5-E02 |
| WGEP-71 | A selected AI task's installment checks **integer conversion of (spendable goal resources minus bill) < 0**. On that failure branch, a computed goal budget below the bill marks the difference for spending and returns false. Otherwise debit min(bill, spendable resources) from the plan player and spend that amount from the goal. With no task but a retained build identity, the queue instead spends through the current planet owner's galactic AI budget with no goal and debits that account; missing owner/AI rejects. Fractional shortfalls therefore follow the conversion, not an invented exact float affordability test. The production consumer handles completion before this billing route, with no terminal catch-up bill. | debug build G5-E02; GQP-08..10 |
| WGEP-72 | Dropping political control from at least **1** to below **1** bulk-stops both actual queues before the planet-owner change notification. Bulk stop sends cancellation for each entry then clears it, with no direct refund. A matching AI task cancellation checks type, recorded builder and build identity, removes its production listeners and enters failure state; the queue clears its matching current build. These inspected callbacks contain no account refund. On ownership change the AI queue drains prequeue candidates, failing negative-score tasks and resetting/reassigning other tasks. Failure-state handlers and other listeners remain an end-to-end refund qualification; these local absences do not prove universal zero refund. | debug build G5-E01/E02; WGEP-56 |
| WGEP-73 | Reduction target selection enumerates types matching **any included category**, rejects any excluded category and explicit excluded type, and requires affiliation with the source owner's faction unless the source is a planet. Append explicit `Applicable_Unit_Types` separately, without applying those category/exclusion/affiliation gates again; sort and deduplicate the resulting identities. Price application then requires positive base build cost without location; the inspected local time application has no equivalent positive-build-time guard. Register authored reduction/stacking category under the unique source with no expiry. Keep selection separate from later production admission. | debug build G5-E04 |
| WGEP-74 | Price and time reductions use equivalent ownership callbacks. A galaxy-wide planet source removes its contribution from the old player and applies it to the new player, when those players exist; a local planet source does nothing in this callback. A nonplanet source must currently be in the changed planet: remove when its owner equals the old owner or old owner is absent, and apply when its owner equals the new owner. Source removal deletes its per-type modifiers. Inspected application/removal callbacks and modifier-table creation update lists, with no direct existing-entry price, duration or countdown adjustment. WGEP-44's ordinary service uses stored values; complete external callback parity remains U-08. | debug build G5-E04 |
| WGEP-75 | An unblocked actual-income calculation removes expired modifiers and refreshes base, alignment, political and route sources, then sums the **retained modifier list**; it does not clear that list first. WGEP-08's neutral/nonpositive political branch returns without removing its old source, so do not infer cleanup from its numeric zero return. Alignment uses stored type and faction alignments: a nonnull nonneutral faction is like on equality and opposing on inequality, including against a neutral planet; the corresponding multiplier is `Force_Strength`, otherwise zero. Planet `The_Force` and faction `Force_Alignment` bind the same enum converter; `Force_Strength` binds the type strength. Full ownership-transition cleanup and duplicate-x interpolation remain capture gates. | debug build G5-E05; G5-M04; WGEP-08/63 |
| WGEP-76 | A valid gambling service consumes **two synchronized random draws**: the first is an integer sample divided by **32767.0**, with a win only when the float is strictly below `Chance_To_Win`; equality loses. The second uses the same synchronized source scaled between the ordered endpoints of the selected win/loss factor interval. Missing source/type/current planet/planet behavior/data returns before drawing. Set this unique source's nonexpiring income contribution to unmultiplied basic planet income times the factor; there is no mining-colony multiplier step in this consumer. Save/replay continuation requires qualification beyond recognizing the RNG source. | debug build G5-E06 |
| WGEP-77 | Gambling activation returns false and performs no initial roll. Removal requires a valid planet target/data and deletes its unique source; termination finds the source's current planet and invokes removal. Ownership callbacks do nothing for a planet acting as its own source; a nonplanet source removes its effect when its owner equals the old owner or old owner is absent, with no application-to-new-owner branch here. First service, interval/reset, external callback delivery and restoration are the [galactic frame schedule](../galactic-frame-service-schedule.md), GFS-19/GFC-09/GFSC-16 and GFS-U05/08; they are not inferred from activation. | debug build G5-E06 |
| WGEP-78 | The per-type owned-count getter counts every nonnull owned-list object whose **original type** is the requested type, with **no dead or pending-deletion guard**. Matching type-only purchased reinforcement entries also count. The debug getter reads the skirmish purchase pool, not the campaign persistence roster: each matching nonnull type slot counts until consumed or removed, including duplicate purchases; an AI-consumed null slot contributes zero. The pool append is restricted to tactical skirmish mode. Campaign forces marked retained (barred from space, missing an initial placement marker, or a removed empty transport) and campaign reserves retain their galactic fleet/cargo identities; those identities already count once in the direct owned traversal, independently of tactical deployment. They must not be added again as purchase-pool entries. For a company or squadron type, add the **maximum** recursively obtained count among its authored company members, company transport and squadron members, rather than summing those counts; each recursive query disables the all-allies option. The optional all-allies branch broadens the direct owned-list/reinforcement traversal only. GC current-limit admission uses this getter; queued instances are added separately under WGEP-25. Do not reuse capacity's live-source filter or population's immediate-container suppression for this count. Owned-list removal timing stays with the frame/lifecycle programme. **Implementation:** the census gathers fleet, detached, landed and production identities once, retaining original types and movement children after landing (WGM-49..53). Explicit static-type bindings supply company members, company transport and squadron members. A validated acyclic member graph resolves direct count plus recursive MAX in partitioned player proposals and an ordered commit; queued requested instances remain separate. A sparse census tail saves those links without changing legacy atomic census bytes. **Mode qualification (legacy EAWR-2199):** `census_retained_reinforcements` names the type-only purchase-pool input. GC has no such pool and correctly contributes zero; the campaign retained/reserve roster is not this input. The campaign-backed regression checks an exact owned count and current-limit refusal before/after load, plus no duplicate count during an active conflict and its save/load continuation, on 1/2/4/8 workers. Composite production expansion remains a separate placement boundary. | debug build G6-E01; WGEP-25/48/68 |
| WGEP-79 | Without a tactical build queue, the per-player producing-count query enumerates planet producers and sums each producer's matching queued-type count only when its **current** controller is allied and, with all-allies disabled, equals the queried player. GC admission disables all-allies for both this query and WGEP-78. This filter uses current planet control, not the entries' recorded builder identities. Queue ownership/listener transitions remain the frame programme's GFD-07/GFS-U02 qualification. | debug build G6-E02 |
| WGEP-80 | Human black-market menu refresh retains owned offers but disables them and labels them purchased. Other offers are disabled when `int(credits + 0.5) < int(prepared_price + 0.5)`. The purchase sender requires an active menu and an in-range index, then queues the prepared offer, buyer, target and absence duration, not an immediate debit; it notifies story and hides the menu. That sender has no owned-item or affordability recheck. Execution requires a resolved planetary target and buyer, then compares **float** credits to prepared price before debiting. A missing buyer fails before debit; there is no advance charge to refund in these inspected routes. Execution has no already-owned rejection: unlock insertion deduplicates, but a directly submitted repeated offer can still pay and despawn. Stale-menu delivery is not a verified ordinary UI action. | debug build G6-E03; WGEP-55/58 |
| WGEP-81 | Black-market item tech lookup returns authored ordinary `Tech_Level` or tutorial `Tutorial_Tech_Level` directly; parsing does not subtract one from the stored values. The absence-duration helper subtracts **1** from its supplied tech to index the authored ranges, accepting indices **0..4**. The inspected ability application supplies the buyer's **displayed tech**, while menu layout uses the item's ordinary tech. Keep these three consumers distinct. | debug build G6-E04; WGCH-38 in [corruption/heroes](gc-corruption-heroes.md) |
| WGEP-82 | Player checkpoint writing includes credits, cap, previous/current income, paid maintenance, current/max tech, historical production, black-market unlocks and per-type price/time modifier lists. Planet writing includes current/target allegiance and player, base income/control, live structure/station caps, base identities, initial placements, orbit/ground/influence references, siphon beneficiary/fraction/deadline/accrual/discovery, piracy accrual, corruption transition state and source modifier lists. Galactic mode writing includes fiscal start, end and cycle alongside shared-mode state. These are a scoped writer audit, not proof of complete nested restoration, RNG continuation or first-resumed-frame parity. | debug build G6-E05; WGEP-57; [session/save](gc-session-save.md) WGS-U05/08 |
| WGEP-83 | The human GC production list enumerates **all registered types**, including Consortium station types, when the selected planet's current controller is the local player. It applies can-produce and eventual-production queries, the ground/space tab and object filter, and the authored GUI row. A currently producing base can remain shown but disabled; an otherwise unavailable base is not kept solely because eventual production would admit it. This list does not use WGEP-52's Rebel/Empire-only next-type fallback. Consequently that helper's omission does not establish missing Consortium human build buttons. Exact stock station menus and AI selection remain U-11 qualification. | debug build G6-E06; WGEP-18..27/51/52 |


## Wave 6: additional results and request return contracts

| ID | Rule | Source |
|---|---|---|
| WGEP-84 | The extra result is authored with **`Create_Additional_Object`**, a resolved type reference; `Create_Additional_Object_Type` was not found as an authoring row in the inspected table. After primary placement, the dispatcher recursively creates that extra type at the **same planet and supplied player ID**. It returns the **primary result** and discards the recursive return, so callers cannot treat that return as an inventory of all created objects. The recursion is direct creation, with no second purchase, queue entry or affordability/population check in this dispatcher. Primary company/squadron/ship container returns are WGS-80. | debug build G6-E07 |
| WGEP-85 | An executed strategic production command has **no return value**. It resolves the issuing player/type/producer, applies the strategic admission check and begins production on acceptance; rejected admission emits a cancelled-entry signal with zero stored price/duration and the request's builder/build/location identities. Cancellation takes the queue domain and optional build index/type path and requests refund. Consumers must distinguish command submission, admission notification and later completion; they cannot infer completion from a synchronous command boolean. | debug build G6-E08 |
| WGEP-86 | The inspected AI task-force production binding resolves an AI target or planetary object, accepts its optional second boolean, creates build tasks for the force requirements and returns a **blocking status handle** tracking those tasks. Invalid target input reports a script error and returns nil. That handle is a request/wait contract, not an immediately completed roster or success boolean. The separately inspected object `Build` binding operates on a **tactical construction pad** and returns the delegated construction boolean; it does not expose a GC production queue merely because it shares that verb. Fiscal/queue event notifications remain WGEP-43..46. | debug build G6-E08 |

| WGEP-88 | In a retail capture (2026-10-08), a human Consortium owner bought two stock-priced **320-credit Mercenary Squads** in Shola’s ground queue: credits **12000 → 11680 → 11360**. Right-click cancellation of the waiting tail returned **320**; cancellation of the partially progressed head returned the other **320**, leaving an empty queue and **12000** credits. The last sampled head progress before cancellation was **55%**, not a sample at command execution. This fixture authored a **900-second** build duration and **3600-second** fiscal interval; every sample remained on Day 1 and the native credit getter agreed with the displayed balance. This qualifies full prepaid human refunds for these two entries, not AI budgets, pay-as-you-go work, mismatched recorded builders or listener delivery. | retail capture (2026-10-08); WGEP-45/46/67; U-06 |
| WGEP-89 | In the same retail capture (2026-10-08), the human Consortium owner again prepaid two **320-credit Mercenary Squads**, leaving **11360** credits. Scripted removal of Shola’s owned ground barracks made that barracks invalid and both queued icons disappear, while credits remained **11360** in immediate and later samples. Planet control remained Consortium and the fiscal display remained Day 1. The last pre-removal head sample was **7%**. This bounds automatic required-facility loss to queue removal without a refund in this fixture; it does not qualify other facility/base/orbit requirements, political-control loss, completed-company placement, AI failure handlers or complete cancellation-listener accounting. | retail capture (2026-10-08); WGEP-35; U-06 |

## Source income and reduction implementation boundary

The galactic income service applies WGEP-10/11/31/54 through validated original
economic profiles, including planet definitions. Campaign preparation resolves
WGEP-73's category selection and separate explicit-type append. Admission reads
unrounded price/time bases, applies the current source reductions and retains the
entry's resulting purchase values under WGEP-44. Factory, technology, difficulty
and corruption factors remain inputs from their existing production owners.

Gambling initialization retains a randomized inclusive first deadline without an
activation sample. Admitted turns stage source work on the partition executor,
then commit in stable source order. The galaxy's keyed RNG uses separate reserved
domains for first deadlines and the two per-turn samples; its distribution follows
GFC-09 and WGEP-76, while bitwise equivalence with the retail synchronized stream
is not claimed. The source turn follows control/effects and precedes production
and fiscal; its relative ordering against production completion inside the object
manager pass remains the named **income_production_turn_order** seam:
gambling samples before production in the GFS-19 object ability turn, with sample
RNG and source lifecycle remaining GFS-U05. Order against production completion
is unqualified; no U row covers that ordering. This placement is the approved
implementation boundary; registration-order parity is not established.
Fiscal bonus gathering reads freshly placed production objects under GFD-08.

The current source adapter binds stable live fleet members, detached units, landed
forces, production objects and retained nested actors, and retains local planet
sources across ownership changes. Nested originals follow native actor owner/location
writes, contribute nowhere in transit and disappear on loss; startup requires their
binding to agree with the actual live parent. The shared gather supplies payment,
forecast and new production prices, with WGEP-31 category winners rather than one
discount per container. A targeted debug-build recheck of local eligibility,
application, removal and ownership callbacks confirms WGEP-73/74: local targets must
be controlled by the source owner, and removal deletes the source's per-type modifiers.
Economic category selection treats the authored `All` as a full mask on either
side of an intersection; empty masks match nothing. Includes, excludes and the
separate explicit-type append retain WGEP-73's gates.
External enable/cancel/resume delivery,
corruption callback delivery and complete retail load reconstruction remain the
named **income_external_lifecycle** seam (U-04/U-08, GFS-U05/08). The project
checkpoint retains admitted deadlines/contributions and reconstructs them with its
typed replay; that does not settle the retail restoration qualification. The
bounded debug-build trace rechecked gambling activation/service and equivalent
reduction ownership callbacks without finding direct existing-entry mutation;
**income_live_queue_rebinding** remains U-08, rather than a claim that all external
listeners leave queued work unchanged. Missing authored economic defaults remain
diagnostics or explicit owning-provider inputs.

WGEP-76 gambling service checks the current planet independently of the source
owner. WGEP-77's nonplanet ownership cleanup removes the old contribution while
preserving the service deadline; a later due service can replace it. Mining keeps
its distinct WGEP-10 owner-match rule.

## Production feedback producer boundaries

These reads close the debug/data part of U-09. They establish event requests,
not audible playback or complete advisor delivery; those remain in the owning
presentation/audio consumer and the U-09 capture recipe.

| ID | Rule | Source |
|---|---|---|
| WGEP-95 | Countdown feedback requires changed remaining seconds, `Build_Countdown_Timer` enabled and a nonempty `Build_Speech_Countdowns` prefix. With current remaining seconds **s**, at **s >= 60** compare `int(previous)/60 + 1` with `int(s)/60 + 1`; a changed bucket requests **prefix + decimal(int(s)/60 + 1) + `_Minutes`**. At **10 <= s < 60**, compare integer tens buckets and request **prefix + decimal(int(s) + 1) + `_Seconds`** when they differ. Below **10**, compare integer seconds and use the same seconds suffix. Division here is integer division and conversion truncates; for example, a qualifying sample at **119.9**, **59.9** or **9.9** requests **2_Minutes**, **60_Seconds** or **10_Seconds** respectively. A skipped interval produces only the current sample's request, not every intervening bucket. The helper does not test the builder against the local player. After successful speech queue admission it resolves the event's first associated text ID and adds nonempty localized text as white advisor text. WGEP-43/65 determines whether this unfinished-work helper is reached. | debug build GUW5-E02; instruction and string-data recheck |
| WGEP-96 | Authored completion speech is queued without a producer-side local-owner check. When absent, completion fallback compares the stored builder with the local player before strategic SFX and planet ping, or tactical SFX. An accepted indexed cancellation similarly queues authored stopped speech without that local-player comparison; its fallback instead compares the supplied cancellation player with the local player, then selects strategic/tactical cancellation SFX. Authored speech suppresses the inspected fallback branch, including the strategic completion ping. These are producer admission rules; they do not prove every queued nonlocal event is played. | debug build GUW5-E01/03; WGEP-47/64/67 |
| WGEP-97 | The human build-button handler uses the local player and selected planet for its can-produce check. On admission, it queues authored `Build_Speech_Underway`, or the local strategic/tactical start SFX when that speech is absent, **before** constructing and submitting the synchronized production request. A failed UI check submits neither this request nor this feedback. The inspected native queue-begin path emits its begin notification without this start-audio selection. Underway feedback therefore belongs to this UI submission path and is not proof of later command acceptance or a universal AI/script production-begin sound. | debug build GUW5-E04/05; WGEP-85 |
| WGEP-98 | The inspected effective stock Death Star type enables `Build_Countdown_Timer` and authors `Build_Speech_Countdowns` as **`Speech_Death_Star_Build_`**, alongside underway/completed/stopped speech. In the inspected effective speech-definition file, its **10/5/4/3/2-minute** and **60/30/10/5-second** countdown definitions are inside an XML comment and do not register from that file; its underway/completed/stopped definitions are active. A type's prefix and generated request therefore do not by themselves establish available stock countdown speech. Mods supplying matching active definitions can exercise WGEP-95. | effective XML GUW5-X01; WGEP-95/97 |

## Economy residuals: displayed technology and fiscal collection

This read-only debug-build pass answers the economy implementation's remaining
static questions. Opaque evidence **GER-E01..16** and effective XML evidence
**GER-X01/02** identify private research retained outside tracked files. No
retail capture or new frame-service cadence evidence is claimed. These rows
clarify WGEP-03/04/12/14/58/81 and WGCH-37/38/71; they do not settle ordinary
stale-menu reachability, actor restoration or the whole U-12 capture question.

### Displayed technology and offer preparation

| ID | Rule and conditions | Source |
|---|---|---|
| WGEP-99 | In GC, displayed technology is **internal current technology + faction `Displayed_Tech_Level_Adjustment`**, with **no clamp** in the getter and no human/AI or tutorial distinction. Faction construction defaults the adjustment to **0**; stock Rebel authors **+1**. The getter's separate multiplayer-tactical branch returns internal technology without adjustment. A missing faction asserts and returns internal technology. Do not substitute displayed technology for WGEP-19/22's internal production technology. | debug build GER-E01; effective XML GER-X01 |
| WGEP-100 | For multiplayer GC and a human buyer, uncommon pricing initializes highest other-player displayed technology to **−1**, then takes the maximum over every nonnull player except the buyer. It does not filter those other players by faction, human/AI control, playability or alliance. Apply the item's uncommon multiplier when this maximum is **strictly below** the item's ordinary/tutorial technology selected for this preparation. No other player leaves the maximum at −1; this helper does not refuse an offer for that reason. This pricing rule is distinct from the menu header in WGEP-109. | debug build GER-E02 |
| WGEP-101 | Outside WGEP-100's multiplayer-human branch, uncommon pricing scans all nonnull players with a faction whose identity matches the item's faction. The first such player whose **displayed** technology is strictly below the selected item technology activates the multiplier. It does not select only the planet owner, omit the buyer explicitly, or require a human matching-faction player. No match leaves the factor **1**; an absent item faction also leaves it **1**. | debug build GER-E02 |
| WGEP-102 | Item enumeration appends every nonnull registered black-market item; it contains **no buyer-tech threshold, owned-item filter or ability `Tech_Level_Adjustment` calculation**. Preparation skips mismatching faction identities only when both the target owner faction and item faction resolve. Target appropriateness separately requires an opposing planet, a resolved current-allegiance player with a playable faction, and black-market corruption. Item technology feeds pricing/layout, not an additional technology admission gate in these inspected helpers. | debug build GER-E02/03/04 |
| WGEP-103 | Tutorial selection comes from the buyer's tutorial flag. It reads `Tutorial_Base_Cost_Credits` and skips ordinary planet additive cost, ability `Price_Modifier` and technology-purchase multiplier application. It **still applies uncommon pricing** under WGEP-100/101 using `Tutorial_Tech_Level`, then floors the resulting price at **0**. Ordinary/tutorial item technology getters return their stored authored integers directly; neither subtracts one nor falls back to the other field. Stock Empire items at ordinary tech **1/2** explicitly author tutorial tech **0** and cost **1000**; tech **3/4/5** author tutorial tech **3/4/5** and cost **100000**. A tutorial item tech of zero is therefore data, not an automatic omission policy. | debug build GER-E02/05; effective XML GER-X02 |
| WGEP-104 | The inspected item allocation, constructor, parser and tutorial getters establish **no initialized ordinary-value or zero fallback** for missing `Tutorial_Tech_Level` or `Tutorial_Base_Cost_Credits`. The constructor does not initialize these two fields; parsing dispatches present nonempty entries, and tutorial getters read the tutorial storage directly. All five stock Rebel items omit both fields. **Unverified:** a stable ordinary gameplay result for those missing fields; observing a zero in one allocation is insufficient to source a default. The implementation may retain an explicit missing-tutorial-data refusal as a named project seam rather than encode an inferred FoC value. | debug build GER-E05/06/07/08; effective XML GER-X02; omitted-value gameplay unverified |
| WGEP-105 | Item construction initializes ordinary `Tech_Level` to **1**, `Base_Cost_Credits` to **0**, and `Uncommon_Price_Multiplier` to **0.0** before parsing. These are separate from WGEP-104's unset tutorial fields. An omitted uncommon multiplier therefore does not default to 1: when uncommon pricing selects it, it can make the prepared price zero. Stock items explicitly author **5.5**. | debug build GER-E06; effective XML GER-X02 |
| WGEP-106 | Black-market ability construction defaults `Price_Modifier` to **1.0** and `Tech_Level_Adjustment` to **0**. It initializes the two respawn vectors with reserved capacity **5**, without supplying five authored duration entries. The inspected item enumeration, price preparation, target check and buyer-absence call do not apply the ability's technology adjustment. Do not confuse that stored field with the faction adjustment consumed by WGEP-99. Other consumers of that ability field are outside this bounded read. | debug build GER-E02/03/04/09/10 |

### Absence, menu and price-modifier boundaries

| ID | Rule and conditions | Source |
|---|---|---|
| WGEP-107 | Each authored `Min_Respawn_Times` and `Max_Respawn_Times` list must contain **exactly five** nonnegative values. Within each list, either every value is **zero** or every value is **positive**; mixing zero and positive values fails validation. After both lists pass, each minimum must be no greater than its corresponding maximum. Failure of either list or a reversed pair clears **both** vectors. Missing lists therefore provide an empty-vector fallback after validation, not five assumed zero entries. Validation also forces ground activation and `Causes_Despawn` true when those authored settings violate this ability's supported route. | debug build GER-E11 |
| WGEP-108 | Application prepares offers, then calls the absence helper with the buyer owner's **displayed technology before the local-human versus AI route split**. Empty minimum vector returns **0 seconds** without calling the random helper; a nonempty maximum in that branch asserts. With nonempty minimum, displayed technology outside **1..5** asserts and returns zero without calling the helper. Otherwise sample the corresponding minimum/maximum at **displayed tech − 1**; item technology and tutorial mode do not replace this operand. The sample is prepared even before checking whether an AI has an affordable choice or the human menu can open. Zero seconds feeds the existing hero removal/default-time interface, not a promise of instant respawn. | debug build GER-E03/10; WGCH-70/71; WGM-79 |
| WGEP-109 | The menu's technology header initially reads displayed technology from a player of the first offer's item faction. In multiplayer GC, it instead uses the highest displayed technology among nonnull players **other than that faction player**, initialized to **−1**; if the maximum remains −1, keep the initial faction-player value. This exclusion differs from WGEP-100's exclusion of the buyer. Item slot layout always reads the item's **ordinary** technology, including in tutorial mode. These display choices are not new item-purchase admission checks. | debug build GER-E12 |
| WGEP-110 | The buyer's technology-purchase price multiplier is **1** when its modifier list is empty. Otherwise remove expired modifiers, sum the retained additive entries and add **1**. If the resulting multiplier is nonpositive, assert and replace it with **0.0001**. This is a price multiplier over a separate modifier ledger, not a function of displayed technology. WGEP-103 bypasses its price application in tutorial preparation. | debug build GER-E13 |

### Fiscal mutations within the established payment boundary

| ID | Rule and conditions | Source |
|---|---|---|
| WGEP-111 | Black-market planetary influence refresh visits planets in the returned planet-list order. A planet without `Planet_Black_Market_Influences` neither changes its factor nor calls the synchronized float sampler. An affected destroyed planet sets its factor to **1.0**, also **without a random draw**. Only an affected nondestroyed planet samples the authored minimum/maximum. WGEP-14's after-payment placement is unchanged; these branches define which planets consume that sample. | debug build GER-E14 |
| WGEP-112 | Fiscal payment visits player-list indices in ascending order and calculates each nonnull player's aggregate with collection enabled before advancing to the next player. A neutral player, missing faction, nonplayable faction or faction unable to control planets returns zero before planet traversal. Within each planet, evaluate **owner income or nonowner corruption income**, then matching siphon recipient income, then positive piracy income/reset. A nonzero aggregate is added to the account **before** the initialized AI budget callback; the callback receives the **original aggregate**, not the difficulty/cap-adjusted account delta. A zero aggregate skips both. This corrects WGEP-03's earlier before-adjustment wording without changing the established outer frame schedule. | debug build GER-E15/16 |
| WGEP-113 | Piracy income is independent of whether the eligible beneficiary owns the planet or whether the planet currently has piracy corruption. After WGEP-112's player eligibility gates, any `Benefits_From_Corruption` faction adds a planetary piracy balance **strictly greater than zero**. Collection immediately clears it at that point; the **first eligible beneficiary in player-list order** receives it and later eligible beneficiaries see zero. Zero/negative balances are neither added nor reset. Preview disables only this reset: previews can each report the same positive balance. Do not fan the immutable balance out to every beneficiary on an actual payment. | debug build GER-E16; WGEP-04/112 |
| WGEP-114 | A normal unblocked actual-income call refreshes siphoning and subtracts the returned amount; a pre-siphon call skips that helper. The siphon helper **replaces**, rather than accumulates, its stored credit amount with current income times its fraction. The fiscal recipient branch checks the stored recipient ID, then invokes normal actual income to refresh/expire the record, then reads its stored amount **without repeating recipient equality**. Collection's preview flag does not suppress these refresh/expiry mutations. Income blocking returns zero **before** siphon refresh; the recipient branch still reads the retained amount, so this bounded routine can report a previously stored siphon value on a blocked planet. No universal zeroing rule for blocked siphons is established here. | debug build GER-E16 |
| WGEP-115 | Siphon expiry is checked only by the normal-income siphon helper: on a noncorrupted planet, terminate when current mode frame is **strictly greater than** its end; equality remains active, and corruption bypasses the deadline. The missing-recipient sentinel returns zero; this helper does **not** test whether a player object for a nonsentinel recipient still exists. Do not make recipient presence among the currently paid players an extra condition for deducting the owner's siphon amount. A valid fraction is **0..1**, including zero; an invalid fraction asserts and returns zero without replacing the stored amount. Complete discovery/termination callbacks remain the corruption/hero and lifecycle interfaces. | debug build GER-E16; WGEP-12; WGCH-103..107 |

### Implementation comparison and remaining qualification

The comparison is against commit `7756c831`, before the parallel economy
implementation. Its `src/sim/galactic/corruption_model.cpp` is a fixture-level
consumer; these observations do not claim campaign integration is complete.

| Rules | Comparison at the inspected base |
|---|---|
| 99 | **Missing:** player accounts retain internal technology and the faction loader parses the adjustment, but the black-market fixture receives caller-supplied displayed values without the sourced getter. |
| 100 | **Differs:** fixture pricing refuses multiplayer input with no other player instead of retaining maximum −1. |
| 101..103 | **Same** for fixture matching-faction comparison, supplied target flags, tutorial price factors and owned-offer retention. **Missing** live item enumeration and resolved allegiance/menu integration. |
| 104..107 | **Missing** original-data omission/validation adapters. Fixture item fields default to zeros and uncommon factor 1, so they must not supply original-data omission semantics. Missing tutorial values retain the named refusal seam; this is a project choice, not verified original gameplay. |
| 108..110 | **Same** fixture displayed-tech index and out-of-range zero return. **Missing** preparation-time sampling/actor/menu integration, validated-vector repair and the technology-purchase modifier-ledger consumer. |
| 111 | **Same** fixture factor arithmetic for unaffected/destroyed planets. **Missing** campaign sampling/commit integration and the sourced draw admission. |
| 112..115 | **Differs:** the fixture gives the same immutable piracy balance to every beneficiary and requires a currently present siphon recipient before deducting owner income. **Missing** eligibility gates, account/budget callback integration, mutable siphon refresh and retained blocked-income state. |

Against the earlier walk, displayed-tech arithmetic, omission/validation,
multiplayer maximum initialization and mutable collection boundaries are
**missing there**. WGEP-103 **narrows** the tutorial price formula by retaining
uncommon pricing; WGEP-112 **corrects** the account/budget callback order;
WGEP-113..115 **narrow** the preview and absent-recipient shorthand. Existing
corruption/economy ownership remains the same (legacy EAWR-1130); no duplicate
implementation ticket or application claim in the tag registry is added.

The five highest-impact findings are unbounded faction-adjusted displayed tech,
the buyer/item/faction-player operand split, first-beneficiary piracy collection,
retained siphon state when income is blocked, and the absence of a sourced
tutorial-omission fallback. U-12 still needs observed ordinary menu/actor
outcomes, including tutorial omitted fields and changed ownership. A stock
single-beneficiary capture cannot qualify multiple-beneficiary collection;
that case needs an authored scenario with two eligible corruption-benefiting
players and reversed player-list order. The within-payment rules above provide
the static expected result; no new frame-order trace or cadence claim is made.

## Global population and actual-queue reservations

Read-only **debug build** GCB-E06/07, researched 2026-10-10; comparison base
`0a29f9d121787df01115d8433c7173b984f47664`. Private raw reads and the evidence
map remain in ignored research storage. WGEP-48/62/68/69 already settle capacity,
original-type usage, immediate-parent suppression, list lifetime and the false
AI-cap default. These rows clarify the live factory and queue providers; no
retail capture or new frame schedule is claimed.

| ID | Rule | Source |
|---|---|---|
| WGEP-122 | Authoritative GC usage is computed on demand from the requested player's **owned list**. Include-production asks every eligible production object for its contribution: all nonnull entries in **both actual queues**, using each entry's requested type's galactic population value, then integer conversion of that producer's sum plus **0.5**. The outer sum rounds separately under WGEP-68. The queue helper does not filter entries by their recorded builder, remaining time, paid amount or position; affiliation to this population query comes through the eligible owned producer. AI prequeue candidates, team/freestore reservations and goal cash are not additional actual-queue entries. The requested entry type is counted, not a speculative expansion of every eventual company/squadron/additional result. | debug build GCB-E06; WGEP-49/50/68; WGA-60/66 |
| WGEP-123 | Paid production admission performs the population check with **include-production=true** after capability and human queue-length gates. On success it constructs the entry, debits prepaid credits when applicable, appends to the chosen actual queue and only then emits production-begin. Appending is the population reservation; no separate reservation-ledger increment occurs in this inspected entry path. A subsequent admitted request for the same player sees the already appended demand, including demand on another owned producer. Different players query their own lists, not a galaxy-wide shared player budget. Pay-as-you-go versus prepaid changes billing, not which queued entry reserves population. | debug build GCB-E07; WGEP-27/28/50/122 |
| WGEP-124 | Indexed cancellation optionally refunds, emits cancellation and **then removes** its matching actual entry; removal releases that entry's queued population, independently of refund size. An expired ground front held by WGEP-39 remains in the queue and continues to count. Admitted completion removes the front **before** completion presentation, result placement or tech assignment: its queue contribution ends, and successfully created owned originals contribute through WGEP-68's ordinary census. Failed ordinary placement follows WGEP-40's refund path; it does not retain a phantom population reservation. Additional/composite results and capacity sources need their actual resulting identities, not an assumed equal reservation-to-live transfer. These are local entry/removal boundaries; automatic cancellation listeners and cross-object service ties remain U-06 and GFS-U02. | debug build GCB-E06/07; WGEP-39..46/68/84 |
| WGEP-125 | The inspected periodic queue revalidation and admitted completion do **not** recheck population availability. Capacity loss can leave the player over cap without itself removing an already admitted entry; the next positive-population request fails against max(capacity minus current usage including queues, **0**), while a zero-population request can pass this gate. Other capability/base/occupancy/cancellation gates still apply. Uncapped AI returns zero capacity/usage and explicitly bypasses admission under WGEP-69; capped AI uses the same query with its enclosing-planet usability filter. Ordinary AI candidate/contrast checks under WGA-60/66 do not replace the later actual producer check; magic creation remains WGA-63's separately sourced route. | debug build GCB-E06/07; WGEP-35/50/62/69; WGA-60/63/66 |

### Ready to implement: population census and reservations

For global population work (legacy EAWR-1129), these are **proposed tests** for
the composed live providers, rather than a request to replace the existing
census implementation.

| Contract | Rule IDs | Provider seam | Proposed observable test |
|---|---|---|---|
| Owned identity and capacity | WGEP-48/62/68/69/122; WGS-141 | Owned original types, immediate parent, enclosing planet, live/delete state, current station and AI usability | Company/squadron/transport children with zero/positive immediate parents; a positive grandparent behind a zero parent; dead/delete-retained usage versus excluded capacity. Verify first purchase against placed live identities, not display zero. |
| Shared actual queues | WGEP-27/50/122/123 | Both actual queue vectors on each eligible owned producer; ordered command/admission commit | Two same-player planets compete for one remaining slot: first admitted append reserves it and second refuses without debit. Include front/tail, both domains and different-player controls; reverse command order and compare 1/2/4/8 workers. A builder-ID change alone does not change the getter's queue sum. |
| Reservation removal and result identity | WGEP-39..46/68/84/124 | Matching indexed cancel, completion hold/removal, nested placement and actual owned-result registration | Cancel front/tail with zero/partial/full refund; held completed front; success/failure/additional/composite result. After each completed boundary, recompute both queue and live usage and prove no lost or duplicated identity. |
| Capacity and AI admission | WGEP-50/62/69/125; WGA-60/63/66/89 | Human/capped-AI policy, enclosing-planet usability, candidate query, actual producer append and separately owned magic route | Lose capacity with admitted positive demand; it persists, a new positive request fails and a zero-population request passes only this gate. Test stock uncapped AI, capped AI and an unusable enclosing planet. Prequeue and freestore reservations alone must not increase actual queued usage. |

The census and actual queues in `src/sim/galactic/production.cpp` already
implement the core supplied-state path; `src/campaign/setup.cpp` installs
placed originals and producer/census inputs, while `live_setup.cpp` refuses
capped AI without an explicit usability provider. This is **partial live
composition**, not missing census arithmetic. The WGEP-78 type-only purchase-pool input is skirmish-scoped; campaign
retained and reserve forces already contribute through their galactic owned IDs. Live AI production
requests, ownership/lifetime notifications and nested result registration
must supply these providers; no new independent global reservation service is
sourced here. Preserve partitioned proposals and ordered commits in the remake.

U-10 is already settled by WGEP-62/68/69; it is not a default blocker.
U-06/U-07 and AI U-04/U-05 remain qualified for listeners, complete accounting
and mixed roster/AI requests. Extend their existing captures with two producers,
full population, cancel/held completion and capacity loss; record requested
type, producer owner, builder, both queues, resulting owned identities and
credits before/after the action. Use GFS-U02 and GAI-U03 for service/callback
ties and saved continuation, rather than assigning a new census timer.

## Economic construction defaults

| ID | Rule | Source |
|---|---|---|
| WGEP-92 | Original type construction starts build cost and build time at **0**. Effective inherited and authored values precede these omission defaults; malformed consumed authoring refuses rather than becoming an omission. Fresh players and production objects construct empty production price/time modifier maps and an empty technology-price modifier list. An empty price/time reduction calculation yields **1.0**; an empty technology-price list also yields **1.0**. This sources only the fresh empty-list state: automatic ability effects, ownership callbacks, corruption factors, base/tech factors, AI difficulty and factory counts still supply their respective live inputs. The no-location build-time getter multiplies authored time by `Production_Speed_Factor` before the location factors in WGEP-32. | debug build CSP-E01; CSP-E02 |
| WGEP-93 | Both production price and time reduction ability constructors start enabled, with galactic automatic activation, reduction **0**, stacking category **0** and galaxy-wide **false**. Effective authored values replace these omission defaults. A fresh enabled galactic/global automatic profile contributes through WGEP-73/74; a disabled or manual/tactical profile does not apply during galactic object initialization. Unknown activation or malformed consumed values refuse. | debug build CSP-E03 |

| WGEP-94 | Fresh mining ability construction starts percentage and absolute modifiers at **0** and mining-colony multiplier at **1**. Fresh gambling ability construction starts chance, minimum, maximum and evaluation interval at **0**. Both economic ability kinds and political-control protection start enabled with galactic-automatic activation. Effective authored values precede these omission defaults; malformed consumed values refuse. | debug build CSP-E04 |

## Bounded campaign account qualifications

Evidence **ECU-E01** is a debug-build campaign capture (2026-10-10), with
structured Lua debugger reads and observers that preserve the original
production callbacks. The four-planet fixture has human Consortium ownership
of Shola, AI suspended, fog enabled, stock **320-credit Mercenary Squads**,
authored **60-second** duration, `Pay_As_You_Go` enabled and a **3600-second**
fiscal interval. Production and refund samples remain on Day 1. Private input
hashes, typed callbacks, account samples, roster identities and screenshots
remain in ignored capture storage. These are account observations; the
recorder does not expose stored queue fields, AI category cash or complete
listener/failure-state ledgers.

| ID | Observed rule and bounded qualification | Source |
|---|---|---|
| WGEP-116 | One isolated human pay-as-you-go Mercenary Squad began at mode time **62.966671** with **12000** credits and completed at **123.000008** with **11680.078125**. Nearby samples at **77.833336**, **92.766670** and **108.233337** read **11920.908203**, **11841.283203** and **11758.814453**: payment was incremental, rather than a single completion charge. The final debit was **319.921875**, and the completion balance remained unchanged through the later sample at **139.033340**; one completed company identity was retained. This confirms the human incremental-payment path in WGEP-28/43 for this fixture. It does not measure stored last-bill fields, assign the small shortfall to a particular internal operation, or qualify AI prequeue reservation and goal/category parity. | debug-build campaign capture ECU-E01; WGEP-28/43; U-07 |
| WGEP-117 | The same human bought a head and waiting tail with pay-as-you-go. Right-click cancellation of the waiting tail produced a typed cancellation callback while the head continued spending; surrounding balances **11485.458984 → 11482.615234** show no positive refund jump. Cancelling the partially built head then left an empty queue and **11680.137695**, against its prequeue balance **11680.078125**: spending was restored within **0.06** credits of floating-point residue. The last precommand UI sample is not command-time progress. This narrowly confirms WGEP-46/64/67 for matching human builder/recipient and this queue; it does not qualify mismatched builders, exact stored-progress arithmetic or AI windfall ledgers. | debug-build campaign capture ECU-E01; WGEP-46/64/67; U-06 |
| WGEP-118 | In a separate pair of human pay-as-you-go entries, the prequeue balance was **11680.137695**. Scripted removal of Shola's owned ground barracks at mode time **259.633362** left **11607.266602** both immediately before and after removal. Both entries emitted typed cancellation callbacks at that same observed time and balance, their UI queue icons disappeared, and the account remained **11607.266602** at **275.233337**; the barracks was invalid and the previously completed company remained. Automatic producer-loss cancellation therefore **did not refund the 72.871094 credits already spent** in this fixture, unlike the manual head cancellation in WGEP-117. This confirms WGEP-35's no-direct-refund path and narrows the end-to-end listener question for human barracks loss only; other requirement losses, political control, AI failure handlers and restore remain open. | debug-build campaign capture ECU-E01; WGEP-35/46/64/67; U-06 |
| WGEP-119 | With one human-owned planet, a large positive `Give_Money` request stopped at **40000** credits. After scripted acquisition of a second planet, a separate large request stopped at **80000**; returning that planet to its old owner clamped the balance to **40000** in the immediately following read within the same Lua chunk. After losing the last owned planet, another large request still stopped at **40000**, with all four planetary owners independently read. This confirms WGEP-17's **max(1, owned count)** cap for these scripted controls. The acquisition itself changed credits **40000 → 42400** before the separate grant; that delta is not assigned to fiscal income or a particular reward here. These observations do not establish ordinary command/object/listener order, same-frame fiscal admission or save/load parity. | debug-build campaign capture ECU-E01; WGEP-17/60; GFD-02/05; U-02 |


Evidence **ECU-E02** is a separate bounded campaign capture with the same human
pay-as-you-go, stock price, authored duration and delayed fiscal inputs.
Coruscant is an existing planet source with galaxy-wide **50%** price and time
reductions: both profiles include `Infantry`, exclude `Underworld_Merc_Squad`
through `Excluded_Unit_Types`, and explicitly include that same type through
`Applicable_Unit_Types`. The source initially belongs to Empire, is transferred
to Consortium before purchase, and is returned to Empire while Shola's head is
partially built and its tail waits. The acquisition balance is normalized to
**12000** before production, with an unchanged prequeue control read. Private
structured callbacks and roster identities accompany the UI captures.

| ID | Observed rule and bounded qualification | Source |
|---|---|---|
| WGEP-120 | With the global planet source owned by the human, Shola's displayed Mercenary Squad price was **160**, against the **320** control, even though the source explicitly excluded and explicitly included that type. Returning the source to Empire at mode time **62.500004** restored the displayed price to **320**, with the producer remaining Consortium-owned. The two entries already admitted under the source completed at **79.200005** and **109.233337**: the head took **30.033337** seconds from its **49.166668** admission, and the waiting tail took **30.033333** seconds after the head completed, not from its earlier **56.000004** admission. Each debited **159.960938**, leaving **11680.078125** from **12000**; both completed company identities remained. This confirms WGEP-73's separate explicit-type append and WGEP-44/74's retained queue outcome for these profiles despite source-owner loss. Stored price/duration fields and complete modifier lists were not read directly. Category-exclusion ordering, nonplanet faction filters, zero-base types, other source removals and restore remain capture-needed. | debug-build campaign capture ECU-E02; WGEP-44/73/74; U-08 |
| WGEP-121 | After the source-owner loss in WGEP-120 and completion of both previously queued entries, a fresh human Mercenary Squad purchase displayed **320**, began at mode time **159.466675** with **11680.078125** credits, and completed at **219.500015** with **11360.156250**. Its **60.033340-second** observed duration and **319.921875-credit** debit match the undiscounted control in WGEP-116, and the balance was unchanged at **256.700012**; all three completed company identities were retained. The source was Empire-owned and Shola remained Consortium-owned. Thus this source-owner transition restored the undiscounted outcome for a later purchase while preserving the already-admitted outcomes in WGEP-120. It does not establish every removal/ownership callback, complete modifier-ledger cleanup or checkpoint continuation. | debug-build campaign capture ECU-E02; WGEP-44/73/74; U-08 |

## Fiscal and production delivery contracts

Read-only **debug build** GEH-E01..27, researched 2026-10-10, and effective
**XML/Lua data** GEH-X01/02. The private evidence map and raw reads remain in
ignored research storage. These are bounded producer and consumer reads, not
an executed retail trace. The [galactic frame schedule](../galactic-frame-service-schedule.md)
owns phase order: GFS-01/09/11/12/14, GFD-02/05/07/09 and GFSC-06/08/10.
No new object-service, listener-ranking or first-return schedule is inferred.

| ID | Rule | Source |
|---|---|---|
| WGEP-126 | Every admitted account-add call applies difficulty and lower/upper clamps, tests the credit victory condition, then sends the active story the **integer-truncated resulting whole balance**, even when the admitted addition leaves that balance unchanged. A requested AI windfall follows with the actual adjusted delta. The inspected adder emits no separate cap-reached signal or cap-specific speech. Credit-cap refresh directly writes the cap and clamps credits; it does **not** call this adder or its accumulation/windfall notifications. These are bounded routine absences, not proof that no other observer can compare displayed credits with the cap. Fiscal's zero-aggregate skip and original-aggregate AI notification remain WGEP-112. | debug build GEH-E01/02; WGEP-16/17/112; WST-33 |
| WGEP-127 | Native production notifications are sent by the **producing location**, carrying the entry's requested type, recorded builder, build identity, location identity, stored duration/price/progress and nullable primary result. Accepted begin follows prepaid debit and actual-queue append. Indexed cancellation refunds when requested, sends cancellation while the entry is still present, removes it, starts a successor when the front was removed, then requests stopped feedback. Completion removes the front and requests completion presentation first; placement or tech assignment and historical-count increment precede finished delivery. Human unification and paid-cost assignment follow that delivery, then the successor starts. A finished notification can therefore contain a null ordinary result or precede the result's paid-cost annotation. WGEP-85's rejected-request cancellation is a different entry-shaped payload, not evidence that a queued purchase was refunded. | debug build GEH-E04..06; WGEP-40/45/65/85/123/124 |
| WGEP-128 | The native dispatcher delegates immediately to the producing object's registered listeners for that notification kind. It invokes their callbacks before returning to the producer; it does not first publish a next-frame production inbox. A consumer can change state or issue another request during that call. This does not prove a suspended Lua continuation resumes there, that HUD/audio output renders there, or a universal order between scoring, AI and script-block listeners. Story dispatch separately retains WST-13/14's active-mode/foreground/sub-plot/player gates and delayed-event path. Implement synchronous authoritative effects at the sourced mutation boundary and publish immutable presentation records separately. | debug build GEH-E10/11; WST-13/14/20; GFS-14/25; project delivery boundary |
| WGEP-129 | A script production block matches notifications by **requested type identity, recorded builder and build identity**, in addition to its subscribed producer. Matching begin records begun and spends an optional script budget using the type's ordinary build cost. Its callback can mark matching cancel or finished as terminal and retain a nonnull result; a returned fleet is reduced to its first child with an asserted single-child count. Completion polling requires **both begun and terminal**, rather than treating any cancellation as a successful admitted build. The inspected post-load subscription adds begin and finished listeners only; the cancellation branch in the callback alone does not establish cancellation subscription. AI build-task/planetary-queue subscriptions and failure/freestore effects remain WGA-158..160, not this script block's policy. | debug build GEH-E07..09/22; WGEP-86; WGA-158..160 |
| WGEP-130 | Tech assignment stores/clamps the authored target and sends story tech notification **before** unlock enumeration; there is no unchanged-level early return in this setter. Enumerate affiliated, initially locked, slicer-unlockable, not-already-unlocked types with required tech strictly below the stored level. Request new-construction-options SFX only when unlock processing produced at least one new entry, sound was requested, the player is local and the faction event exists. Completion's separate HUD notification merely invalidates its buildable-list cache; it neither identifies a newly available type nor proves a new unlock or sound. Build-choice enumeration/flashing remains WGUI-91. | debug build GEH-E03/16; WGEP-42; WST-33; WGUI-91 |
| WGEP-131 | Starbase level change sends destroy-base story notification for positive-to-zero, then sends native level-change from the planet with the **new type or null** while the old station type/level is still installed. Only after listeners return does it install the new type, revalidate space work and, for zero, clear station ownership. Consumers must use the supplied new type and the prechange planet state at this boundary; rereading the planet later cannot reconstruct the old state. This qualifies WGEP-53 without moving it to a new frame phase. | debug build GEH-E21; WGEP-53; WST-34 |
| WGEP-132 | Galactic scoring registers production begin/cancel/finished and starbase-change listeners on the inspected planets at mode start, without a local-owner filter. The production callbacks directly call stock Lua `Galactic_Production_Begin_Event(planet, type)` and `Galactic_Production_Canceled_Event(planet, type)`. Finished calls `Galactic_Production_End_Event(planet, result)`: a nonnull fleet result is reduced to its **first direct child**; with no primary result, pass the requested **type** instead. These callbacks do not append a builder or build ID. Stock begin/cancel bodies make no credit/budget mutation; stock end distinguishes object from type and attributes build statistics using the **planet's current owner**, not the entry's recorded builder. The separate `Galactic_Starbase_Level_Change(planet, old_type, new_type)` reads old type before WGEP-131 installs new type; missing old type is nil, missing new type leaves its optional final argument absent/nil. Scoring callbacks are distinct from `STORY_CONSTRUCT`, `STORY_TECH_LEVEL` and `STORY_ACCUMULATE` evaluation. | debug build GEH-E12..15/23..25; stock Lua GEH-X01; XML GEH-X02; WST-33/34 |
| WGEP-133 | Script production-block save/load retains its command continuation, builder/type/producer references, optional budget, begun and terminal flags, build ID, nullable retained result and reference-fixup identity. The inspected post-load hook reconnects begin/finished listeners to the restored producer; it does not reissue production or repeat budget reservation/spend there. Restoring only the actual queue, or treating a saved result as permission to replay finished, loses this consumer continuation. This is a scoped field/hook audit, not full listener/Lua restoration completeness or compatibility with FoC's save bytes. | debug build GEH-E22/26/27; WGEP-57/82/129; WGS-U05/08 |

### Observer and implementation boundaries

The native producers and script-block identity checks above do not impose a
local-human filter. Fiscal accounts and initialized AI budgets are per-player;
the HUD, advisor and audio retain each producer's explicit observer gates.
WGEP-95..98 and WGUI-117 establish speech requests and local fallback/ping
precedence; they do not establish accepted playback. Black-market execution
retains WGEP-55/80's independent paid-unlock route.

Compared with the earlier walk, WGEP-126 **narrows** cap/accumulation delivery;
WGEP-127/128 **source previously missing** entry payload and direct-delivery
boundaries; WGEP-129 **qualifies** script-block admission/terminal/subscription
state; WGEP-130/131 **narrow** unlock and prechange-base consumers; WGEP-132/133
**source previously missing** scoring arguments and saved script-block state. Existing
queue, fiscal, speech and AI arithmetic rules remain the same.

At comparison base `d011385a`, `src/sim/galactic/production.cpp` and immutable
`galactic/events.hpp` provide begin/complete/cancel commit records, but the
composed scoring, story and script-block consumers remain **missing**.
`GalacticSession::CreditNotification` carries resulting balance and adjusted
delta; its fiscal payload lacks WGEP-112's distinct original aggregate. This
is a **different fiscal consumer contract**, even when uncapped Normal income
makes the two values equal. `src/campaign/game_session.cpp` provides the retained
AI ledger seam; it does not make that delta an original-aggregate notification.

### Ready to implement: fiscal and production events

The delivery work (legacy EAWR-1131) can use the following contracts. These are
**proposed tests**, not executed captures or a request for a new story interpreter.

| Contract | Rule IDs | Provider seam | Proposed observable test |
|---|---|---|---|
| Fiscal/account consumers | WGEP-16/57/82/112/126 | Ordered player account mutation, original aggregate, adjusted delta, truncated whole balance, initialized AI budget and active story | Aggregate 100 with Hard multiplier 1.2 and only 5 cap headroom: account +5, AI fiscal input 100, story resulting whole balance; zero aggregate skips payment callbacks. Compare cap refresh with an admitted zero add and retain window/balance across restore without repay. |
| Production mutation delivery | WGEP-40/45/65/85/123/124/127/128; WGA-158..160 | Immutable copies of producer/type/builder/build/location, stored entry state and nullable result, with synchronous authoritative listeners | Accepted begin versus rejected request, head/tail explicit cancel, automatic facility loss, null ordinary result, tech/base completion and successor. Observe queue membership, account, historical count and result paid cost **inside** each callback; a finished event is not a final catch-up bill. |
| Unlock/base/HUD feedback | WGEP-42/47/53/55/95..98/130/131; WGUI-91/117 | Tech/unlock state, old/new base type, local observer, data-resolved speech/music/SFX, HUD invalidation and selected-planet choices | Same-level tech assignment, new unlock versus none, sound-enabled/disabled and nonlocal owner; base listener reads old type while payload gives new/null; ordinary completion invalidates choices without inventing a generic unit-available sound. Count requests separately from accepted/played output. |
| Scoring Lua projection | WGEP-127/128/131/132 | Registered planet listeners, nullable primary result, first fleet child, current planet owner and old/new station types | Begin/cancel receive planet/type; finished receives object or type without builder/build ID. Verify changed-owner build statistics separately from recorded-builder correlation and verify old station type inside the level-change callback. Stock begin/cancel callbacks must not invent a refund or spend. |
| Script waits and restoration | WGEP-57/82/86/129/133; WST-13/14/20; WGA-158..160 | Producer listener subscriptions, build correlation, begun/terminal/result, optional budget, saved account/queue/fiscal and story/AI contributors | Interleave equal-type builds for two players and two build IDs. Wrong correlation is ignored; begun and terminal are independent. Save before begin/finish and restore fresh owners before delivery; no duplicate spend, refund, result or callback. Verify canonical events/state at 1/2/4/8 workers and unchanged hashes when presentation is attached. |

U-06/U-09/U-13/U-14 and GFS-U01/02/08 remain qualified runtime questions. Use their
existing refund, playback, restore and same-frame recipes; direct notification
is not permission to assume listener ranking, audible nonlocal speech or complete
save/load equivalence. The queue-state inventory for checkpoint work
(legacy EAWR-681) is WGEP-57/82 plus the script/AI consumer state above, rather than an
after-load replay of historical notifications.

### Delivery qualification

| ID | Still unverified | Capture recipe |
|---|---|---|
| U-14 | Runtime subscription/listener ordering under reentrant mutations, scoring attribution when planet control changes, and complete script-block continuation across cancellation/load. WGEP-127..129/132/133 source bounded delivery and fields, not every registration/lifecycle path. | At two producers queue equal types with distinct recorded builders/build IDs; use a script wait with an optional budget and an AI task as separate consumers. Break at begin/cancel/finished and scoring Lua entry; log queue presence, builder, current owner, result child/type, paid-cost annotation, begun/terminal/result and actual subscriptions. Change ownership and cancel head/tail, then save before begin, before completion and after cancel; restore fresh state and compare three admitted steps with uninterrupted controls. Record direct callback, story dispatch/delay, later coroutine poll and presentation separately. Add a listener that removes itself and a listener that submits successor work; qualify ordering through GFS-U02 rather than choosing lowest entity ID. |

## Refusal and base/tech completion provider distinctions

Read-only **debug build** GPC-E02/06..14 and effective **XML/Lua** GPC-X01..04,
inspected 2026-10-10. These rows refine WGEP-19/27/40/42/53/85/95..98/130/131;
they source local producer branches rather than a new outer schedule. Use
[GFS-01/09/11/12/14 and GFD-11/13](../galactic-frame-service-schedule.md) for
admission and later consumers. No new runtime recording is claimed.

| ID | Rule and bounded qualification | Source |
| --- | --- | --- |
| WGEP-134 | Strategic submitted purchase first rechecks general capability. A false result sends WGEP-85's cancellation-shaped temporary payload; it does not represent an appended paid entry. With capability true, actual begin separately refuses a human queue at `Max_Build_Queue`, then checks population room including production; either refusal returns before entry allocation, debit, append or begin delivery. The inspected strategic caller ignores that begin return and does not manufacture a second cancellation-shaped notification or rejected-build sound for these two later failures. Thus early capability rejection, actual-queue refusal and cancellation of admitted work are three different event contracts. Underway UI feedback may already have happened under WGEP-97. | debug build GPC-E06/09/12/13; WGEP-27/85/97/123 |
| WGEP-135 | General capability has a Boolean result and optional reason/parameter outputs; the reason is initialized to the ordinary no-reason value. Inspected failures for insufficient prepaid credits, tech-advance current/maximum mismatch, explicit lock and exhausted ground-structure capacity return false without assigning a detailed reason. An initially locked type absent from the unlocked list assigns the story-reward category; ordinary insufficient tech and ground/starbase prerequisites assign their corresponding category/required level. Blocking station orbit and type-instance limits have other categories. Callers must preserve the Boolean and optional metadata separately: a no-reason value does not prove success, and population/full-queue refusal is a later gate. Neither these metadata writes nor the checked UI/request readers establish reason-specific audio. | debug build GPC-E09/06/12/13; WGEP-18..27; WGP-61; WGUI-91 |
| WGEP-136 | The ordinary campaign starbase placement branch sends story construction and directly initializes station level, map identity, recorded builder as station owner and requested station type. Its initializer only writes those fields. This route does **not** call WGEP-131's prechange level-notification setter or WGP-58's tactical replacement ability; ordinary completion still sends production-finished after placement under WGEP-127. Consequently the setter's old/new scoring callback cannot be assumed for every constructed base. The stock `Galactic_Starbase_Level_Change` body handles loss statistics only when old type is present and new type absent; it does not play upgrade audio. Keep constructed-base, explicit level-change and tactical replacement providers distinct. | debug build GPC-E07/08/11/14; Lua GPC-X04; WGEP-40/53/127/131/132; WGP-63 |
| WGEP-137 | Tech completion removes the front, invalidates the buildable cache, selects ordinary authored completion music and speech/local fallback/ping, then assigns `Build_Advances_Tech_Level` with unlock processing and sound requested. It does not create an ordinary result. Story tech notification precedes unlock enumeration; actual new unlock plus local recipient and present faction event gates the separate new-options request. Historical completion count and production-finished follow tech assignment. A direct setter call, including a repeated same-level assignment, has its own story/unlock behavior but does not imply a production-complete announcement. Stock campaign tech completion uses type `SFXEvent_Build_Complete`, not an invented faction tech-complete event. | debug build GPC-E02/07/10; XML GPC-X02; WGEP-40/42/47/96/130; WGP-64 |

These rows **narrow** the earlier refusal and base-change descriptions;
WGEP-136 is a separate completion route, not a replacement for the sourced
setter behavior in WGEP-131. WGP-59..64 supplies population, blocked-queue
advisor and type-versus-faction sound distinctions. Credit observer triggering
remains WGP U-12; refund/playback/reentrant/restore qualifications remain
U-06/U-09/U-14 and GFS-U02.

| Ready-to-implement contract | Provider seam | Proposed acceptance check |
| --- | --- | --- |
| Refusal stages, WGEP-134/135 | UI admission, general capability Boolean/reason, later queue/population admission and immutable entry correlation | A locally admitted UI click can later fail. Separately force explicit lock, tech prerequisite, slot exhaustion, full queue and global population exhaustion; compare cancellation-shaped delivery versus no appended entry/no begin, unchanged credits and no invented rejection SFX. |
| Base/tech completion, WGEP-136/137 | Requested type, recorded builder, nullable result, ordinary type feedback, constructed station state, separate level setter and actual unlock list | Observe completion requests before tech/base mutation and finished after it. Compare local/nonlocal builder, speech present/absent, zero/new unlock, script setter and ordinary base construction; prove which route has a level-change callback and preserve null primary results. |
| Live population feedback, WGP-59; WGEP-122..125 | Authoritative census including actual queues, capacity and retained previous local observation | Two owned producers contend for one remaining slot on 1/2/4/8 workers; then test rising/full/equal/initial-zero/capacity-only loss and presentation attachment without hash changes. Notification state must not become census usage or an extra reservation. |

## Black-market offers, encyclopedia and acquired state

Read-only **debug build** BMR-E01..13 and effective **XML/Lua data** BMR-X01..04,
inspected 2026-10-10. Private evidence and the source audit remain in ignored
research storage. This section supplies the provider facts for H063/064 and
GHUD-S29 in the [HUD inventory](../gc-hud-inventory.md); GHUD-C09 still owns
retail visual qualification. These are bounded routine reads, not an executed
purchase/save trace or a new outer frame schedule.

### Offer and action contracts

| ID | Rule and bounded qualification | Source |
| --- | --- | --- |
| WGEP-138 | Registry initialization creates an item for each XML database node in traversal order and resolves faction references after reading. This initializer and WGEP-102's enumeration have **no per-campaign item whitelist or buyer-tech gate**. Stock data defines ten items, five for Empire and five for Rebel, at ordinary tech 1..5; that is a data census, not a runtime limit or fixed list. Availability combines the loaded registry, target faction/corruption/opposition and ability admission. Campaign story state can alter grants or admission under WGEP-139; tutorial data and pricing remain WGEP-103..105. | debug build BMR-E01/02; XML BMR-X01 |
| WGEP-139 | HUD black-market admission first rejects a planet present in the tutorial restriction list, then scans `Activated_Black_Market_Ability_Names` and succeeds when an actor/subobject can activate a listed ability against that planet. `RESTRICT_BLACK_MARKET` resolves the named planet type and applies to its first found object: an absent second reward parameter restricts; integer zero removes restriction, nonzero restricts. `GIVE_BLACK_MARKET` searches registered item names and adds the match to the subplot's local player's independent, deduplicated acquired list; this reward does not debit, queue production, consume a buyer or use the paid-purchase feedback helper. Stock story grants Buzz Droid technology at the main-story opening, and an authored demo plot grants all ten. Stock Lua can set the player's black-market tutorial flag true or false with `Set_Black_Market_Tutorial`. These are data-driven campaign differences; the inspected helpers establish no extra campaign-name filter. | debug build BMR-E03/04/05; XML/Lua BMR-X02/03; WST-41 |
| WGEP-140 | Ability application prepares offers and buyer-displayed-tech absence before its control split. A **local human** receives a menu; a nonlocal, non-AI owner receives no menu from this route, and AI uses WGCH-37's affordable weighted selection. Human menu activation proceeds only without an active dialog and with a present, hidden market shell. It copies the prepared offer vector and retains buyer identity, target planet and absence duration, then publishes target display name, menu strings and WGEP-109's faction/technology header, not a new price calculation. It sends the generic tutorial event `open_black_market_menu` and refreshes the menu. The inspected activation assumes a first offer and resolved faction/player; it supplies no empty-list fallback. | debug build BMR-E02/06; WGEP-108/109 |
| WGEP-141 | Menu refresh reads **current local credits**, but item price and owned flags come from the copied prepared vector. It visits at most the authored component-group capacity, places each visited offer using ordinary item tech, binds the original vector index and item identity, and resolves `Icon_Name` and `Localized_Name`. Owned offers remain visible and disabled with `TEXT_BLACK_MARKET_PURCHASED`; other offers use WGEP-80's rounded-credit comparison. This refresh does not recompute faction eligibility, prepared price or acquired state. A fresh preparation observes the acquired list. Hiding an active menu removes its dynamic descriptions, clears the copied offers, target and buyer binding and resets stored absence to zero; the cancel button takes this hide route without purchase/debit. A mod with more offers or colliding tech slots needs a bounded presentation policy, not an inferred additional sim gate. | debug build BMR-E07/08; WGEP-80/102/109 |
| WGEP-142 | A purchase button submits its bound **prepared vector index**. The active-menu/in-range sender copies that offer into an outgoing special-action event together with buyer identity, target planet identity, prepared absence and a flag for a one-entry prepared vector. Event execution resolves the target and requires planetary behavior, then performs WGEP-55/80's buyer-resolution, exact-price debit, independent unlock and despawn route. The inspected executor does not read the one-entry flag. **Before execution**, the sender notifies `STORY_BUY_BLACK_MARKET` using the item's authored name, then hides/clears the menu. That story notification proves submission, not a successful debit or unlock. Neither sender nor execution invents an ordinary production entry; execution has no owned-item, target-faction, target-corruption or prepared-price recalculation in these inspected routes. Ordinary stale-menu reachability remains WGEP-BM-U01. | debug build BMR-E09/10; WGEP-55/80; WST-36 |
| WGEP-143 | Item encyclopedia selection requires an active market menu and an in-range prepared index. It resolves the selected item's `Icon_Name`, `Localized_Name` and `Localized_Description`, adds a localized header and description, then reads `Used_By_Units` in authored order for **three icon slots**. Each slot resolves the unit type and its icon; an absent/unresolved/empty-icon entry leaves a blank slot. Later unit references are not displayed by this consumer. This item routine adds no prepared price line, no acquired-status line and no unit creation or ability activation; menu price/status remain separate under WGEP-141. Item ability benefits are authored text, not an automatically generated description from `Ability_Names`. Stock text keys are listed below; localized wording and overflow remain GHUD-C09. | debug build BMR-E11; XML BMR-X01 |
| WGEP-144 | Player save writes the acquired black-market item count and each item's name-derived identity. Load clears the existing list, reads those identities, resolves each against the loaded registry and appends resolved items in saved order; an unresolved item asserts and is not appended. This load branch does **not** call purchase or the unlock-feedback helper, spend credits, grant production or schedule buyer absence. The player writer separately retains its tutorial flag. This sources acquired-list reconstruction, not complete tutorial/menu/event/hero restoration, collision policy, foreign save compatibility or first-resumed parity; those remain WGEP-BM-U02 and WGS-U05/08. | debug build BMR-E12; WGEP-82; [session/save](gc-session-save.md) |
| WGEP-145 | Acquired-item entitlement is queried by the **current owning player**, not by the hero that bought it. When black-market availability checking is enabled outside multiplayer tactical, a special ability marked `Must_Be_Bought_On_Black_Market` resolves its authored ability-name identity to the **first matching registered item's `Ability_Names`** and denies availability when that item and owner resolve but the owner lacks it. No matching item or no resolved owner does not deny in these inspected entitlement checks; disabling the check or entering multiplayer tactical bypasses them. The unit-ability variant uses the GUI-activated ability identity and, for a team, the first member's ability data when present, while retaining the team's owner for entitlement. The separate autofire consumer also tests entitlement outside multiplayer tactical before its own cinematic/modifier/AI policy; an acquired item alone does not prove activation or autofire. `Used_By_Units` is encyclopedia metadata in WGEP-143, not this unlock gate or a spawned-unit list. Every skirmish is a multiplayer tactical game (abilities walk WLS-01, WST-01; heroes walk U-04), so the gate applies to single-player battles launched from galactic conquest; whether a battle of a multiplayer galactic campaign counts as a multiplayer tactical game was not read (unverified). Checked against the Focumentation wiki (Unit Abilities). | debug build BMR-E13, F2165-E03; XML BMR-X01/04 |

### Stock encyclopedia keys

This is the effective XML census for WGEP-138/143, not a hard-coded offer or
localization table. Read these fields from each registered item, including mod
items. Ordinary costs are inputs to WGEP-58, not the final displayed/executed
prices. Tutorial field omissions remain WGEP-104.

| Item name | Faction / ordinary tech | Ordinary base credits | Localized name key | Localized description key |
| --- | --- | --- | --- | --- |
| `Buzz_Droid_Tech` | Empire / 1 | 1000 | `TEXT_BM_BUZZ_DROID_TECH` | `TEXT_TOOLTIP_BM_BUZZ_DROID_TECH` |
| `Proximity_Mine_Tech` | Rebel / 1 | 1000 | `TEXT_BM_DEF_MINES` | `TEXT_TOOLTIP_BM_DEF_MINES` |
| `Disruptor_Tech` | Empire / 2 | 2000 | `TEXT_BM_DISRUPTOR_TECH` | `TEXT_TOOLTIP_BM_DISRUPTOR_TECH` |
| `Full_Salvo_Sync` | Rebel / 2 | 2000 | `TEXT_BM_WEAPON_SYNC` | `TEXT_TOOLTIP_BM_WEAPON_SYNC` |
| `Core_Overload_Tech` | Empire / 3 | 3000 | `TEXT_BM_STRAT_DEMO` | `TEXT_TOOLTIP_BM_STRAT_DEMO` |
| `Sensor_Jamming_Tech` | Rebel / 3 | 2800 | `TEXT_BM_SENSOR_JAMMING` | `TEXT_TOOLTIP_BM_SENSOR_JAMMING` |
| `Vehicle_Cloaking_Tech` | Empire / 4 | 3900 | `TEXT_BM_CLOAKING_TECH` | `TEXT_TOOLTIP_BM_CLOAKING_TECH` |
| `Weaponized_Carbonite_Tech` | Rebel / 4 | 3700 | `TEXT_BM_WEAPONIZED_CARBONITE` | `TEXT_TOOLTIP_BM_WEAPONIZED_CARBONITE` |
| `Point_Defense_System_Tech` | Empire / 5 | 4800 | `TEXT_BM_POINT_DEFENSE` | `TEXT_TOOLTIP_BM_POINT_DEFENSE` |
| `Shield_Leech_Tech` | Rebel / 5 | 4200 | `TEXT_BM_SHIELD_REDIST` | `TEXT_TOOLTIP_BM_SHIELD_REDIST` |

The inspected menu also uses `TEXT_BLACK_MARKET_TITLE`, `TEXT_BUTTON_CANCEL`,
`TEXT_TECH`, `TEXT_BLACK_MARKET_SLOTS` and `TEXT_BLACK_MARKET_PURCHASED`.
These keys and authored item icons are distinct from the planetary corruption
information text; H064 must bind the selected prepared item.

### Comparison and ready-to-implement contracts

Comparison base `82c58523`: `src/sim/galactic/economy_corruption.cpp` already
prepares copied offers, buys by retained index, debits the current buyer owner,
retains independent unlocks and publishes purchase/absence requests. Its
`economy_corruption_encoding.cpp` retains unlocks and prepared menus. Those
are **same scoped retained-offer/debit/unlock contracts** under WGEP-55/58/80,
not proof of retail command delivery or live actor integration.
`src/campaign/planet_economy.cpp` reads item price/faction/tech data, while
`include/eawr/sim/galactic/snapshot.hpp` has no black-market menu publication.
Live ability admission, story-submission ordering, observer-filtered market
publication and stable item-to-encyclopedia binding are **missing provider
seams** at this base. Encoding our prepared menus is a project choice; the
player save read in WGEP-144 does not establish that retail restores an open
menu. Successful session purchase marks the menu inactive but retains its
vector, whereas WGEP-141 clears the native UI copy at submission. The provider
must use active status and its owning presentation lifecycle rather than expose
retained inactive offers as an open menu. Remaining modifier-ledger,
validation/omission and live lifecycle seams are not declared complete here.
No application-status or completed-capture claim is added here.

The checklist below proposes acceptance tests; this walk executes none of them.
Implementation consumes authoritative copied state and queued commands. The
viewer receives immutable facts filtered for the local observer; presentation
attachment must keep simulation hashes unchanged.

| Work item | Rule IDs | Provider seam | Proposed observable acceptance |
| --- | --- | --- | --- |
| Economy and campaign availability (legacy EAWR-1130) | WGEP-55/58/80/81/99..110/138/139; WGCH-37/38 | Loaded item registry, current allegiance/faction/corruption, live actor/ability eligibility, tutorial flag/restrictions, grants and player acquired list | Custom item and renamed catalog input, buyer tech below item, opposing faction switch, absent tutorial fields, common/uncommon and zero-floor prices; direct story grant changes a newly prepared owned flag without debit/despawn. No new fixed campaign or item list. |
| Menu and encyclopedia (legacy EAWR-2308); GHUD-S29 | WGEP-80/109/140/141/143; H063/064 | Observer-local copied offers with stable index/item/buyer/planet, exact prepared price, owned flag, current balance, ordinary layout tech, header tech and data-resolved text/icon/unit references | Owned disabled, affordable/unaffordable, balance 100.10 versus price 100.40 (same rounded bucket, exact execution rejects), cancel with unchanged account, nonlocal observer receives no private menu, tutorial pricing with ordinary layout, missing icon and four unit references. Capture GHUD-C09 before claiming pixel/text parity. |
| Submitted purchase and lifecycle (legacy EAWR-1130) | WGEP-55/80/108/142 | Queued copy of selected item/price/buyer/planet/absence; distinct submission story event, execution result, account/unlock mutation and lifecycle request | Hide after submission, story event before a later exact-credit rejection, buyer or target missing before execution, repeated direct owned offer, changed buyer owner, no queue entry and success-only absence request. Compare canonical state at 1/2/4/8 workers. Stale ordinary interaction remains WGEP-BM-U01. |
| Acquired-state checkpoint (legacy EAWR-1130) | WGEP-82/144; WGS-U05/08 | Registry-resolved player item identities, credits, tutorial and lifecycle contributors; observer menu preparation after restore | Restore a purchased and a story-granted item into fresh state: one unlock each, unchanged account, no repeated grant/debit/feedback/despawn, freshly prepared purchased-disabled offer. Compare acquired state independently of open-menu restoration and use WGEP-BM-U02 for runtime continuation. |
| Acquired ability entitlement (legacy EAWR-1130) | WGEP-139/143/145 | Current unit/team owner, authored ability identity/market-required flag, ordered item ability references and acquired list; separate ability/autofire service | Grant an item, transfer the unit between owners, and compare required/nonrequired abilities, missing item mapping, two items sharing an ability, first team-member data and multiplayer-tactical bypass. Encyclopedia unit references must not grant units or become an entitlement filter. Tactical activation and autofire modifiers remain the owning services. |

WGEP-138/139 **source previously missing** registry/campaign consumers;
WGEP-140..142 **narrow** preparation/publication/submission boundaries;
WGEP-143 **sources previously missing** H064 item content;
WGEP-144 **extends** WGEP-82's writer audit with a bounded acquired-list reader;
WGEP-145 **sources previously missing** current-owner ability entitlement.
Existing exact-price, tutorial, uncommon and absence arithmetic stays the same.
The separate corruption income/discovery seams in WGEP-111..115 and U-11 are
unchanged by this focused offer walk.

### Runtime qualifications

| ID | Still unverified | Capture recipe |
| --- | --- | --- |
| WGEP-BM-U01 | Ordinary stale-menu delivery after buyer/target/ownership changes, including which copied facts remain visible and which UI action remains reachable. WGEP-80/141/142 source bounded readers, not reachability. | Prepare on an opposing corrupted planet, then separately remove buyer, remove target, change owner/faction/corruption, grant the item through story and change balance. Log retained offer/index/price/owned flag, enabled button, submitted payload, story notification, execution account/unlock and lifecycle result. Reopen to distinguish fresh preparation; direct command injection is a separate control. |
| WGEP-BM-U02 | Complete purchased-state continuation with tutorial flag, open menu, outgoing event, shared random state and absent hero. Acquired-list reconstruction alone does not settle first-resumed parity. | Save before submission, after submission but before debit, and after successful debit with hero absent; restore fresh state and compare three admitted steps against uninterrupted controls. Record credits, registry-resolved unlocks, tutorial state, prepared menu and event identities, feedback requests, random draws and hero return. Reopen the market and inspect purchased-disabled state; no byte-format compatibility claim. |
| WGEP-BM-U03 | Retail pixel layout, final integer price-label conversion, localization/overflow, and display behavior for duplicate tech slots, excess offers or omitted tutorial fields. | Use GHUD-C09 with both target factions, owned/high-tech offers, balance/price on either side of a half-credit boundary and encyclopedia selection. Capture title, target, header, prices, purchased state and beneficiary icons at native resolution; add a mod item with four unit references and a colliding tech slot, plus missing icon/tutorial data. Log exact account/price beside displayed integer text; do not infer a default or a sim admission gate from one screenshot. |

## Research technology: end-to-end implementation audit

This appended audit covers the technology contribution to full campaign
sign-off (legacy EAWR-1969). It checks merged code at
`1710b5a2ef1068b2135fac3149a160b888781df2` against the existing economy,
[corruption/heroes](gc-corruption-heroes.md), [AI](gc-ai.md) and
[HUD inventory](../gc-hud-inventory.md) rows. The original walk's historical
"missing" columns are not a verdict on this later implementation. A sourced
rule can still have a missing consumer; a fixture or copied menu does not
establish a playable campaign operation.

Read-only **debug build** RT-E01..10 and effective **XML/Lua data** RT-X01..04
were checked on 2026-10-10. Their raw reads and opaque evidence map remain in
ignored research storage. These are bounded call-path facts; they add no new
frame-service schedule. WGA-379/390..399 and the existing frame-order walk own
shared tech and timeline publication. Retail-only qualifications below remain
questions.

### Additional rules and qualifications of earlier rows

| ID | Rule and conditions | Source |
| --- | --- | --- |
| WGEP-146 | Stock Empire research uses four active, authored tech-building types with required internal tech **1, 2, 3, 4** and target tech **2, 3, 4, 5** respectively. Their unmodified costs are **2000, 4000, 6000, 8000 credits** and authored durations are **200 seconds each**. Every one requires a local `E_Ground_Research_Facility`, ground-base level **1**, and current-per-player type limit **1**; multiple-factory time reduction is false. The fifth example in this file is commented out and is not a registered offer. These are stock data, not a fixed research roster or intrinsic facility-name gate: mods can change references, requirements, cost, time and target levels. WGEP-19/21/25/29..34 govern actual admission and adjusted quantities; GHUD-S03 places these purchases in ordinary production. | effective XML RT-X01; WGEP-19/21/25/29..34; WGUI-91 |
| WGEP-147 | The tech setter's unlock pass traverses the **complete registered object-type list**, not the current planet's legal offers or the player's owned producers. It adds affiliated, initially locked, slicer-unlockable, not-already-unlocked types whose required tech is **strictly below** stored current tech. It does **not** exclude the dynamic explicitly locked list here; that list still overrides production admission under WGEP-19 and theft enumeration under WGCH-54. Store/clamp and story notification precede this pass, even for an unchanged direct assignment. A price-zero type or a type with an unmet facility requirement can enter the unlock list without becoming currently buildable. | debug build RT-E02; same WGEP-42/130; qualifies complete input coverage |
| WGEP-148 | Increment-tech first compares stored current tech with stored maximum. Below maximum it calls the shared setter with **current + 1**, unlock processing enabled and new-options sound requested. At or above maximum it returns current tech without calling the setter: no setter story notification or bulk unlock pass follows that increment. Theft's explicit advance flag calls this increment helper **after** successfully debiting and unlocking the selected type; an ordinary failure, unaffordable execution or generic auto-capture does not advance. This is distinct from Empire completion's authored target assignment and from a same-level direct setter call. | debug build RT-E03/04/09; WGCH-34/35/100/102; WGEP-42/130/137 |
| WGEP-149 | Next-tech lookup traverses registered types in their retained order and returns the **first** type whose `Build_Advances_Tech_Level` equals the requested target and whose affiliation contains the player faction. No match returns absent. This helper does not compare the candidate's `Tech_Level`, prerequisites, locks, price, maximum tech or buildability, and does not reject multiple matches or try a later match when the first is unusable. `CanAdvanceTech` requests **current + 1**, then considers the current perception system's target-location list. At each target it first accepts that exact type at either production head when the recorded builder is this player, otherwise applies ordinary eligibility ignoring payment. An absent next type returns numeric zero; all tested targets failing returns zero. This settles WGA-290's previously unqualified first-type selection; general eligibility still belongs to WGEP-18..27. | debug build RT-E01/08; WGA-290 |
| WGEP-150 | Ordinary theft preparation requires an opposing planet with a resolved playable controlling faction and a **nonempty** actor-faction sliceable list under WGCH-54. The empty-list increment/retry inside application is reached only after this appropriateness gate; it is not a normal free-advance action with unchanged inputs. Human submission and AI execution use advance exactly when the **whole prepared list** has one item, not when the affordable subset has one item or the clicked item is last in display order. AI filters by prepared-price affordability and gives each remaining choice weight **1**. Opening a human menu leaves the actor present and returns false; no affordable AI choice also returns false without theft/removal. | debug build RT-E05/06/07; same WGCH-54/64/101/102; qualifies WGCH-32 |
| WGEP-151 | Stock Underworld has **stored current/maximum technology** even though its ordinary HUD hides the tech readout and it has no Empire-style research chain in the checked stock tech-building data. The checked Underworld campaign entry authors starting/max **2/5**; this is not a universal campaign default. Market purchase adds an item to the player's independent acquired-item list and does not set/increment player tech or create ordinary production. Displayed buyer tech still indexes market absence through WGEP-81/99; item tech arranges the offers, and `Ability_Names` entitlement follows WGEP-145. Corruption admission/effects remain WGCH-01..19, rather than a technology-level substitute. Preserve authored tech and mod/script writers; do not force Underworld accounts to zero or gate items by buyer tech. | effective XML RT-X01/02; WGEP-55/81/99/102/138/145; WGUI-35 |
| WGEP-152 | Stock Empire's advance-tech Lua plan authors a production task force, waits on `Produce_Force`, and exits on production failure; its alternatives include research types and the strategic weapon. Stock Rebel's plan instead obtains an enemy slice target through an authored equation, produces its droid force at that target, moves there and lands it. Its plan-result success is not itself a tech grant or proof of a successful theft draw. The checked tech-windfall intervention reads current player tech, authors a **current + 1** `SET_TECH_LEVEL` story reward for Empire/Rebel and exits for other factions. These data-driven routes are separate consumers of shared tech and must follow authored scripts, not fixed task-force or module names. | effective Lua RT-X03/04; WGA-342; WGCH-34; WST-41 |
| WGEP-158 | **Ground-construction type predicate.** A type is ground-constructible when its stored `Build_Cost_Credits` is at least **1** (the type value, before any location modifier) and either `Required_Ground_Base_Level` is **nonzero** or the type behaves like `DUMMY_GROUND_STRUCTURE`. The debug build's typed role metadata confirms that this public `Behavior` token denotes the role tested by the predicate. Inheritance resolves first: a variant is judged by its effective tags, never by its parent's identity or name. A positive price alone never qualifies. | debug build GEP-E47; G3531-E01/E02: ground-construction reader and typed role mapping; data |
| WGEP-159 | **Space-construction type predicate.** A type is space-constructible when its stored `Build_Cost_Credits` is at least **1** and either `Required_Star_Base_Level` is **nonzero** or the type behaves like `DUMMY_STAR_BASE` with `Base_Level` exactly **1**. A higher-level starbase qualifies through its own nonzero required starbase level, not through this role clause. A positive price alone never qualifies. | debug build GEP-E47; data |
| WGEP-160 | **Gate placement.** Direct production accepts a type only when the ground predicate **or** the space predicate holds. The check follows WGEP-18's content rejections (cinematic-only, unaffiliated, price below **1**, negative time, direct ground-base types) and precedes affordability, tech, lock, ownership, prerequisite, slot and population gates; failing it refuses without a reason payload, like the other content gates. This is the clause that WGEP-18 words as "neither ground nor space production". Independently of that admission OR, the WGEP-158 ground predicate selects the production domain: true selects ground, false selects space. GHUD-S90 uses the same predicate for the visible tab; category flags do not override it. Because the answer is a property of the type, the human purchase list, queue command, Lua production and AI producer options must all read the same result. | debug build GEP-E48; G3531-E01 |
| WGEP-161 | **Stock consequence.** Of the stock types with a positive authored cost that the other shape gates would admit for direct production, exactly seven fail both predicates: `Home_One`, `Fin_Home_One` (its cinematic-only variant, already refused by the cinematic gate), `Millennium_Falcon_Space`, `Millennium_Falcon_Ground`, `Sundered_Heart`, `Sundered_Heart_Cinematic_Clone` and `Red_Squadron`. Each authors a positive cost with both required levels **0** and neither role. Ordinary ships and companies carry a nonzero required level; ground structures qualify by role; the level-one starbase types qualify by role. `Millennium_Falcon_Ground` inherits the parent's cost and is refused through its own tags. | data; debug build GEP-E47 |
| WGEP-162 | **Mod behaviour.** There is no name or hero list: a named hero, a variant or any authored type becomes constructible by authoring a nonzero required level (or the structure/starbase role) and stops being constructible when both are zero. A positive price on a type with zero levels and no role is never offered, whatever its class or affiliation. | debug build GEP-E47; data |

### Source-scoped income blockers and the credit-halt writer

| ID | Rule and conditions | Source |
| --- | --- | --- |
| WGEP-163 | **Saved blocker ledger.** An actual planetary income is blocked while any unexpired Boolean income-blocking modifier holds on that planet; several sources combine with logical OR, and each source is identified by its unique ability source, not by the planet owner. The saved row is planet, source identity, the GC frame it registered and its first expired GC frame. Expiry is measured in active GC frames, so an ordinary pause (which does not advance the frame) never expires a halt, and the row is removed at the commit that advances the frame to its first expired frame, before the next fiscal read. Orbital blockade (WGM-C02), AI hypothetical income and political ownership are **separate inputs** and never write or read this ledger. The ledger lives in the income state section; the checkpoint galaxy section version is **25** for it. | debug build CPI-01..03; WGEP-05/75; WGM-C02 |
| WGEP-164 | **Consumers read one predicate.** A blocked planet's actual income, the pure forecast and the published modifier breakdown are all zero: the calculation returns before any base, alignment, political, trade or mining refresh and before siphon subtraction (WGEP-05/75). Collection of a blocked planet does **not** rewrite the retained political source (WGEP-08/75): the next unblocked collection refreshes it. Expired sources are removed first, so a planet whose last source expired at frame F is collected normally at F. Gambling service, ownership cleanup and the retained siphon/piracy ledgers keep their own cadence; whether the original's corruption payment consumers read the blocked flag is not qualified (WGEP-BLK-U02). | debug build CPI-01..03; WGEP-05/08/75 |
| WGEP-165 | **Native credit-halt writer.** The sabotage route registers `true` under the acting object's unique ability source for the type's authored `Duration_Of_Credit_Halt` seconds, converted once to GC frames at the nearest frame (half up), the project's conversion for fiscal-cycle and gambling-interval seconds. The type's `Can_Halt_Credit_Production` flag and duration come from authored ability data: a mod that authors them on any object type enables the writer for that type, and no type or hero list exists in code. A zero-length halt registers nothing. A repeated write by a **live** source never duplicates or extends its row (the receipt reports the retained expiry); an expired source writes again as a new halt. Explicit removal (the source terminated) deletes the row and is journaled once. Writes and removals are native effect 18 (barrier admission) and checkpoint reconstruction replays the recorded frames. Admission, the sabotage price, the actor's consumption and planet eligibility belong to the ability host; the writer only requires a live object of the income gather on the target planet. | debug build CPI-01..03; WGCH-31; WGEP-163/164 |
| WGEP-BLK-U01 | Whether a halt written by a source that is already registered, while it is live, replaces, keeps or extends the original expiry, and the exact rounding of authored seconds to the original's frame time base. The project keeps the first row (never extends) and rounds to the nearest frame; a capture with a single actor sabotaging the same planet twice would settle it. Stock FoC never authors either tag, so this is a mod-facing question. | Sabotage the same planet twice with one actor under a mod that authors `Can_Halt_Credit_Production`; record the second expiry and the planet's actual income at the frames around each. |
| WGEP-BLK-U02 | Whether the corruption payment consumers (siphon fraction, piracy, benefit route) read the blocked predicate of a collected planet, or only the zero actual income. The project feeds the zero total to those consumers and leaves their ledgers untouched. | Capture a corrupted, siphoned, income-blocked planet across one fiscal cycle; compare account delta, siphon expiry and piracy. |

### Gap table against merged campaign code

Evidence status below distinguishes **sourced**, **qualified unknown** (a named
U-row), and **missing evidence**. All bounded behaviors listed here have source
rows; the remaining questions have capture recipes. Implementation status
describes inspected code, not whether a test passed during this research task.

| Implementer behavior | Evidence status / rule IDs | Merged file and function; implementation status | Required seam or focused test |
| --- | --- | --- | --- |
| Starting/max/internal versus displayed tech for every faction | sourced: WGS-13/73/76/77, WGEP-99/151 | `src/campaign/setup.cpp`, `plan_campaign_setup` / `CampaignRoot::create`; `src/campaign/live_setup.cpp`, `prepare_live_campaign_services` assignment callback: implemented startup inputs. `src/sim/galactic/economy_corruption.cpp`, `execute_black_market`: implemented displayed buyer-tech consumer. | Keep current/max independent of visible label; compare Rebel adjustment, Underworld authored 2/5 and mod faction adjustment. |
| Empire facility, ownership, current-level equality, maximum, lock and global type-limit admission | sourced: WGEP-18..27/146 | `src/campaign/live_setup.cpp`, `offer` / `complete_production`; `src/sim/galactic/production.cpp`, `production_admission`: implemented within supplied production catalog and live planet facts. | Rename the facility/type; absent local prerequisite, target above maximum, old-tier purchase and simultaneous producers with the same type limit. |
| Empire research cost/time and modifier lifecycle | sourced: WGEP-29..34/73/74/92/146 | `src/campaign/setup.cpp`, `prepare_production_offer`; `src/campaign/live_setup.cpp`, `offer`; `src/sim/galactic/production.cpp`, `prepared_production_offer`: partial. Live income/corruption factors exist, but `offer` supplies technology-purchase factor 1 and the runtime re-preparation does not apply a live technology-price modifier list. | Shared expiring technology-price provider; synthetic research-specific price modifier versus ordinary ship, time/factory and corruption inputs. |
| Actual ordinary queue, completion moment and authored target | sourced: WGEP-28/35..43/65/127/137 | `src/sim/galactic/production.cpp`, `execute_production` / `service_production`: implemented tech assignment after front removal and completion event, with no ordinary result. Partial notification coverage: historical count increments before tech assignment; no shared setter/story/new-options callback is installed there. | Notify completion, setter/story/unlocks, historical count, then finished using stable build/builder identity. Test an authored non-unit target jump, cancellation on facility loss and zero ordinary result. |
| Full-registry automatic unlocks; explicit lock precedence | sourced: WGEP-19/42/130/147 | `src/campaign/live_setup.cpp`, `complete_production`; `src/sim/galactic/production.cpp`, `service_production`: partial, both scan per-planet offers. The live offer catalog excludes a type with nonpositive build cost unless initial-placement authoring retains it. | Canonical registered-type unlock input independent of producer offers; price-zero locked sliceable type, unmet prerequisite, strict lower/equal boundary and explicit lock retained after unlock. |
| Current/eventual research offers and purchase/queue HUD | sourced: WGUI-91, GHUD-S03/S05; qualified unknown: WGEP-RT-U01 | `src/sim/galactic/production.cpp`, `observed_production_offers`; `apps/viewer/src/galactic/galactic_hud.cpp`, `refresh_hud_facts`: partial live publication and renderer exist. Current admission is computed; the inspected publication does not populate eventual eligibility. | Current/eventual distinction, prepared quantities and cache invalidation after every tech writer; ordinary producer purchase, progress/cancel and newly unlocked choices under GHUD-C02/C09. |
| Rebel opposing-planet and registered-type choice preparation | sourced: WGCH-33/36/54/101, WGEP-150 | `src/sim/galactic/hero_actions.cpp`, `prepare_theft`: implemented bounded fixture enumeration/price/chance/timer. Campaign actor/type/admission provider and canonical action execution: missing. | Supply actor faction/current ownership, registry order, independent locked/unlocked lists, authored curves and synchronized draws; initially empty list must refuse without tech mutation. |
| Whole-list advance flag, chance/failure, debit and absent hero | sourced: WGCH-34/35/64/100/102, WGEP-148/150; qualified unknown: WGEP-RT-U01 | `src/sim/galactic/hero_actions.cpp`, `prepare_theft` / `select_theft_ai` / `steal`: fixture success/failure exists. `apps/viewer/src/galactic/hero_action_hud.cpp`, `choose_hero_action`: controlled fixture acceptance only; live queued action missing. | Two prepared choices with only one affordable must not advance; success/failure/capture each preserve their own account and actor lifecycle; no ordinary production queue. |
| Theft success to shared current/max tech and bulk unlock/story state | sourced: WGEP-147/148; WGCH-34 | `src/sim/galactic/hero_actions.cpp`, `steal`: differs even within the fixture, directly increments unsigned tech without maximum or shared setter/automatic lower-tier unlock pass. Canonical production/economy bridge: missing. | Current at/below maximum with advance true; selected type unlock and debit still occur at maximum, while tech remains fixed. Below maximum, notify story and unlock other lower-tier types through the shared owner. |
| Underworld market/corruption equivalents and ability entitlement | sourced: WGEP-55/80/81/99..110/138..145/151; WGCH-01..19 | `src/campaign/planet_economy.cpp`, `prepare_corruption_economy` / `prepare_black_market_buyer`; `src/sim/galactic/economy_corruption.cpp`, `execute_black_market`: partial loaded items, prepared menu, debit and acquired state exist. Live setup does not populate buyers through `prepare_black_market_buyer`; shared lifecycle and acquired-item ability consumer remain missing. | Bind the live actor/admission and acquired-item entitlement; high item tech with lower buyer tech, purchased-disabled offer and no change to current tech. Existing market U-rows own detailed qualification. |
| Type construction gate (positive cost plus level or role) | sourced: WGEP-18/158..162; qualified unknown: WGEP-BG-U01 | `src/campaign/live_setup.cpp`, `offer` derives the type predicates; `src/sim/galactic/production.cpp`, `production_admission` refuses an offer that fails them; human list, queue command, Lua production and AI producer options share that call: implemented. | Positive-cost zero-level type refused on both queues, nonzero level or structure/level-one starbase role admitted, equal result on 1/2/4/8 workers. |
| Build offers versus independent starbase level/cap | sourced: WGEP-22/24/51..53/136 | `src/sim/galactic/production.cpp`, `production_admission` / `service_production`: implemented separate tech, station and planet-cap gates within supplied offers. | Raise tech while leaving station level fixed; station upgrade must not raise tech. Compare low station/high tech, high station/low tech and planet cap. |
| AI next-tech type selection and target eligibility | sourced: WGA-289/290, WGEP-149 | `src/script/foc/galactic_ai_economy.cpp`, `GalacticWorld::player_economy_token` (`CANADVANCETECH`): differs for multiple matches, returns unavailable instead of selecting the first registered type. It scans snapshot producers instead of explicitly supplied perception target-location order. | Ordered registry identity and perception-target provider; first match blocked/later match buildable, reverse registration, exact queued head and no-match numeric zero. |
| AI tech-dependent selection and authored advance plans | sourced: WGA-289/342/382, WGEP-152 | `src/campaign/ai_perception_content.cpp`, `prepare_ai_perception_content`; `src/script/foc/galactic_ai_economy.cpp`, `player_economy_token`; `src/campaign/ai_selection_content.cpp`, `prepare_ai_selection_content`: partial tech metadata/readers and authored plan catalog exist. These do not establish completion of the Rebel theft host or shared tech writers. | Feed completed tech/locks into production and force selection; equal versus greater timeline comparison, authored replacement plans, denied research and failed theft must not invent success. |
| Shared tech through tactical child and timeline readers | sourced: WGA-375..384/390..399; qualified unknown: GCT-U01 | `src/campaign/space_entry_stock.cpp` supplies conflict-owner tech to tactical purchasing accounts. `src/campaign/game_session.cpp` timeline methods and `set_child_timeline_tech` retain separate scalars/provider: partial; full retained-player authoritative publication is a separate seam. | Highest-tech noncombatant, swapped combatant order and later authoritative write; do not replace all-player tech with two purchasing accounts. Existing child-tech/frame-service work owns order. |
| Script/story writer and all downstream invalidations | sourced: WST-41/64, WGEP-130/147/152 | `src/sim/galactic/story_adapters.cpp`, `apply` has `TechReward` in its independent adapter state. Bridge to canonical campaign tech/locks, production publication and AI readers: missing in the inspected campaign host. | Shared setter with explicit maximum-change policy; direct same-level assignment versus max-level increment; story reward updates later consumers without resetting station level. |
| Save/restore and first resumed action/reader | sourced local data ownership: WGEP-57/82, WGA-397/398; qualified unknown: WGEP-RT-U02/U03 | `src/sim/galactic/encoding.cpp` / `replay.cpp` encode/decode economy current/max; `production_encoding.cpp` carries production lists; `src/campaign/game_session.cpp`, `save_timeline` / `restore_timeline` retain scalars. Full theft menu/event/actor and writer continuation: missing. | Saved prepared request, maximum, unlock/lock lists and deliberately stale scalars; compare resumed results rather than only successful decoding. |

This table is an implementation-readiness audit, not a request to duplicate
existing production (legacy EAWR-682), theft (legacy EAWR-1143), market economy
(legacy EAWR-1130), story (legacy EAWR-2044), HUD (legacy EAWR-1155) or child-tech
(legacy EAWR-1955) ownership. The highest-impact remaining work is the shared tech
writer/unlock registry, live theft host, live market actor binding, and ordered
AI next-tech identity. Existing production tests cover the bounded Empire
route; they do not close those consumers.

### Data/tag coverage boundary

Keep `Build_Advances_Tech_Level` numeric and separate from `Tech_Level`,
`Required_Ground_Base_Level`, `Required_Star_Base_Level`, `Max_Space_Base`,
`Starting_Tech_Level`, `Max_Tech_Level` and
`Displayed_Tech_Level_Adjustment`. Research uses ordinary
`Required_Special_Structures`, `Build_Cost_Credits`, `Build_Time_Seconds`,
`Build_Limit_Current_Per_Player`, `Build_Initially_Locked` and
`Build_Can_Be_Unlocked_By_Slicer`. A locked production offer and a black-market
acquired item are different ledgers.

At the audited tip, `docs/tag-coverage/statuses.json` still marks tech-building
`Build_Advances_Tech_Level` **partial**, researched production inputs **partial**
outside their live closure, and faction `Tech_Level_Icon_Name` **todo**
(legacy EAWR-654). The remaining slicer/technology-price and market entitlement
consumers need their own applied/partial locations when implemented. Do not
promote a parsed tag or fixture-only consumer to full application. This docs
PR changes no registry status.

### Ready to implement for campaign sign-off

- **Shared tech owner:** WGEP-42/130/147/148 and WGA-390..399. Supply the full
  registered-type view, current/max fields, explicit locks/unlocks and separate
  setter/increment operations. Extend `production_tests.cpp` and
  `hero_actions_tests.cpp` with a zero-cost registered candidate, explicit lock,
  same-level assignment and max-level theft; assert ordered semantic results
  and later production/AI readers, equal on 1/2/4/8 workers.
- **Playable Empire research:** WGEP-18..43/127/137/146, WGUI-91 and
  GHUD-S03/S05. Reuse authoritative production admission and quantity providers,
  attach tech-price modifiers and setter/story delivery, and supply eventual
  offers. Extend `live_campaign_setup_tests.cpp` with renamed prerequisites,
  altered target/cost/time and facility-loss cancellation; qualify the ordinary
  live purchase/queue pose with WGEP-RT-U01.
- **Playable Rebel theft:** WGCH-33..36/54/64/100..102 and WGEP-148/150.
  Connect live actor/planet admission, copied whole-list request, synchronized
  draws, account debit and shared tech/lifecycle commits. Extend
  `hero_actions_tests.cpp` / `hero_action_views_tests.cpp` for one versus two
  prepared choices, one affordable among two, at-maximum success, all failures
  and stale current ownership; the controlled widget must submit to that host.
- **Market and AI downstream:** WGEP-81/99/138..145/149/151/152, WGA-290/382.
  Bind live buyers and acquired-item ability admission independently of global
  tech; preserve registered next-tech order and perception targets in AI. Extend
  `ai_economy_tokens_tests.cpp` with two competing next types and a queued first
  match; add an authored market item with high item tech and low buyer tech.
  Reuse the existing child/timeline provider programme for all-player reads.

### Runtime-only qualifications

| ID | Still unverified | Capture recipe |
| --- | --- | --- |
| WGEP-BG-U01 | Whether the retail galactic build menus, as drawn, list none of the six types refused by WGEP-161 for a human Rebel, Empire and Underworld player, and whether any scripted or story-driven production route reaches a type without passing the WGEP-158..160 predicates. The type predicates and their position in the production gate are sourced statically; no runtime menu capture exists. | Through the existing Lua debugger harness, open the space and ground production lists on a base-level-1 planet for each playable faction and record the visible type names and the per-type production-allowed result, then issue a scripted production of one refused hero type and record the outcome and queue. Keep opaque evidence labels only. |
| WGEP-RT-U01 | Ordinary Empire research purchase/progress/cancel and resulting current/eventual choices, plus Rebel whole-list theft reachability after affordability or ownership changes. Static call-path facts do not settle the final HUD pose or stale-action reachability. | Extend GHUD-C02/C09 and WGCH U-08 using Empire current/max and a local research facility, then remove the facility mid-queue. Record producer/domain/build identity, exact credits, stored target, current tech and visible choices before/after completion or cancellation. Repeat Rebel one/two prepared choices, two with only one affordable, at-maximum success and changed actor/planet ownership. Retain price/chance/advance payload and unlock/absence results through the existing Lua/debug capture interfaces; distinguish ordinary clicks from injected commands. |
| WGEP-RT-U02 | End-to-end first-resumed tech action across an Empire front or queued Rebel request, including independent actor absence, pending feedback and all writer consumers. Local archive fields do not establish uninterrupted parity. | Save before Empire completion, after Rebel submit but before execution, and after success with actor absent. Restore fresh state and compare three admitted steps with uninterrupted controls: current/max, locks/unlocks, producer queues, prepared request, exact ledger, hero return, synchronized draw position, story feedback, build offers and AI reads. Reuse WGEP-BM-U02 for market continuation; no compatibility requirement for old remake saves. |
| WGEP-RT-U03 | First actual tech/AI/timeline read after an authored story write, child entry or load. Registered first-match lookup is settled by WGEP-149; the runtime first-reader boundary is not. | Extend GCT-U01/GAI-U05 with two research candidates in reversed registration order, a higher-tech noncombatant, an explicit tech reward and saved parent/child scalars deliberately different from current tech. Log stable player/type IDs, current/max, selected first candidate, active mode and queried timeline. Preserve measured outputs and feed native order/cadence questions to the existing frame-service programme; no ad-hoc schedule is introduced here. |

### Authored planet technology-price sources

| ID | Rule and conditions | Source |
|---|---|---|
| WGEP-166 | The technology-price ability starts enabled, with `Galactic_Automatic` activation and reduction **0**. Validation forces another activation style to `Galactic_Automatic`; an authored percentage at or above **1** is replaced by **1**, while a negative value remains a price penalty. Enabled automatic abilities activate at object initialization. Effective inherited ability blocks replace these omission defaults; malformed present numbers refuse in the remake. | debug build TPA-E01/02/03; effective XML TPA-X01 |
| WGEP-167 | Ordinary application accepts only a null/self target and needs planetary data, an intact planet and a current-allegiance player. Intimidation refuses ordinary application. Registration writes the **negative authored percentage** into that player's technology-purchase ledger with no expiry. Removal deletes that source's entries from the recipient's ledger. A traveling nonplanet holder has no planetary data and cannot supply this bonus. | debug build TPA-E04/05/06/14 |
| WGEP-168 | Original modifier identity includes the planet object and authored ability name. Writing the same source replaces its retained value, rather than compounding it; distinct sources sum through WGEP-110. The remake's canonical ledger row is keyed by planet object only: authored names deduplicate ability blocks at load time, and the distinct enabled percentages form that planet's aggregate row. This preserves the summed effect and planet-scoped removal for the qualified lifecycle. No production stacking category or galaxy-wide flag participates. | debug build TPA-E07/08; WGEP-110 |
| WGEP-169 | Ownership delivery removes the old player's source, then directly registers it for the new player and tracks the self target. This direct registration still refuses destroyed planetary data but bypasses the ordinary intimidation gate. Entering intimidation removes the source; leaving intimidation runs ordinary application again. Other corruption transitions do not rewrite it. These are callback commits; no periodic technology ability timer is introduced. | debug build TPA-E05/09/10 |
| WGEP-170 | A galactic ruins request terminates all abilities after corruption cleanup, including when the request leaves the destroyed flag unset. Termination removes each retained target's effect. Full destruction then prevents subsequent registration. A no-flag ruins source remains absent until a later qualified activation/ownership/corruption callback; idle income services do not recreate it. | debug build TPA-E11/12/13 |

The live economic loader includes these profiles; initialization and the existing
ownership, corruption and ruins commit paths update the canonical technology-price
ledger. The callback operates on one changed planet and stages affected player
ledgers before their ordered commit. It introduces no serial per-entity tick pass.
The existing research offer/queue consumer (WGEP-29/30) and ordinary black-market
preparation (WGEP-103/110) read the same scalar. Ordinary ship prices do not.
Prepared market menus retain their copied price, and tutorial market preparation
keeps its existing bypass. Checkpoint/replay carries the authored economic profile
and canonical ledger; the income payload now begins with version **2**.

`Name`, `Initially_Enabled`, `Activation_Style` and
`Price_Reduction_Percentage` are the consumed technology-price authoring. Nearby
production ability filters, `Stacking_Category` and `Galaxy_Wide` belong to the
separate production reduction kinds; this ability's constructor/parser and
registration do not use them. Ability name/description/icon presentation remains
the separate planet-metadata work below.

`tests/galactic/income_tests.cpp` covers arbitrary identities/percentages, published
research offers and paid queue prices, unaffected ordinary units, source addition
and replacement, capture, disabled/destroyed/nonplanet admission, intimidation,
both ruins modes, checkpoint continuation and 1/2/4/8 equality. Installed coverage
loads The_Maw and `DS_Primary_Hyperdrive`, prepares the latter's actual authored
price/time, then publishes and purchases that offer in an isolated live session.
This isolates the bonus from faction/difficulty/factory multipliers; it is not a
new full-campaign or viewer screenshot claim. Loader tests cover inheritance,
replacement/empty blocks, malformed numbers and repeated authored source names.

## Stock planet ability audit (legacy EAWR-3308)

Audit at the 2026-10-13 implementation boundary. Effective `planets.xml` has
**46 ability records on 43 distinct planet type names**, including story/prologue
variants: 21 production-price, 19 upgrade, four production-time, one technology-price
and one spy record. Kamino and Kamino_Prologue each have two kinds; Saleucami has
two upgrade records. These are audit fixtures, never a runtime planet whitelist.

Existing debug-build rules qualify production (WGEP-29..34/73/74/92/93), the
technology-price ledger (WGEP-110), space upgrade admission (WGT-15/113/117) and
spy effects (WGUI-19..25/98/101/102). XML authoring alone does not qualify an
otherwise unsourced ability lifecycle. Parsed/manual modifier primitives are
not counted as implemented planet bonuses.

| Kind | Records | Status | Code path | Test / remaining work |
|---|---:|---|---|---|
| `Reduce_Production_Price_Ability` | 21 | implemented | `src/units/galactic_types.cpp`, `src/campaign/setup.cpp::prepare_economic_type`, `src/campaign/live_setup.cpp` income service, `src/sim/galactic/income.cpp`, `src/sim/galactic/production.cpp` | P: explicit/category targets, exclusions, current-owner/global versus local footprint, card and queue price |
| `Reduce_Production_Time_Ability` | 4 | implemented | Same economic path, retaining price/time reductions independently | P: category/explicit targets, exclusions, local negative reduction (time penalty), global reduction and card/queue time |
| `Galaxy_Wide_Upgrade_Ability` | 19 | partial | `src/campaign/space_entry_stock.cpp::prepare_stock_space_entry` and `StockSpaceEntry::stage`, `src/campaign/space_entry.cpp` | S: space admission exists; 10 land-only, seven space-only and two dual-layer records. Land reader/application missing ((legacy EAWR-3354)); per-stock space effect qualification remains (legacy EAWR-2006) |
| `Reduce_Technology_Price_Ability` | 1 | implemented | `src/units/galactic_types.cpp`, `src/campaign/setup.cpp`, `src/campaign/live_setup.cpp`, `src/sim/galactic/economy_corruption.cpp::refresh_planet_technology_price` and existing research/market consumers | T: WGEP-166..170 qualified lifecycle, authored installed offer and real debit |
| `System_Spy_Ability` | 1 | missing | `src/sim/galactic/observer.cpp::validated_spy`, `observer_service.cpp`, `hero_actions.cpp` are primitives; no planet XML-to-live source producer | `tests/galactic/observer_tests.cpp` covers commands; installed source/influence/owner integration (legacy EAWR-3353) (planet slice of (legacy EAWR-1153)) |

Code/test keys used in every planet row:

- **P:** economic loader, `prepare_economic_type`, live income service and production
  paths above. `tests/galactic/income_tests.cpp::installed_data` loads the installed
  catalogs, exercises all 25 stock reduction records, resolves every explicit target,
  checks all category-selected targets/exclusions and calls `planet_reduction_card`
  for every prepared target, preserving its resolved fixture binding in the offer,
  card and queue command. Kuat's explicit `Star_Destroyer` price target and stock
  category price targets must reach both the published card and accepted queue.
  The helper uses an isolated 100-credit/10-second base offer; Kuat's authored
  20% reduction yields an 80-credit card and queue entry. `planet_reduction_cards`
  repeats all four arithmetic/footprint shapes on 1/2/4/8 workers with state hash
  equality and arbitrary identities. The fixture isolates the reduction from
  difficulty/factory/corruption factors, covered by other production contracts.
  This proves the session view model, not a new viewer screenshot or full installed
  campaign bootstrap. `lifecycle_and_queues` covers capture/intimidation changes.
- **S:** `space_entry_stock.cpp` reads space declarations and current planet ownership;
  `tests/galactic/space_entry_tests.cpp::scene_contracts` covers faction/chain
  admission and origin creation without a holder. `installed` exercises the stock
  provider, but does not prove each listed planet's actual tactical modifier effect.
  Space rows therefore remain partial audit closure.
- **L:** no live land entitlement/application test; (legacy EAWR-3354) requires one.
- **T:** authored planet-source tests in `tests/galactic/income_tests.cpp::technology_price_planets`, `technology_price_callbacks` and `installed_data`; WGEP-166..170 above.
- **I:** observer primitive tests exist; no automatic planet-source test ((legacy EAWR-3353)).

| Planet type | Kind / authored target | Status | Code path | Test / follow-up |
|---|---|---|---|---|
| `Abregado_Rae` | price: Imperial_Heavy_Scout_Squad; reduction 0.20; global | implemented | P economic path | P |
| `Anaxes` | price: Victory_Destroyer; reduction 0.20; global | implemented | P economic path | P |
| `Bespin` | upgrade: space: `Bespin_Krayt_Damage_Upgrade` | partial | S space path | S; individual effect (legacy EAWR-2006) |
| `Bespin_Story` | upgrade: space: `Bespin_Krayt_Damage_Upgrade` | partial | S space path | S; individual effect (legacy EAWR-2006) |
| `Bestine` | price: Acclamator_Assault_Ship; reduction 0.20; global | implemented | P economic path | P |
| `Bothawui` | spy: automatic, duration -1, planet `Galactic_Influence_Range` 150, authored `See_*` flags | missing | observer primitives only | I / (legacy EAWR-3353) |
| `Byss` | upgrade: land: `Byss_ATAT_Enhanced_Damage_Bonus` | missing | no land consumer | L / (legacy EAWR-3354) |
| `Carida` | price: Rebel_Infantry_Squad, Rebel_Tank_Buster_Squad, Rebel_Infiltrator_Team, Imperial_Stormtrooper_Squad, Imperial_Light_Scout_Squad; reduction 0.20; global | implemented | P economic path | P |
| `Corellia` | price: Corellian_Corvette; reduction 0.20; global | implemented | P economic path | P |
| `Corulag` | price: Rebel_Infantry_Squad, Rebel_Tank_Buster_Squad, Rebel_Infiltrator_Team, Imperial_Stormtrooper_Squad, Imperial_Light_Scout_Squad; reduction 0.20; global | implemented | P economic path | P |
| `Endor` | upgrade: land: `EL_Light_Reflective_Armor_L1_Upgrade` | missing | no land consumer | L / (legacy EAWR-3354) |
| `Eriadu` | price: Rebel_Heavy_Tank_Brigade; reduction 0.20; global | implemented | P economic path | P |
| `Felucia` | upgrade: land: `Felucia_Plantary_Advantage` | missing | no land consumer | L / (legacy EAWR-3354) |
| `Fondor` | price: Alliance_Assault_Frigate; reduction 0.20; global | implemented | P economic path | P |
| `Fresia` | price: A_Wing_Squadron; reduction 0.20; global | implemented | P economic path | P |
| `Geonosis` | price: Rebel_Artillery_Brigade; reduction 0.20; global | implemented | P economic path | P |
| `Honoghr` | upgrade: land: `Honoghr_Planetary_Advantage` | missing | no land consumer | L / (legacy EAWR-3354) |
| `Honoghr_Story` | upgrade: land: `Honoghr_Planetary_Advantage` | missing | no land consumer | L / (legacy EAWR-3354) |
| `Hypori` | time: Vehicle category; exclude DarkTrooper_P1_Company, DarkTrooper_P2_Company, DarkTrooper_P3_Company, Destroyer_Droid_Company; reduction 0.25; global | implemented | P economic path | P |
| `Ilum` | price: Imperial_Artillery_Corp; reduction 0.20; global | implemented | P economic path | P |
| `Kamino` | price: Infantry category; exclude Night_Sister_Company, Vornskr_Wolf_Pack; reduction 0.99999; local | implemented | P economic path | P |
| `Kamino` | time: Infantry category; exclude Night_Sister_Company, Vornskr_Wolf_Pack; reduction -0.15; local | implemented | P economic path | P |
| `Kamino_Prologue` | price: Infantry category; exclude Night_Sister_Company, Vornskr_Wolf_Pack; reduction 0.99999; local | implemented | P economic path | P |
| `Kamino_Prologue` | time: Infantry category; exclude Night_Sister_Company, Vornskr_Wolf_Pack; reduction -0.15; local | implemented | P economic path | P |
| `Korriban` | price: Imperial_Armor_Group; reduction 0.20; global | implemented | P economic path | P |
| `Kuat` | price: Star_Destroyer; reduction 0.20; global | implemented | P economic path | P |
| `Mandalore` | price: StarViper_Squadron, Crusader_Gunship, Kedalbe_Battleship, Canderous_Assault_Tank_Company; reduction 0.20; global | implemented | P economic path | P |
| `Mandalore_Prologue` | price: StarViper_Squadron, Crusader_Gunship, Kedalbe_Battleship, Canderous_Assault_Tank_Company; reduction 0.20; global | implemented | P economic path | P |
| `Mandalore_Story` | price: StarViper_Squadron, Crusader_Gunship, Kedalbe_Battleship, Canderous_Assault_Tank_Company; reduction 0.20; global | implemented | P economic path | P |
| `MonCalimari` | price: Calamari_Cruiser; reduction 0.20; global | implemented | P economic path | P |
| `Mustafar` | time: Calamari_Cruiser, Star_Destroyer, Kedalbe_Battleship, Krayt_Class_Destroyer; reduction 0.25; global | implemented | P economic path | P |
| `Naboo` | upgrade: land: `RL_Improved_T4B_Damage_L1_Upgrade` | missing | no land consumer | L / (legacy EAWR-3354) |
| `NalHutta` | upgrade: space: `NalHutta_Planetary_Advantage`; land: `NalHutta_Planetary_Advantage` | partial | S space path | S; L / (legacy EAWR-3354) |
| `NalHutta_Story` | upgrade: space: `NalHutta_Planetary_Advantage`; land: `NalHutta_Planetary_Advantage` | partial | S space path | S; L / (legacy EAWR-3354) |
| `Ryloth` | upgrade: land: `EL_Stamina_Boost_Upgrade` | missing | no land consumer | L / (legacy EAWR-3354) |
| `Ryloth_Prologue` | upgrade: land: `EL_Stamina_Boost_Upgrade` | missing | no land consumer | L / (legacy EAWR-3354) |
| `Saleucami` | upgrade: space: `R_Saleucami_Plantary_Advantage` | partial | S space path | S; individual effect (legacy EAWR-2006) |
| `Saleucami` | upgrade: space: `U_Saleucami_Plantary_Advantage` | partial | S space path | S; individual effect (legacy EAWR-2006) |
| `Sullust` | price: Tartan_Patrol_Cruiser; reduction 0.20; global | implemented | P economic path | P |
| `Taris` | price: Rebel_Light_Tank_Brigade; reduction 0.20; global | implemented | P economic path | P |
| `Tatooine` | upgrade: land: `RL_Light_Armor_L1_Upgrade` | missing | no land consumer | L / (legacy EAWR-3354) |
| `The_Maw` | technology price: reduction 0.15 | implemented | authored source → current-owner technology ledger | T |
| `Utapau` | upgrade: space: `Utapau_Kedalbe_Shield_Upgrade` | partial | S space path | S; individual effect (legacy EAWR-2006) |
| `VergessoAsteroids` | upgrade: space: `RS_Improved_Weapons_L1_Upgrade` | partial | S space path | S; individual effect (legacy EAWR-2006) |
| `Wayland` | upgrade: land: `RL_Improved_Treads_L1_Upgrade` | missing | no land consumer | L / (legacy EAWR-3354) |
| `Yavin` | upgrade: space: `RS_Enhanced_Shielding_L1_Upgrade` | partial | S space path | S; individual effect (legacy EAWR-2006) |

### Presentation and audit boundaries

The validated planet model and shared planet view publish inherited
`Planet_Ability_Name`, `Planet_Ability_Description` and `Planet_Ability_Icon`.
The selected advanced-map bonus icon and its timed encyclopedia consume the
shared provider (GHUD-S95/S96, (legacy EAWR-3355)); the galaxy ability filter remains
(legacy EAWR-2987) and can consume the same metadata. The sourced popup title is
`Describe_Advantage`, and its body is the first `Encyclopedia_Text` entry.
`Planet_Ability_Name` and `Planet_Ability_Description` have distinct storage;
they are parsed and preserved, with no retail presentation reader found in
the bounded debug-build getter, reference and direct-offset search. Stock data
duplicates the popup keys, but mods may distinguish them. Planet-info advantage
text also uses `Describe_Advantage`; its full dialog remains a separate seam.
This presentation is independent of whether the gameplay bonus works.

Follow-ups (legacy EAWR-3353), (legacy EAWR-3354) and (legacy EAWR-3355)
keep planet identities, targets, categories, percentages,
upgrade references, localization and assets data-driven. Technology and land
ability-specific lifecycle rules outside WGEP-166..170 still need bounded debug-build qualification.
No new retail behavior is inferred, and no tag status is promoted merely because
existing paths were inspected.
