# Mod campaign rules checked against the debug build

This read-only walk checks orbital classification, empty companies, omitted
planet ratings, unresolved prerequisites and starting forces, signed income,
empty raid masks, and planet Force strength. Evidence is static debug-build
inspection, not a runtime capture of a mod campaign. Assertions described below
can stop interactive debug execution; continuation means the inspected branch after that assertion.
No release-binary assertion policy is inferred.

The private evidence map and raw reads use the rule IDs below. Binary symbols,
addresses and decompiler output remain in ignored research storage. Source
comparisons are pinned to main `3c0e06b10905eb730b607f7f5ecb1c7649d10b2b` and the
bring-up branch `99bd381f99d92c41df9ccbc24c346c2197efc516`; line numbers refer to
those revisions, not an unspecified future tip. This note changes no runtime code.

## Orbital structures and their consumers

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-01 | `DUMMY_ORBITAL_STRUCTURE` is the registered orbital-special-structure behavior token. `DUMMY_SPACE_STRUCTURE` is absent from the inspected behavior registration/string searches. | Debug build: the registered token selects the orbital dummy behavior factory; production checks use that same behavior. |
| RGC-02 | Orbital behavior gives structure-specific admission and creation, not unconditional buildability or an automatic UI tab. | Debug build: admission counts completed plus queued orbital structures against space slots, requires an existing starbase, and applies the exact-type instance limit. Completion dispatch adds the space special structure. Queue service also checks the supporting starbase owner. Ordinary price, affiliation, prerequisites and production-domain gates still apply. |
| RGC-03 | Galactic build-bar filtering reads authored `Build_Tab_*` flags; its space/ground page comes from production-domain classification. | Debug build: list refresh calls current and eventual production eligibility, obtains the authored filter and `GUI_Row`, then separates available and eventual choices. The filter checks land units, space units, special structures, heroes, outpost and space station in that order. It does not independently replace an orbital behavior with a tab flag. |
| RGC-04 | AI treats orbital special structures as targeted construction and excludes live orbital special structures from the galactic freestore. | Debug build: unallocated galactic production candidates require a live target planet, a free space slot after plan reservations, and room under the exact-type limit. Targeted producer adjustment calls production eligibility at that target. Orbital structures are exempt from the magic candidate's hyperspace-capability rejection, while its other gates remain. An already allocated freestore candidate returns its existing preference before the production checks. |

The scoped consumer map is:

| Consumer | What an orbital match receives | Related rules |
|---|---|---|
| Production eligibility and queued behavior counting | Space-special slot accounting, exact-type limits and the starbase prerequisite. | RGC-02; WGEP-23/25 |
| Production queue service and completion | Supporting-starbase checks, space-special placement and successful-structure handling even when the placement result is null. | RGC-02; WGEP-41 |
| Starting-force delegated placement | The same orbital-special creation branch. | RGC-02; CGS-03 |
| Build-bar list refresh | Shared production eligibility; authored filter/page/row selection remains separate. | RGC-03 |
| AI production preference | Targeted-planet, per-type and space-slot checks; special-structure exception in magic hyperspace qualification. | RGC-04; WGA-60 |
| AI producer adjustment and production-time estimate | Targeted structures use the target's production eligibility; structure time is accumulated separately from ordinary distributed producer queues. | RGC-04; WGA-58 |
| Galactic freestore insertion | Rejected as a special structure rather than admitted as a mobile unit. | RGC-04; WGA-61 |

**Ours:** five locations use the wrong spelling at both compared revisions:
`src/campaign/live_setup.cpp:101` on main / `:116` on the bring-up branch
(offer kind and direct admission),
`src/campaign/starting_forces.cpp:188` (placement),
`src/campaign/ai_selection_content.cpp:72` (fresh freestore classification),
the same file at `:176` on main / `:180` on the bring-up branch (selection structure
trait), and `src/campaign/galactic_ai_stock_runtime.cpp:61` (live freestore
classification). These disagree with RGC-01/02/04. The correct spelling already
appears in `src/campaign/ai_perception_content.cpp:129` and
`src/campaign/space_entry_stock.cpp:330`. Fixing only the offer leaves placement
and AI inconsistent. The authored build-tab boundary in
`apps/viewer/src/galactic/scene_content.cpp:107` and
`apps/viewer/src/galactic/planet_hud.cpp:235` agrees with RGC-03; a behavior fix
must retain those flags. Orbital behavior alone does not guarantee an icon.

## Empty companies at production completion

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-05 | A missing company transport asserts and returns no primary object. Empty members alone do not take that early return: with a valid transport, the helper creates the transport, asserts that members should be nonempty, then runs a zero-iteration member loop and continues transport placement. | Debug build: transport resolution precedes transport creation and the member-count check. |
| RGC-06 | Production completion branches on `Build_Advances_Tech_Level`. A positive value sets the player's technology without object creation. Otherwise it delegates creation; a company returning null takes the production refund path. There is no generic upgrade or unlock effect selected by an `Upgrade_*` name. | Debug build: completion chooses the technology or creation branch, then records historical completion and emits the ordinary finished signal. Refund delegates to the credit adder. |
| RGC-07 | `Create_Additional_Object` is checked after ordinary creation dispatch even if the primary result is null. A null primary skips its ordinary story construction notification; an authored additional creation still recurses. | Debug build: additional creation is outside the nonnull-primary guard. Script listeners to the finished signal remain a separate possible source of effects. |

Thus a company with neither transport nor members is **not inherently refused
before queuing** by the inspected company helper. If admitted and completed with
no technology advance, it creates no primary object and takes the refund path;
an additional creation can still occur. With a positive technology advance,
completion uses the no-spawn technology branch instead. These facts do not prove
that any particular abstract mod definition is offered to the player.

**Ours:** the bring-up branch's `src/campaign/setup.cpp:883` treats either missing
transport or empty members as uncreatable, and
`src/campaign/live_setup.cpp:574` removes those types before census/offer setup.
That is broader than RGC-05 and loses the completion distinctions in RGC-06/07.
Main already rejects incomplete nested company bindings in
`src/sim/galactic/production.cpp:552`; the no-spawn technology offer distinction
exists in `src/campaign/live_setup.cpp:106`. The missing-primary refund and
additional-creation cases need deliberate coverage before claiming parity.

A private raw-tag audit also found a transport on the shared company-upgrade
template and members on derived variants. Missing tags on an intermediate
definition are not evidence that the inherited, registered leaf lacks them.
The bring-up count of empty upgrade records therefore needs qualification by
effective inheritance, registration and actual production eligibility.

## Omitted planet ratings

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-08 | Fresh `Planet_Black_Market_Modifier`, `Planet_Tech_Availability_Rating` and `Planet_Slice_Difficulty_Rating` are all **0.0** before XML parsing. An omission with no inherited value retains zero. | Debug build: the type constructor explicitly writes all three zeros, then parses the database entry. XML table entries map the three tags to those fields. |

**Ours:** the bring-up branch's zero fallbacks in
`src/campaign/planet_economy.cpp:114` and `src/campaign/live_setup.cpp:1047` match.
Their unverified labels can be removed in the implementation follow-up. Main
still requires the ratings (`src/campaign/planet_economy.cpp:112`,
`src/campaign/live_setup.cpp:992`), so its missing-tag refusal differs.
The zero defaults do not erase an authored or inherited nonzero rating.

## Unresolved prerequisites and starting forces

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-09 | An unresolved name reference remains null and is diagnosed. Local structure eligibility starts satisfied and skips a null outer entry. A null alternative reached inside a nonnull-led OR chain makes that chain satisfied. | Debug build: name fixup reports the unresolved reference; the local eligibility reader only enters presence testing for a nonnull outer entry and explicitly sets its result true for a null OR alternative. |
| RGC-10 | A null `Required_Orbiting_Units` entry asserts and is then skipped. Nonnull entries still require a matching live, non-delete-pending unit. | Debug build: the failed-match return is inside the nonnull branch, and successful loop completion returns true. |
| RGC-11 | An undefined starting-force unit type is diagnosed during campaign fixup and retained as null. Placement asserts on that null and skips creation; an undefined location cannot match a live planet type. | Debug build: fixup logs, shows an error popup and asserts; placement has a null/nonnull branch around all creation. |

RGC-09 has a positional quirk; it must not be simplified into an ordinary
null-is-false OR expression. With no other failing gate:

| Structure requirements | Local result |
|---|---|
| Undefined name only | Pass: null outer entry is skipped. |
| Defined but absent structure only | Fail. |
| Defined but absent structure OR undefined name | Pass: null alternative satisfies the chain. |
| Undefined name OR defined but absent structure | Fail: the null first entry is skipped without consuming its OR chain; the following nonnull entry is tested. |

These are control-flow deductions from the inspected reader, not runtime captures.
Other consumers, such as tech-tree construction, can assert independently;
passing these two eligibility readers alone does not promise that the whole UI
or campaign handles malformed data without diagnostics.

**Ours:** main refuses unbound prerequisites in
`src/campaign/live_setup.cpp:130` and `:140`. The bring-up branch continues
loading, but sets direct admission false for an unresolved orbit requirement at
`:150` and for an all-null structure group at `:177`; it also discards null OR
alternatives. Neither matches RGC-09/10. The bring-up branch's unknown-force
diagnostic and skip in `src/campaign/setup.cpp:540` match RGC-11's continued
placement; main's unresolved-force refusal at `src/campaign/setup.cpp:522`
differs. A remake diagnostic is
appropriate without reproducing an interactive assertion or popup.

## Signed planet income

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-12 | `Planet_Credit_Value` is a signed integer. The basic reader preserves its sign; ordinary actual-income calculation sums modifiers without flooring a negative result. Owned planets contribute that signed actual result to the player's fiscal total. | Debug build: signed getter and current-base setter, basic-income conversion, modifier summation and owner aggregation. The destroyed-value branch and explicit income blockers remain separate. |
| RGC-13 | A nonzero negative fiscal total reaches the credit adder and reduces the balance. There is no zero floor on the income total in that path. The credit balance itself is floored at zero; the AI difficulty credit multiplier applies only to positive additions. | Debug build: fiscal payment tests nonzero rather than positive, then invokes the shared credit adder. That adder adjusts only positive AI deltas and clamps the resulting balance. |

For example, without other modifiers, planets contributing 20 and -15 yield
5 credits; a total of -15 debits 15 from an adequate balance and leaves a
balance of 10 at zero. These examples state the arithmetic, not observations of
the named mod planets. Alignment, politics, trade and other modifiers can change
the actual total before payment.

**Ours:** `src/data/galactic/galactic_world.cpp:73` reads the signed value and
`src/campaign/setup.cpp:437` forwards it. The actual nonnegative gate is
`src/sim/galactic/setup.cpp:124`, at both compared revisions. That gate differs
from RGC-12/13; removing it requires preserving overflow checks and auditing
downstream fiscal/AI assumptions, including
`src/sim/galactic/ai_funding.cpp:342`'s negative-income refusal.

## Empty raid-category mask

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-14 | Both raid-category masks reset to zero. The constants XML reader skips truly empty values, so a fresh empty tag retains zero. Nonempty separator-only input passed to the category converter also succeeds as zero. No stock category mask is substituted. | Debug build: reset writes zero to both fields; XML parsing bypasses mapping for an empty string. The raid-mask XML row uses the category converter, which clears its destination before tokenization and restores the previous value only on conversion failure. |
| RGC-15 | A zero `Raid_Force_Free_Object_Category_Mask` makes no object free. Each object must instead overlap the limited mask and count toward its cap. Required faction, hero cap, transport payload restrictions and the requirement for at least one limited object still apply. | Debug build: the raid predicate tests the free-mask intersection before testing/incrementing the limited count, independently checks heroes, and rejects a final limited count of zero. |

**Ours:** `src/data/galactic/galactic_data.cpp:51` delegates constant lookup to
the placeholder-skipping reader; `src/data/galactic/galactic_world.cpp:320`
therefore reports the empty constant absent, and `src/campaign/fleets.cpp:38`
refuses an incomplete policy. That differs from an explicitly empty valid mask.
Once a zero-membership policy reaches it, the pure predicate at
`include/eawr/sim/galactic/raid.hpp:21` has the correct no-free-category behavior.
An empty occurrence after an earlier nonempty value leaves that earlier value
unchanged, because empty XML values are skipped. This differs from explicitly
parsing a nonempty separator-only string to zero. Missing tags, empty tags,
separator-only masks and invalid category names must stay distinct.

## Comment-ready findings

- Orbital classification (legacy EAWR-4610): RGC-01/04 confirm `DUMMY_ORBITAL_STRUCTURE`; fix the five setup/AI token consumers together, retaining authored build-tab flags and ordinary admission gates.
- Empty company upgrades (legacy EAWR-4611): RGC-05/07 distinguish null-transport refund, valid empty transport, no-spawn technology and additional creation; rejecting every empty composite before offers is too broad, and abstract-template counts do not prove broken playable upgrades.
- Omitted planet ratings: RGC-08 confirms zero for all three constructor defaults; the bring-up fallbacks match.
- Unresolved prerequisites: RGC-09/10 contradict the never-buildable rule; null entries are skipped, with a positional OR quirk that needs explicit coverage.
- Undefined starting forces: RGC-11 confirms diagnosis and skip on continued placement; preserve the diagnostic without aborting campaign admission.
- Signed income: RGC-12/13 preserve negative planet contributions and debit a negative fiscal total, flooring only the resulting balance; the refusal is in simulation setup.
- Empty raid mask: RGC-14/15 define a valid zero mask with no free categories, rather than an incomplete raid policy.

## Planet Force strength above one

This follow-up compares our sources at main
`c4e49a4d2756f2992713a22f25ca6c5399a3651d`. The question is whether authored
`Force_Strength` values such as 2 and 4 retain their magnitude. They do in the
inspected readers, **after continuing any debug assertion**; this is not a
claim that the debug build accepts those campaigns without interruption.

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-16 | `Force_Strength` uses the ordinary numeric parser and stores a single-precision float, initially zero. Parsing and the inspected load fixups neither clamp it nor enforce 0..1. Its magnitude is separate from the alignment enum selected by `The_Force`. | Debug build: XML mapping row, numeric conversion followed by a float store, shared type constructor, load fixups and direct float readers. |
| RGC-17 | A nonnull, nonneutral faction receives the full strength as its like-alignment multiplier on equality, or its opposing multiplier on inequality, including a neutral planet against a nonneutral faction. Other relations return zero. Neither multiplier reader clamps strength. A separate planet-alignment enum reader asserts that strength lies in 0..1, then returns the enum without changing strength. | Debug build: relation predicates, both multiplier readers and the enum reader. Refines WGM-111 and WGEP-75. |
| RGC-18 | Alignment income asserts that both multipliers lie in 0..1 and are not simultaneously positive. Continuing those assertions preserves the raw multipliers. A positive like product contributes basic income × strength × `MaxCreditIncomeAlignmentBonus`; otherwise a positive opposing product contributes the negative of basic income × strength × `MaxCreditIncomeAlignmentPenalty`. There is no strength clamp or income-contribution cap in this reader. | Debug build: checks precede multiplication and signed source insertion/removal; ordinary income and AI potential-income paths call the same helper. |
| RGC-19 | The political-control producer uses full strength for a nondegrading gain: a positive like effect installs time multiplier 1 − strength × `MaxInfluenceTransitionAlignmentBonus`, otherwise a positive opposing effect installs 1 + strength × `MaxInfluenceTransitionAlignmentPenalty`. It does not clamp strength or the resulting modifier at this boundary. | Debug build: target-control change branch, relation getters and modifier insertion. Refines WGM-72. |
| RGC-20 | AI potential-income evaluation inherits RGC-18's raw magnitude and assertions. Its alignment term uses the planet's current controlling player's faction, even though an observer is supplied to the outer estimate. | Debug build: potential-income helper delegates to the same alignment reader; traced callers include `IncomeNBPTM`, `MaxAllPlanetsIncomeUnnormalized` and `MaxOwnedPlanetsIncomeUnnormalized`. |

The static consumer survey found direct strength-field accesses only in
initialization, copying, the enum reader and the two multiplier readers. The
traced multiplier callers lead to income and political control. It found no
separate direct corruption or hero strength reader, and no call site for the
planet-alignment enum getter beyond its forwarding entry. This is a scoped
field-access and caller survey, not proof about arbitrary mod scripts or every
indirect call. Effects on AI income decisions or political control can still
affect other systems indirectly.

For positive basic income 100 and an authored maximum income bonus/penalty of
0.5, strength 2 contributes +100 or −100; strength 4 contributes +200 or −200.
These are alignment contributions before other income sources, not total income
or runtime observations. With transition maxima 0.5, the corresponding time
modifiers produced by RGC-19 are 0/2 and −1/3. That establishes the producer's
arithmetic, not the downstream outcome of a zero or negative transition time.

**Ours:** `src/data/galactic/galactic_world.cpp:70` parses the number and
`src/campaign/setup.cpp:469` forwards it without normalization. Campaign
validation reaches `src/sim/galactic/income.cpp:110`, which refuses strength
outside 0..1. That turns a retail reader assertion into a campaign-admission
failure; clamping to 1 would also differ from RGC-17/18. Once admitted, relation
selection at `src/sim/galactic/income.cpp:597` and contribution arithmetic at
`:68` and `:73` already preserve the magnitude, subject to checked fixed-point
arithmetic. Separately, `src/campaign/live_setup.cpp:354` refuses any nonzero
planet strength because the live control alignment producer is not connected;
removing only the income bound does not resolve that seam. This walk changes
no runtime behavior.

## Undefined per-tech starting-spawn types

This follow-up compares main `61b0741dc25a7df80f8340e2d91dbaf2e50168e2`.
An undefined `Starting_Spawned_Units_Tech_N` name is diagnosed and remains null;
the inspected consumers skip that entry, sometimes after another assertion.
These are static control-flow findings after continuing debug assertions, not
a runtime qualification of the mod campaign or a release-binary assertion policy.

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-21 | `Starting_Spawned_Units_Tech_0..5` bind separate ordered type/count lists. Each occurrence appends its first name/count pair; inherited pairs remain, as in WAR-84. Names are retained for later resolution against loaded object types. | Debug build: six XML mapping rows, pair parser, name-reference registration and post-load fixup traversal; WAR-84. |
| RGC-22 | An undefined name resolves to null, with a log diagnostic, popup and assertion. Continuing the assertion returns from fixup without removing the pair or substituting another type. The explicit case-insensitive name `None` instead resolves to null without that missing-name diagnostic. | Debug build: shared name fixup stores the lookup result before diagnosing failure; its getter returns that stored result. |
| RGC-23 | Downward tier selection tests row size, including unresolved entries. A higher row containing only null references is still nonempty and blocks fallback to a lower row. Indexed lookup returns the retained null; starting-count lookup returns the first matching resolved type's count in the selected row. Exact-tier enumeration instead reads only the requested row. | Debug build: ordinary and exact-tier count/type getters and starting-count getter; refines WAR-77 and WGT-58. |
| RGC-24 | Null starting-spawn entries contribute nothing to the inspected AI force estimates and proposal garrisons. Planet-force and proposal readers assert and skip; player-wide force totals skip without a local null assertion. A null entry alone does not make those force readers fail or discard valid entries. Their tier policies differ as listed below. | Debug build: both planet-force totals, both player-wide totals and proposal-garrison expansion, including their continuation and success paths. |
| RGC-25 | Tactical garrison setup selects using the spawning object's owner's current tech and downward fallback. A null type asserts and is skipped before any spawn-ledger entry is appended; valid entries retain their starting cap and reserve calculation. Both ordinary service and instant spawning reach this setup. | Debug build: spawn-ledger setup and its two callers; WGT-58/59. |
| RGC-26 | The inspected auto-resolve strength, attack and attrition loops assert and skip null garrison types. Auto-resolve bomber discovery and tactical bombing initialization skip null exact-tier entries before reading squadron members. | Debug build: all remaining direct consumers found through the ordinary and exact-tier starting-spawn getter references. |

