# Co-op Galactic Conquest on single-player campaign maps

Status: research and proposed design, 2026-10-29. Nothing below is built. It answers the owner's question:
the multiplayer GC lobby loads only the multiplayer GC modes; could co-op also load the single-player GC maps,
and what blocks it?

Evidence labels used throughout:

- **[code]**: verified in our source at the cited `file:line` on main 809cfbd6.
- **[data]**: verified in stock FoC data or in the named mod's files, cited by file and entry or tag.
  The mods are Republic at War (Workshop 1129810972, "RaW") and Battlefront Commander GCW 1.1
  (Workshop 2712013423, "BFC").
- **[rule]**: an existing behaviour-note rule ID backed by debug-build evidence.
- **[assumption]**: not verified. The text says what would verify it.

No decompiled output was used for this note. The retail facts come from data and from existing behaviour-note rules.

## Short answer

Loading the stock FoC single-player GC maps in co-op (two or more humans on **one** faction against AI) has
**no hard blocker in the data or the lockstep design**. The blockers are in our code:

1. The lobby, agreement, scene and resume paths all refuse campaigns without `Is_Multiplayer`.
2. One enum value, `CampaignSetupMode::multiplayer`, controls two separate things: "this session is networked"
   and "use retail's multiplayer campaign rules". The second includes `Basic_AI` instead of `AI_Player_Control`,
   multiplayer heroes, multiplayer victory lists, story events muted unless tagged `Multiplayer`, and the
   battle vote. An SP map in co-op needs networked transport with **single-player campaign rules**.

Stock SP GC story plots use only features our story engine already runs: elapsed, trigger and tech-level events
with hero, unique-unit and buildable rewards, plus one intro holocron popup. The story campaign and the tutorials
are a different matter, as are mods whose story Lua calls unbound APIs. Those are not co-op problems; they already
fail in solo.

Estimated first milestone (load one stock SP GC map with 2 co-op seats, play one galactic day): **about 35 worker-hours
in 5 PRs** (see "Blockers ranked").

## 1. How MP GC modes and SP campaigns differ in the data

Stock FoC loads five campaign files: `Campaigns_Underworld_GC.xml`, `Campaigns_Underworld_Story.xml`,
`Campaigns_Multiplayer.xml` and two tutorial files [data: `Data/XML/CampaignFiles.xml` lines 4-8]. The base game's
`Campaigns_SinglePlayer.xml` is not listed [data]. Whether FoC also reads it through another route is an
[assumption]; our loader follows the list.

| Aspect | SP GC map (`Campaigns_Underworld_GC.xml`) | MP GC mode (`Campaigns_Multiplayer.xml`) |
|---|---|---|
| Entries | 18 = 6 maps × 3 faction variants; each map's 3 variants share one `Campaign_Set` [data] | 6, one per map, all in `Multiplayer_Campaign_Set` [data] |
| Human faction | fixed per entry by `Starting_Active_Player` (e.g. `Sandbox_Gateways_Underworld`: Underworld, lines 84ff) [data] | no `Starting_Active_Player`; the human players come from lobby slots, because the playable factions set `Create_Player_In_Multiplayer_Games No` [data: `Factions.xml` lines 7, 802] |
| AI factions | `AI_Player_Control` lists only the non-human factions (`Empire, BasicEmpire`, `Rebel, BasicRebel`, `Pirates, None`) [data: lines 90-92] | no `AI_Player_Control` (0 hits in the file) [data] |
| Story plots | `<Faction>_Story_Name` for all three factions: the human gets a per-map sandbox plot, the AIs get the shared AI-hero plots [data: lines 86-88] | none (0 hits) [data] |
| Heroes | arrive through story rewards (`SPAWN_HERO`) [data] | placed in `Starting_Forces`, plus each faction's `Multiplayer_Campaign_Heroes` [data, code: `src/campaign/setup.cpp:648-655`] |
| Credits | human 12000, AIs 8000 [data] | all 8000 [data] |
| Victory | `Galactic_Super_Weapon_Destruction, Galactic_All_Planets_Controlled` [data] | `Galactic_All_Planets_Controlled` [data] |

Story plot content [data, summarised in `docs/behaviour/walks/gc-story.md` lines 355-362]:

- The SP GC plots carry an empty `Lua_Script`. Their events are only `STORY_ELAPSED`, `STORY_TRIGGER` and
  `STORY_TECH_LEVEL`, and their rewards are `SPAWN_HERO`, `UNIQUE_UNIT` and `BUILDABLE_UNIT`, plus one
  `Story_Dialog` intro goal on the human's plot.
