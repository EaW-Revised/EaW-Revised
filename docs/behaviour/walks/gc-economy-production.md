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
the symbol/address map and raw reads remain in ignored research output. No new
retail capture is claimed. **Unverified** entries are questions, never inferred
original-game rules. XML examples below describe effective values rather than
comments in the data, several of which disagree with those values.

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
| WGEP-03 | Fiscal service visits players in player-list index order. A null, neutral, nonplayable, or non-planet-controlling faction earns zero from the planet aggregation. A nonzero aggregate is added once; an initialized galactic AI budget is then notified with the aggregate before the account's difficulty/cap adjustment. A zero aggregate skips both addition and budget notification. | debug build GEP-E03/E04 | economy_rules, AI engine; missing |
| WGEP-04 | For each planet owned by the player, aggregate its actual income. For other planets, corruption income is possible only for a faction with `Benefits_From_Corruption`. Independently add income assigned to this player's siphon and positive piracy income for a corruption-benefiting faction. Actual fiscal collection resets consumed piracy income; a preview does not. | debug build GEP-E04 | economy_rules, new corruption state; missing |
| WGEP-05 | Basic income reads the planet's current base credit value; a destroyed planet instead reads `Planet_Destroyed_Credit_Value`. Without planetary state, use `Planet_Credit_Value`. An income-blocked planet's actual income is zero. Otherwise remove expired modifiers, refresh base/alignment/political/trade contributions, sum the income modifiers, and subtract any applicable siphon. | debug build GEP-E05/E06; data | economy_rules, new planet state; missing |
| WGEP-06 | The base contribution is basic income multiplied by the stored black-market income multiplier. Alignment, political, trade and structure bonuses are separate additive contributions based on **unmultiplied** basic income. Do not multiply the complete income sum by the black-market factor. | debug build GEP-E06/E07 | economy_rules; missing |
| WGEP-07 | Planet/faction alignment adds `basic × alignment_bonus × MaxCreditIncomeAlignmentBonus` when that product is positive; otherwise a positive penalty subtracts `basic × alignment_penalty × MaxCreditIncomeAlignmentPenalty`. Zero removes the old alignment contribution. Both maximum constants are **0.50**; authored bonus and penalty are validated in the range 0..1 and are not both positive. | debug build GEP-E07; data | economy_rules; missing |
| WGEP-08 | Nonneutral planetary political control is evaluated through `Political_Income_Curve` (**0,0; 1,1; 5,1**). A positive curve value contributes `basic × (curve_value − 1)`; a zero contribution removes its old source. The inspected nonpositive-curve and neutral branches return zero without replacing that source. U-03 covers interpolation and transition cleanup. | debug build GEP-E08; data | economy_rules, movement/control ownership interface; missing |
| WGEP-09 | For each valid trade link to an allied endpoint, add its positive `Credit_Gain_Factor` to the route factor sum; the planet's trade contribution is `basic × factor_sum`. A zero contribution removes its old source. Actual income does not use the AI's hypothetical all-links preview. | debug build GEP-E09; data | economy_rules, galaxy/trade-route model; missing |
| WGEP-10 | A planetary income bonus adds `basic × Percentage_Income_Modifier + Absolute_Income_Modifier`, multiplied by `Additional_Multiplier_On_Mining_Colonies` on `Planet_Is_Mining_Colony`. The target must be the source planet itself or the source's current planet. A nonplanet source loses its effect when its owner loses that planet, and gains it when its owner gains that planet; ending the effect removes its unique source. Stock mining facilities author **2**, **0**, and **1.5**: +200% basic income before the colony multiplier, despite stale XML comments. | debug build GEP-E10; data | economy_rules, Lua bindings/ability lifecycle; missing |
| WGEP-11 | A gambling income service finds its current planet, draws a win/loss sample, then draws a signed factor from **[0, Max_Income_Modifier]** on a win or **[Min_Income_Modifier, 0]** on a loss. It replaces this source's contribution with `basic × factor`. Stock arena data authors **Chance_To_Win 0.80**, **Min −0.20**, **Max 0.60**, and **Evaluation_Interval_In_Secs 30.0**; service scheduling and random source are U-04. | debug build GEP-E11; data | economy_rules, ability service, save/replay RNG; missing |
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
| WGEP-18 | A nonnull type and producer with production support are required. Reject cinematic-only or unaffiliated types, a location-adjusted whole price below **1**, negative location-adjusted time, direct ground-base types, and types classified as neither ground nor space production. A purchase checking affordability compares the price with whole available credits unless pay-as-you-go applies. | debug build GEP-E17 | production, validated loaders; missing |
| WGEP-19 | A type with `Build_Advances_Tech_Level` additionally requires current player tech **equal** to its `Tech_Level`, and its target level no greater than the player's maximum. Then reject the explicit locked list; an initially locked type requires membership in the explicit unlocked list. Explicit locking takes precedence. | debug build GEP-E17 | production, Lua bindings, save/replay locks; missing |
| WGEP-20 | The planet's current controlling player must be the builder, and target control must equal current control. Reject direct company/squadron members and directly built transport wrappers; reject a type restricted by this planet. For space work, a present starbase must have the same owner as ground control; an absent owner does not itself fail this check. | debug build GEP-E17 | production, ownership model; missing |
| WGEP-21 | `Required_Special_Structures` must pass local presence checks. The parsed list supports OR-linked alternatives within groups, with separate groups required together. Next, every `Required_Orbiting_Units` entry must be found by exact type among the attached fleets' contents, excluding pending-deletion units. This reader does not perform a second owner or ally test on those contents. | debug build GEP-E17/E18/E19; data | production, galaxy fleet index; missing |
| WGEP-22 | Require player tech at least `Tech_Level`, ground base at least `Required_Ground_Base_Level`, and starbase at least `Required_Star_Base_Level`, in that order. Base levels are validated **0..5**. These are independent gates; a high station level does not raise global tech. | debug build GEP-E17 | production, economy_rules; missing |
| WGEP-23 | Ground and orbital structures reserve separate planet slots: completed count plus queued count must be below the corresponding live land/space capacity. Planetary state supplies the capacity when present; otherwise use type `Special_Structures_Land` or `Special_Structures_Space`. Then require this planet in `Required_Planets` when that list is nonempty. Orbital structures additionally require a starbase level of at least **1**. | debug build GEP-E17/E20; data | production, new planet slot ledger; missing |
| WGEP-24 | A starbase purchase must be exactly the next buildable `Base_Level`, fit the planet's `Max_Space_Base`, and have no qualifying nonowner fleet in orbit. The blocking fleet is not pending deletion, does not skip space combat, and declares that its presence cancels starbase production. The check uses **different owner**, not the ally relationship. | debug build GEP-E17/E21; data | production, movement/control fleet interface; missing |
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
| WGEP-38 | For a front starbase, when no space conflict is pending, any orbiting fleet satisfying WGEP-24's blocker conditions cancels it, emits cancellation, starts its successor and returns from the service. The local builder also gets interrupted-production advisor text. A pending conflict skips this fleet-cancellation check. Direct refund is absent; U-06 covers listeners. | debug build GEP-E30 | production, movement/control conflict interface; missing |
| WGEP-39 | Only the front completes or pays. An expired ground front is held completed in the queue if landed transports are at least `Max_Ground_Forces_On_Planet` (**10**) and a nonnull occupying orbit player differs from the occupying ground player. This condition is applied to the ground front without first distinguishing units from structures. Removing the hold permits normal completion. | debug build GEP-E30; data | production, movement/control occupancy; missing |
| WGEP-40 | On completion, remove the front, emit the completion UI/audio work, then either place its result or set tech to `Build_Advances_Tech_Level`. Increment its historical build count and emit production-finished. Human-created orbiting results trigger fleet unification. Record paid build cost on the result, or the first result inside a created fleet, and start a successor if present. A failed ordinary placement calls the refund helper; structure/base results are allowed to return no ordinary object. | debug build GEP-E30 | production, Lua bindings, fleet model, save/replay; missing |
| WGEP-41 | Creation dispatches starbase data, ground structure, orbital structure, ground company, squadron and single-ship fleet to their distinct placement routes. A starbase completion records its authored level/type/owner; creation of level **1** clears orbital special structures. Base completion and successfully created ordinary results notify story construction. An additional result-type reference recursively creates an additional result; its authoring route is U-13. Fleet landing, squadron assembly and tactical persistence remain interface boundaries. | debug build GEP-E31 | production, galaxy model, Lua bindings; missing |
| WGEP-42 | Tech completion sets the **authored target level**, rather than incrementing by one, clamps it to **0..player_max**, and notifies story tech-level events. With unlock processing enabled, affiliated initially locked slicer-unlockable types whose `Tech_Level` is **strictly lower** than the new player level enter the unlocked list. New-options audio is local and requires an actual new unlock. | debug build GEP-E32 | economy_rules, production, Lua bindings; missing |
| WGEP-43 | For unfinished pay-as-you-go work, due bill is `stored_price × (last_serviced_remaining − current_remaining) / full_duration`, with no rounding here. A human pays only if credits cover it; AI delegates payment to its planetary AI queue. Failure resets the timer to last serviced remaining seconds. Successful work updates countdown notifications and the stored remaining value. AI budget/prequeue policy and final-frame accounting are U-07. | debug build GEP-E30/E48 | production, AI engine; missing |
| WGEP-44 | An entry records its duration and paid price when bought. Its construction timer is initialized then; when promoted to front, reinitialize it from the stored full duration, so time spent waiting does not build it. Entry service only updates last remaining seconds. No current-price or current-duration recalculation occurs in this service; modifier-driven queue updates remain U-08. | debug build GEP-E33/E34 | production, save/replay; missing |
| WGEP-45 | Indexed cancel requires the indexed entry's type to match the command type. If requested, refund before emitting cancellation and removing the entry. Canceling the front restarts the next front; canceling a later entry does not reset the running front. Wrong type or absent index fails without mutation. Command ownership/refund-flag selection is U-06. | debug build GEP-E35 | production, command/Lua bindings; missing |
| WGEP-46 | Refund amount is the stored full price for prepaid work, or stored price multiplied by `clamp(1 − last_serviced_remaining / full_duration, 0, 1)` for pay-as-you-go. A missing builder gives zero. Refund passes through WGEP-16 and is an AI windfall; it uses the stored service value and is not recalculated from today's modifiers. | debug build GEP-E36/E48 | economy_rules, production, AI engine; missing |
| WGEP-47 | Completion first signals the command bar and queues authored completion music. Build-complete speech takes precedence over local `SFXEvent_Build_Complete`; local strategic fallback also activates the planet completion ping. Cancel speech similarly precedes local `SFXEvent_Build_Cancelled`. Countdown speech checks minute transitions above **60 s**, ten-second transitions from **10..60 s**, then second transitions below **10 s**. Exact displayed suffix formatting and speech ownership are U-09. | debug build GEP-E30/E35/E37 | production events, presentation/audio; missing |