| Inspected consumer | Tier enumeration | Null entry |
|---|---|---|
| AI planet-force space and ground totals | Downward fallback | Assert, then skip its contribution. |
| AI player-wide space and ground totals | Exact current tier | Skip its contribution; continue successfully. |
| AI proposal garrison expansion | Downward fallback | Assert, then append nothing for that entry. |
| Tactical garrison setup | Downward fallback | Assert, then append no spawn-ledger entry. |
| Auto-resolve strength, attack and attrition | Downward fallback | Assert, then skip the entry's work. |
| Auto-resolve bomber discovery and tactical bombing initialization | Exact tiers visited by those searches | Skip before inspecting squadron members. |

For example, with valid A at tech 0 and only an undefined name at tech 3,
downward selection at tech 3 selects the nonempty tech-3 row and contributes or
spawns nothing from it; it does not recover A from tech 0. If the selected row
contains an undefined entry followed by valid B, B remains usable. These are
synthetic deductions, not captured mod outcomes. Filtering undefined pairs out
of the stored lists before tier selection would change the first result.

The tech suffix selects garrison composition; these reads do not establish an
immediate grant of units on a tech-up event. The inspected player tech setter
updates tech, notifies story logic and may unlock construction; it does not
directly enumerate these lists. Garrison setup reads current tech when its
callers initialize an empty spawn ledger. This does not settle arbitrary
story-script effects or every later ledger reset. An all-null selected row leaves
that ledger empty, so a later setup attempt can repeat the null assertions.

**Ours:** `src/campaign/ai_perception_content.cpp:95` throws when its spawn-name
identity lookup fails, while `:113` performs that lookup for every authored tier
of every registered force type during preparation. This turns the unresolved
reference into a campaign-preparation refusal before an AI reader selects a
tier. RGC-22/24 support preserving the diagnostic and skipping the unresolved
entry at consumption, while RGC-23 requires preserving row presence and tier
selection. Removing invalid pairs globally or marking the entire force estimate
unavailable would differ from the inspected null handling. This walk changes no
runtime code.

## Playable Remake upgrade variants after inheritance

This audit uses the owner-confirmed three-layer Remake chain, leaf first, and
compares the bring-up sources at
`10e4e6f518095062166404a7415a230ce6625cd9`. It follows the winning
`GameObjectFiles.xml` includes through loose files and manifest-active archives,
then resolves each `Variant_Of_Existing_Type` chain before reading transport,
members, technology and admission tags. Unregistered files are not extra offers.
The private input manifest records the selected sources and their hashes.

Here **offered** means an authored production candidate for a registered playable
faction once its local prerequisites, tech, credits, ownership, slots and limits
permit it. It does not mean visible on the first campaign frame or verified by a
runtime build-bar capture. The registered standard Web Way campaign supplies the
seven affiliated factions below with maximum tech 5; the two minor factions can
be AI builders. All rows are initially unlocked, require timeline 0, have positive
price and time, and require tech 0 (companies and mercenaries), 2 (trade hubs),
3 (Golan 1-to-2), or 4 (Golan upgrades to 3). The tables preserve each exact local
structure requirement rather than claiming an unconditional offer.

| ID | Finding | Evidence summary |
|---|---|---|
| RGC-27 | The effective registry has 218 `Upgrade_*` names, matched without case sensitivity. Its 84 ground companies comprise 20 unaffiliated intermediate definitions and 64 concrete variants. Every one of the 84 inherits or authors `Company_Transport_Unit=Imperial_Landing_Craft`; each concrete variant has exactly one registered company member. Of the concrete variants, 48 have playable affiliation and 16 are Neutral-only. | Registered mod XML, effective variant chains, faction registry and local requirements; RGC-05/06. |
| RGC-28 | All 76 affiliated, priced upgrade candidates listed below have resolved `Build_Advances_Tech_Level=0` and no `Additional_Creation_Type`. The 48 companies use normal transport/member creation; the other 28 use `DUMMY_STARSHIP` proxy creation. None selects the no-spawn technology branch or the missing-transport production refund. | Registered mod XML; RGC-05..07; CGS-03. |
| RGC-29 | The intended building replacement is authored by the created member's or ship proxy's `Lua_Script`, not by its `Upgrade_*` name. In 74 rows the selected script has an exact matching uppercase dispatch literal and names registered old/new structures. This is a static completion-and-script-path qualification, not an end-to-end campaign capture. | Effective mod scripts and registered replacement targets; RGC-06. |
| RGC-30 | Top-level type parsing stores the XML `Name` in uppercase. `Get_Type().Get_Name()` returns that stored name as a Lua string, without changing its case. Ordinary Lua string equality remains case-sensitive. | Debug build: top-level XML type parser, stored-name getter and mod-facing Lua getter; Lua string comparison. |
| RGC-31 | The selected orbital upgrade script compares the two affiliated minor-faction proxy names against `UPGRADE_Hapan_GOLAN_2_DUMMY` and `UPGRADE_Mandalorians_GOLAN_3_DUMMY2`. RGC-30 returns uppercase names instead, so neither literal matches. The script reaches its final exit without replacing the building or despawning that proxy in these branches. This is a mod script failure after successful ship creation, not a production refund or CGS-08 rejection. | Effective mod script, registered proxy identities and RGC-30; static path deduction, not a runtime capture. |

### Company offers

For **every row** in this table, the resolved transport is
`Imperial_Landing_Craft`, the member column is the complete one-element
`Company_Units` list, and the resolved technology advance is **0**. `Works` means
the ordinary retail company-creation path is valid and the member has the
matching scripted replacement path, provided the listed source structures are
present. `Admit` means the bring-up branch's CGS-08 recursion returns true for
the complete inherited transport/member graph; it does not promise that every
later campaign subsystem supports the mod script.

| Exact playable company | Affiliation | Required local structures (all) | Sole member | Retail / CGS-08 |
|---|---|---|---|---|
| `UPGRADE_TAXATION_AGENCY_URBAN` | Empire | `Taxation_Agency_Urban` | `UPGRADE_TAXATION_AGENCY_URBAN_DUMMY` | Works / Admit |
| `UPGRADE_TAXATION_AGENCY_TEMPERATE` | Empire | `Taxation_Agency_Temperate` | `UPGRADE_TAXATION_AGENCY_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_TAXATION_AGENCY_BARREN` | Empire | `Taxation_Agency_Barren` | `UPGRADE_TAXATION_AGENCY_BARREN_DUMMY` | Works / Admit |
| `UPGRADE_ADMINISTRATION_OFFICE_URBAN` | Empire | `Administration_Office_Urban` | `UPGRADE_ADMINISTRATION_OFFICE_URBAN_DUMMY` | Works / Admit |
| `UPGRADE_ADMINISTRATION_OFFICE_TEMPERATE` | Empire | `Administration_Office_Temperate` | `UPGRADE_ADMINISTRATION_OFFICE_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_ADMINISTRATION_OFFICE_BARREN` | Empire | `Administration_Office_Barren` | `UPGRADE_ADMINISTRATION_OFFICE_BARREN_DUMMY` | Works / Admit |
| `UPGRADE_AGRICULTURAL_COMMUNITY_E_TEMPERATE` | Empire | `Agricultural_Community_E_Temperate` | `UPGRADE_AGRICULTURAL_COMMUNITY_E_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_AGRICULTURAL_COMMUNITY_2_E_TEMPERATE` | Empire | `Agricultural_Community_2_E_Temperate` | `UPGRADE_AGRICULTURAL_COMMUNITY_2_E_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_MINING_COMPANY_E_BARREN` | Empire | `Mining_Company_E_Barren Mining_Facility_E_Barren Mining_Facility_2_E_Barren` | `UPGRADE_MINING_COMPANY_E_BARREN_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_E_TEMPERATE` | Empire | `Mining_Facility_E_Temperate Mining_Company_Lv2_E_Temperate` | `UPGRADE_MINING_FACILITY_E_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_E_BARREN` | Empire | `Mining_Facility_E_Barren Mining_Company_Lv2_E_Barren` | `UPGRADE_MINING_FACILITY_E_BARREN_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_2_E_TEMPERATE` | Empire | `Mining_Facility_2_E_Temperate Mining_Company_Lv2_E_Temperate` | `UPGRADE_MINING_FACILITY_2_E_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_2_E_BARREN` | Empire | `Mining_Facility_2_E_Barren Mining_Company_Lv2_E_Barren` | `UPGRADE_MINING_FACILITY_2_E_BARREN_DUMMY` | Works / Admit |
| `UPGRADE_AGRICULTURAL_COMMUNITY_R_TEMPERATE` | Rebel | `Agricultural_Community_R_Temperate` | `UPGRADE_AGRICULTURAL_COMMUNITY_R_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_AGRICULTURAL_COMMUNITY_2_R_TEMPERATE` | Rebel | `Agricultural_Community_2_R_Temperate` | `UPGRADE_AGRICULTURAL_COMMUNITY_2_R_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_MINING_COMPANY_R_BARREN` | Rebel | `Mining_Company_R_Barren Mining_Facility_R_Barren Mining_Facility_2_R_Barren` | `UPGRADE_MINING_COMPANY_R_BARREN_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_R_TEMPERATE` | Rebel | `Mining_Facility_R_Temperate Mining_Company_Lv2_R_Temperate` | `UPGRADE_MINING_FACILITY_R_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_R_BARREN` | Rebel | `Mining_Facility_R_Barren Mining_Company_Lv2_R_Barren` | `UPGRADE_MINING_FACILITY_R_BARREN_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_2_R_TEMPERATE` | Rebel | `Mining_Facility_2_R_Temperate Mining_Company_Lv2_R_Temperate` | `UPGRADE_MINING_FACILITY_2_R_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_2_R_BARREN` | Rebel | `Mining_Facility_2_R_Barren Mining_Company_Lv2_R_Barren` | `UPGRADE_MINING_FACILITY_2_R_BARREN_DUMMY` | Works / Admit |
| `UPGRADE_AGRICULTURAL_COMMUNITY_U_TEMPERATE` | Underworld | `Agricultural_Community_U_Temperate` | `UPGRADE_AGRICULTURAL_COMMUNITY_U_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_AGRICULTURAL_COMMUNITY_2_U_TEMPERATE` | Underworld | `Agricultural_Community_2_U_Temperate` | `UPGRADE_AGRICULTURAL_COMMUNITY_2_U_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_AGRICULTURAL_COMMUNITY_Republic_TEMPERATE` | Republic | `Agricultural_Community_Republic_Temperate` | `UPGRADE_AGRICULTURAL_COMMUNITY_REPUBLIC_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_AGRICULTURAL_COMMUNITY_2_Republic_TEMPERATE` | Republic | `Agricultural_Community_2_Republic_Temperate` | `UPGRADE_AGRICULTURAL_COMMUNITY_2_REPUBLIC_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_MINING_COMPANY_REPUBLIC_BARREN` | Republic | `Mining_Company_Republic_Barren Mining_Facility_Republic_Barren Mining_Facility_2_Republic_Barren` | `UPGRADE_MINING_COMPANY_REPUBLIC_BARREN_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_REPUBLIC_TEMPERATE` | Republic | `Mining_Facility_Republic_Temperate Mining_Company_Lv2_Republic_Temperate` | `UPGRADE_MINING_FACILITY_REPUBLIC_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_REPUBLIC_BARREN` | Republic | `Mining_Facility_Republic_Barren Mining_Company_Lv2_Republic_Barren` | `UPGRADE_MINING_FACILITY_REPUBLIC_BARREN_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_2_REPUBLIC_TEMPERATE` | Empire | `Mining_Facility_2_Republic_Temperate Mining_Company_Lv2_Republic_Temperate` | `UPGRADE_MINING_FACILITY_2_REPUBLIC_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_2_REPUBLIC_BARREN` | Republic | `Mining_Facility_2_Republic_Barren Mining_Company_Lv2_Republic_Barren` | `UPGRADE_MINING_FACILITY_2_REPUBLIC_BARREN_DUMMY` | Works / Admit |
| `UPGRADE_TAXATION_AGENCY_URBAN_REPUBLIC` | Republic | `Taxation_Agency_Urban_Republic` | `UPGRADE_TAXATION_AGENCY_URBAN_DUMMY_REPUBLIC` | Works / Admit |
| `UPGRADE_TAXATION_AGENCY_TEMPERATE_REPUBLIC` | Republic | `Taxation_Agency_Temperate_Republic` | `UPGRADE_TAXATION_AGENCY_TEMPERATE_DUMMY_REPUBLIC` | Works / Admit |
| `UPGRADE_TAXATION_AGENCY_BARREN_REPUBLIC` | Republic | `Taxation_Agency_Barren_Republic` | `UPGRADE_TAXATION_AGENCY_BARREN_DUMMY_REPUBLIC` | Works / Admit |
| `UPGRADE_ADMINISTRATION_OFFICE_URBAN_REPUBLIC` | Republic | `Administration_Office_Urban_Republic` | `UPGRADE_ADMINISTRATION_OFFICE_URBAN_DUMMY_REPUBLIC` | Works / Admit |
| `UPGRADE_ADMINISTRATION_OFFICE_TEMPERATE_REPUBLIC` | Republic | `Administration_Office_Temperate_Republic` | `UPGRADE_ADMINISTRATION_OFFICE_TEMPERATE_DUMMY_REPUBLIC` | Works / Admit |
| `UPGRADE_ADMINISTRATION_OFFICE_BARREN_REPUBLIC` | Republic | `Administration_Office_Barren_Republic` | `UPGRADE_ADMINISTRATION_OFFICE_BARREN_DUMMY_REPUBLIC` | Works / Admit |
| `UPGRADE_AGRICULTURAL_COMMUNITY_CIS_TEMPERATE` | CIS | `Agricultural_Community_CIS_Temperate` | `UPGRADE_AGRICULTURAL_COMMUNITY_CIS_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_AGRICULTURAL_COMMUNITY_2_CIS_TEMPERATE` | CIS | `Agricultural_Community_2_CIS_Temperate` | `UPGRADE_AGRICULTURAL_COMMUNITY_2_CIS_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_MINING_COMPANY_CIS_BARREN` | CIS | `Mining_Company_CIS_Barren Mining_Facility_CIS_Barren Mining_Facility_2_CIS_Barren` | `UPGRADE_MINING_COMPANY_CIS_BARREN_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_CIS_TEMPERATE` | CIS | `Mining_Facility_CIS_Temperate Mining_Company_Lv2_CIS_Temperate` | `UPGRADE_MINING_FACILITY_CIS_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_CIS_BARREN` | CIS | `Mining_Facility_CIS_Barren Mining_Company_Lv2_CIS_Barren` | `UPGRADE_MINING_FACILITY_CIS_BARREN_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_2_CIS_TEMPERATE` | CIS | `Mining_Facility_2_CIS_Temperate Mining_Company_Lv2_CIS_Temperate` | `UPGRADE_MINING_FACILITY_2_CIS_TEMPERATE_DUMMY` | Works / Admit |
| `UPGRADE_MINING_FACILITY_2_CIS_BARREN` | CIS | `Mining_Facility_2_CIS_Barren Mining_Company_Lv2_CIS_Barren` | `UPGRADE_MINING_FACILITY_2_CIS_BARREN_DUMMY` | Works / Admit |
| `UPGRADE_TAXATION_AGENCY_URBAN_CIS` | CIS | `Taxation_Agency_Urban_CIS` | `UPGRADE_TAXATION_AGENCY_URBAN_DUMMY_CIS` | Works / Admit |
| `UPGRADE_TAXATION_AGENCY_TEMPERATE_CIS` | CIS | `Taxation_Agency_Temperate_CIS` | `UPGRADE_TAXATION_AGENCY_TEMPERATE_DUMMY_CIS` | Works / Admit |
| `UPGRADE_TAXATION_AGENCY_BARREN_CIS` | CIS | `Taxation_Agency_Barren_CIS` | `UPGRADE_TAXATION_AGENCY_BARREN_DUMMY_CIS` | Works / Admit |
| `UPGRADE_ADMINISTRATION_OFFICE_URBAN_CIS` | CIS | `Administration_Office_Urban_CIS` | `UPGRADE_ADMINISTRATION_OFFICE_URBAN_DUMMY_CIS` | Works / Admit |
| `UPGRADE_ADMINISTRATION_OFFICE_TEMPERATE_CIS` | CIS | `Administration_Office_Temperate_CIS` | `UPGRADE_ADMINISTRATION_OFFICE_TEMPERATE_DUMMY_CIS` | Works / Admit |
| `UPGRADE_ADMINISTRATION_OFFICE_BARREN_CIS` | CIS | `Administration_Office_Barren_CIS` | `UPGRADE_ADMINISTRATION_OFFICE_BARREN_DUMMY_CIS` | Works / Admit |

The Republic second temperate mining-facility upgrade is authored for **Empire**,
not Republic. Its Republic-named prerequisites remain required. This is an
admission mismatch for the expected Republic builder, not a missing transport:
an Empire builder needs those structures locally. The table preserves that
conditional offer rather than silently repairing its affiliation. Similarly,
the 16 Neutral-only concrete companies are not player-faction offers under
WGEP-18; an unlock alone does not remove the affiliation gate.

### Orbital and mercenary ship-proxy offers

For **every row** below, resolved company transport is absent, company members
are empty, and technology advance is **0**. These are `DUMMY_STARSHIP` records,
so absence of company data does not invoke the company failure rule. They create
the proxy ship normally; its script performs the authored structure replacement.
`Works` and `Admit` have the same scoped meanings as above.