- The story campaign (`Full_Story_Campaign_Underworld`) is a different class. It has 882 events, a galactic Lua
  script, 9 linked tactical missions, `LOCK_CONTROLS`, `SCREEN_TEXT`, `MULTIMEDIA` and camera rewards, and it
  switches the other factions to a story opponent AI.

**What the MP lobby loads today** [code]:

- `list_mp_lobby_campaigns` keeps a campaign only if it is listed, has `Is_Multiplayer` and is not a tutorial
  (`src/campaign/mp_lobby_catalog.cpp:28`).
- The same refusal repeats in four places:
  - lobby validation: `src/net/galactic_lobby.cpp:138`
  - agreement: `src/campaign/mp_agreement.cpp:90`
  - scene load: `apps/viewer/src/galactic/scene_content.cpp:326-327`
  - MP save resume: `src/platform/net/galactic_lobby_backend.cpp:323-329`

## 2. Our co-op architecture and what SP maps break

Today's design [code and docs]:

- **Lockstep.** Deterministic lockstep over a full mesh; no peer is authoritative. AI, story and session-service
  payloads are produced on every peer and never sent (`include/eawr/net/galactic_adapter.hpp:16-24`,
  `docs/net-mesh.md`).
- **Seats command, players own** (ADR-020). Co-op means up to 8 human seats on one faction against AI.
  Versus means exactly two human sides, one faction each (`include/eawr/sim/galactic/mp_roster.hpp:104-131`).
  At least two humans are required (`:114`).
- **Battles.** Inside battles the seats split control of mobile units (legacy EAWR-4590). Every conflict with a human
  side goes to Begin/Auto-resolve voting (GCMP D10, `src/campaign/game_session_lifecycle.cpp:921-934, 952-973`).
  Co-op against AI is therefore **not** auto-resolved, even though the plain arbitration function would do that
  in MP (`src/campaign/conflict_policy.cpp:243-246`, retail rule WGM-44).
- **Desync detection.** Per-tick state and history hashes with bisect (`docs/multiplayer-readiness.md`).
  Checkpoints carry the seat table, story state and script scheduler state (`docs/checkpoint-format.md`).

What changes with an SP map, assumption by assumption:

| Assumption | Effect with an SP map | Verdict |
|---|---|---|
| Story scripts assume one human | Stock SP GC plots have no Lua and name factions, not "the player" [data]. The rewards they use already have live owners (`src/campaign/story_rewards_live.cpp:121-127`) [code]. | fine for stock |
| Story events gated by mode | `eligible()` runs an event only if `config.solo` or the event is tagged `Multiplayer` (`src/sim/galactic/story_engine.cpp:171`, WST-14). `solo` is set to `mode != multiplayer` (`src/campaign/auto_resolve_live.cpp:764`). In today's MP mode **every SP plot event is muted**. | **must change** (rules split, below) |
| `Find_Player("local")` | returns nil in MP by design, because a per-peer local player cannot enter lockstep (`src/campaign/galactic_story_scripts.cpp:152`) [code]. In co-op every human shares one faction, so "local" has a deterministic answer, identical on every peer. | small change (B4) |
| Player-faction query | Stock SP story scripts use `Find_Player("<faction>")` only. They never use a local or human lookup, and contain no multiplayer or game-mode checks [data: all 95 `Data/Scripts/Story/*.lua`]. | fine |
| Tutorial and forced-battle scripts | Tutorials and the story campaign link tactical missions. `LINK_TACTICAL` and `SET_TACTICAL_MAP` are refused even in solo (`src/sim/galactic/story_engine.cpp:136-143`) [code]. | out of scope (D3) |
| Campaign progression | Victory installs the human list for `Starting_Active_Player` and the AI list for the others only in single-player (WGS-15, `src/sim/galactic/victory.cpp:138-146`). In multiplayer every non-neutral player gets the human list, and AI-owned planets are ignored (WGS-18/20, `victory.cpp:165, 231`). | **must use SP rules**; otherwise an AI can never win, and see risk R1 |
| Hero and asymmetric starts | SP heroes come from story rewards. MP setup adds `Multiplayer_Campaign_Heroes` (`setup.cpp:648`). With the SP map plus MP rules, heroes would be doubled. Human 12000 vs AI 8000 credits is intended SP balance. | SP rules |
| AI factions | MP mode replaces the authored AI type with the faction's `Basic_AI` (`src/campaign/galactic_ai_runtime.cpp:1037-1040`). SP rules use `AI_Player_Control`. Human factions are already skipped (`:1030`), which also covers BFC listing its human faction as `SandboxHuman`. | SP rules |
| One-player UI | The intro `Story_Dialog` opens the holocron summary as a popup (WST-58). Its dialog playback uses a wall clock and a `PAUSE` keyword (WST-83). | presentation per seat (B5) |