### Population, starbases, modifiers and black-market interface

| ID | Rule and conditions | Source | Planned integration; GC status |
|---|---|---|---|
| WGEP-48 | GC maximum population is faction base capacity plus `Additional_Population_Capacity` from eligible live, non-deleting owned objects. For owned planets, also add the current starbase type's capacity. Faction base capacity defaults to **0**; `Base_Population_Capacity` is parsed but absent from stock XML. An AI using the cap skips sources under a planet unusable by that AI. Humans always use the cap; AI can bypass it through `AI_Uses_Galactic_Pop_Cap`, whose default is U-10. | debug build GEP-E38/E47/E49; data | economy_rules, galaxy ownership census; missing |
| WGEP-49 | Global usage sums owned objects' **original type** GC `Population_Value`, skipping a child when its immediate container's original type already has a positive value. An AI using the cap skips objects under unusable planets. The include-production variant adds the queues' population contribution through producer objects. Round the total using integer conversion of **total + 0.5**. | debug build GEP-E39 | economy_rules, fleet/company model; missing |
| WGEP-50 | Population admission allows a type when its GC population value is at most **max(capacity − usage_including_production, 0)**. Each producer sums every entry in both actual queues and converts **sum + 0.5** to an integer. AI with the cap disabled passes. Thus a zero-population structure can still be bought when the player is over cap. Unusual container and dead-object accounting remain U-10. | debug build GEP-E40 | economy_rules, production; missing |
| WGEP-51 | Next starbase level is unavailable while any starbase is queued, at an invalid planet owner, at level **5**, or when the next level exceeds the planet's live station cap. Planetary state supplies that cap when present, limited to **5**; otherwise use `Max_Space_Base`. With no starbase owner the next level is **1** only if the planet permits a station; otherwise it is current level plus **1**. This progression does not change global tech. | debug build GEP-E21/E20 | production, new planet starbase state; missing |
| WGEP-52 | The inspected next-type helper chooses an existing station's `Next_Level_Base`; without one it recognizes the Rebel and Empire level-1 XML object types from target control faction. It returns the candidate only when its `Base_Level` does not exceed the available next level. It does not contain a Consortium fallback; Consortium construction discovery is U-11, not inferred from the Rebel/Empire branch. | debug build GEP-E41 | production, content discovery, AI engine; missing |
| WGEP-53 | A starbase level change to zero from a positive level emits the story destroy-base event. Emit the level-change notification, set the new type, then revalidate space production. Zero clears station ownership. These are strategic state changes, separate from WPR-52's tactical hardpoint and selection transfer. | debug build GEP-E42 | production, movement/control, Lua bindings; missing |
| WGEP-54 | A production reduction ability with `Galaxy_Wide` on a planet applies to that planet's current controlling player; other cases use a planet-local route. Destroyed planet sources fail application; global application also rejects intimidation corruption. Register each affected type's authored reduction/category under the unique source, with no expiry. A galaxy-wide time effect on ownership change removes the old player's source and applies to the new player; a local source planet keeps its local effect. Nonplanet time sources remove/apply on loss/gain by their owner. Removal uses the corresponding global/local route. Type filtering, equivalent price rebinding and live queue adjustment remain U-08. | debug build GEP-E43/E27; data | economy_rules, Lua/ability lifecycle; missing |
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
defaults to zero; the AI flag default remains U-10. Tactical
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