| Exact playable ship proxy | Affiliation | Required local structure | Retail / CGS-08 |
|---|---|---|---|
| `UPGRADE_E_GOLAN_3_DUMMY2` | Empire | `E_Golan_1` | Works / Admit |
| `UPGRADE_E_GOLAN_3_DUMMY` | Empire | `E_Golan_2` | Works / Admit |
| `UPGRADE_R_GOLAN_3_DUMMY2` | Rebel | `R_Golan_1` | Works / Admit |
| `UPGRADE_R_GOLAN_3_DUMMY` | Rebel | `R_Golan_2` | Works / Admit |
| `UPGRADE_U_GOLAN_3_DUMMY2` | Underworld | `U_Golan_1` | Works / Admit |
| `UPGRADE_U_GOLAN_3_DUMMY` | Underworld | `U_Golan_2` | Works / Admit |
| `UPGRADE_CIS_GOLAN_3_DUMMY2` | CIS | `CIS_Golan_1` | Works / Admit |
| `UPGRADE_CIS_GOLAN_3_DUMMY` | CIS | `CIS_Golan_2` | Works / Admit |
| `UPGRADE_REPUBLIC_GOLAN_3_DUMMY2` | Republic | `Republic_Golan_1` | Works / Admit |
| `UPGRADE_REPUBLIC_GOLAN_3_DUMMY` | Republic | `Republic_Golan_2` | Works / Admit |
| `UPGRADE_MANDALORIANS_GOLAN_3_DUMMY2` | Mandalorians | `Mandalorians_Golan_1` | Broken script / Admit |
| `UPGRADE_E_GOLAN_2_DUMMY` | Empire | `E_Golan_1` | Works / Admit |
| `UPGRADE_R_GOLAN_2_DUMMY` | Rebel | `R_Golan_1` | Works / Admit |
| `UPGRADE_U_GOLAN_2_DUMMY` | Underworld | `U_Golan_1` | Works / Admit |
| `UPGRADE_CIS_GOLAN_2_DUMMY` | CIS | `CIS_Golan_1` | Works / Admit |
| `UPGRADE_REPUBLIC_GOLAN_2_DUMMY` | Republic | `Republic_Golan_1` | Works / Admit |
| `UPGRADE_HAPAN_GOLAN_2_DUMMY` | Hapan | `Hapan_Golan_1` | Broken script / Admit |
| `UPGRADE_TRADE_HUB_E` | Empire | `E_Trade_Hub` | Works / Admit |
| `UPGRADE_TRADE_HUB_R` | Rebel | `R_Trade_Hub` | Works / Admit |
| `UPGRADE_TRADE_HUB_U` | Underworld | `U_Trade_Hub` | Works / Admit |
| `UPGRADE_TRADE_HUB_CIS` | CIS | `CIS_Trade_Hub` | Works / Admit |
| `UPGRADE_TRADE_HUB_Republic` | Republic | `Republic_Trade_Hub` | Works / Admit |
| `UPGRADE_E_MERCENARY_CITADEL_DUMMY` | Empire | `E_Mercenary_Settlement` | Works / Admit |
| `UPGRADE_R_MERCENARY_CITADEL_DUMMY` | Rebel | `R_Mercenary_Settlement` | Works / Admit |
| `UPGRADE_U_MERCENARY_CITADEL_DUMMY` | Underworld | `U_Mercenary_Settlement` | Works / Admit |
| `UPGRADE_E_MERCENARY_SETTLEMENT_DUMMY` | Empire | `E_Mercenary_Outpost` | Works / Admit |
| `UPGRADE_R_MERCENARY_SETTLEMENT_DUMMY` | Rebel | `R_Mercenary_Outpost` | Works / Admit |
| `UPGRADE_U_MERCENARY_SETTLEMENT_DUMMY` | Underworld | `U_Mercenary_Outpost` | Works / Admit |

The Hapan row has a direct prerequisite witness: the registered standard casual
Web Way campaigns place `Hapan_Golan_1` at `Hapes_WW`. The Mandalorian row is
retained as an affiliated conditional candidate, not a demonstrated reachable
build-bar offer: no direct placement of its level-one prerequisite was found in
the registered campaign starting-force records.

The two `Broken script` rows qualify **creation succeeds, upgrade effect fails**
if queued with their required source structure present.
The other 74 rows qualify **works on the inspected creation and matching-script
path**; no row takes the production refund or no-spawn technology path. RGC-31
does not claim that the CGS-08 filter can or should fix mod-authored Lua string
comparisons. The selected script also uses mixed-case literals for the related
minor-faction spawners; this walk does not claim that their prerequisite structures
are reachable through those broken spawner branches.

### Exclusions and implementation consequence

The remaining 142 registered prefix matches are 20 unaffiliated company
intermediates, 16 Neutral-only companies, 65 unaffiliated ground-vehicle members
or member templates, 14 unaffiliated ship intermediates, 16 Neutral-only ship
proxies (including comma-prefixed Neutral affiliation), and 11 tactical upgrade
templates with no authored production price or concrete menu entry. The
unaffiliated records are operational templates, not proof of a separate engine
abstract flag. Their missing members do not describe the derived offers.
Unregistered population/research files elsewhere in the mod directories were
excluded by the active registry, not by their filenames.

**Ours:** at the pinned bring-up revision,
`src/campaign/setup.cpp:874`'s `campaign_type_creatable` checks resolved behavior,
transport, company members and squadron members recursively. A ground company
with an empty transport **or** empty members returns false. Its starting-force
caller skips that type; `src/campaign/live_setup.cpp:594` also removes it from
the census/offer input. Replaying that exact predicate over the effective graph
admits **all 76 listed candidates**, including all 48 companies, and rejects the
20 memberless company intermediates. There is no CGS-08 offer suppression of
these playable companies after inheritance. Valid company creation and later
Lua building replacement remain separate implementation responsibilities.

The earlier count of 20 empty company upgrades therefore does **not** establish
20 broken playable upgrades: it describes unaffiliated intermediates, and even
they have a transport. RGC-05 still establishes that a genuinely offered
transport-with-no-members type may create an empty transport after an assertion;
RGC-06/07 still require preserving a real null-transport refund, positive-tech
no-spawn completion and independent additional creation. Those generic
corrections remain useful, but this installed playable roster supplies no such
refund or technology example. No runtime code or mod files change in this walk.

## AI XML discovery, registration and failures

This follow-up compares main `800c6a49f678b45fed6930fb31de7c06c47da1a5`
and the bring-up branch `10e4e6f518095062166404a7415a230ce6625cd9`.
Evidence includes the debug build's executable and its matching debug perception
module, inspected in a separate analysis project. These are static startup and
refresh paths, with assertion continuation where stated; no mod campaign was
launched. The directory-registration finding refines AI-16: discovering names,
opening their winning bytes, registering definitions and selecting a player's
mode-specific dependencies are separate operations.

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-40 | Fresh static AI registration unconditionally enumerates `*.XML` in `Data/XML/AI/Players/`, `Templates/`, `Goals/`, `GoalFunctions/` and `PerceptualEquations/`. There is no stock filename allowlist, selected-player dependency test or preceding stock-closure failure gate in these loaders. They consume the directory enumerator's returned names directly, without recursive descent. | Debug build: four executable directory loaders, the equation-token prepass and the perception module's equation loader. |
| RGC-41 | The inspected enumerator returns the requested native disk directory's matches first, then public archives in mounted-list order, visiting each archive's indexed entries in order. It tracks already returned logical names and suppresses weaker archive copies. It does not globally sort filenames or walk the alternate mod loose-root list. Archive matches require the requested directory, apart from separate directory-result handling; the AI loaders do not recurse into returned directories. | Debug build: executable and perception-module first/next enumeration bodies, native API entry and archive-name matching branches. |
| RGC-42 | After discovering a name, XML loading opens one complete winning file. Ordinary reads try alternate mod loose roots in their registered order, the requested native disk path, public archives, then fallback loose roots, stopping on success. Nodes from shadowed copies are not merged. The perception module receives a copy of the executable's registered mod loose roots before its XML reads. File-byte precedence is distinct from RGC-41's discovery order. | Debug build: XML reader, ordinary file-open path and perception-module initialization; ML-04/05/08/17. |
| RGC-43 | The relevant fresh registration sequence is equation-name token collection, equation construction, goals, goal-function sets, plan definitions, templates, then player types. Evaluator scripts and hint tables are initialized before equation construction. These are shared static catalogs, not a separate galactic filename corpus. A failed stage records failure and still calls subsequent stages. | Debug build: static AI initialization and the executable-to-perception-module call. This is startup registration order, not a frame-service schedule. |
| RGC-44 | Templates and goals take their names from the XML child keys, equations from their child keys, and a player type from its authored `Name`. A goal-function set instead takes its name from the returned filename up to the **first dot**. Thus synthetic `extra.policy.xml` registers set `extra`, not `extra.policy`. The containing file is not the identity of each template, goal or equation. | Debug build: individual file readers, function-set directory loader and ordinary definition lookup. |
| RGC-45 | On fresh loading, duplicate templates, goals and player types retain the earlier registered definition and report a failed read. Templates and goals continue to later siblings after a duplicate; a duplicate player rejects the newly constructed player. Within a newly constructed function set, duplicate entry keys leave the earlier entry untouched but make construction fail, discarding that new set. Duplicate set identity reaches an assertion and does not replace its earlier binding. Template, goal, function-set and equation managed-name lookup folds case. | Debug build: duplicate branches, definition lookup, set construction and function-set insertion. This is definition identity, separate from same-file shadowing under RGC-42. |
| RGC-46 | A duplicate equation retains its earlier definition, diagnoses/asserts and marks the read unsuccessful. Continuing assertions allows later sibling equations and later files to be visited. An equation that fails construction is not registered; valid earlier and later equations remain registered. The prior token-name prepass can therefore know a name whose equation never became usable. | Debug perception module: equation file reader and directory loop; executable equation-token prepass. |
| RGC-47 | These loaders do not implement an atomic all-files transaction. A bad template construction stops that file immediately but leaves already registered templates; its directory loader proceeds to the next file. Goal and equation readers continue after their inspected per-entry failures, retaining their successful registrations. Failed player creation registers no player; a newly constructed function set that fails parsing is discarded before registration. Missing directories or failed files produce loader failures and diagnostics; equation-directory absence and equation-file failures also assert. | Debug build: template, goal, player, set and equation construction/readers, directory loops and aggregate startup result. Continuation is not proof that the outer application accepts every failed startup. |
| RGC-48 | Galactic, land and space AI select from shared registered data. The player type supplies mode-specific template vectors; goal-proposal functions are routed by their goal's `GameMode` and read back through the current mode's vector. Registering a tactical definition does not activate it for a galactic player, and selecting galactic AI does not justify omitting unrelated files from static discovery. | Debug build: global initialization, template-by-mode parsing and goal-function mode readers; WGA-02 and AI-16. |
| RGC-49 | The explicit XML refresh path is different from fresh loading: it skips unchanged tracked files and can reparse existing templates, goals, function sets and player types in place. Equation refresh rebuilds existing equations but diagnoses/asserts and refuses new equation names. Fresh duplicate rejection is not an authorization to treat refresh as another ordinary registration pass. | Debug build: XML refresh caller, refresh flags in directory/file readers and debug perception-module equation refresh branch. |
| RGC-50 | Related initialization is separately scoped: hint tables enumerate `Data/XML/AI/HintSets/*.XML`; evaluator scripts enumerate `Data/Scripts/Evaluators/*.lua`; plan registration enumerates its configured script directories. Those paths do not imply a recursive scan of every file under `AI`. Galactic markup is selected separately by the player/planet authoring. | Debug build: static initialization, related directory loaders and existing WGA-426..428 / PL-02. |

RGC-41/42 qualify the phrase "every mounted AI file." The inspected globs
discover the requested disk directory and public archive names. A new basename
present **only** in an alternate mod loose tree is not found by an alternate-root
loop in these bodies; an already discoverable basename can nevertheless open
that mod's replacement bytes. A runtime check of a uniquely named loose mod file
is still needed to establish whether launch setup or another external mechanism
changes the discovery environment. Neither copying mod roots into the perception
module nor ordinary file-open precedence proves such discovery. AI-16's
unqualified "mounted directories" wording should be read with this limitation.
There is no evidence here for imposing a stock filename list or a conditional
discovery fallback.

The following locations refer to the pinned revisions above. The tactical
and shared-reader locations are the same in both revisions.

| Read boundary | Main | Bring-up branch | Difference from the inspected path |
|---|---|---|---|
| Galactic inventory seed | `src/script/foc/galactic_ai_loading.cpp:33`; `src/campaign/galactic_ai_runtime.cpp:898` | Same helper at `:33`; runtime at `:900` | Requires 34 stock-named AI files before directory discovery, plus four shared XML files. A missing seed file fails its immediate open before the bring-up fallback can run. RGC-40 has no such filename prerequisite. |
| Additional galactic files | `src/campaign/galactic_ai_runtime.cpp:901` | `src/campaign/galactic_ai_runtime.cpp:903`, `:910`, `:915` | Main discovers additional players only. Bring-up discovers the other four directories only after its initial load fails, so a successful stock closure hides valid additional definitions. Both enumerate recursive VFS prefixes without excluding nested files; RGC-40's loaders do not recurse. |
| Bring-up admission after failure | No corresponding fallback | `src/script/foc/galactic_ai_loading.cpp:97`; `src/campaign/galactic_ai_runtime.cpp:922` | Trials reparse the accumulated parser inputs and discard a whole extra file on failure. RGC-46/47 retain successful registrations from partially failed files and continue per entry or per file as specified. The final full dependency check remains separate. |
| Tactical discovery | `src/skirmish/ai.cpp:278`; `src/script/foc/ai_data.cpp:254` | Same | Already enumerates all five directories, excludes nested names, then passes the frozen inventory to the shared parser. The fixed fixture lists in `src/script/foc/ai_mode.cpp:10` do not describe this host's actual AI XML discovery. Its VFS order and fail-fast parsing still have the differences below. |
| File precedence and enumeration | `src/vfs/vfs.cpp:527`, `:771` | Same | VFS groups loose files and archives by layer and emits winners sorted by canonical path. RGC-41/42 separate native/archive enumeration from mod-loose-first byte lookup; ML-05 already records the priority difference. The recursive mounted inventory also includes alternate-root-only and nested names beyond the inspected glob. |
| Definition names and duplicates | `src/script/foc/ai_data.cpp:342`, `:376`, `:380`, `:389`, `:394`; `src/script/foc/ai_equations.cpp:435` | Same | Maps silently retain the first player/template/goal/set/equation without the original duplicate failure reporting. Set naming removes only the final `.xml`, so dotted filenames differ from RGC-44. Function entries become a vector without retaining their XML entry keys, so duplicate entry keys are not rejected as in RGC-45. |
| Failed records and dependencies | `src/script/foc/ai_data.cpp:279`, `:308`, `:420`; `src/script/foc/galactic_ai_loading.cpp:89` | Same shared parser; galactic caller at `:125` | Shared parsing returns one failure and exposes no partially registered catalog; galactic validation additionally refuses unresolved template category/equation/trigger references. RGC-47 establishes partial registration, not blanket permission to ignore missing dependencies or continue the outer application. |

Synthetic acceptance cases for a later implementation should include: a new
equation/template/goal/set while the stock closure remains valid; replacement
bytes at an already discoverable filename; a unique basename only in an
alternate loose root; a nested XML file; two files declaring one definition;
a dotted set filename; duplicate function-entry keys; and a valid/bad/valid
sequence within one file followed by another valid file. Pin discovery order,
winning bytes, registered definitions and diagnostics separately. Keep fresh
loading and refresh cases distinct, and exercise galactic, land and space
selection against one registered corpus. These are proposed cases, not tests
executed by this read-only walk. The discovery/registration gap remains open
(legacy EAWR-1700, EAWR-4609); this note changes no runtime code.

## Level-zero starting starbases

Walk Q07 compares main `c1168fe81e94ea93c29db51a2667e873f2d81ca4`.
The result is static debug-build continuation after assertions, not a release
capture. A zero-level dummy updates the planet's base record and removes its
orbital special structures through the ordinary clear path; it does not create
a standalone base object or reject the type merely because its level is zero.

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-90 | Shared type construction initializes `Base_Level` to zero. A resolved variant inherits the base value before applying its own nonempty scalar overrides; an omitted or empty leaf tag is not necessarily zero. With no inherited or authored value, zero reaches the base-level getter. | Debug build: XML field binding, shared constructor and direct getter; ML-23/24 supply scalar inheritance and empty-value rules. |
| RGC-91 | Delegated `DUMMY_STAR_BASE` placement diagnoses a new level that is not greater than the existing level. Continuing that assertion still emits the starbase construction story notification, then unconditionally stores the supplied level, type and owner in the planet's base record. The initializer has no special zero branch, type clearing, owner clearing or level clamp. | Debug build: delegated placement and base-record initializer. Extends WGS-180 to zero without changing its positive-level rule. |
| RGC-92 | After that update, an unsigned new level below two invokes orbital-special-structure clearing: both zero and one take it, two does not. The shared removal helper removes the structure's influence, erases its list entry, detaches the planet's listener and requests destruction unless already deletion-pending. A null entry asserts and is discarded; a failed removal asserts and stops clearing. This path does not clear the ground-special list. | Debug build: placement threshold, clear loop and shared removal; CGS-10's positive-level case. Ordinary successful removal, rather than an arbitrary listener's behavior, is the scope of the expected empty list. |
| RGC-93 | A starbase branch leaves the dispatcher's primary-object result null at every level; null does not mean that its base update failed. Starting placement continues to the next authored record. A squadron or a type with a nonempty ship class can additionally assert on a null result; continuing that assertion also resumes the loop. Only a nonnull result for a human owner invokes the caller's guard-fleet unification. | Debug build: initialized result, starbase branch, dispatcher return and starting-force caller. Additional creation retains RGC-07's independent dispatch. |

Use this discriminating synthetic state: an existing level-two base owned by A,
one orbital special structure and one ground special structure. Place a valid
dummy starbase type owned by B, followed by a distinguishable valid force.
Disable authored additional creation and unrelated callbacks. The table gives
static expectations after continuing the non-increasing-level assertion and
successful ordinary structure removal; these are not executed captures.

| Authored input | Resulting base record | Earlier orbital special structure |
|---|---|---|
| No starbase force record | Existing level/type/owner retained. | Retained. |
| Starbase record, `Base_Level` absent throughout its inheritance chain | Level 0, supplied dummy type and owner B. | Removed. |
| Explicit `Base_Level` 0 | Level 0, supplied dummy type and owner B. | Removed. |
| Explicit `Base_Level` 1 | Level 1, supplied dummy type and owner B. | Removed. |
| Explicit `Base_Level` 2 | Level 2, supplied dummy type and owner B. | Retained. |

In each placed-record case the ground structure remains and the following
force is attempted. Reversing orbital-structure/base order distinguishes removal
of earlier entries from a permanent prohibition on later placement. A leaf
omitting `Base_Level` but inheriting two belongs to the level-two row, not the
omission-to-zero row. Zero retains a nonnull authored type and supplied owner;
it is not equivalent to deleting every base-record field. This walk does not
qualify later UI, tactical-map or owner readers of that zero-level record.

**Ours:** `src/campaign/starting_forces.cpp:176` refuses absent and zero levels,
although RGC-90/91 supplies their inspected continuation. The adjacent
below-two check at `:178` refuses when space-domain objects exist because their
owning lifecycle service is not connected. RGC-92 requires that lifecycle work
for zero as well as one; simply relaxing the level bound would leave earlier
orbital structures alive. The level-zero unverified bullet in
`docs/behaviour/campaign-setup.md:259` can be narrowed to release/runtime
qualification and downstream readers. No new mod failure is asserted, and
this walk changes no placement code.

## Nonpositive default hero delay: scheduling is not creation

This answers the evidence boundary of walk Q08, rather than claiming a timing
result. The comparison base is main
`c1168fe81e94ea93c29db51a2667e873f2d81ca4`.

| ID | Retail rule or unresolved boundary | Evidence summary |
|---|---|---|
| RGC-94 | After its eligibility, destination and team gates, hero scheduling uses a positive requested absence unchanged; otherwise it reads `Default_Hero_Respawn_Time` once and forwards that value without a second positivity test. A nonpositive default is therefore not refused at this inspected boundary. | Recovered debug-build scheduling receipt; WGM-78..80. This is a scheduling fact, not a creation-time verdict. |
| RGC-95 | **Unresolved:** zero or negative `Default_Hero_Respawn_Time` does not yet have a qualified galactic producer-to-consumer deadline case establishing same-frame creation, next-service creation or no creation, separately from first ordinary service. The existing positive-delay rules must not be extended into an unconditional same-frame result. | GFS-10, GFC-25, GFC-C-03, GFD-17/22; GFSC-17 and C-03 retain creation/first-service qualifications. Nonpositive-default follow-up is queued in the frame-service programme (legacy EAWR-4621). |