## 3. Determinism

- **Script RNG** is counter-based (seed, tick, instance, draw), saved and hashed (`docs/lua-sandbox.md`).
  `GameRandom.Free_Random` falls back to the synced stream in MP with a warning
  (`src/campaign/galactic_story_random.hpp:176-183`) [code]. Stock story Lua uses `GameRandom` only, never
  `math.random` [data].
- **GlobalValue.Set** issues a command committed in order, not a direct write (`src/script/authoritative/bindings.cpp:916-926`,
  bound in the story host (legacy EAWR-5467)) [code].
- **Time.** `os`, `io` and `debug` are absent from the sandbox; script time is completed ticks [code: `src/script/authoritative/clock_rng.cpp:28-32`].
- **Unbound APIs** (109 names, `src/script/authoritative/galactic_api_inventory.hpp`): a call faults and retires the
  script instance (`src/script/authoritative/galactic_bridge.cpp:64-67`).
  - This is deterministic: every peer retires the same instance at the same tick. Unbound APIs therefore cost
    behaviour, not sync.
  - `GiveDesireBonus` is a warned nil stub (legacy EAWR-5494) and `Get_Story_Plot` is still unbound (legacy EAWR-5495).
    Both are called by AI plan scripts in solo just the same. They block mod fidelity, **not** co-op on stock SP maps.
- **What would desync.** Only per-peer inputs entering the sim:
  - local player: handled by returning nil today; B4 below keeps it deterministic;
  - local profile auto-resolve and "local involved" in SP arbitration: `inputs.local_involved` is derived from
    `solo_reference_human` (`auto_resolve_live.cpp:727`), which is unset in MP (`setup.cpp:845`);
  - the dialog wall clock and `PAUSE`;
  - `STORY_SPEECH_DONE`, which waits for audio playback. It is not evaluated today (`story_engine.cpp:83-88`).
    A future binding must stay tick-based in networked sessions.

Stock SP maps hit none of these once the arbitration keeps the D10 vote path.

## 4. Picking the human and mapping seats

**SP** [code, data]:

- The menu offers one entry per `Campaign_Set` (`src/presentation/ui/campaign_setup.cpp:38-48`).
- The faction pick selects the set entry whose `Starting_Active_Player` equals the chosen faction (`:106-116`).
- The human seat goes on that faction; every other faction gets an AI seat
  (`apps/viewer/src/galactic/galactic_mode_seats.hpp:20-29`).

**Retail MP GC** [rule, data]:

- No faction is pre-assigned. Players are created from lobby slots.
- A battle not between two humans auto-resolves (WGM-44).
- Quick-save and autosave are off (WGS-33).

Nothing in the data shows retail MP GC with two humans on one faction. Retail co-op on one faction does not exist
[assumption: no data or rule shows it; a retail MP capture would confirm. It is not needed for this design, since
ADR-020 already makes co-op a project rule]. GHUD-C19 already lists the retail MP screens as "capture-needed".

**Proposed mapping:**

- **Co-op, 2+ humans on one faction (first scope).** The players choose a `Campaign_Set` and a faction. The lobby
  loads exactly that faction's variant entry unchanged, and seats every human on its `Starting_Active_Player`.
  No data is merged, all victory, story and credit rules apply as authored, and there is no per-mod code.
- **Versus, 2 humans on 2 factions (later, owner decision D2).** Each human faction comes from a different variant
  of the same set. The variants differ in `Starting_Forces`, credits and story routing:
  - stock: human 12000 vs AI 8000;
  - RaW Clone Wars: 113 differing start-force lines between the `_Rebel` and `_Empire` variants [data].
  So a composition rule is needed. One example: take each human faction's own rows from its own variant, and the
  AI factions' rows from the first human's variant. That is new design, and every such campaign becomes a
  project-defined mode.

## 5. Save/load, battle hand-off, pause and speed, observers

- **Save/load.** MP GC save is admitted on every peer (`docs/galactic-architecture.md`) but cannot be resumed from
  the lobby yet (legacy EAWR-5373). SP campaigns are long, so this is the first follow-up after the milestone.
  Checkpoints already carry story plot state and scheduler state [docs: `docs/checkpoint-format.md`].
  The resume check must accept the new rules flavour (B1).
