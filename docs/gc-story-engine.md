# Live story engine (galactic campaigns)

The rules are in [the story walk](behaviour/walks/gc-story.md) (WST-*) and the session walk (WGS-07, WGS-140). This note
records what the engine runs today and the decisions it makes while rules are open.

## What runs

- **Loading (WGS-07, WST-01..08).** A campaign start loads, for each active player, the plot named by the campaign
  (`Rebel_Story_Name`, `Empire_Story_Name`, `Underworld_Story_Name`, routed by the GameConstants side names) in that order.
  The plot's sub-plots keep their active/suspended state, the preceding `Lua_Script` and the owning player. A missing plot
  or sub-plot file, an unknown event type or a repeated event name is reported in the loader's notes and never fails the start.
- **Evaluation (WST-10..28).** Events with prerequisites, perpetual events, branches (`RESET_BRANCH` to three levels,
  `DISABLE_BRANCH`, `DISABLE_STORY_EVENT`, `RESET_EVENT`), the flag table (`SET_FLAG`, `INCREMENT_FLAG`) and
  `STORY_ELEMENT` run inside the engine. `STORY_TRIGGER`, `STORY_ELAPSED` (WST-12 unpaused time), `STORY_FLAG` (WST-27) and
  `STORY_TECH_LEVEL` (WST-33; the assigned level of the owning player, including the levels set up before the first service)
  are evaluated. The story is serviced once per galaxy step after the hosted AI work, before the frame timer advances.
- **Rewards (part 2, WST-40/41/64).** The engine queues every non-graph reward; the session applies the queue once per galaxy
  step, after the story service and inside the tick executor (never from inside a notice, the world is mid-service then). A
  reward can notify the engine again (a tech change), so the queue is drained until empty (64 rounds per step at most). Each
  record on `GameTick::story_triggers` carries its `outcome`: `applied`, `applied by the engine`, or `refused: <reason>`; a
  refusal is also published once per reward type and reason as an `EAWR-STORY-0001` diagnostic. The applied rewards are
  `src/campaign/story_rewards_live.cpp`:
  - `SPAWN_HERO`, `UNIQUE_UNIT` (type, planet, count, default 1): units created for the owning player through the due-hero
    placement path (`place_due_hero`, the original-type builder of the hero returns). Planet: the named planet's first
    instance; when the galaxy has no such planet, the first planet the owner holds (ascending planet id; the enumeration order
    is unverified, WST-U15, **interim**); the anchor gives way to the nearest planet the owner holds; an enemy fleet at that
    orbit sends the unit to the nearest planet whose orbit has none. An event that names no planet is refused (a plot trigger
    carries no object). Heroes of an AI owner reach its free store, and the human's 1.5 s flash is presentation: both are seams.
  - `BUILDABLE_UNIT` (unlock type, lock type): the unlock removes the build lock and lists the type as unlocked, then the lock is
    listed; an absent or unresolved type skips its half. Then, for the faction named by `Good_Side_Name` only, the post-check:
    when no affiliated, initially locked, slicer-unlockable type at or below the current level is left off the unlocked list,
    the level goes up by one (unlocks run). The "at or below" boundary follows the walk text; the level-assignment unlock pass
    uses a strict boundary (WGEP-147). `LOCK_UNIT` lists the lock only.
  - `SET_TECH_LEVEL` (faction, level): the maximum rises first when needed, then the level is set with its notices (the story
    sees the new level, so tech events follow in the same step). `SET_MAX_TECH_LEVEL` sets the maximum alone and is refused
    below the current level (unverified). The faction names the first active player of that faction.
  - `CREDITS` (amount): a whole-number windfall with multiplier one.
  Every applied reward is a journaled native effect (kind 12 for tech, 19 for locks, 20 for spawns and credits), so a recording
  reconstructs it without the engine; a step that replays the journal applies nothing itself.