The tactical FS-05/FC-04/FD-03 rules concern a different delayed-creation
vector. Galactic scheduled hero creation instead follows ordinary objects,
requires an active unpaused galaxy, and tests due records at frame >= deadline
(GFS-10, GFS-C-04, GFC-25). GFD-17 distinguishes immediate initialization or
notifications from the newborn's first ordinary periodic service. These
existing rules narrow the question; they do not supply the missing
nonpositive-default producer case.

The requested discriminating case uses one eligible named hero, a valid owned
destination and available placement capacity. At a positive galactic frame t,
request zero absence with authored defaults +1, 0 and -1 seconds, preserving the
logical FPS input. Repeat admission just before and just after the scheduled
creation pass. Record the admitted deadline, record retention/removal, created
identity, creation notifications and first ordinary periodic service separately
at t, t+1 and t+2; keep the positive control through its deadline. Repeat an
inactive/paused galaxy and a failed placement without treating their missing
creation as a timer result. This is a programme recipe, not an executed test.
The negative-time conversion near frame zero also needs explicit qualification;
do not infer its integer behavior from a positive-delay formula.

The supplied Remake chain's three loose `GameConstants.xml` files each author
-1; RaW's authors 360. The leaf therefore does not inherit this value from its
parents. This narrow file check establishes the relevant authored input, not
campaign runtime qualification or activation of every other constants file.
No omission default for the constant is established here.

**Ours:** `src/campaign/live_setup.cpp:673` refuses a nonpositive default while
building the ability host, and `src/sim/galactic/ability_host.cpp:85` repeats the
positive-default admission requirement. RGC-94 shows that this refusal is
stricter than the inspected scheduling helper. RGC-95 leaves the replacement
timing policy open: the bring-up note's CGS-12 same-frame assumption is not a
verified retail rule. Keep a named scheduling seam until the programme supplies
the deadline/creation/first-service case. This walk changes no runtime code.

## Undefined names in AI TaskForce declarations

This follow-up compares main `4a451455d2b1cdf9940c4480b6cf5dd31a5d5b45`.
The findings below are static debug-build control-flow reads. An undefined
exclusion and an undefined inclusion take different paths; neither keeps a
null candidate. Assertion continuation is required for the inclusion findings,
and no release-binary assertion policy or running mod campaign is inferred.

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-60 | A TaskForce table starts with its name, followed by declaration strings or script-variable tables. A declaration splits its left side from `=` and reads `a,b` as minimum/maximum, `a` as both, or `p%` as a percentage. A maximum of zero becomes the minimum. Recognized options, staging passability, `MinimumTotalSize` and `MinimumTotalForce` are handled before exclusions or roster rows. | Debug build: definition construction and declaration parser; PL-11. |
| RGC-61 | A leading `-` makes the trimmed remainder a **single object-type lookup**, not a category mask or a split list. A defined type is recorded for exclusion; an undefined name is ignored, with no null inserted and no local assertion or diagnostic. Counts on that declaration do not turn it into an inclusion. | Debug build: exclusion branch and type lookup. |
| RGC-62 | An inclusion first attempts category conversion of the entire left side. On failure or a zero mask it interprets the `\|`-separated tokens as object-type names. An undefined token asserts and logs an AI error, then continues with the next token. Valid alternatives remain. A newly parsed roster row with no possible types is removed, even when its authored minimum is positive; its minimum/maximum are not retained as an impossible null request. Single-count, minimum/maximum and percentage rows share this handling. | Debug build: category/type branches, tokenizer continuation and final empty-row removal. |
| RGC-63 | A recognized category mask enumerates registered types matching any bit, excluding team-creating types, squadron members, bases and cinematic-only objects, and applies authored staging passability. Explicit named types have their own eligibility checks and base-family expansion. Exclusions are applied after all declarations to every retained roster row, including percentage rows, by removing matching types. This pass does **not** remove a row that becomes empty, so a valid type excluded later differs from a row that had no possible type during parsing. | Debug build: category enumeration, named-type branch, definition construction and final exclusion pass; refines PL-11. |
| RGC-64 | Definition construction ignores the declaration parser's Boolean return; option and exclusion entries also return false. Plan loading adds the constructed TaskForce definition without requiring a nonempty roster. The no-TaskForce rejection counts definitions, not roster rows. Thus an unknown exclusion, or removal of every unknown-only roster row, does not itself discard the TaskForce definition or plan. Other script-load, goal and table-validity checks remain separate. | Debug build: definition constructor and plan-load acceptance branch; PL-02/10. |
| RGC-65 | Parsing a percentage entry sets the TaskForce's percentage-mode flag before resolving its names. Dropping an unknown-only row does not undo that flag. Plan loading totals percentages from retained rows across its TaskForce definitions and rescales retained percentages when that total is nonzero; a zero total leaves them unchanged. It also asserts that each TaskForce's percentage mode agrees with the plan's mode. | Debug build: declaration parser, plan-load percentage handling and normalization helper. |
| RGC-66 | Shared plan selection builds its team layout from retained roster rows. Its roster proposal contribution enumerates those rows' possible types, applying affiliation/base/freestore admission and galactic garrison expansion; it does not resolve discarded names again. Count-based weighting uses the retained maximum, and validation rejects a retained zero-percentage row whose selected count is below its minimum. A row removed during parsing supplies neither candidates nor that minimum-count check. A retained row emptied by exclusions still supplies its minimum-count check. | Debug build: selection initialization, roster proposals, weighting and validation; WGA-25/60. |
| RGC-67 | Plan registration is separate from successful selection and live TaskForce creation. Selection with no units is invalid unless at least one TaskForce is required; required-category checks still apply. Minimum total force/size checks in the inspected roster validator run through retained team rows, so a definition with no rows has no such per-row visit. At live creation, an optional TaskForce with neither instantiated nor reinforcement units is omitted; a required TaskForce bypasses that omission and can be created empty. | Debug build: selection validator and live TaskForce creation; refines PL-25/40. |

For synthetic types `Valid` and undefined `Missing`, these parser outcomes follow:

| Declaration | Result after parsing and exclusions |
|---|---|
| `-Missing` | No exclusion recorded; other rows unchanged. |
| `Missing = 2,4` | Assert and diagnose, then remove that roster row. |
| `Missing \| Valid = 2,4` | Assert and diagnose for `Missing`; keep a row containing `Valid` with minimum 2 and maximum 4. |
| `Missing = 25%` | Assert and diagnose, then remove that roster row; the definition's percentage-mode flag has already been set. |
| `Valid = 2,4` followed by `-Valid` | Retain a row with minimum 2, maximum 4 and no possible types after exclusions. |

These are deductions from the inspected parser, not captured executions.

The scoped consumer chain is definition load, declaration parsing, final
exclusions, selection-team initialization, roster proposal enumeration,
weighting/validation, then live force creation. The selected roster reaches
ordinary production through the existing build-task path (WGA-340). The
declaration's unknown spelling is not a deferred production request. This
survey does not qualify every later growth/refit operation or arbitrary Lua
script action.

**Ours:** `src/script/foc/galactic_ai_plan_materialization.cpp:85` refuses the
entire catalog on an unresolved excluded type, contrary to RGC-61/64. Its
exclusion reader also splits on `|`, whereas retail performs one lookup of the
whole remainder. At `:102`, any note from the shared parser becomes another
catalog failure. The parser's `src/script/foc/ai_plans.cpp:104` diagnoses an
unknown inclusion, but `:119` keeps an empty row marked as unloaded; recognized
categories with no candidates also remain. These differ from RGC-62/66's
parse-time row removal. Resolving the exclusion refusal alone addresses the
reported gate but does not establish parity for undefined inclusion/count
rows. Preserve diagnostics for inclusion failures without turning them into
campaign-admission failures, and preserve retained rows emptied by valid
exclusions. This walk changes no runtime code.

## Location admission, empty campaigns and duplicate locations

Walk C answers Q05 and qualifies Q06 in the [remaining-question queue](remake-walk-queue.md).
Source comparisons in this section use main
`c1168fe81e94ea93c29db51a2667e873f2d81ca4`. Evidence is read-only debug-build
inspection, including delegated creation and lookup; none of these cases was
launched in the shipping executable. Continuing every assertion below is a
condition of the static result, not an observed release policy.

| ID | Retail rule / qualification | Evidence summary |
|---|---|---|
| RGC-80 | A fresh campaign's `Locations` list is empty. The campaign XML reader ignores empty values and `TBD`, preserving an earlier value. Name fixup visits every stored location reference without compacting the list: an undefined name is diagnosed, shown in a popup and asserted, and its resolved value remains null. Indexed location access returns that null. | Debug build: campaign and name-list construction, campaign parser, campaign fixup, shared name fixup and indexed getters. |
| RGC-81 | An undefined listed location does not cause a campaign-wide failure in the inspected continuation. The creation pass asserts on its null type, delegates creation, and asserts on the null result; delegated creation itself asserts and returns null before reading the supplied position. The placement pass asserts again, obtains an empty exact-type lookup result, asserts on its empty count and does no placement for that entry. Both authored-entry loops continue, retaining valid locations before and after it. | Debug build: object assignment, position accessor, delegated creation, planet lookup and underlying exact-type lookup. The position accessor computes a field address; this null creation branch does not dereference it. |
| RGC-82 | A valid planetary location is created for the neutral player independently of whether any `Starting_Forces` record names it. No matching record means no authored force placement there; subsequent political-control initialization still visits it. Conversely, a force record for a defined type outside `Locations` does not create that location or place its force, as in RGC-11. Nonplanet scene locations remain WGS-01a's separate case. | Debug build: neutral creation precedes per-location force matching; the stored force traversal matches exact location type; WGS-09/70 and RGC-11. |
| RGC-83 | Neither the inspected campaign registration nor its general list builder rejects a campaign solely for having no locations. Registration retains the definition; general listing applies listed/multiplayer and campaign-set grouping gates, without testing location count. With no resulting live planets, object assignment asserts that the planet list is nonempty; continuation skips its planet loops and proceeds to player caps and homes. The path initializer supports a zero-node graph. This establishes continuation through these boundaries, not a captured usable empty campaign or the outcome of arbitrary AI, story, victory or UI consumers. | Debug build: campaign registration and listing, object assignment, startup caller, path initialization and zero-size storage helpers. The startup caller has no location-count refusal and its final success return is not a runtime qualification of every delegate. |
| RGC-84 | For repeated valid `Locations`, existing WGS-184/setup evidence still establishes multiple creation attempts, then repeated force placement on the first exact-type live match, which is the last-created matching object if creation succeeds. Home assignment stores a type reference; the inspected local camera/selection lookup takes the first matching live object, also the last-created instance. It does not assign a separate home instance to each repeated entry. | Debug build: retained object-assignment evidence, home-type setter/getter and local startup home lookup; WGS-06/09/184. This adds the home distinction without upgrading the existing duplicate-location shipping qualification. |
| RGC-85 | **Shipping behavior remains unverified** for duplicate-location creation, repeated placement and home selection, and for exposure of an empty campaign. No inspected release/capture receipt settles whether execution reaches the continued debug branches. Keep the duplicate-name planner guard's existing project-policy qualification; a shipping deduplication or refusal rule cannot be inferred from WGS-184 or from either mod running. | Existing duplicate-location research explicitly leaves shipping continuation open; this walk adds static consumers and authored-input inventory, not a release run. |

RGC-80 describes campaign construction and field preservation, not a new
campaign-inheritance mechanism. No campaign variants appeared in the registered
declarations inventoried here. A referenced object type may inherit its own
fields normally; location lookup still requires its registered name. Removing
null entries or deduplicating the stored list during fixup would lose the
per-entry assertions or repeated work established above.

The authored-input inventory followed the registered campaign/object include
files through the full leaf-first Remake chain and standalone RaW. It found
67 Remake campaign declarations: five without locations and five containing
ten unresolved location occurrences. RaW's 24 declarations had neither empty
nor unresolved location lists. Neither inventory found duplicate locations.
Three Remake object includes failed the inventory XML parser; a separate text
check found neither unresolved location name in those files. These are scoped
input checks, not proof that all registered objects complete loading or that
any campaign is playable. RaW's starting-force warning must not be relabeled
an undefined-`Locations` case.

The following synthetic cases discriminate the branches; they are deductions
from static reads and proposed capture inputs, not executed game tests. A and B
are valid planetary types; X is undefined; S is a valid ordinary force type
owned by an active player. Other campaign prerequisites are held valid.

| Authored input | Continued debug result / observation needed |
|---|---|
| `Locations = A, X, B` | A and B are created; X retains its slot, diagnoses/asserts and creates nothing. Reverse X's position to distinguish per-entry continuation from an early abort. |
| `Locations = A`, no force record for A | A exists independently of force records. Its later controller is political-control behavior, not proof of an implicit force grant. |
| `Locations = A`, force S at B | No B is created and S is not placed at B; compare with the preceding case. |
| Omitted/empty `Locations`, versus `Locations = X` | Both can reach the no-live-planets assertion after continuation, but only X takes the per-entry fixup/creation/lookup diagnostics. Capture actual admission and the resulting screen/session separately. An empty occurrence after a valid list preserves that list. |
| `Locations = A, A`, one force S at A, home A | If all assertions continue and both creations succeed, two A instances are created and the force record is dispatched twice to the last-created A; the local home lookup selects that instance. Guard-fleet unification can combine containers, so count created units as well as fleets. A shipping capture must identify both instances, placement targets and selected home before deciding Q06. |

**Ours, Q05:** `src/data/galactic/galactic_campaigns.cpp:104` diagnoses an empty
list but retains the definition. `src/campaign/setup.cpp:491` explicitly refuses
an empty authored list; that is stricter than RGC-83's inspected continuation.
For a nonempty list, `src/data/galactic/galactic_world.cpp:384` diagnoses unknown
names and excludes them from the derived planet list, while
`src/campaign/setup.cpp:484` retains unbound location requests and `:503` skips
them during force planning. Preserve these per-entry diagnostics and skips;
RGC-81 does not justify rejecting the whole campaign. If admitting a zero-planet
root, separately verify service preparation at
`src/campaign/live_setup.cpp:1167` and placement-context construction at `:304`.
Their fiscal, observer and subsystem requirements are not retail evidence for
an empty-location refusal, and this static walk does not qualify that root.

**Ours, Q06:** `src/campaign/setup.cpp:807` refuses duplicate planet-name
bindings in the built model. That is not a complete authored-duplicate gate:
`src/data/galactic/galactic_world.cpp:385` diagnoses repeated `Locations` and
omits their repeated derived planet entry, while leaving the authored list
intact. Repeated requests can therefore alias the one derived planet before
later service validation. Do not mistake this loader path for evidence that
shipping deduplicates locations, or assume the model guard covers it. Retain
the explicit unverified policy in [campaign setup](../behaviour/campaign-setup.md);
an implementation follow-up needs an authored-input regression as well as
model-level coverage. This walk changes no runtime behavior.

## Nested starting creation and incomplete variants

Walk B answers the nested-creation and variant-admission questions at main
`c1168fe81e94ea93c29db51a2667e873f2d81ca4`, with the earlier bring-up comparison
still at `99bd381f99d92c41df9ccbc24c346c2197efc516`. These are static debug-build
reads, including callers after a helper returns null. They do not establish an
uninterrupted mod run, release assertion policy or recovery from a null access.
The two kinds of incomplete input below must remain separate.

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-70 | Starting placement sets the matching active owner's target allegiance before delegated creation. A null returned primary does not remove earlier objects, undo that allegiance, refund a purchase or terminate the starting-force loop. Squadron inputs and inputs with a nonempty ship classification assert on that null; human fleet unification requires a nonnull result. Continuing a returning assertion reaches the next starting-force record. | Debug build: starting-placement caller after delegated creation, including its null-result assertion and loop tail; RGC-05/07. |
| RGC-71 | A company creates its transport, then constructs each **immediate** `Company_Units` type as an ordinary actor and attaches it to that transport. It does not recursively delegate company/squadron placement for a member whose type has a dummy composite behavior. Squadron assembly likewise constructs immediate `Squadron_Units` actors. A nonnull nested dummy's missing inner transport therefore does not invoke the missing-transport return at this placement boundary. | Debug build: company and squadron member loops, ordinary actor constructor/initialization and attachment; distinct from delegated placement of the outer type. |
| RGC-72 | An unresolved roster member is not a sourced skip-and-continue case. Per-type roster fixup asserts on a null member and then accesses that member without a null guard. If a null immediate member is supplied to company placement, ordinary construction returns null, attachment still proceeds after assertions, and further member reads are unguarded. These paths contain no rollback or safe sibling-skip branch. | Debug build: roster/member fixup, null-type construction, company member loop and transport attachment. Extends WGA-363's unresolved-member qualification. |
| RGC-73 | A fresh variant first receives ordinary constructor state, its authored fields and ordinary fixups. Failure to find its named base returns false from overlay without copying a base. The file parser ignores that return and still inserts the nonnull derived type into the type list and public name map. A missing base does not itself omit the derived type. | Debug build: type construction, overlay caller, unconditional list/name insertion and lookup. Missing-name diagnostics remain the separate reference-fixup behavior of RGC-22. |
| RGC-74 | Overlay copies/reparses only when the base is loading-complete. An isolated self-base or closed inheritance cycle never seeds a completed base, so its types keep their own initialized/authored state and remain incomplete through the bounded parse passes. They remain registered: the inspected loader does not purge them and finishes with success after its other work, provided execution reaches that return. | Debug build: completed-base guard, reuse of existing registered types on later passes, ten-pass loader and final return; ML-22 and WGA-364. |
| RGC-75 | Name lookup returns the registered type without filtering on loading completion. Starting placement passes that nonnull type to delegated creation; the inspected production-eligibility reader also has no missing-base or loading-completion exclusion. Its ordinary faction, price/time, technology, location, lock, membership and other gates still apply. Registration therefore permits consideration and authored creation behavior, rather than guaranteeing an offer or successful creation. | Debug build: all three name-lookup forms, starting-placement caller and production eligibility. |

### Nested inputs: which reference is actually consumed

Use an otherwise valid outer company with transport T and immediate members A
and B. A is an ordinary valid actor. B is a registered dummy company with a
valid own roster but an unresolved `Company_Transport_Unit`. Inherited fields
count: omission on B is not a missing transport if a completed base supplies it.
Ordinary member-list inheritance/replacement remains WAR-84.

| Synthetic input | Static outcome at the inspected boundary |
|---|---|
| Outer members A, B; B's inner transport is null | Outer transport T and immediate actors A, B are constructed in that order; B's inner company is not expanded. No recursive failure removes T or A. |
| Outer members B, A; same null inner transport | T and immediate actors B, A are constructed in that order. B's inner transport is still not read by delegated company creation. |
| Either member order; B instead has an unresolved inner roster member | Earlier per-type roster fixup reaches an unguarded null access after asserting. Changing the outer order does not supply a safe campaign-start continuation; no final placement result is established for this input. |
| Outer's own transport is null; valid starting-force record follows | No primary transport is created for the outer company. If the caller's assertions return, the next record is processed; this is RGC-70, not sibling rollback. |
| Outer's immediate roster contains A, null, or null, A | There is no guarded skip of the null entry. The first ordering can construct A before the unsafe entry; the second cannot establish that A is reached. Neither proves a stable final partial company. |

These are control-flow deductions with ordinary construction inputs and no
script-driven creation effects, not executable retail captures. An unresolved
inner roster cannot be described as a successfully started partial company just
because the placement loop itself would not recursively visit it. Arbitrary
object scripts/listeners and recovery after invalid memory access are outside
this walk. Additional creation and production refund retain RGC-05..07; starting
placement is not a paid production completion.