- **Battle hand-off.** Unchanged: the ADR-020 seat split, caller-owned reinforcements, the D10 vote.
  - The TaskForce delivery abort (legacy EAWR-5484) is an AI-script problem that occurs in solo too. Its fix (never
    end the campaign on a retired script owner) is a prerequisite for mods, not for stock.
  - SP maps run more authored AI player types than MP (`AI_Player_Control` instead of `Basic_AI`), so they reach
    that path more often [assumption].
- **Pause and speed.** Any active human seat pauses or changes speed through the command stream, with rate limits
  (`src/sim/galactic/session.cpp:262-291`, `docs/net-galactic-clock.md`). Unchanged. The top-left SP controls
  in MP are a separate open item (legacy EAWR-5462).
  - Story-driven pauses need one rule. Stock has none outside the dialog `PAUSE` keyword (WST-83). BFC's SP plots
    pause the galaxy and play a movie at time 0 [data: BFC `Story_sandbox_56_rebel.xml`].
  - Proposal: in networked sessions a story pause becomes nothing in the sim and a per-seat presentation overlay.
- **Observers.** There are no spectator seats (`include/eawr/sim/seat.hpp:21`, seat kinds human, AI and script).
  Not needed for the milestone.

## 6. Mods

**RaW** [data]:

- 21 SP entries in 11 files; ten sets have `_Rebel` and `_Empire` variants, and Fall of the Jedi is Empire only.
- 3 MP entries in `Campaigns_Multiplayer.xml` with their own `Campaign_Set` each, unlike stock's single set.
  Clone Wars mirrors its SP map (108 planets). Rush is a 40-planet subset. BF2 has its own planets.
- 9 of the 11 SP GCs have **no** MP version at all (Core Assault, Mid Rim, Outer Rim Sieges, Triad, Carnage,
  Surrounded, Fortunes Reversed, From Ashes, Fall of the Jedi). For RaW players, co-op on SP maps is the only
  way to reach most of the mod's GCs.
- Every SP plot has a story Lua script, e.g. the Clone Wars Rebel script watches one faction's planet count and
  fires `Story_Event("Win_Event_Start")`. `Story_Event` is unbound in our galactic host, so those scripts retire
  today, in solo as well [code: `galactic_api_inventory.hpp:97`].

**BFC** [data]:

- `Campaignfiles.xml` lists only `Campaigns_Underworld_GC.xml`: 12 SP entries and **no MP GC at all**.
  So co-op on SP maps is the only way to play BFC GC in multiplayer.
- 3-faction and 2-faction sets. Story plots are XML only (no Lua): intro movie and galaxy pause, `SCREEN_TEXT`,
  `TRIGGER_AI`.
- The human's own faction appears in `AI_Player_Control` as `SandboxHuman`; we already skip human factions [code].

**Can one rule cover them?** Yes, with no per-mod code:

1. Group the non-MP, listed, non-tutorial, non-story entries by `Campaign_Set`.
2. A set's playable factions are its variants' `Starting_Active_Player` values, cross-checked with `Is_Playable`.
3. Co-op loads the chosen variant as authored.
4. Honour `Supports_Custom_Settings` (False in several RaW and BFC sets) [data].

Counting planet owners would be wrong: RaW From Ashes gives the non-playable Underworld thousands of start
forces [data]. The mods' authoring errors (a missing comma in a RaW MP planet list; one in BFC Outer Rim) stay
as authored, per the owner rule.

## 7. Blockers ranked, hours and PR order

Hours are rough worker-hours including tests and review fixes. They are not taken from measured comparable PRs,
so treat them as **assumption** (± 50 %).

