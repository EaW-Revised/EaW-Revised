# Blast observation evidence

This follow-up to [the area-damage walk](walks/area-damage.md) distinguishes
targeted **debug build** reads from live Lua observations. A capability lookup
proves that a name is present or absent in the observed Lua state; it does not
prove a projectile rule or replace a controlled shot.

## Random hardpoint selection

**WAD-R05**, targeted debug-build read, 2026-10-02: start at one synchronized
random index across all of a recipient's hardpoints, then scan forward with
wraparound for the first living, destroyable hardpoint. Targetability is not
an eligibility condition. If the guarded search has no eligible result it
returns no hardpoint. Consequently this is not a uniform draw over only the
eligible hardpoints: intervening ineligible entries affect the probabilities.

The retained damage-route read establishes that each application of a blast
share invokes this selection again when
`Projectile_Damages_Random_Hard_Points` is enabled. A successful selection
replaces the supplied collision mesh, while the already calculated share
amount stays unchanged. It can therefore select a hardpoint excluded from
the original share list by the direct-hit mesh rule. The initial share list
and its denominator still follow WAD-19..22; this does not mean damage is
redistributed only among living hardpoints.

This settles the algorithm part of U-05, which the walk previously left
unspecified. A custom-payload station shot with per-share observations is
still unverified. Private lookup receipts and retained damage-route reads
stay in ignored research output.

## Live preparation

`Invoke-FocMapCapture.ps1 -BlastEvidence` requires the existing bank staging,
the Lua debugger and a battle-only session. After rearming the battle debugger
it disposes the staged ship, disables further bank staging, suspends the AI
and reads named public globals and station methods through protected Lua
lookups. The reader validates every lookup's success flag, type and text.
The route retains ordinary FIFO ownership and Stop/Cleanup teardown.

No capability result settles U-01..U-04 or U-06..U-08. Damage contacts,
logical-frame delays, source attribution and charged/manual presentation
require their specific witnesses from the walk.

## Private cap-order probe

`-BlastCapEvidence` stages four named stationary probe craft at deliberately
unsorted distances. It repeats caps of one and two with the same spawn order,
reversed spawn order and changed positions. A small direct-only control shot
measures the primary hit before the area cases; only the aimed craft's measured
primary delta is removed from subsequent deltas. The payload has one pulse and
a long recharge, no repair and no area falloff. Its XML is generated on the
game host from local type inheritance, with a separate session registry.
These are probes, not stock data.

The reader requires an observed single projectile, unchanged recipient IDs and
positions, complete before/after health records and a completed shot. A
timeout, multiple shots, repair, recipient movement or an affected count that
does not match the cap is rejected. Lack of a second enemy owner is recorded;
single-owner cases cannot establish the walk's player traversal order. The
hardpoint ship with no eligible hardpoints remains a separate witness.

The 2026-10-02 live attempts established the four probe craft's identities,
positions and full initial hull state, but produced no accepted completed
single shot. The initial recipe reached its no-projectile timeout. The
direct-only control retry ended when the loaded scoring script disappeared;
its preceding reads showed no probe projectile or recipient damage. Two
further retries enabled native shooter services, removed the stock mobile
fleets and finally used an ordinary anti-fighter donor. Both reached their
no-projectile timeout. The final shooter's typed snapshots confirmed the
private projectile selection, active orders and movement toward its target,
with unchanged recipient hull. These attempts establish no victim selection,
ordering or slot consumption. The reason firing did not complete is unverified.

## AI addition observation

`-ReinforcementEvidence` selects the `reinforce_observe` staging scenario when
the battle starts. It records the initial mobile fleet and enemy difficulty,
leaves the AI active, points the camera at the enemy station and retains the
first subsequent mobile additions. Projectile objects and stock starbase
upgrades are excluded. A debugger reader can recover these observations even
when it attaches after the event. A legacy bank-start session is explicitly
labelled `debugger_resume` when the reader initializes it later.

`-AIDifficulty Normal` selects the stock menu's Medium AI entry using a
checked, freshly observed popup and requires `-GraphicsPreset Highest`.
An optional owned `ai_normal_selected` template permits reuse of an already
selected Medium AI lobby. The ignored `ai_choices` template must be
pinned from an owned 1280-by-720 lobby. The reader also requires the observed
Lua difficulty to be Normal; a different difficulty preserves the receipt
and fails the assertion.