**Ours:** `src/campaign/starting_forces.cpp:61` refuses an unresolved member while
preparing every nested binding; `:227` delegates the actual one-level placement.
`src/sim/galactic/production.cpp:552` also refuses empty member lists and unbound
company transports in the shared nested setup. The bring-up branch's recursive
`src/campaign/setup.cpp:883` exclusion and
`src/campaign/live_setup.cpp:574` offer/census filtering are broader than RGC-71:
they can discard a valid outer company because a nested dummy's unconsumed
transport is absent. Do not replace that exclusion with invented recursive
expansion or a universal null-member skip. RGC-72 requires a separately stated
project diagnostic/admission policy for the unsafe roster case.

### Missing bases and cycles: registration is distinct from completion

| Synthetic input | Retained state and downstream distinction |
|---|---|
| Leaf names an absent base and authors its own creation behavior, affiliation and valid production fields | Leaf remains findable with its own parsed/fixed-up state. Starting placement reads its own behavior; production considers it through ordinary eligibility. Nothing is inherited from the absent base. |
| Same absent base, but creation behavior was authored only on that absent base | Leaf still remains registered; it has no such inherited behavior. Delegated placement can take the unknown-type/null branch. Registration does not synthesize the missing behavior. |
| Self-base with an otherwise sufficient own definition | Type remains findable and incomplete. Overlay cannot copy from its own incomplete state; own fields remain available to the same consumers. |
| A derives from B and B derives from A; each authors a different own `Population_Value` | Both remain registered and incomplete. Neither receives the other's population value through overlay. A dependent leaf also cannot copy an incomplete base. |

Missing-base reference fixup can diagnose/assert as in RGC-22. A cycle whose
names all resolve has no missing-name diagnostic for those edges; that does not
make its overlay complete. Other malformed fields can assert independently.
The loader's successful return and public lookup are not evidence of complete
inheritance, successful script setup or playability.

A private audit followed the registered loose XML through the owner-specified
three-layer Remake chain and standalone RaW, using the existing stock reference
as fallback. It found authored missing-base examples in both and a Remake
self-base example. It excludes archive-only overrides and does not emulate
retail XML recovery, so its counts are not a runtime census. Those data examples
motivate the two synthetic cases; warnings alone do not decide their consumers.

**Ours:** main's `src/units/galactic_types.cpp:222` records a failed resolution and
falls back to the leaf definition at `:230`, retaining relevant own fields.
That matches the retained-leaf distinction, without reproducing the complete
parse-pass lifecycle. The bring-up revision instead executes `continue` at
`:230`, dropping the type before table insertion and making a later campaign
reference appear unknown. That exclusion differs from RGC-73..75 for both a
missing base and a cycle; restore the registered partial leaf distinction and
preserve diagnostics, rather than inferring buildability from registration.

## Missing corrupt side and political transition consumption (walk F)

This follow-up answers Q11/Q12 in the [walk queue](remake-walk-queue.md)
against main `c1168fe81e94ea93c29db51a2667e873f2d81ca4`. Evidence is static
debug-build inspection and synthetic arithmetic, not a captured mod campaign.
The constructor and shared modifier aggregator are essential to the political
answer: reading the final division alone gives the wrong zero/negative result.

### Missing side: Q11

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-100 | On fresh construction, `Corrupt_Side_Name` is an empty string. The constants parser skips an empty value; omission likewise performs no assignment. Neither supplies a built-in faction name. If that constants instance already holds a name, a later empty occurrence does not clear it. Loading the constants has no required-field check for this tag. | Debug build: constants constructor, reset, parser, initialization success path and direct string getter. This is stored-value retention, not evidence of merging fields across mounted files. |
| RGC-101 | With the faction table initialized, side lookup compares the stored name case-insensitively with loaded faction names and returns null when none matches, without a missing-name diagnostic or substitution. The subsequent player lookup compares faction identities across actual players; with ordinary faction-bound players, a null faction yields no player. It does not select a neutral or stock corrupt faction as fallback. | Debug build: complete faction-name lookup and faction-to-player lookup; getter-linked caller survey. |
| RGC-102 | Campaign corrupt-plot routing loads the authored plot only for a found active player. Missing, empty or unloaded side names therefore route no corrupt plot and do not refuse the campaign at this branch; good/evil and additional plot routing remain separate, as in WGS-07. Sandbox objective text separately compares the local faction name with the configured side names. Failure to match the corrupt name leaves the ordinary spawn-text selector at index 1 unless the good name matches (index 0); a corrupt match selects index 2. This text default does not change the player's faction. | Debug build: campaign plot caller; sandbox objective name comparisons and friendly/enemy spawn-text selection. |
| RGC-103 | The History chart resolves the three configured sides separately and reads samples only for players found by those lookups. An unresolved corrupt side contributes no series or samples; it does not prevent the other present sides being charted, subject to GREF-01's sample requirement. Galactic AI perception initialization instead enumerates actual players to create their tokens and player evaluators; it does not require or substitute `Corrupt_Side_Name`. A missing chart side therefore does not remove that actual player from this AI initialization. | Debug build: chart construction, complete galactic perception initialization and its base initializer; GREF-01..03. |

For a discriminating synthetic campaign, keep two ordinary faction-bound
players with valid good/evil plots and at least two History samples each.
Omitting the corrupt tag, giving it an empty value on a fresh instance, or
naming an unloaded faction each leaves those two routes/series intact and adds
no corrupt route/series. As a positive control, name a loaded active third
player with a valid plot and samples: its route and series are present, and all
three actual players remain eligible for the inspected AI evaluator setup.
Parse a valid name followed by an empty occurrence separately: the previous
name remains. These are branch deductions, not executed campaign captures.

The checked standalone RaW constants omit this tag; the effective Remake leaf
and both parent files provide it. At the compared source,
`src/campaign/story_plots.cpp:182` already skips an absent, empty or unmatched
side. `src/campaign/live_setup.cpp:1104` likewise adds no matching History
player while still preparing observers from campaign player bindings.
`src/data/galactic/galactic_world.cpp:319` nevertheless emits a missing-required
issue for every absent listed constant, including this one. RGC-100..103 support
consumer-specific absence handling, not treating that issue alone as proof of
a failed campaign. Arbitrary script consumers and other presentation readers
are outside this bounded answer.

### Nonpositive alignment multiplier: Q12

RGC-16..20 still own strength parsing, alignment relations and the raw producer.
The following rules describe its downstream lifetime and consumer. Existing
GFS-07/08 own service order, and WGM-C05 owns the target-player reset; no extra
service or same-frame deadline is inferred here.

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-104 | Fresh default and value-initialized allegiance state enable a minimum transition multiplier of **0.01**. Source insertion retains authored zero/negative values. At consumption, the aggregator selects the smallest value in each category, multiplies category results, then applies that minimum to the aggregate. The empty-source service uses multiplier 1. The cap is neither a per-source clamp nor a clamp on `Force_Strength`. | Debug build: both constructors, cap-enabling setter, source setter, category aggregator and political service. Refines WGM-73. |
| RGC-105 | With a positive base transition time and positive service frames/FPS, a raw zero or negative alignment product is consumed as **0.01**. Duration per control point is base time × effective multiplier; step is service frames / (duration × logical FPS). There is no local nonpositive-source assertion or campaign refusal. For base 7 seconds the duration is **0.07 seconds per point**, rather than zero, a negative duration or an immediate whole transition. | Debug build: constructor-enabled aggregate cap followed by duration/division and ordinary target comparisons. Supersedes the zero-multiplier immediate-completion inference in WGM-197 and GPC-02. An independently nonpositive authored base time is not settled here. |
| RGC-106 | Same-player progression adds the step only when current < target, and subtracts it only when current > target, clipping directional overshoot to the target. At exact equality neither write occurs, although the step is computed beforehand. Generic different-player progression subtracts to zero, sets current to zero and changes current player, without also rising to the requested amount in that call. Ordinary planet target-player changes separately use WGM-C05's immediate current-player/zero reset before this service. | Debug build: complete political progression reader and planet caller. |
| RGC-107 | Alignment-source changes require both a changed target amount/player and a nondegrading update relative to current state. Such an update can install/replace the source or remove it when neither alignment effect applies. An unchanged target or a degrading update leaves the old source installed, including a changed degrading target/player. The source is installed without ordinary timed expiry. Protection may skip degrading progression, but does not itself remove this source. | Debug build: entire target-update branch, source replacement/removal, permanent-lifetime conversion and protection gate; WGM-C06. |
| RGC-108 | Initial placement calculates control without trade routes and directly assigns both current and target amount/player. It does not install an alignment time source as an imagined gain from zero. This alignment producer adds no source until a later changed nondegrading target installs one; other ability sources have their own producers. A subsequent route-inclusive calculation can be that change; an unchanged target is insufficient. | Debug build: placement initializer, fresh constructor, plain current/target setters and later target-change branch; WGM-C04/05. This does not qualify arbitrary owner-change callback producers. |
| RGC-109 | The inspected current/target setters, current getter, allegiance copy and progression reader do not assert or clamp current control to 0..5. Target calculation has WGM-68's separate bounds. For normal in-range targets/current, positive base time and the enabled aggregate minimum, zero/negative alignment sources do not drive control outside the range. An externally supplied out-of-range current is not repaired by a general range clamp in this consumer. | Debug build: direct field setters/getter, copy constructor and complete service/caller; WGM-68. No whole-campaign acceptance claim for arbitrary invalid state. |

For a synthetic like-aligned planet with base time 7 seconds, one service frame,
logical FPS 30, same-player current/target 1/5 and no other transition sources:

| Strength | Maximum bonus | Raw multiplier | Effective multiplier | Duration per point | First service current |
|---|---|---|---|---|---|
| 2 | 0.25 | 0.5 | 0.5 | 3.5 s | About 1.009524. |
| 4 | 0.125 | 0.5 | 0.5 | 3.5 s | About 1.009524. |
| 2 | 0.5 | 0 | 0.01 | 0.07 s | About 1.476190. |
| 4 | 0.25 | 0 | 0.01 | 0.07 s | About 1.476190. |
| 2 | 0.75 | -0.5 | 0.01 | 0.07 s | About 1.476190. |
| 4 | 0.5 | -1 | 0.01 | 0.07 s | About 1.476190. |

The capped cases reach 5 on the ninth such service, not the first. A private
single-precision arithmetic check also covers decreasing 4 toward 1, generic
different-player decay, equality and a retained negative source. Two distinct
categories with multipliers -1 and -2 produce **2**, not 0.01: aggregate before
capping. Both signs of zero instead cap to 0.01. These are arithmetic deductions,
not retail runtime measurements or a service-cadence assertion.

To distinguish lifetime and startup, first place current = target = 3 and
verify no alignment source was synthesized. Change to a nondegrading target 5
to install it, then change to a lower target: the source remains and its capped
effective rate is still used unless protection skips that service. An unchanged
target does not recompute a changed alignment relation. A later changed
nondegrading target with neither effect removes the source.

**Ours:** `src/campaign/live_setup.cpp:354` refuses nonzero authored strength
before connecting its transition producer. Independently,
`src/sim/galactic/control.cpp:236` rejects nonpositive explicit multipliers;
`:397` forms the category product without the retail aggregate minimum, and
`:402` treats zero as immediate movement. Connecting only the producer or
clamping its individual sources would still differ from RGC-104/105. Preserve
raw source values and their lifetime, apply the minimum after category
aggregation, and retain the equality/startup distinctions. This research
changes no runtime code.

## Negative additional population capacity

This follow-up compares main `a17cc5f9a9e8ee47350db5069e253adc8c05bf0b`
with static debug-build reads of the XML mapping, capacity query and production
admission. It extends WGEP-48/50/69 to negative capacity contributions; it is
not a captured RaW campaign or a claim that every other campaign gate passes.

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-140 | `Additional_Population_Capacity` uses decimal integer conversion into a signed 32-bit field. A representable negative integer is stored unchanged and returned unchanged by the getter: no float conversion, unsigned reinterpretation, absolute value or zero clamp occurs on this path. Omitted/inherited values retain WGEP-69's rules. | Debug build: tag-table entry, shared mapping dispatch, integer assignment and direct getter; getter instruction width independently checked. This does not define overflow or malformed-text acceptance. |
| RGC-141 | In GC for a human or capped AI, capacity starts at faction `Base_Population_Capacity`, then adds each eligible owned object's current type's `Additional_Population_Capacity`. An eligible owned planet also adds its current starbase type's contribution when that type exists. Dead or delete-pending objects do not contribute; capped AI skips sources enclosed by an AI-unusable planet. Negative contributions participate in the same sum as positive ones. | Debug build: complete capacity query, both contribution sites and AI enclosing-planet filter; WGEP-48/62. |
| RGC-142 | That sum is one player-wide capacity, with no per-planet capacity floor and no final floor at zero or at faction base capacity. A starbase contributes its current type's authored value, not the sum of its previous levels or a fixed positive allowance. An ordinary negative total is returned as negative without a local assertion or campaign-refusal branch. The allied-object extension is gated to standalone multiplayer tactical, not GC. | Debug build: capacity accumulator initialization, owned-planet current-starbase lookup, complete return path and tactical-only allied scan. Arithmetic overflow is outside this rule. |
| RGC-143 | Population admission compares the requested type's GC `Population_Value` with `max(capacity - usage_including_production, 0)`. A capacity below current usage, including a negative capacity with nonnegative usage, therefore leaves zero purchase room: positive-population types fail this gate and zero-population types pass it. Nonhuman players with `AI_Uses_Galactic_Pop_Cap` disabled bypass this gate; their reported capacity and usage are zero as in WGEP-69. | Debug build: complete admission and usage queries; ordinary production-start caller passes the include-production flag. Other affordability, facility and eligibility gates remain independent. |
| RGC-144 | Ordinary production start checks that population gate before constructing/appending the queue entry or debiting prepaid credits. A failed population check returns false without those operations; it does not make negative capacity a campaign-load error. The inspected capacity/admission queries neither delete existing units nor cancel existing queue entries. | Debug build: production-start control flow and complete capacity/admission readers. This is an enqueue boundary, not a claim about every later queue-service or script consumer. |

The checked RaW XML authors -2/-5/-7/-10/-15 on its five Empire starbase
levels and -1/-3/-5/-7/-10 on its five Rebel levels. These signed inputs are
meaningful capacity reductions under RGC-140..142, not invalid solely because
stock starbases use positive values. Other owned objects and faction base
capacity can offset them; the negative tag alone does not establish a negative
campaign total.

For synthetic arithmetic, let faction base capacity be 10 and the other
eligible owned contributions total 3. All rows assume a human, ordinary
nonnegative usage including production, and otherwise valid purchase inputs:

| Current starbase contribution | Player capacity | Usage including production | Purchase room | Population 0 / 1 / 3 |
|---|---|---|---|---|
| -5 | 8 | 5 | 3 | Pass / pass / pass. |
| -5 | 8 | 10 | 0 | Pass / fail / fail. |
| -15 | -2 | 0 | 0 | Pass / fail / fail. |
| -15 | -2 | 5 | 0 | Pass / fail / fail. |

Splitting the same eligible contributions across two owned planets preserves
that player total. Replacing a current -5 station by a current -15 station
reduces capacity by 10, rather than adding both levels. These are deductions
from the inspected branches, not runtime retail measurements.

**Ours at the compared main revision:** `src/units/galactic_types.cpp:87` and
`:250` already retain a whole signed additional-capacity input. Campaign
`src/campaign/setup.cpp:177` nevertheless refuses negative contributions in
`prepare_census_type`; `src/sim/galactic/production.cpp:722` repeats that
restriction in census validation. Separately, `:582` refuses a negative
production-player capacity. Removing only the first rejection therefore does
not cover RGC-140..143's signed state end to end.

`src/sim/galactic/production.cpp:820` computes the census, adds object
contributions at `:846`, initializes faction capacity at `:853`, and adds
current-station capacity at `:890`/`:899`. There is no final total clamp there.
The population gates at `:422` and `:811` already floor remaining room at zero
and preserve the AI bypass. A future implementation should preserve signed
contributions and signed aggregate capacity through validation while retaining
that admission floor; it should not clamp individual sources or the total to
zero. This research changes no runtime code.

## Alignment transition category and multiplication order

This follow-up refines RGC-19/104 and WGM-73/197. Evidence is static debug-build
inspection and synthetic single-precision arithmetic, not a captured campaign
with multiple transition sources. RGC-107/108 still govern whether the alignment
source is installed or retained; these rules apply to sources actually present.
No new service-order or cadence claim is made.

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-130 | Both the like-alignment bonus and opposing-alignment penalty install their raw transition multiplier in category **−1** of the planet's ordinary transition-modifier list. The no-effect removal uses that same category and planet source identity. Alignment is not a separate factor outside the category reducer. | Debug build: complete planet target-update branch, both source setters and source removal; RGC-19/107. |
| RGC-131 | `Political_Transition_Bonus_Ability` forwards its authored signed integer `Stacking_Category` to that same list. Its default is 0; the inspected category parsing, validation and insertion do not reserve −1 or exclude negative categories. An admitted ability with category −1 therefore shares alignment's category: the **smallest raw value across those sources** wins, rather than multiplying alignment by the ability. Distinct source identities remain distinct entries. | Debug build: ability constructor, XML mapping and signed parser, validation, application, source insertion and shared category reducer. Refines WGM-197's statement that alignment has its own category: its fixed category is not exclusive. |
| RGC-132 | Nonempty categories are visited in **ascending signed integer order**, independent of category insertion order. Each category contributes its smallest source value. The first nonempty category seeds the aggregate; each subsequent category multiplies the running aggregate by its selected value. Empty category vectors are skipped. The service uses 1 when no sources remain. The enabled 0.01 minimum applies after the complete category product, as in RGC-104. | Debug build: signed category comparator, ordered traversal, category comparison, first-value branch, multiply operation, aggregate cap and empty-list service branch. |
| RGC-133 | The debug build carries category values and the running product as single-precision floats, with a float multiplication result at each reduction step. Reordering three or more category factors can change the rounded result even when real-number multiplication is equal. With categories −3 = 0.1, −2 = 0.2 and alignment category −1 = 0.9, ascending reduction gives approximately **0.01800000109**; multiplying alignment first gives approximately **0.01799999923**, one single-precision step lower. Both survive the 0.01 floor. | Debug build: float-valued reducer and multiplication helper; synthetic binary32 evaluation of the two orders. This is an arithmetic deduction, not a measured control-slope difference. |
| RGC-134 | When no other source uses −1 and alignment belongs first among the present categories, multiplying alignment separately first follows the same category grouping/order. That arrangement does not generalize: shared −1 sources require one category minimum, and categories below −1 precede alignment. The RGC-133 case proves an observable arithmetic distinction between these reduction orders. | Debug build: RGC-130..133; source comparison and synthetic same-category/order cases. This does not establish bitwise equivalence between remake fixed-point and debug-build float arithmetic. |

With an already installed alignment source and otherwise admitted ability
sources, the following synthetic inputs distinguish the category grouping:

| Present sources | Retail aggregate before the floor | Separate alignment product |
|---|---|---|
| Alignment 0.5; ability 0.25 in category −1; ability 0.8 in category 0 | `min(0.5, 0.25) × 0.8` = about 0.2 | `0.5 × 0.25 × 0.8` = about 0.1 |
| Alignment penalty 1.5; ability 0.75 in category −1 | 0.75 | 1.125 |
| Alignment 0.5; ability 0.25 in category 0 | 0.125 | 0.125 |