| # | Item | Class | Hours |
|---|---|---|---|
| B1 | **Split session from rules.** Add a campaign-rules flavour (`single_player` / `multiplayer`) to the setup plan, separate from networked transport. Each consumer reads the right one: story `solo`, `Basic_AI` substitution, MP heroes, victory context (WGS-15..20), arbitration inputs, autosave (WGS-33 stays keyed to transport). The setup identity and agreement digest include the flavour; replay and checkpoint domains bump (no back-compat before 1.0). | hard blocker | 12 |
| B2 | **Lobby catalog and agreement for SP sets.** Group by `Campaign_Set` and list playable factions from the variants. The agreement carries the exact variant entry and the rules flavour. Lift the `Is_Multiplayer` refusals in lobby validation, agreement and scene load for that flavour; MP resume stays as is until B6. | hard blocker | 8 |
| B3 | **Arbitration under SP rules in a networked session.** Keep the D10 vote path keyed to transport. Never read the local profile or `solo_reference_human` per peer. | big | 4 |
| B4 | **Deterministic `Find_Player("local")` in co-op.** When all human seats share one faction, return that faction on every peer; otherwise nil as today. Project deviation, goes on the fidelity list. | small | 3 |
| B5 | **Story presentation per seat.** The intro holocron popup on every seat. Dialog wall clock and `PAUSE` presentation-only when networked. Story pause rewards as overlays. | small | 5 |
| B6 | **MP save resume for SP sets** plus the flavour (legacy EAWR-5373). | big, after the milestone | 15 |
| B7 | **Lobby UI.** SP sets in the GC lobby with a co-op label, faction pick, custom settings when supported. Uses the existing data-driven layouts. | small | 6 |
| B8 | **Versus on SP maps**: composition rule over variants (D2). | big, optional | 15-20 |
| B9 | **Mod story Lua**: bind `Story_Event` in the galactic story host, plus the TaskForce, desire-bonus and story-plot gaps (legacy EAWR-5484, EAWR-5494, EAWR-5495). Already GC-lane work and not co-op specific. | big, independent | (lane estimates) |
| B10 | **Story campaign and tutorials** (`LINK_TACTICAL`, `LOCK_CONTROLS`, cinematics, story opponent AI switch) for two humans. | hard, not proposed | > 60 |

**PR order:** B1, then B2, then B3 and B4 (B4 can run in parallel), then B7 (the UI can start against B2's
catalog). B5 can come before or after the milestone; B6 comes after it.

**Minimal first milestone (B1+B2+B3+B4+B7, ≈ 33 h, plus ≈ 2-4 h acceptance)**:

- Two seats in a LAN or loopback lobby pick stock FoC `Sandbox_Gateways` (Rebel variant).
- Both peers load it, story plot events fire (the intro goal and the first hero trigger), the AI uses `BasicEmpire`
  and the other authored AI types, and one galactic day passes with equal state hashes on both peers.
- One space battle against AI goes through the vote to tactical and returns.
- A headless loopback test pins this (equal hashes, story events fired, no `Multiplayer_Campaign_Heroes`).
- The owner playtest follows.
- Expected wall time: about 2 days with two workers [assumption].

## 8. Risks and owner decisions

**Risks:**

- **R1 (check now, may affect today's builds).** Under MP victory rules, "opponent controls no planets" skips
  non-human owners (`victory.cpp:165`, WGS-20), and MP installs that condition for every player (`:146`).
  A co-op team against only AI factions could therefore meet it once its 30-frame delay passes.
  - The predicate is verified in code. Whether this fires in a live co-op-vs-AI session on today's MP maps is an
    **[assumption]**: the owner's co-op playtests did not report it.
  - It needs a five-minute headless check before release. B1 avoids it for SP maps.
- **R2.** SP balance in co-op: 2 humans on one faction with the human credit bonus against SP-tuned AI may be
  easy. Difficulty and handicap stay lobby options.
- **R3.** Mods that rely on story Lua (RaW) get silent stories until B9. This is the same as solo today, but it
  becomes more visible.
- **R4.** Documentation drift: `docs/net-mesh.md` and `docs/checkpoint-format.md` say 9 human seats and 3 owners,
  while the GC roster code says 8 seats and 2 sides (`mp_roster.hpp:104-107`). Fix the docs in B2.

**Owner decisions (recommendation first):**

- **D1.** Co-op on SP maps uses **SP campaign rules** (authored AI types, story heroes, human credit bonus,
  SP victory), not MP balance. *Recommended:* yes.
- **D2.** Scope: **co-op on one faction only** first; versus on SP maps (B8) later or never.
  *Recommended:* co-op only.
- **D3.** Story campaign and tutorials stay out of co-op (filtered by `Is_Story_Campaign`, `Tutorial`,
  `Is_Listed`). *Recommended:* exclude.
- **D4.** Story popups, dialogs and story pauses show on every seat and never pause the shared sim.
  *Recommended:* yes.
- **D5.** `Find_Player("local")` returns the shared co-op faction (a project deviation). *Recommended:* yes.
- **D6.** MP save resume (legacy EAWR-5373) is scheduled right after the milestone. *Recommended:* yes.