**RBO-T01**, targeted debug-build clock read: `GetCurrentTime()` returns the
logical frame multiplied by the reciprocal logical FPS using float
arithmetic, then promotes that value to a Lua number. The reader preserves
that raw value and recovers the nearest tick only when it agrees with the
30-FPS clock. An addition is bracketed by the previous and current observer
services; the requested service interval can span multiple logical frames.
The bracket is retained rather than choosing an invented birth frame.

The observer stores per-unit position and health snapshots at the first
observed service at or after +0, +1, +3 and +5 game seconds. At +6 seconds it
seals the window and stops changing its recorded fields. The reader waits for
that completion flag before validating the full window: a native suspension
alone did not prevent fields advancing between expression evaluations in a
rejected live attempt. Reads still use an owned suspension and automatically
resume. Credits are sampled at observer start and in the first addition
service; an unavailable lookup records a reason instead of a balance. A mobile addition
alone does not identify its reinforcement route or prove visible flyout.
The current harness resumes before returning a read/break result and offers
no persistent held-state API for a still sequence. Any missing visual witness
remains an owner capture request rather than an asserted animation verdict.

The accepted 2026-10-02 observation used Coruscant Siege, human Rebellion
against Easy Empire AI, native lit Highest, fog off and Environment 1. The
observer began at tick 0 with 6000 AI credits. Its baseline contained two
interceptor squadron objects and fourteen interceptor craft. The first mobile
addition service was after tick 4800 and by tick 4802: one
`TIE_INTERCEPTOR_SQUADRON` object and seven `TIE_INTERCEPTOR` craft. The AI
balance in that same service was 276.81512451172 credits. Position and health
snapshots completed at ticks 4802, 4832, 4892 and 4952; the reader accepted the
sealed window and explicitly left visible reinforcement unasserted.

The matched Normal-difficulty follow-up on a second test machine used the same map, opposing
factions, native lit Highest, fog-off and Environment 1 settings. Its observer
began at tick 0 with 6000 AI credits and the same fourteen craft plus two
squadron objects in the baseline. First additions were after tick 3129 and by
3131: one interceptor squadron object and seven interceptor craft. The
first-service balance was 169.59690856934 credits. Sealed snapshots at ticks
3131, 3161, 3221 and 3281 passed the reader, including the independent Normal
difficulty assertion. The menu's Medium AI label was also checked visually.
This observation retains raw unit positions; visible flyout remains unasserted.

The comparison clip uses the same Coruscant map and opposing Rebel/Empire
players, Normal AI, 6000 initial credits, prebuilt bases, free starting units
and seed 67. Its initial Empire fleet also contains Tartan and Acclamator.
Seven interceptor craft are created at tick 5222, become visible at 5257 and
land at 5372. Retail object addition is a different event from visible
appearance. The Normal follow-up matches difficulty, while starting fleets
still differ; these observations do not establish a timing-parity verdict.
The missing matched visual witness is
tracked as an owner recording request (legacy EAWR-1096), with proceed-without
date 2026-10-05.

## Request disposition

| Request | Evidence from this follow-up | Relation to the walk |
|---|---|---|
| U-01 | Unverified; controlled boundary contacts and per-hardpoint deltas were not obtained. | No boundary rule changed. |
| U-02 | Unverified; private recipients staged, but four firing attempts failed to produce an accepted completed shot. Two-owner traversal and empty-hardpoint slot consumption were not observed. | No enumeration or cap-order policy confirmed. |
| U-03 | Unverified; per-hardpoint health, direct-route identity and same-frame death ordering were not observed. | Existing split/exclusion reads remain the evidence. |
| U-04 | Unverified; a positive delayed packet and source-retention sequence were not observed. | Existing queue-branch reads remain the evidence. |
| U-05 | Chooser algorithm verified by WAD-R05; custom-payload per-share runtime witness remains unverified. | Matches WAD-27's random-start, forward-wrapping eligibility search. |
| U-06 | Unverified; charge, gun destruction and two-shot presentation sequence not run. | No charged-shot timing rule changed. |
| U-07 | Unverified; fog-on proxy, height and expiry sequence not run. | No barrage flight or proxy rule changed. |
| U-08 | Unverified; two-station command acceptance, readiness and turret sequence not run. | No manual-command rule changed. |