Reverse the insertion order of the three RGC-133 sources: the signed category
order and retail result stay the same. One category below −1 alone merely
exchanges the first two factors when alignment is moved first; the rounding
witness deliberately uses **two** categories below −1. These cases qualify the
aggregator, not ability admission, alignment-source lifetime or whole-campaign
acceptance for arbitrary authored data.

**Ours:** the CGS-16 implementation on the bring-up branch, compared at
`ba89a9e3988b3fd082493cd3997bd313e5ead650`, merges explicit and automatic sources
into the signed category map in `src/sim/galactic/control.cpp`, but seeds a
separate accumulator with retained alignment before reducing that map. It
therefore misses the shared −1 minimum and moves alignment ahead of lower
categories. The usual category-0 ability case has matching category grouping
and order, which does not qualify arbitrary mod categories. Insert retained
alignment into category −1 before choosing category minima, then reduce in
ascending signed order and keep the aggregate floor afterward. Preserving the
source's separate lifetime state does not require a separate multiplication.
This research changes no runtime code.

## Undefined company and squadron roster members

This follow-up separates roster name resolution from member marking. It
refines RGC-72 and WGA-363/364; RGC-22/24's safe null handling in selected
starting-spawn consumers does not establish safe handling for `Company_Units`
or `Squadron_Units`. Evidence is static debug-build inspection, not a captured
successful startup with a missing roster member.

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-150 | `Company_Units` and `Squadron_Units` bind indexed lists of name references. An undefined member name stores a null resolved type and produces the missing-name log, popup and assertion of RGC-22. That operation neither substitutes another type nor removes the slot. Roster counts and indexed access retain the entry. | Debug build: both XML mapping rows, company/squadron count and indexed getters, shared roster indexing and individual name fixup. |
| RGC-151 | Roster name fixup visits every retained reference. When a missing-name assertion returns, the loop advances to the next slot; it does not return failure, truncate the roster or abort the whole reference-fixup traversal. This continuation applies to **name resolution**, before per-type member marking. | Debug build: complete roster reference-fixup loop, individual fixup return and catalog reference-fixup traversal; WGA-364. |
| RGC-152 | Per-type member marking has no safe null-member skip. For each company member it asserts on a null type and then unconditionally writes that type's ground-company member flag and reads its properties; the squadron loop does the same for the squadron member flag. If the assertion returns, the next operation still accesses the null type. Inferred company/squadron cost aggregation can pass the null to a member-cost reader even earlier. This is an unsafe path, not a controlled pass-abort or a successful mark-the-valid-siblings pass. | Debug build: complete per-type fixup, both member loops and the preceding conditional cost aggregation; independently rechecked against RGC-72. |
| RGC-153 | Missing roster resolution does not itself unregister the owning company/squadron or remove its authored roster. That retained identity is not evidence that the owner remains safely creatable: initial loading invokes the unsafe RGC-152 consumer before normal starting placement, and the inspected immediate-member creation loops also lack a safe null-sibling skip. No successful creation of the malformed composite, or recovery of later siblings, follows from these paths. | Debug build: reference fixup, catalog load caller and retained registry, company placement and squadron assembly; RGC-71/72/75. No malformed-roster runtime capture was made. |
| RGC-154 | The inspected member-marking pass only sets member flags on referenced types; it does not clear all types' flags or install a catalog-wide unavailable state when a member is missing. Marks already written are not rolled back. After an unresolved member reaches the unsafe access, completion and later marks are not established. Preserving the other flags and safely continuing would require an explicit recovery policy beyond this debug-build path. | Debug build: member writes and null branches, reference fixup and catalog load caller; WGA-363/364. This distinguishes partial prior state from a completed valid timeline. |

The following static cases distinguish the two stages. They are control-flow
deductions, not observed retail startup outcomes:

| Roster | Name fixup if assertions return | Per-type member marking if reached |
|---|---|---|
| Valid A, undefined B, valid C | Resolves A and C; retains B's null slot and count. | Marks A, then takes the unsafe B path; reaching C is not established. Earlier inferred-cost processing also lacks a null skip. |
| Undefined B, valid A | Retains B's null slot and resolves A. | Takes the unsafe B path; A is not guaranteed to be marked. |
| All valid members | Retains all resolved entries. | Sets each member's corresponding flag without clearing earlier marks, subject to ordinary variant copy-time history. |

**Ours:** compared with `origin/main` at
`cc15f246db7c2e511a2942fe3ca69b91200bc024`,
`src/data/xml_merge.cpp:114` detects an unresolved member,
`:115` records one catalog-wide timeline issue and `:117` returns from
derivation. It leaves prior internal marks in the vector, rather than clearing
them, but `Catalog::member_flags` at `src/data/xml_merge.cpp:18`–`:20` refuses
every otherwise-known type while that issue is present. Consequently,
`src/units/galactic_types.cpp:213`–`:219` leaves both member optionals unset for
each type and records an issue; `src/campaign/ai_selection_content.cpp:41`–`:47`
then refuses freestore member qualification when those values are unavailable.
The controlled global refusal differs from RGC-151's name-fixup continuation
and RGC-152's unsafe member access. It does **not** prove that retail safely
skips a missing member or successfully starts the same malformed data.

For a mod roster whose member appears undefined, first qualify the actual
mounted object namespace. If it is genuinely unresolved, a tolerant policy
that diagnoses and skips that member while completing other flags is a remake
recovery decision, not a sourced retail rule. Retaining an unknown slot for
creation remains a separate consumer decision. This research changes no
runtime code.

**EAWR recovery policy (owner decision 2026-10-22; CGS-14).** Retail behaviour is undefined here, and this is not a retail claim. Member marking skips the unresolved slot, completes the other slots and reports a note naming owner type, roster, slot index and name. The catalog-wide member-flag refusal is removed for this case; a duplicate identity or an incomplete variant chain still refuses it.

## Failed plan scripts and plan admission (walk A, Q01)

Static debug-build inspection of how galactic and tactical plan scripts are read, how a script that does not load
or does not describe a usable plan is treated, and what the surrounding startup does with a failed plan stage.
No mod campaign was launched; assertions described here let the inspected branch continue. No release-binary
assertion policy is inferred. Source comparisons are pinned to main
`60b5b83be3fbc285dc562ec99d10dd22525ad2ef`; the AI discovery rules cited as RGC-40..50 are the follow-up note on the
discovery walk and are referenced by ID. This note changes no runtime code.

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-160 | The embedded Lua 5.0 table-constructor grammar is: before each field, one optional `;` is consumed; then `}` ends the constructor; after a field, one `,` or one `;` continues the loop and anything else ends it. Consequences, each a separate case: a leading `,` is rejected; a doubled `,,` is rejected; `;,` is rejected; `,;` and `;;` are accepted; a single trailing `,` or `;` before `}` is accepted; `{ ; }` is accepted and `{ , }` is rejected. A `,` at the start of a new line after another `,` (the empty-field shape in the bring-up note) is the doubled-comma case and is rejected with the ordinary unexpected-symbol syntax error. | Debug build: the constructor parser and its record-field reader; the token-by-token loop matches the vendored 5.0.2 parser. |
| RGC-161 | A plan script that fails to load (syntax error or load failure) produces a script-creation assertion and no script object; the plan definition manager asserts, reports the file as unreadable and returns failure for that file only. A failed file registers no plan definition. | Debug build: script creation (load-from-file branch), plan script reader. |
| RGC-162 | After a script loads, the plan reader reads the globals Category, TaskForce, IgnoreTarget, MagicPlan, MagicPlanStealing, AdditionalCost, MinimumCost, RequiredCategories, AllowFreeStoreUnits, AllowEngagedUnits and PerFailureContrastAdjust through type-checked casts. Admission gates, in order: (1) no TaskForce table: error, file refused; (2) a plan of the same name already registered on a fresh load: error, **earlier definition kept**, file refused; (3) every override whose cast is null (absent **or wrong type**) is simply not applied: it is not an error and does not refuse the plan; (4) Category is split on `\|`, each part trimmed; zero goal types: error, a plan created by this read is discarded, file refused; (5) each TaskForce entry that is not a table: assertion and error, **that entry skipped**, the others still read; (6) zero surviving taskforces: error, a plan created by this read is discarded, file refused; (7) a RequiredCategories entry whose name the category converter does not recognise: error and that entry skipped; a non-string entry is skipped silently; the plan is still admitted. | Debug build: plan reader body, taskforce-list loop, required-category loop. A wrong-typed Boolean or numeric override being a silent skip rests on the null-cast branch; whether the cast itself returns null for a mismatched type is the helper's contract and is not separately traced. |
| RGC-163 | Plan files are enumerated as `*.lua` in a fixed directory order: `AI/`, `AI/SpaceMode/`, `AI/LandMode/`, `AI/StoryArc/`, then `Interventions/` under the scripts folder. Each directory is read non-recursively, `.` and `..` are skipped, and the plan name is the file name without its extension. One registry serves every mode: the plan's Category goal types, not its directory, decide which mode can use it. A failing file never stops the enumeration. The enumerator's return value is the result of the **last attempted file** (a failed middle file does not make it false); it is also false when no directory had any matching file. Files unchanged during a refresh are skipped. | Debug build: plan manager initialisation loop and its directory-search wrapper. |
| RGC-164 | A false plan-stage result is not a startup refusal. The static AI initialiser logs the failure, marks its aggregate false and **still runs every later stage** (templates, player types, static evaluators). Startup then shows an error popup for the AI data manager only; it neither shuts the systems down nor returns failure, and the next system (game scoring) initialises. Other failing systems in the same function do shut down and fail. At campaign start with a script reset, the player-type, template and plan managers are shut down and re-initialised from disk in the order plans, templates, player types, with their results discarded. The refresh path logs each stage's failure and continues. | Debug build: static AI initialiser, system initialisation function, campaign start, XML reload. This is startup control flow, not a frame-service schedule. |
| RGC-165 | The static AI stage order observed at the data-manager level is evaluator scripts, hint tables, perception functions (equations), goal types, goal-proposal function sets, plan definitions, templates, player types, static evaluators. This refines the stage list in RGC-43 with its plan position and confirms that a failed stage records failure and the next stage runs. | Debug build: static AI initialiser; RGC-43. |

What this does **not** establish: the gameplay consequence of a galactic AI player with zero registered plans
(only the loader and startup are traced); the behaviour of a missing Category global (the reader was not seen to
test it before use); whether the taskforce definition constructor can itself fail for an authored entry; whether
adding a goal-type name filters unrecognised names (RGC-162 counts the names the reader adds); and the order of
files inside one directory or archive (RGC-41 gives enumeration order, not a sort).

### Q01 answers and our code

| Question | Answer | Ours (main 60b5b83be) | Match | Synthetic case |
|---|---|---|---|---|
| Leading, doubled and trailing separators, separately | RGC-160 | The embedded parser is the vendored Lua 5.0.2 (`third_party/lua/src/lparser.c:482`–`:523`), identical loop; no reimplementation. The existing receipt only exercised the doubled-comma shape. | Same by construction | One TaskForce table per row of RGC-160 (leading `,`, `,,`, `;,`, `,;`, `;;`, trailing `,`, `,;` before `}`, `{;}`, `{,}`): pin load/refuse per row, with the syntax error text for the refused ones. |
| One plan script fails | RGC-161, 163: that file only, the rest continue | `src/script/foc/galactic_ai_plans.cpp:205`–`:208` skips and lists it | Same | Three plan files: syntax error, valid, valid. Two plans registered; the first listed; names and order stable. |
| Failure classes after load (no TaskForce, empty Category, zero taskforces) | RGC-162 gates 1, 4, 6: file refused | `:68` and `:85` are marked unloadable and skipped | Same | One file per gate; each skipped and listed, siblings unaffected. |
| Failure classes after load (wrong-typed override, non-table or malformed TaskForce entry, bad required category) | RGC-162 gates 3, 5, 7: ignore or skip the item, keep the plan | `:70`–`:77` (override types), `:88`, `:90`, `:94`, `:106` return the non-skippable error, so `:219` **refuses the whole catalog** | **Differs** | A plan with a wrong-typed Boolean override, a non-table taskforce entry beside a valid one, and an unrecognised required category: plan admitted, the three items reported once each. Whether a *table* entry with no name (`:88`/`:90`) matches needs the taskforce-definition constructor walk (not traced). |
| Every plan script fails / no plan file exists | RGC-163, 164: loader returns false, startup continues with an empty plan registry and a popup-level diagnostic | `:132` refuses with no top-level modules; `:235` refuses when none load | **Differs** (stricter) | Catalog of only broken plans: decide whether the remake admits an empty catalog (no AI plans, loud diagnostic) or keeps a controlled refusal. |
| Which plan files | RGC-163: five directories, one registry | Only top-level `data/scripts/ai/*.lua` (`src/campaign/galactic_ai_runtime.cpp:939`, `galactic_ai_plans.cpp:37`) | **Differs** for galactic plans placed in the sub-directories | A galactic plan authored under the space or land sub-directory: registered in retail, ignored here. |
| Duplicate plan name | RGC-162 gate 2: earlier kept, file refused | Names come from file stems of unique top-level paths, so no duplicate arises | Not reachable until the sub-directories are read | Same stem in two directories: first in RGC-163 order wins; second reported. |

Decision for walk A, Q01: the failed-plan policy in the bring-up note is correct for load and shape failures and
wrong only in three places: (a) wrong-typed overrides and malformed taskforce or required-category entries should
be item-level skips, not catalog refusal; (b) an empty surviving catalog is not refused by the debug build's
loader or startup; (c) plan files from the other four directories are registered. For (b) the remake may keep a
controlled refusal until the empty-catalog gameplay consequence above is traced. The change is a follow-up
implementation task; the `:132` and `:235` refusals are the sites.

## AI file discovery: order and failure gaps (walk A, Q02)

The discovery, registration order, duplicate-winner and per-file failure questions are answered by RGC-40..50
(the AI discovery note). This walk adds only what was left open, using RGC-164/165 above.

| Gap in Q02 | Settled by | Ours |
|---|---|---|
| Does one malformed AI file stop the later files or the launch? | RGC-47: no all-files transaction, later files and stages continue. RGC-164 closes the qualification at the outer level: a failed AI data stage gives a popup and a continued startup, not a refusal. | Closure refusal on the first failing file (`src/script/foc/ai_data.cpp:279`, `:308`; `src/script/foc/galactic_ai_loading.cpp:125`–`:126`), and the discarded-whole-file trial loop for extras (`galactic_ai_loading.cpp:107`, `src/campaign/galactic_ai_runtime.cpp:922`). Differs. Per-entry registration (RGC-47) is the target; per-file skip with a diagnostic is an acceptable interim, listed as a deviation. |
| Which files and in what order (galactic and space) | RGC-40..43: five directories, one shared registry, same stages for every mode. The campaign-start reset in RGC-164 re-reads only plans, templates and player types. A battle-start re-read was not traced. | Space and land discovery enumerate all five directories (`src/skirmish/ai.cpp:278`); galactic uses the stock seed first (`src/campaign/galactic_ai_runtime.cpp:898`–`:920`). Differs for galactic (RGC-40). |
| Duplicate-definition winner across files | RGC-45 (first registered wins, read reported failed). The file order inside a directory or archive is the enumerator's (RGC-41), not a sort, and the archive index order was not walked. | VFS sorted by canonical path (`src/vfs/vfs.cpp:527`, `:771`). Deterministic and acceptable as a deviation, provided the duplicate is diagnosed and the first sorted file wins. |

Narrowed questions left open (not blocking an implementation task): index order of entries inside an archive
(decides duplicate winners between two archive files); the empty-registry gameplay consequence; a missing
Category global; the taskforce definition constructor's failure branches; and battle-start AI re-reads for space
and land.

## Faction story pairs and Web Way starting forces (walk E)

This follow-up answers Q09/Q10 in the [walk queue](remake-walk-queue.md)
against main `60b5b83be`. Evidence is static debug-build inspection plus the
mounted Remake data read in place; no mod campaign was run. WST-86..89 already
own the `Story_Name` token stream and the loaded-plot bookkeeping, and WST-01..03
own plot-file parsing: this section adds the caller that consumes the pairs, the
player lookup it relies on, and the creation call the Remake starting-forces
script uses. Binary symbols and addresses stay in private evidence.

### Faction/plot pairs: Q09

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-170 | Pair *i* is read from slots *2i* and *2i+1* of the stored token stream. The accessor returns "no pair" when *2i* is out of range or when either slot is empty; the pair loop treats "no pair" as the end of the list. It checks only slot *2i* against the size before reading slot *2i+1*, so an odd final token reads past the end (WST-86). Nothing is skipped over: the first non-pair index ends the list. | Debug build: the two pair accessors and the campaign-start loop. Token splitting is WST-86. |
| RGC-171 | Campaign start clears the story's plot list, then tries the three named routes in order: good side, evil side, corrupt side. Each route needs a non-null plot name; it resolves the side constant by case-insensitive faction-name lookup, then the player of that faction, and loads the plot only for a found **active** player. A route that fails any step is skipped silently and does not stop the next route or the pairs. An empty plot name is a successful no-op. Pairs are tried only after all three routes. | Debug build: complete campaign-start function and plot loader; consistent with WGS-07 and RGC-102. |
| RGC-172 | For each pair, the faction name is resolved by the same case-insensitive lookup over **every** loaded faction (major, minor, pirates, neutral, hostile alike), then the first player of that faction. A pair loads its plot for exactly that one player. A name that matches no loaded faction, or a faction whose player is missing or inactive, prints a diagnostic and **ends the pair loop**: neither that pair nor any later pair is attempted, and earlier loads stand. The plot loader's result is ignored, so a missing plot file (diagnostic, nothing recorded) does not stop the next pair. An unloaded faction name is therefore not a skipped pair and not a campaign refusal; it truncates the list. | Debug build: campaign-start pair loop, faction lookup, faction-to-player lookup and plot-loader return path. Refines WST-88. |
| RGC-173 | A fresh player is active. Default player creation adds exactly one player per loaded faction, not per listed campaign location or per playable faction, and sets none inactive. The only code that deactivates players before pair loading is the selected-faction branch that deactivates **playable** factions other than two kept ones, and all eight inspected campaign-start callers pass no selection, so it is not taken. In an ordinary start every defined faction, including a minor faction whose data marks it playable, has an active player and its pair loads. Player deactivation elsewhere is quitting a game. | Debug build: player constructor, default-player creation, deactivation callers, campaign-start callers. Lobby-required players are added first but the remaining factions still receive one player each. |
| RGC-174 | Each loaded pair plot is recorded as (player, plot name) at load time and every sub-plot is owned by that one player; repeating a faction or a plot name repeats the load, and recorded plots are not merged. A held `Lua_Script` is trimmed and attached to the next sub-plot entry only, then cleared even when that entry is empty (WST-02). A listed sub-plot file that cannot be opened logs, asserts and is skipped. Pair loading happens inside campaign start, before starting objects are assigned, but no story service runs inside it (see RGC-175). | Debug build: plot loader; complete campaign-start body. This confirms WST-02/WST-89 against the caller. |

Synthetic cases, all branch deductions rather than captured campaigns:

| Pairs (faction, plot), all plot files present | Expected loaded plots, in order |
|---|---|
| A, B, C where all three are defined factions | A, B, C each for its own faction's player |
| A, X, C where X is not a loaded faction | A only; X ends the loop, C is never tried |
| A, B, C where B's plot file is missing | A, C (B logs, loader result ignored) |
| A, B, A, again A's plot | A, B, A, A: duplicates load again |
| A, plot only, odd final token | A, then an out-of-range read for the final faction |
| Case-different faction spelling | Resolves; the lookup ignores case |