- **Setup binds the plots' units.** A unit type a plot creates (`SPAWN_HERO`, `UNIQUE_UNIT`) joins the creation inputs of the live
  setup with what it needs (company members, transport, additional creation), so any plot can place it later
  (`prepare_live_starting_forces`, from `story_unit_names`). A type with an unbound dependency stays out and its reward is
  declined by name. A type that only a plot creates may carry a political-protection source (Tarkin does): it is placed, but the
  protection lifecycle (legacy EAWR-2359) is not implemented, so the ability does not act yet; a type a campaign *starts with* keeps the
  setup refusal. Binding a unit with the slicer ability (the Rebel droids) turns the theft list on: a slicer-unlockable type
  without an authored `Slice_Cost_Credits` (some Underworld star bases) has no theft price and stays out of the list (unverified).
- **Not applied, declined by name.** `DISABLE_BUILDABLE`, `ENABLE_BUILDABLE`, `REMOVE_UNIT`, `DESTROY_OBJECT`, `SET_HEALTH`,
  `CHANGE_OWNER`, `FORCE_RESPAWN`, `STATISTIC_CHANGE`, `GIVE_BLACK_MARKET`, `NEW_POWER_FOR_ALL`, `REMOVE_POWER_FROM_ALL`, the
  victory, reveal, camera, dialog and sound rewards and every other type outside the list above: the record is published
  with `refused: this reward type has no live owner yet`. A save taken while a tactical child is active keeps rewards queued
  in the engine only in memory (they are not saved); the stock plots queue none then.
- **State.** The triggered state, the clocks and the flag table are saved in the campaign owner blob and are part of the session
  digest (see [checkpoint format](checkpoint-format.md)). Sessions without plots hash as before.

## Interim rule: event order (WST-U01, an open owner question) (legacy EAWR-4344)

The original walks same-type events and dependants in a hashed container's order, which is not recovered. Until a debug-build
trace settles it, **every ordered walk uses document order**: events of one type inside a sub-plot, dependants of an event,
sub-plots of a story (plot order, then file order) and members of a branch. The rule is the single constant
`story::interim_event_order` in `include/eawr/sim/galactic/story_engine.hpp`; flipping it means changing the container walk
that fills the per-event dependant lists and the sub-plot order the loader appends.

## Seams

- **Land and tactical events and rewards** (`STORY_LAND_TACTICAL`, `STORY_TACTICAL_DESTROY`, `STORY_LAND_ON`, `STORY_DEPLOY`,
  `LINK_TACTICAL`, `SET_TACTICAL_MAP` and the other tactical kinds) are parsed and refused with "not available until land
  battles": they are listed in `Engine::unavailable()`, never fire, and a triggered `LINK_TACTICAL` carries the refusal.
- **Later evaluators.** The galactic event types of WST-31..39 other than the four above (arrival, conquest, construction,
  destruction, battle results, corruption, notifications) are listed in `Engine::unavailable()` as "no evaluator yet".
- **Lua.** `Lua_Script` names are stored with their sub-plot; the `Story_Event_Trigger` callback of WST-20 step 3 and the
  story Lua service of WST-54 belong to the Lua story host.
- **Unverified choices** (least visible behaviour, to confirm with a debug-build capture): an absent switch parameter of
  `DISABLE_BRANCH`/`DISABLE_STORY_EVENT` disables, an absent `INCREMENT_FLAG` delta is 1, a sub-plot activated by
  `STORY_ELEMENT` restarts the clocks of its waiting elapsed events, and a reset event without prerequisites is active again.
- **Not implemented, to confirm against the debug build.**
  - WST-13 queueing: while a tactical child is active, notices go straight into the parent's engine instead of being queued
    for the next eligible service.
  - `RESET_EVENT` uses the sub-plot reset semantics of WST-26 (a reset event with prerequisites goes inactive and is not
    re-checked); the single-event rule is unverified.
  - One plot per side: the first active player bound to a side's name loads that side's plot. A mod with several human
    players on one side, or extra faction plot pairs (WGS-07), is not covered.
- **Recursion.** A `TRIGGER_EVENT` or prerequisite cycle in mod data stops at 128 nested triggers with a reported refusal
  (`Engine::diagnostics()`); tactical events named by `TRIGGER_EVENT` or a branch reset are refused the same way.
- **Reports.** The loader's notes, the engine's refusals and a count of events and rewards that do not run are published once
  as `EAWR-STORY-0001` diagnostics of the first step.