## Unverified and capture requests

These are scoped evidence gaps, not additional verified rules. They remain open
for M3 planning; the walk does not implement a guessed branch.

| ID | Unsettled detail | Targeted read or capture that settles it |
|---|---|---|
| U-01 | Initial fiscal start/end, pause/speed and tactical-time advancement. | Read initialization and the shared mode timer; stage GC just before the first and subsequent 45-second boundaries, pause and enter/leave a tactical conflict, record logical frames and account deltas. |
| U-02 | Global command/planet/player/fiscal order and credit-cap refresh after ownership change. | Shared frame loop read plus same-frame buy/ownership/completion/fiscal staging; record old/new owner, cap, credits and all queue entries. |
| U-03 | Political curve interpolation, neutral/nonpositive transition modifier cleanup, alignment getter/parser mapping. | Read the curve evaluator and ownership transition listeners; stage political values 0, fractional, 1 and 5 with a prior contribution and inspect the source ledger. |
| U-04 | Gambling first-service phase and RNG source; activation/removal callbacks surrounding verified income target checks. | Read scheduler/ability lifecycle and RNG producer; compare completed facilities on ordinary/mining planets and source removal; record seeded arena samples and source keys. The interval getter rounds `logical_frames_per_second × interval + 0.5` to an integer for a positive interval, otherwise uses zero (debug build GEP-E11). |
| U-05 | Construction countdown initialization and quantization of fractional seconds; price and fiscal-frame rounding are verified. | Read countdown initialization and remaining-seconds conversion; stage durations straddling one logical frame and compare stored/service remaining time. |
| U-06 | Human/UI cancel refund flag, command ownership validation, and refunds from automatic-cancellation listeners. | Read cancel event execution and signal listeners, especially AI budgets; capture head/tail cancel and facility/starbase/orbiting-unit loss for human and AI, logging account and budget separately. |
| U-07 | AI prequeue reservation/payment policy and completed-front final-frame accounting. | Read AI planetary queue payment and budget callbacks; stage repeated shortage/resume and cancel at 0%, partial and completion for every difficulty. Due-bill, stored completed fraction and suspension reset are verified in WGEP-43/46. |
| U-08 | Reduction target filters, global/local source removal on ownership change, and adjustment of already queued prices/durations. | Read player/planet effect helpers and modifier change listeners; stage discounted types and exclusions, move/remove a hero, change planet control and count factories while one entry builds and another waits. |
| U-09 | Exact countdown speech suffix and whether explicit speech plays for nonlocal work; start-event speech routing. | Read authored speech-name getters and command admission UI handler, or capture minute/ten-second/second thresholds and two players' completions/cancels with an event trace. |
| U-10 | AI-cap default and unusual container/dead-object accounting. | Read AI default and container lifecycle; stage company/squadron/transport wrappers, queued units on several planets, an unusable AI planet and capacity loss. Faction base default and actual queue sums are verified in WGEP-48/50. |
| U-11 | Consortium station discovery and its higher-level unlock menus: inspected next-type helper has only Rebel/Empire fallback. | Read GC build-button/AI discovery and Consortium types; stage each faction at base levels 0..5 with planet and global-tech caps, including a already queued station. |
| U-12 | Black-market menu acceptance, item tech numbering adjustment, already-owned item execution and buyer disappearance/refunds before execution. | Read menu/item getters and command preparation; stage common/uncommon items, insufficient cash, owned item and buyer loss, with prepared price and item flags. Offer price construction is verified in WGEP-58. |
| U-13 | Account/planet/fiscal save fields, additional result authoring route, and command/Lua return values beyond direct notifications. | Read remaining save/load, type parser and command bindings; request a GC checkpoint immediately before payment and completion, then compare replay state and events. Queue fields are verified in WGEP-57. |

Captures should use the established original-game/Lua harness and stay in ignored
research output. No owner recording is required to accept this docs-only walk;
any unresolved observation remains **unverified** until evidence arrives.