For the checked Remake Web Way entries, the pair list names only factions that
are defined in the mod's faction files, and none is deactivated before loading,
so all eight minor-faction plots resolve under RGC-172/173. The casual
variants carry no pairs. This is a data-versus-rule check, not a campaign run.

**Ours:** `src/campaign/story_plots.cpp:194`–`:206` handles the named routes
(side constant required, only a found active binding loads, a failed route does
not stop the next) and `:208`–`:219` the pairs: a missing active binding records
the WST-88 note and `break`s, `same()` at `:22` is case-insensitive, and a failed
plot-file open is a nonfatal note. That matches RGC-171/172 given a binding for
every loaded faction. The parity risk is the roster, not the loop:
`plan.player_bindings()` comes from the campaign's faction roster (lobby rows
must equal `campaign.factions` row for row), so a faction missing from that
roster ends the pair loop exactly like an undefined one, silently dropping every
later minor plot. Verify, with a Remake campaign, that the roster includes all
the loaded factions and not only playable ones. The odd-final-token read is a
validated issue in ours (WST-86), not an out-of-range read.

### Web Way starting forces: Q10

`Spawn_Starting_Forces_Web_Way` is **not an engine binding**. It is a mod story
Lua script. The plot's `Lua_Script` attaches it to a sub-plot, and its
`Universal_Story_Start` handler runs when the plot's zero-second elapsed event
first fires. Everything it creates goes through the generic galactic
`Spawn_Unit` binding.

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-175 | Campaign start finishes starting-object assignment and linking before any story service: its body contains no story-mode service, and the application loop services the active story afterwards (WST-11). The script's handler therefore runs against a fully populated authored start. It neither precedes nor replaces authored `Starting_Forces`: those objects exist first, and the script does not move, remove, filter or reassign any of them. Its work is purely additive. For the checked Remake entries (15 campaigns), the script is attached exactly once per campaign, from the single human-side named route; the AI-side routes do not attach it. | Debug build: campaign-start order; plot-file reader. Mounted Remake plot and campaign files for attachment counts. Ordering inside one service is the frame-service programme's question and is not decided here. |
| RGC-176 | Galactic `Spawn_Unit(type, planet, player)` needs three arguments. Fewer than three, a non-player third argument, a non-type first argument, or a second argument that is not a live object with production behavior each raise a script error naming the argument, and create nothing. No requirement relates the player to the planet: the new object is owned by the **given player** (a script passing the planet's owner is the script's choice), is placed at the **given planet**, and no planet-ownership, visibility, trade-route, or "already has forces" test applies. Routes are irrelevant to creation. | Debug build: complete galactic spawn binding and its argument checks. Whether a script error also stops the calling Lua thread is the Lua error policy, not established here. |
| RGC-177 | Creation dispatches on the type's dummy behavior. A single-ship starship type creates a new **galactic fleet** owned by the player at the planet's position, with the next automatic fleet name, linked into the planet's first orbit slot, then one ship added to it. A squadron type assembles a fleet through the squadron assembler. A ground-company type uses the company creator (RGC-71/72/75). Special ground and orbital structure types are added to the planet's special-structure lists, with the planet's capacity checked by popup and assertion. A starbase type sets or raises the base and requires a level above the current one (assertion). A ground-base dummy type asserts and creates nothing; any other type asserts as unknown and creates nothing. When an object is created the story is notified of the construction, and a type's additional-creation type is then created at the same planet **regardless of whether the first creation returned an object**. | Debug build: creation dispatcher and the single-ship and squadron creators. |
| RGC-178 | A land-based type (ground-base dummy or special ground structure) on a planet that is not surface-accessible raises an error popup and returns nothing, before any object is created. The company, squadron, starship and orbital types are **not** guarded: a ground company is attempted on a surface-inaccessible planet. A null creation yields a script warning and no table. On success the script receives a one-element table holding the new object; for a fleet creation that object is the fleet's **first ship**, not the fleet. If the player is AI-controlled, the object is also handed to that AI's free store for the active mode. | Debug build: galactic spawn return paths, creation guard and free-store hand-off. Surface accessibility is the planet type's own accessibility test. |
| RGC-179 | Web Way's own branch logic is script data: it loops all planets, reads each owner's faction name, rolls a power budget with the free random generator, and spawns space units for every listed faction and, for the pirate faction only, ground units and buildings on planets the script judges surface-accessible. A nil type in the script's tables is skipped by the script's own insert helper before it reaches `Spawn_Unit`. A planet owned by a faction the script does not list (neutral, hostile or any unlisted name) receives nothing; human and neutral owners skip the script's perception queries, so only the base power budget and no surface-based extras apply to them. A fleet lacking a route is not a retail rule gap: creation never consulted routes. The Underworld authored start-force inventory (644 rows across twelve owners in the standard entry, 56 of them Underworld) is recorded privately; the script adds space forces on top of it. | Debug build: spawn binding and creator; mounted Remake script and campaign data read in place. The random generator's multiplayer synchronisation and the faction-name text casing are Lua-API questions outside this walk. |

For a discriminating synthetic start, take three planets: one owned by a listed
faction with surface, one owned by a listed faction without surface, and one
neutral-owned. With one authored fleet on each, after the story's first service:
the authored fleets remain exactly where they were, the listed-faction planets
each gain script fleets owned by their owners (the pirate planet with surface also
gains ground units), the neutral planet gains nothing, and a land structure
requested on the surface-less planet produces an error popup and no object. A
script that spawned a nil type would raise the type-argument script error and
create nothing. A script passing a different player than the planet owner would
create a fleet of that player in orbit without relocating any other fleet.

**Ours:** the stock-hero path at `src/campaign/story_rewards_live.cpp:182`
(`spawn_one`) serves the XML `SPAWN_HERO`/`UNIQUE_UNIT` reward through
`place_due_hero` and a force builder; it is not retail's Lua `Spawn_Unit`, whose
creator takes the **given** owner and recreates fleet/company/structure behavior
by dummy type (RGC-177). The galactic `Spawn_Unit` name appears in
`src/script/authoritative/galactic_api_inventory.hpp:96` but is not bound, and
`src/campaign/story_plots.cpp:110`–`:119` stores the sub-plot's script **name**
without ever running it. So the Web Way script does nothing in ours today: no
authored force is touched and no script force is added. That differs from
RGC-175/176/179 only by the missing additive forces. Implementing the binding
must create at the given planet for the given owner, return one-element tables
(first ship for fleets), apply the surface guard of RGC-178 only to land-based
types, and leave all authored objects in place. `src/campaign/setup.cpp` placement
(the former `:1245` reference now sits in the stock placement context) needs no
change: start-force placement already precedes any story service. This research
changes no runtime code.

## Duplicate game-object definitions (same file and across files)

This read-only walk answers what the original does when two game-object
definitions carry the same `Name`, in one registry file or in different ones,
and which of the two the member-marking pass and the type table see. It follows
the earlier AI-selection note (legacy EAWR-3741) and the Remake top layer
(workshop id 3229239424) that defines `Space_LAAT_Squadron_R` twice in
`Units_Space_Rebel_LAAT.xml`, the second time as `Variant_Of_Existing_Type`
of its own name. Evidence is static debug-build inspection of the type-registry
load, not a captured startup of that mod. As above, an assertion or popup
"returns" only on a path where the debug handler lets execution continue; no
release behaviour is inferred. Binary symbols and decompiler output stay in
ignored research storage (`out/research/gc-remake-walk-dup-objects`).

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-180 | Game-object loading is one loop over the registry's file list in order, and one loop over the file's top-level nodes in document order. Every node with a `Name` creates a **new** type entry appended to the indexed type list; its type index is the list size at that moment. A same-file duplicate takes exactly the code path of a later-file duplicate: nothing in the loop compares the file of the earlier entry. The entry is parsed and created in full, so it exists for every consumer that walks the list. | Debug build: the per-file loader, the node loop and the creation call. |
| RGC-181 | A type's id is the CRC of its **uppercased** name, so names collide case-insensitively. The name map is keyed by that CRC and is written right after each node is appended. Before inserting, the loader looks the name up; a hit raises the duplicate-registration assertion, and the map insert is then a no-op on an existing key. The **first** registered entry keeps the name; a later same-name entry stays in the list but is never reachable by name. Independently, the loader compares the new entry's id with every entry already in the list: each equal id shows a "matching IDs" popup and a second assertion, once per earlier same-name entry (A, A, A gives one popup for the second and two for the third). When the assertions return, loading moves on to the next node; it does not stop the file, the registry or the load. | Debug build: the id assignment in the post-parse fixup, the id comparison loop, the name-map insert and its lookup-before-insert assertion. |
| RGC-182 | Name lookup returns the first registered entry and nothing else. Every name consumer inherits that: variant base resolution, `Company_Units` and `Squadron_Units` member references (RGC-150..154), task-force named type lists and exclusions, hero/story/Lua type lookups and the post-load reference fixups. A second same-name entry is never the target of a name reference. | Debug build: the three name-lookup overloads (string, string object, CRC) and the reference fixup. |
| RGC-183 | A `Variant_Of_Existing_Type` node is overlaid **at creation**, before the new entry is registered in the name map. Its base is the name lookup of the variant tag at that time. A variant whose tag equals its own `Name` therefore finds the earlier same-name entry when that entry is registered and complete. The overlay copies the base into the **new** entry, keeps the new entry's index, id, name, file and variant reference, reapplies the node's own tags and marks it complete. Result: a complete, separate entry (base plus the node's overrides) that is not reachable by name; the first entry is unchanged. If the base is not yet complete the overlay does nothing and the entry stays incomplete. | Debug build: the variant overlay at creation and the copy-and-reparse routine, which requires a completed base. |
| RGC-184 | A self-named variant that registers **before** any complete entry of its name never completes: its creation-time lookup finds nothing, and in later passes the name lookup returns itself, which is incomplete, so the overlay does nothing for all ten passes. The load continues; the incomplete entry keeps its parsed tags and is still in the list. | Debug build: the overlay's completed-base guard applied to a self reference (static deduction; no capture of this ordering). |
| RGC-185 | Variant completion passes run only while some entry is incomplete (at most ten). In pass N>0, for each file, each incomplete entry of that file triggers one reparse of the **whole** file; for each node the overlay target is the **name-resolved first entry**, with **that entry's own** variant base and the **current node's** tags. Node order therefore matters: when several same-name variant nodes sit in a file that triggers a reparse, the first entry is re-overlaid once per node and the last node's data wins on it. A non-variant first entry is untouched (the overlay returns false). A later duplicate entry is never itself completed by a pass. | Debug build: the completion-pass loop and the reparse in overlay mode. |
| RGC-186 | Reference fixups and per-type post-load work (member marking, masks, inferred cost) run over **every** entry of the list on every pass, duplicates included. Roster members resolve by name (RGC-182), so the ground-company and squadron member flags of a roster written for `A` are set on the **first** entry only; a later duplicate has no flag from references. A duplicate made by RGC-183 carries whatever flags the base held at copy time: usually none, because marking runs after the pass's file loop. Each entry's own roster still marks its members, so a duplicate's roster contributes. | Debug build: the per-type post-load function and the roster accessors. |
| RGC-187 | Consumers that walk the type table by index see each entry once, so a duplicate appears twice. Index walkers found by caller search (only the first was read): category-team candidate lists (filters are evaluated **per entry**, so an unflagged duplicate passes the "not a squadron member" filter that the first entry fails), perception token types, the AI tech tree, production-choice and build-forces lists, tech-level and sliceable-type scans. Name-keyed consumers see only the first entry. Entries stay in table order (first before duplicate). | Debug build: callers of the table size and index accessors; the category-team builder body was decompiled. |

Synthetic cases (one per rule; type names are placeholders):

| Rule | Case | Expected |
|---|---|---|
| RGC-180 | One file `[A, A]`, plain `Squadron` nodes with different `Text_ID` | Type table has two entries, indexes 0 and 1, both parsed. |
| RGC-181 | `[A, a, A]` (case differs) | Entry 0 owns the name. Entry 1: one matching-id popup and one registration assertion. Entry 2: two matching-id popups and one registration assertion. The load completes. |
| RGC-182 | `[A, A]` and a carrier whose `Squadron_Units` names `A` | The reference resolves to entry 0. A named type list `A` yields entry 0 only. |
| RGC-183 | The Remake pair: `[Squadron A, Squadron A variant of A with its own Text_ID]` | Entry 1 is complete, equals entry 0 plus the node's tags, is not name-resolvable; entry 0 keeps its own tags. |
| RGC-184 | `[Squadron A variant of A, Squadron A plain]` | Entry 0 never completes (ten passes). Entry 1 is complete and unreachable by name. Name lookup of `A` returns incomplete entry 0. |
| RGC-185 | `[A variant of B (x=1), A variant of B (x=2), B plain]` | Pass 0 completes only B. Pass 1 overlays entry 0 with x=1, then x=2, and completes it with x=2. Entry 1 stays incomplete through ten passes. |
| RGC-186 | `[Carrier with Squadron_Units A, A, A variant of A]` | Squadron-member flag on entry 0 only; entries 1 and 2 are unflagged. If entry 1 has its own `Squadron_Units B`, B is flagged. |
| RGC-187 | RGC-186 data plus a category team over fighter squadron types | Entry 0 is excluded as a squadron member. Entries 1 and 2 are candidates, in table order, each once. |

Not verified and not claimed: the live-reload path (explicit reload of one type
and its name-map removal); whether the debug handler returns from the
duplicate-registration assertion or the matching-id popup in a normal install
(RGC-181 describes the continuing branch); a captured startup of the Remake
pair; the effect of the same-name entry on saved games (this walk did not read
how saves refer to types).

Data scope. In this layer's game-object registry files, a scan finds exactly one
same-file duplicate name (`Space_LAAT_Squadron_R`, a self-named variant) and no
cross-file duplicate within the layer. Three unit files that a strict XML parser
rejects were skipped; base-layer overlaps were not compared. The many repeated
names in sound, music and ability registries follow their own first-kept rules
(SND-66 and the ability-registry rules) and are outside this walk.

**Ours:** compared with `origin/main` at
`957db34fb5ab2997f8ebb297528b87b2cf9da11b`.

1. **Blocker.** `src/data/xml_merge.cpp:78`–`:80` returns with a
   catalog-wide issue on the first duplicate game-object identity, so
   `Catalog::member_flags` (`:13`–`:27`) refuses every type and the Remake AI
   cannot start (`src/units/galactic_types.cpp:213`–`:219`, then
   `src/campaign/ai_selection_content.cpp:41`–`:47`). Required: do not treat a
   duplicate as an error. Keep `available.emplace` (it already keeps the first
   index, RGC-181/182) and ignore its `second`; **still** run
   `order.push_back(index)` (`:82`) and build the duplicate's `State`, so the
   duplicate has its own flag slot (`flags` is already indexed per definition,
   `:39`) and its own roster marks (RGC-186). Only an empty identity stays an
   error.
2. **Overlay at creation** (`:68`–`:73`, called from `:90`). It already looks
   the base up in `available`, which at that moment holds only earlier entries
   and so gives RGC-183/184 once item 1 lands. It must keep copying into
   `index` (the new entry), never into the name-resolved entry.
3. **Completion passes** (`:96`–`:106`; call at `:104`). `overlay(order[entry])` copies into
   the entry itself. RGC-185 requires, for pass N>0, the target to be
   `available[id(entry)]` (the first entry), using the target's `base` and the
   **entry's** `authored` rosters and definition. A duplicate entry itself is
   not overlaid in passes. This only matters for duplicates that are variants
   in a file with an incomplete entry; it is not needed for the Remake
   blocker.
4. **Roster marking** (`:109`–`:123`) already resolves through `available`
   (first entry) and walks every entry in `order`, which matches RGC-186 once
   item 1 is done.
5. **Catalog collapse.** `Catalog::member_flags` looks the flags up by id
   through `by_id` and the strongest (last) game-object definition
   (`:22`–`:25`), so with duplicates it would return the **last** entry's
   flags, where the original marks the first. The type table
   (`src/units/galactic_types.cpp:205`–`:207`) keeps only `namespace_winner`
   definitions, and `src/data/xml_registry.cpp:640`–`:655` chooses that winner
   by layer rank, then **later** registry order, then later definition order;
   `Catalog::find` (`src/data/xml.cpp:48`–`:58`) uses the same order. The
   original has no layer rank among registered files and keeps the **first**
   registration, and its type table lists every entry. To match RGC-182 and
   RGC-187: (a) name-keyed lookups (`find`, `member_flags(id)`) resolve to the
   first active registered game object by registry/definition order; (b)
   `galactic_type_table` and any category-team enumeration keep one row per
   active definition, in registry/definition order, instead of one per
   `namespace_winner`; (c) member flags are read per definition index (a
   `member_flags(const Definition&)` overload), not per id. PL-20's current
   statement that the remake collapses to the last winner is superseded by
   this list; item 1 alone unblocks starting the Remake AI without matching
   RGC-187's enumeration.

The minimal safe order is item 1 (unblocks the load; the duplicate's flags are
only visible to per-definition readers) and then item 5(c) with 5(b); items 2
and 3 are correctness for variant duplicates and can follow. This research
changes no runtime code.

## Random draws: weighted lists and the free generator (walk random)

This follow-up answers the campaign-start story scripts' random calls
against main `957db34fb` (Web Way starting forces, the neutral reinforcement
script, intervention rewards, fleet events, a few mission hints). Evidence is
static debug-build inspection plus the mounted Remake scripts read in place; no
campaign or multiplayer session was run. L-54 owns the plain `GameRandom` and
`GameRandom.Get_Float` calls and WGA-219 the AI plan picker; this section adds
the script-visible weighted list `DiscreteDistribution`, the
`GameRandom.Free_Random` call, and the shared integer draw both rest on.
Binary symbols and addresses stay in private evidence.

How the Remake scripts call them: a list is made with
`DiscreteDistribution.Create()`, filled with `list.Insert(value, weight)` and
read with `list.Sample()`, all with a dot and no self argument (the dot form is
what the mounted scripts use). `Free_Random` appears as
`GameRandom.Free_Random(low, high)` multiplied by a float modifier to make a
power budget, and as small integer rolls for mission hints. A budget loop
samples until the sum passes the budget (at most 998 passes), so the number of
draws per planet is itself random.

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-190 | `DiscreteDistribution.Create()` takes no arguments (extra arguments are not read) and returns a **new, empty, independent** list on every call; two lists share nothing. The list is a script object with the two members `Insert` and `Sample`. It has save and load handlers, so a live list belongs to the save (the saved layout was not decoded). | Debug build: the list's constructor, factory and registration; the factory is a plain allocate-and-return. |
| RGC-191 | `Insert(value, weight)` needs **exactly two** arguments. Any other count raises a script error and stores nothing. A non-number second argument raises a script error and stores nothing. The first argument is stored without any type check, as the very Lua object that was passed. The weight is narrowed from a double to a 32-bit float. A weight that is **zero, negative or NaN is not stored and raises no error**; the call's own accepted/rejected flag is discarded, so the script cannot tell. `Insert` never returns a value. Each stored entry keeps a running total, so equal values inserted twice are two entries whose weights add. | Debug build: the script wrapper (argument count, number cast) and the list's add routine (positive-weight test, cumulative append). |
| RGC-192 | `Sample()` ignores arguments and returns the **stored object itself** (the same object every time that entry is picked, not a copy), so identity comparison between two samples of one entry is true. The pick: total weight *T* (the last running total, 0 for an empty list); one uniform *f* = a 15-bit draw divided by 32767, so *f* in [0, 1] with 32768 steps; *u* = *f* x *T*, all in 32-bit float. The chosen entry is the first with running total ≥ *u* in the sense *prev < u ≤ cum*; *u* = 0 picks the first entry and *u* = *T* the last. Entry *i* is therefore chosen with the weight share of its slice. A list with one entry still consumes the draw. | Debug build: the list's sample routine and its binary search by running total. |
| RGC-193 | `Sample()` on an empty list, or one whose total is not positive, **draws nothing**, fails a debug assertion and returns an empty value. The script receives nothing (no value) and no script error is raised by the call itself. In the Remake scripts the next line indexes the result, so an empty list becomes a script error there, not in the call. | Debug build: the empty/zero-total guard runs before the draw. What the Lua side sees for the empty return was read from the return path, not run (capture list). |
| RGC-194 | Sampling draws from the **shared synchronised game stream**, the one `GameRandom` uses (L-54), not the AI's separate synchronised stream and not a per-script stream. Every `Sample` is therefore an ordinary advance of the game stream: it shifts every later `GameRandom`, AI and combat draw in all modes, and its position depends on how many samples earlier scripts took. | Debug build: the uniform helper reads the game stream; the AI variant is a different function reading the AI stream. Contrast with WGA-219, whose picker uses the AI stream. |
| RGC-195 | `GameRandom.Free_Random(...)` has the same argument forms as `GameRandom` (none: integer up to the 15-bit maximum; one number *n*: 0 to *n*; two numbers: *a* to *b*; arguments are truncated to integers; a reversed pair is swapped; equal bounds return the bound without a draw; both bounds inclusive). A non-number first argument is a script error that still returns a draw over the default range; a non-number second argument is a script error that still returns a draw over 0 to the first number. A result is always an **integer** (stored as a number). Outside a multiplayer game it takes the one- and two-argument draws from a separate **free** stream. The **no-argument form is the exception: it draws from the synchronised game stream even outside multiplayer.** | Debug build: complete body of the call. |
| RGC-196 | **Multiplayer guard.** If the game mode is LAN, internet or Steam multiplayer, `Free_Random` issues a script **warning** (not an error) saying it cannot give non-synchronised numbers from script in multiplayer, and then performs the plain `GameRandom` call with the same arguments on the synchronised game stream. It returns the same shape and range as in RGC-195. Single-player campaign, single-player skirmish and sub-modes under them are not multiplayer. A multiplayer sub-battle follows the multiplayer test of the game mode that started it. | Debug build: the multiplayer test (solo and skirmish false, LAN, internet, Steam true; anything else false) and the fall-back dispatch, which re-enters the ordinary random call. |
| RGC-197 | The free stream is a process-wide generator **constructed once with a fixed seed at program start**; no game start, load or campaign start reseeds it, and no clock seeding was found. The only reseeds seen belong to the debug cell-colour display. So single-player `Free_Random` sequences depend on how many free draws the process has made so far, not on the game seed: the same save loaded fresh and loaded after a long session can roll differently. Nothing was seen that writes the free stream into a save. | Debug build: static initialiser of the free generator and the callers of the seed-setting routine that were decoded (two debug colour routines, the modem simulator, the cinematics reseed, the multiplayer connect and the game-start seed of the synchronised stream). Two seed-setting callers beside the colour routines were not decoded (capture list). |
| RGC-198 | The shared integer draw: bounds swapped if reversed; equal bounds return the bound with no draw; a span above 32766 is cut to 32766 (debug assertion); the draw is rejection sampling on the smallest covering bit-mask, each rejected word consuming one 15-bit step. The generator's output is **15 bits (0 to 32767)**, so **no `GameRandom`/`Free_Random` call can ever return above 32767**, including the no-argument form, whose range is requested as 0 to 65535 and cut. The float form divides a 15-bit draw by 32767. | Debug build: the integer and float draw routines and the generator's step function. This corrects L-54's "0 to 65535" for the no-argument integer draw; the release build's cut at the span limit was not seen (capture list). |
| RGC-199 | Do these break multiplayer if routed to the synchronised stream? **No.** In multiplayer retail itself runs `Free_Random` on the synchronised stream (RGC-196), and `Sample` always does (RGC-194). In single-player no peer exists, so any stream is safe; the one visible difference is that retail's free draws leave the game stream untouched, while the no-argument form and every `Sample` advance it. A deterministic rule that matches multiplayer exactly and single-player in distribution is: all of them draw from the one game stream, in script execution order. | Derived from RGC-194..198. In multiplayer the story script runs on every machine in the same service on the same state, so a shared stream stays in step. |

Synthetic cases (branch deductions, not captured):

| Case | Expected |
|---|---|
| RGC-190: two `Create` calls, `Insert` into one | The other stays empty |
| RGC-191: `Insert(a)` and `Insert(a, "x")` and `Insert(a, 1, 2)` | Three script errors; list still empty |
| RGC-191: `Insert(a, 0)`, `Insert(b, -3)`, `Insert(c, 2)` | No error; only *c* stored; `Sample()` always *c* |
| RGC-192: weights *a* 1, *b* 3 (T = 4), draw words 0, 8191, 8192, 32767 | *u* = 0, 0.9999, 1.0001, 4 gives *a*, *a*, *b*, *b* (boundary at *u* = 1) |
| RGC-192: `Insert(t, 5)` then two samples of the one entry | Both samples are the same object; 2 draws consumed |
| RGC-193: `Sample()` on a fresh list | Empty value, 0 draws; the next game-stream draw is the one an unsampled run would take |
| RGC-194: sample once between two `GameRandom(1, 100)` calls | The second `GameRandom` value differs from a run without the sample |
| RGC-195 (single-player): `Free_Random(500, 750)`, `Free_Random(750, 500)`, `Free_Random(7, 7)` | Integers in 500..750 twice, then 7 with no draw; none moves the game stream |
| RGC-195 (single-player): `Free_Random()` | An integer 0..32767 taken from the game stream |
| RGC-195: `Free_Random("x", 9)` and `Free_Random(4, "y")` | Script error each; value 0..32767 resp. 0..4 |
| RGC-196 (LAN): `Free_Random(500, 750)` | One warning; an integer 500..750 from the game stream |
| RGC-197: same save loaded twice in one process (single-player), script rolls `Free_Random(1, 6)` | The two rolls need not match |
| RGC-198: `GameRandom()` over 10 million draws | Maximum 32767, never above; `Free_Random(0, 40000)` is cut to 0..32766 |
| RGC-199: two LAN peers run the Web Way script | Same rolls, same spawned fleets on both |

Needs a capture before it is a rule: (1) what a script sees for an empty
`Sample()` in the **release** executable and whether its assertion cut the
return; (2) the release build's cut at a span above 32766 and the no-argument
maximum (a 100,000-call histogram of `GameRandom()`); (3) whether any path
reseeds the free stream or stores it in a save (two uncovered seed callers; a
save-load-roll test would settle RGC-197); (4) the dropped-weight silent
behaviour with NaN and the first argument `nil` in `Insert`; (5) one LAN
session comparing a `Free_Random` roll on both peers to confirm RGC-196 in the
shipping build; (6) the saved layout of a live list (RGC-190). None blocks
implementing RGC-190..196 and RGC-199; (2) corrects L-54 for the no-argument
range.

**Ours:** none of this exists yet. `src/campaign/story_plots.cpp:110`–`:119`
stores the sub-plot's script name and never runs it, `Spawn_Unit` is not bound
(see the Web Way section), and `src/script/authoritative/galactic_api_inventory.hpp:22`
lists only `DiscreteDistribution.Create`, not the members. The sandbox refuses
any binding named `Free_Random` (`src/script/authoritative/scheduler.cpp:469`,
`docs/lua-sandbox.md:168`, `docs/multiplayer-readiness.md:65` and `:204`); that
rule is right for an *unsynchronised* draw, but retail's multiplayer itself
aliases the call to the synchronised stream with a warning (RGC-196), so the
"never silently alias" clause is met by a **warned** alias, not by a ban. The
existing `GameRandom` binding (`src/script/foc/plan_bindings.cpp:887`) draws on
the plan instance's own stream (GR-01) and defaults the no-argument form to
0..65535 (`:889`), which RGC-198 says is 0..32767. Where it goes:

- Stream: one **game-level synchronised stream** owned by the campaign/sim
  state and seeded from the game seed, shared by `GameRandom`,
  `GameRandom.Get_Float`, `Free_Random` and `Sample`, drawing 15-bit words in
  script execution order, saved and hashed with the sim. The per-instance
  `RandomStream` (`src/script/authoritative/clock_rng.cpp:75`) is not the right
  source for story scripts; it remains correct for the AI host's GR-01 until
  that is revisited.
- Bindings: the story host's registration (beside the `Spawn_Unit` binding, in
  the `galactic_bridge.cpp` path that already includes the inventory) registers
  `DiscreteDistribution.Create`, the `Insert` and `Sample` members on the
  returned object, and `GameRandom.Free_Random` as a documented alias that
  draws the game stream and always emits RGC-196's warning once per call site.
  `scheduler.cpp:469` should keep refusing any `Free_Random` binding that does
  not take its draws from the game stream (the refusal becomes an explicit
  `register_synchronised_alias` rather than a name match).
- List arithmetic: running totals and the pick use 32-bit float arithmetic in
  the order of RGC-192 so the same word sequence picks the same entry; the
  stored value is a Lua reference, so identity survives `Sample`.
- Deviations to list: single-player free draws are taken from the game stream
  instead of a process-seeded side stream (RGC-197 makes the retail sequence
  unreproducible anyway), and the no-argument range is 0..32767 once (2)
  confirms it.

## Negative capture reward: load, award and credit floor (walk capture reward)

Republic at War authors a negative `Planet_Capture_Bonus_Reward` on 20 planets
(for example -800 and -1400), and our campaign loader refuses all of them, so 19
of its 24 solo campaigns do not start. This follow-up answers whether retail
treats the value as an error. It extends WGM-76/193, WGEP-16/17 and WGP-53
against main `7d151d2f6` with static debug-build inspection and the unpacked
stock XML; no campaign was run. The 65 stock XML lines that mention the tag
carry no negative value, so the negative case is mod data only. RGC-208 and
RGC-209 are unused.

| ID | Retail rule | Evidence summary |
|---|---|---|
| RGC-200 | **Load.** The tag is read by the same integer entry type as `Additional_Population_Capacity` (RGC-140): decimal text into a **signed 32-bit** field of the planet type. A negative value is kept exactly as authored: no absolute value, no clamp, no float conversion, no load-time warning for the sign. The type starts at **0** (WGS-90), so an absent tag means 0, never an error. The getter returns the field with a full 32-bit load. | Debug build: tag-table entry (same table type code as RGC-140's tag), type initialiser, getter instruction width. Overflow and malformed-text acceptance are not defined here (RGC-140's limit). |
| RGC-201 | **Award gate and order.** In the ownership processor, after the owner switch, the victory tests, the story political-control notice (when no victory ended the game), the credit-cap refresh for every player and the story conquest notice, the award runs when the planet is not destroyed, the new owner exists with a **playable faction**, and the player's captured-type history does not hold this planet type. The type is added to the history first, then the reward is read and credited. **The sign of the reward is not tested:** a negative reward takes the same first-award branch, consumes the history entry and plays/prints the same feedback as a positive or zero one (WGM-76/193). | Debug build: complete award block of the ownership processor and the history adder. |
| RGC-202 | **Credit change.** The reward is converted to single precision and passed to the ordinary credit adder with the windfall flag on. A negative amount is **not** multiplied by the AI difficulty multiplier (only positive additions are, WGEP-16). It is added to the balance; if the sum is below zero the balance is set to **0** and the reported delta is raised by the shortfall (so the delta equals minus the old balance). The account therefore **floors at 0, never goes negative, and the add is never skipped**. The cap test only trims an excess, so a negative add cannot trigger it. After the add, the credits-accrued victory test and the story accumulation notice run with the whole (floored) balance, also for a negative add. | Debug build: complete credit adder; WGEP-16/17 already record the floor and the positive-only multiplier. |
| RGC-203 | **AI windfall.** For a player with AI, the **actual adjusted delta** (negative, or minus the old balance when floored) is passed to the AI budget, which adds `delta × income proportion` to every category's total resources with **no sign test and no floor**. A negative capture reward therefore lowers every AI category total; whether later AI code tolerates a negative category total was not walked. | Debug build: windfall dispatch and budget routine. |
| RGC-204 | **Sound.** The bonus cue plays when the new owner is the local player and the global audio table resolves `SFXEvent_Planet_Capture_Bonus_Reward`, inside the award branch only. A negative reward plays the same cue as a positive one (WGP-53); there is no separate "penalty" sound. | Debug build: sound block in the award branch. |
| RGC-205 | **Text.** With the text-display callback present, the ownership line uses `TEXT_PLANET_CHANGED_HANDS_BONUS` when the new owner is the local player **and the award result value is not -1**, otherwise `TEXT_PLANET_CHANGED_HANDS`. The award result starts at -1 and is set only inside the award branch to the raw reward, so a reward of **exactly -1** takes the plain text although the history entry was consumed and 1 credit was removed; every other negative reward takes the bonus text. Only the new owner, old owner and planet display name are formatted; the amount is not printed. | Debug build: initial value of the result slot, its single assignment, the comparison against -1, and the three name arguments. WGM-193's "bonus text for a newly awarded local reward" holds except for this one value. |
| RGC-206 | **Other readers.** Besides the two award-block reads, the only caller of the getter builds the tactical sandbox objective list: for a tactical planet not owned by the local player and not in the local captured-type history it reads the reward and adds a "planetary credit bonus" objective line with it, again without a sign test, so a negative reward shows as a negative bonus there. No AI planet valuation or other UI path calls the getter. | Debug build: caller list of the getter (three call sites) and the objective builder. Direct field reads were not enumerated; the objective's format string is localisation text (capture list). |
| RGC-207 | **Campaign start pays nothing.** The first allegiance service of a planet initialises its owner without entering the owner-change block, so initial ownership never pays a reward and never adds the type to the history. This branch adds nothing to the history either; a reward can only be paid by a later change of owner. | Debug build: first-service branch, and the old/new owner comparison taken after it. |

**Ours at main `7d151d2f6`.** Parsing is already sign-safe: `Record::whole`
(`src/data/galactic/galactic_data.cpp:80`) keeps a negative integer. Two
refusals stop it, and the award path behind them already matches RGC-202/203:

- `src/campaign/control.cpp:122`–`:124`: `bind_control_data` fails with "missing
  political leaning or capture reward" when the reward is absent **or
  `< 0`**. Drop the `< 0` test (RGC-200/201). The absent test is unreachable
  from the live path (`src/campaign/live_setup.cpp:471`–`:473` already installs
  0, WGS-90) but contradicts RGC-200 for direct callers: install 0 there too.
- `src/sim/galactic/control.cpp:227`: `validate_control_setup` rejects
  `p.capture_reward.raw() < 0`, and it also runs on create and checkpoint
  restore (`:1446` decodes the signed fixed value). Drop that term; the
  `leaning` test beside it stays out of scope (see the fidelity note below).
- Already correct, keep: `adjusted_credits` (`src/sim/galactic/setup.cpp:296`)
  multiplies only positive deltas and clamps the sum to `[0, cap]`;
  `src/sim/galactic/session.cpp:598`–`:624` records history before the credit
  (`src/sim/galactic/control.cpp:603`–`:607`), sends the actual signed delta to
  the AI notification, and raises the first-capture feedback notice with no sign
  test; `src/sim/galactic/ai_funding.cpp:392` applies a negative windfall with
  no sign test.
- Not built yet: both ownership texts (RGC-205, with its -1 case) and the
  tactical objective line (RGC-206). When they land, the -1 special case must be
  kept as written, not "fixed".
- Checkpoint and snapshot already carry the signed value
  (`src/sim/galactic/snapshot.cpp:245`, `src/sim/galactic/control.cpp:1368`).

Synthetic cases (branch deductions, not captured). One setup: player 1 human
with balance 300, cap 40000; a planet type with the stated reward, new to player
1; player 2 owns it and player 1 captures it.

| Case | Expected |
|---|---|
| RGC-200: load a planet type with `Planet_Capture_Bonus_Reward` -800, then one with the tag absent | Stored -800 unchanged, then 0; no load error for either |
| RGC-201: capture the -800 planet, then lose and retake it | First capture consumes the history entry and changes credits; the retake changes nothing |
| RGC-201: capture a destroyed planet, and a planet whose new owner is not playable | No history entry, no credit change |
| RGC-202: reward -100, balance 300 | Balance 200, reported delta -100 |
| RGC-202: reward -800, balance 300 | Balance 0, reported delta -300; never negative |
| RGC-202: reward -800, new owner is Hard AI with balance 300 | Balance 0 (no 1.2 multiplier on the debit), reported delta -300 |
| RGC-202: reward -800, balance 0 | Balance 0, reported delta 0; history entry still consumed; feedback still raised |
| RGC-202: reward 0 | No change, history entry consumed |
| RGC-203: reward -800, AI new owner, balance 300 | The AI budget windfall receives -300 and every category total drops by its share; no clamp at zero in the budget |
| RGC-204: reward -800, local new owner, audio table resolves the event | The bonus cue is requested once |
| RGC-205: local new owner, rewards -800, -2, -1 and 0 | Bonus text for -800, -2 and 0; plain text for -1 (balance still down 1 when affordable) |
| RGC-205: a remote (non-local) new owner, reward -800 | Plain text, no cue |
| RGC-206: tactical planet not owned by the local player, reward -800 | Objective line built with the signed value; planet owned by the local player or type already in the history: no line |
| RGC-207: campaign start with a negative-reward planet owned by player 1 | No credit change, no history entry; a later capture of another planet of that type by player 1 still pays |
| Replay: the capture cases above on 1, 2, 4 and 8 workers | Identical state hashes and balances |

Needs a capture before it is a rule beyond the static branch reading: a rig
capture of capturing Agamar in a Republic at War campaign is the proof. Steps
(not run): (1) start the Republic at War campaign that owns Agamar on the debug
build and note the reward authored on that planet type and the starting balance
of the capturing player; (2) read the balance with the Lua debugger before the
attack, capture Agamar in a tactical battle or by landing unopposed, and read
the balance and the captured-type history right after the owner change
(balance should be `max(0, balance + reward)` when the reward is negative);
(3) record whether the bonus sound and the bonus text appeared (RGC-204/205)
and whether the ownership line was plain or bonus; (4) capture a second planet
of the same type (or retake Agamar) and confirm no second change; (5) repeat
once with a starting balance below the magnitude of the reward to see the floor
at 0 (RGC-202) and, if an AI side can capture, read its budget category totals
before and after (RGC-203); (6) run a planet whose reward is exactly -1 if the
mod authors one, otherwise assign it with a scripted type edit to see the
RGC-205 text choice; (7) open a tactical sandbox battle on a negative-reward
planet and read the planetary credit bonus objective line (RGC-206). Also open:
the format specifier of that objective string and whether anything downstream
of the AI budget rejects a negative category total.

Fidelity note (not in this ticket): `control.cpp:227` also refuses a negative
planet `leaning`, while WGM-69 says retail clamps the political leaning's
magnitude instead; a mod authoring a negative `Political_Control` would hit the
same class of refusal.
