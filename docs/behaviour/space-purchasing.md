# Space skirmish purchasing: credits, the station build queue and hyperspace arrival

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, multiplayer-style space skirmish with
  the lobby defaults, the M2 fixture ([m2-skirmish.md](../../plan/phase-2/m2-skirmish.md)).
  Scope: station purchasing and reinforcements (legacy EAWR-530); owner decisions D2
  hyperspace-arrival scope decision (legacy EAWR-461) and
  station-purchasing scope decision (legacy EAWR-522) = A.
- Bounded question: how a skirmish player earns credits, what a level-1 space station builds and
  how its queue behaves, how a finished unit enters the battle through hyperspace, and how the FoC
  AI buys.
- Source tags: **debug build** (the FoC debug executable, read under the clean-room rule; the
  private evidence stays under ignored `out/`), **data** (a tag or value of the FoC files,
  `Data/Config.meg` and `Data/Patch2.meg` winners), **capture** (a rig screenshot of a retail
  skirmish), **owner** (a decision on an issue), **project** (a remake decision) and
  **unverified** (not established; the remake picks the least visible behaviour).
- Station levels and upgrades follow the production walk; pads and mining follow the
  build-pad walk. The [skirmish AI economy](skirmish-ai-economy.md) mounts the purchasing goals
  and supplies their completed-state inputs. Heroes remain outside this fixture.

## Interface

- Content: the economy rules (`sim::tactical::EconomyRules`), built by
  `skirmish::economy_rules` from the start and the unit tables: each player's starting credits and
  population cap, each station type's build list per faction with price, build time, queue and
  population value, each station type's income stream, the reinforcement facing per player, the
  playable bounds and the prevention radii. Like the other content tables they are passed to the
  session and are neither replay data nor state. A session without them has no economy and hashes
  exactly as before the station purchasing and reinforcements work were implemented.
- Commands (replay opcodes 9 to 11, [replay format](../replay-format.md)): buy a type at a
  station, cancel a queue entry, and bring a pooled unit in at a point.
- State: per player its credits, its two build queues and its reinforcement pool; per arriving
  unit its arrival frame and its exit point; per reinforced unit its population share. All are
  tagged blocks of the state hash, present only with economy rules.
- Snapshot: each player's credits, queues, pool and population, and each arriving unit's arrival
  frame, for presentation.
- Cadence: one economy service per frame after the frame's commands; the arrival motion runs in
  the movement phase.

## Rules

### Credits and income

| Rule | Behaviour | Source |
|---|---|---|
| PU-01 | A skirmish player starts with the lobby's credits, `MP_Default_Credits` = 6000 (the lobby allows 2000 to 8000). | data (`gameconstants.xml`); capture (retail stills, 2026-09-29: 6,535 a few minutes into a default-lobby Coruscant skirmish, consistent with 6000 plus PU-04's rate) |
| PU-02 | A station's income stream pays continuously: every frame each recipient gains `Base_Income_Value / Base_Interval_In_Secs / 30` credits. The level-1 skirmish stations author 30 per 10 s, so 0.1 per frame, 3 per second. The interval only paces the stream's bookkeeping. FoC skips the income service while a setup phase is active; the skirmish data sets `SetupPhaseEnabled` false, so M2 always pays. | debug build; data (`starbases.xml`, `gameconstants.xml`) |
| PU-03 | Recipients: the station's owner and every ally, each the full amount (`Split_Income_With_Allies`, `Full_Amount_To_Everyone`). In M2's one-versus-one that is the owner. | data; debug build |
| PU-04 | Supply dock: while the station's `HP_*_Station_One_Supply_Dock` hardpoint stands, its stream's value gains 20 (`Income_Additive_Value`; the hardpoint enables the bonus ability). A level-1 station with its dock earns 50 per 10 s, 5 per second; without it 30. | data (`starbases.xml`, `hardpoints.xml`); debug build (value = base × (1 + percentage mods) + absolute mods); capture (retail stills, 2026-09-29: 6,354 to 6,567 and 7,531 to 7,740 credits, each in 42.5 s, 4.9 to 5.0 a second) |
| PU-05 | The stream ends with its station. | debug build (streams belong to their creator object) |
| PU-06 | The losing-team credit bonus (`Multiplayer_Losing_Team_Bonus_Credit_Percentage`) needs three non-empty teams, so it never pays in a one-versus-one. | debug build |
| PU-07 | Credits are a real number in FoC. The remake holds them in Q24 fixed point and pays each frame the stream's per-frame amount rounded to the nearest Q24 step (0.1 is off by less than 3·10⁻⁸ a frame). The HUD shows whole credits, rounded down. | project; unverified (HUD rounding) |

### The build menu and queue

| Rule | Behaviour | Source |
|---|---|---|
| PU-10 | A station builds the types its `Tactical_Buildable_Objects_Multiplayer` lists for its owner's faction, in list order. Level 1 (the M2 stations): Rebel `Rebel_X-Wing_Squadron`, `Y-Wing_Squadron`, `RS_Enhanced_Shielding_L1_Upgrade`, `RS_Improved_Weapons_L1_Upgrade`, `RS_Level_Two_Starbase_Upgrade`; Empire `TIE_Interceptor_Squadron`, `TIE_Bomber_Squadron`, `ES_Enhanced_Reactors_L1_Upgrade`, `ES_Reinforced_Armor_L1_Upgrade`, `ES_Level_Two_Starbase_Upgrade`. | data (`starbases.xml`) |
| PU-11 | A type can be produced when: it is in the station's list (PU-10); the station is live and allied to the buyer; its build limits allow another (none of the M2 types authors one); its price and build time are positive; and the buyer owns every `Tactical_Build_Prerequisites` type (none authored). The galactic tags (`Required_Star_Base_Level`, `Tech_Level`, `Build_Initially_Locked`) are not checked: the Y-wing and TIE bomber, galactic level 2, build at a level-1 skirmish station. | debug build; data |
| PU-12 | Price: `Tactical_Build_Cost_Multiplayer` in a multiplayer-style game (`Tactical_Build_Cost_Campaign` is the campaign price). M2: X-wing 500, Y-wing 550, TIE interceptor 550, TIE bomber 550. No price modifier applies in M2. | debug build; data |
| PU-13 | Build time: `Tactical_Build_Time_Seconds` × `Tactical_Build_Time_Multiplier` (1.0); for an AI player also × its difficulty's `Space_Build_Time_Multiplier` (Normal 1.0). M2: X-wing 15 s, Y-wing 17 s, TIE interceptor 15 s, TIE bomber 17 s, that is 450 and 510 frames. | debug build; data |
| PU-14 | Each player has two queues, shared by its stations: units and upgrades (the type's `Tactical_Production_Queue`, `Tactical_Units` or `Tactical_Upgrades`). A human player's queue holds at most `Max_Build_Queue` entries, 5 (FoC's data does not set the constant, so its built-in default holds); an AI player's has no limit. | debug build |
| PU-15 | Paying: `Pay_As_You_Go` is false, so the whole price is taken when the entry is queued; the buy is refused, and nothing changes, when the buyer has fewer credits than the price. | data (`gameconstants.xml`); debug build |
| PU-16 | Only the front entry of a queue builds. It completes in the first frame at least its build time after it became the front (an entry queued into an empty queue becomes the front at once); the next entry becomes the front in that frame. | debug build |
| PU-17 | Cancel: the player may cancel any entry of either queue. It is removed and its whole price refunded (not pay-as-you-go). Cancelling the front makes the next entry the front in that frame. | debug build |
| PU-18 | Every frame, before the fronts build, each entry is checked against PU-11 again (the build limits then allow the entry itself). An entry that fails, for example because its station was destroyed, is removed: a human player gets no refund, an AI player gets the price back. | debug build |
| PU-19 | A completed unit joins its owner's reinforcement pool (PU-30). A completed upgrade object is created on its station; M2 builds none (PU-20). | debug build |
| PU-20 | M2 shows the level-1 upgrades and the level-2 station upgrade in the menu but never buys them (project, station purchasing and reinforcements scope; PU-G1, PU-G2). | project |
| PU-21 | Population: a player's space cap is its faction's `Space_Tactical_Unit_Cap` (Rebel 25, Empire 20). A unit brought in from the pool counts its type's `Population_Value`; a squadron's craft share it equally, so a squadron with craft left still counts a fraction. The count rounds any fraction up (`Allow_Reinforcement_Percentage_Normalized` = 0). Buying ignores the cap; bringing a unit in needs room: its value at most the cap minus the count. | debug build; data (`factions.xml`, `gameconstants.xml`) |
| PU-22 | The starting forces and station-launched squadrons do not count toward the population. | capture (the retail HUD shows 0/25 with the free X-wing squadrons alive); unverified (the debug build registers some starting forces; the station purchasing and reinforcements (legacy EAWR-530) recording checks it) |

### Hyperspace arrival

| Rule | Behaviour | Source |
|---|---|---|
| PU-30 | The pool lists the owner's completed units in completion order. A unit leaves it when its owner brings it in at a point (the command bar's reinforcement drag in FoC). A pooled unit waits until then; nothing arrives by itself. | debug build |
| PU-31 | A point is valid for a unit when: the point is not fogged for the player; it is outside every non-allied object's `Reinforcement_Prevention_Radius` (the stations author 2000), measured in the plane; it is inside the map's playable bounds; and its arrival lane is clear (PU-32). | debug build; data |
| PU-32 | The arrival lane sweeps rectangular hard extents from the point minus the authored `Space_Reinforcement_Collision_Check_Distance` along reinforcement facing to the point, over the current frame through current frame + 115. Layer bypasses, layer-less squadron formation extents, corvette filtering and the static sweep follow WR-24..28 in [the reinforcements walk](walks/reinforcements.md). | debug build WR-E01, WR-E36; data |
| PU-33 | An order for a type not in the pool, without population room (PU-21), or at an invalid point is dropped and the unit stays in the pool. | debug build |
| PU-34 | Bringing a unit in creates it at the point, on the plane (height 0), and raises it by its type's `Layer_Z_Adjust` (space-movement LZ-01; PL-08 sets the point's height to 0 and creation then applies LZ-01), facing the player's reinforcement facing: the yaw of its SK-11 spawn marker. A single ship is created on the point without a search (PL-08). A squadron's craft are searched one after another from the point with the shared free-space search (start angle 0, the point as the fallback, every live unit with a placement box blocking, PL-02 to PL-06) and each is raised by its own height, then its team container. The unit leaves the pool and counts toward the population at once. | debug build; project (the blockers' heading is taken from each unit's rotation) |
| PU-35 | Motion: the unit comes out of hyperspace along its facing. It starts at the point minus the facing times D, D the sum of the table below, and in arrival frame k (k = 1 in the frame after it was created, up to 149) moves d(k) along its facing. d(k) = 0 for k < 25; 750 for 25 ≤ k ≤ 34; 750 − 741.875·E((k − 34)/10) for 35 ≤ k ≤ 44; 8.125 for 45 ≤ k ≤ 119; 8.125 − 8.125·E((k − 119)/30) for 120 ≤ k ≤ 149. E is the ease-out curve E(t) = 4t/3 for t ≤ 1/2 and 2/3 + (8/3)(t − t²/2 − 3/8) above. In frame 150 the unit is set on the point with its facing and the arrival ends. Ships and craft fly the same table. | debug build |
| PU-36 | The unit is hidden (no model, no sound) until arrival frame 35, when the faction's arrival sound plays and the model appears, fading in from black over 115/120 s. | debug build |
| PU-37 | Ordinary damage is blocked until arrival frame 35; privileged script/cheat and vehicle-thief damage bypass invulnerability (WR-33). The enemy cannot see or target it while it is hidden. | debug build WR-E12, WR-E46, WR-E47 |
| PU-38 | From creation, elevated vulnerability lasts for the independently timed `Space_Elevated_Vulnerability_Duration` (5 s, 150 frames), using `Space_Elevated_Vulnerability_Factor` (−3). An ordinary hit takes four times its damage, adding to DG-26's out-of-combat term. With PU-37 the default covers frames 35 to 149; a longer authored duration survives landing (WR-41). | data; debug build WR-E37 |
| PU-39 | Its movement is locked until frame 150; then it idles on the point (FoC issues a move to its own position). The remake also holds its fire and rejects its orders until then. | debug build (movement); unverified (fire and orders; PU-G7) |

### The AI

M2 supplies the skirmish context and economy inputs described by SAE-01..08.
The AI's purchases and reinforcements use these same command paths, and the viewer
preloads bounded purchase-model slots for both human and AI players.

| Rule | Behaviour | Source |
|---|---|---|
| PU-50 | The FoC AI buys through its goal system: `BasicOffensiveSpaceSet` includes the global space goal `Tactical_Multiplayer_Build_Space_Units_Generic` (High_Priority), whose plan reserves 0 to 1 station upgrade and 0 to 3 units of the faction's space types (`Produce_Force`), then waits until the player has 2000 credits (tech level 1) or 30 s have passed. | data (`offensivespacegoals.xml`, `basicoffensivespaceset.xml`, `tacticalmultiplayerbuildspaceunitsgeneric.lua`) |
| PU-51 | An AI player's entries use PU-10 to PU-19 without the queue limit and with the difficulty's build-time multiplier. | debug build |
| PU-52 | An AI player's completed unit joins its pool and its free store gets a reinforce reservation for it; the AI's plans bring it in. | debug build |

### The command bar (presentation)

Presentation only: none of this is simulation state. A click becomes a buy, cancel or reinforce
command through the order scheduler (UI-07), so it enters the replay like any order.

| Rule | Behaviour | Source |
|---|---|---|
| PU-60 | While the selection holds a live unit of the local player whose type has a build menu for the player's faction, the first such unit is the production object. Its build buttons replace the unit cards in the card slots (`s_select_00` on), one per menu entry in list order, up to the slots. The ability buttons are hidden. Otherwise the cards show. | debug build |
| PU-61 | A button is disabled, and tinted grey (128, 128, 128), when its type cannot be built now (PU-11; in M2 also the entries of PU-20), when the player cannot afford its price, or when its queue is full. Its state is 2 when unaffordable, else 0 when the queue is full, else 3. A left release on an enabled button buys its type at the production object. | debug build |
| PU-62 | A button shows its type's icon (`Icon_Name`, `i_button_temporary.tga` without one) and its price in whole credits as its text. The text is white at alpha 200 while the queue has room, else grey at alpha 200. The remake draws the price where a card's count sits (unverified). An entry the session never builds (PU-20) shows its `Tactical_Build_Cost_Multiplayer` from the data, as the retail capture does (850, 800 and 2000 for the Rebel upgrades and level-2 station). | debug build; project (the text position); capture (the upgrade prices) |
| PU-63 | The build queues show in the `tqueue` slots: the units queue in `tqueue05` to `tqueue09`, the upgrades queue in `tqueue00` to `tqueue04`, front first. Each slot shows the entry's icon tinted with `Right_Queue_Tint` (255, 128, 128, 215). The left or right tint is chosen by a component test that always picks the right one (unverified on screen). A right release cancels that entry and refunds its price (PU-17); a left release focuses its producer and keeps the entry (the remake currently only keeps it). | debug build (queue update, action table and cancellation dispatch); data (`gameconstants.xml`); owner observation on the build-queue dial and cancel report (legacy EAWR-982), 2026-10-01 |
| PU-64 | The front entry of each queue shows its completed fraction and "<n>%" (truncated) as its text; the others show no text or dial. The front's `Build_Texture_Name` draws additively as a clock sweep, growing from empty to full as production completes. FoC also flickers a queued slot's alpha now and then (5 % of updates, 15 to 25 lower); the remake does not (PU-G14). | debug build (front-only update and component dial); owner observation on the build-queue dial and cancel report (legacy EAWR-982), 2026-10-01: growing sweep |
| PU-65 | `Text_Credits_tactical` shows the whole credits rounded down (PU-07), right-justified, with its money icon. The pane's population text shows "<used>/<cap>" (PU-21). | debug build; data |
| PU-66 | The reinforcements button (`b_reinforcement`) opens and closes the reinforcement pane, the `i_main_reinforce` shell, with its close button (`r_close`). The pane counts completed entries by type-definition identity, then traverses those keys into its slots `r_RRCC` (4 per row, 20 in all). WR-08/EUS-14 establish completion-order independence within one loaded definition set. The remake uses stable `TypeId` keys; exact stock order across loads remains unverified. Each slot shows the type's icon and "x<n>" when it holds more than one, and it is greyed and disabled when the type's population exceeds the room left under the cap. | debug build EUS-14; data |
| PU-67 | The pane shows its filled rows (rows past the first = `min((n - 1) / 4, 4)`) at the top left of the screen, 100 shell units down. Its shell alternate is that row count (`ALT0` through `ALT4`), independently of faction. The close button moves down with the rows (46 shell units per row), keeping it in the bottom strip of the selected background. Fresh retail placement confirmation remains pending. | debug build (row-count alternate selection and close-bone offset); data (five reinforcement shell alternates) |
| PU-72 | In battle, the reinforcement close button resolves `TEXT_BUTTON_CLOSE` from the localized text database. Its component supplies the normal, hover and pressed textures, `Scale`, font, point size, text colour and offset, outline, emboss and `Swap_Texture`. State art is centred on the shifted close-button bone at texture size times component scale; input uses that same visible rectangle. Setup-phase `TEXT_BUTTON_BEGIN` is outside the battle pane's scope. | debug build (reinforcement label selection and button draw); data (close-button component and text database); project (matching input to the visible art) |
| PU-68 | Hold the left button on an enabled reserve slot, drag its preview models onto the battle plane, and release to request arrival (WR-11..15). Validity tints the models green or red. An invalid release cancels the drag and keeps the reserve; a right click also cancels. The authoritative command rechecks admission. | debug build WR-E04, WR-E09, WR-E38, WR-E41 |
| PU-69 | A unit arriving through hyperspace is not drawn before its frame 35, for any player (PU-36). From then on it is drawn at once; FoC fades its model in (PU-G14). | debug build |
| PU-70 | The reinforcement button stays visible at the shell's `b_reinforcement` anchor in the sidebar beside the minimap, including with an empty pool. It is disabled when the pool is empty, reinforcement permission is absent, or victory is pending. Draw its `Blank_Texture_Name` backing, then the space entry of `Icon_Alternate_Texture_Name` (the first two entries name the same reinforcement icon). Hover adds `Mouse_Over_Texture_Name`; press uses `Selected_Texture_Name` with `Selected_Alpha` and a 0.2 s fade; disabled adds `Disabled_Texture_Name`. | debug build (space button update, button draw and selection fade); data (shell anchor and component art); owner location clarification on the invisible reinforcement button report (legacy EAWR-981), 2026-10-01 |
| PU-71 | Each addition to the local player's reinforcement pool starts a continuous component flash. The completion route passes `CB_Flash_Count` but requests duration -1, which selects continuous flashing and bypasses the count; neither `CB_Flash_Count` nor `CB_Flash_Duration` limits this notification. The component's default pulse lasts 0.5 s and fades `Flash_Texture_Name` additively, repeating until opening the pane or disabling the button stops it. An opponent's completion, queue cancellation and deployment do not start a notification; adding another copy of an existing type does. | debug build (pool addition, flash dispatch, component flash flag/service/draw, space button update); coordinator confirmation of the deeper trace (2026-10-01) |

For PU-70's selected state, the debug build applies `Click_Shift` as one shell unit down
and right to the button art. `Selected_Alpha` chooses alpha blending for the selected
overlay; without it that overlay adds. The 0.2 s selected fade reduces its RGB intensity.
These two data flags are applied to the reinforcement button; their use on other command
bar components remains outside this change.

## Cases

| Case | Input | Expected |
|---|---|---|
| PC-01 | M2 start, no orders, 300 frames | Each player has 6000 + 300 × 5/30 = 6050 credits (dock standing). |
| PC-02 | The Rebel buys an X-wing squadron at tick 10 | 5500 credits after tick 10's commands; the squadron joins the pool in the frame 450 frames later. |
| PC-03 | Buy X-wing, buy Y-wing, cancel the X-wing 100 frames later | Both prices paid, the X-wing's 500 refunded; the Y-wing becomes the front then and completes 510 frames after the cancel. |
| PC-04 | A human queues six units | The sixth is refused (PU-14). |
| PC-05 | Bring a pooled X-wing squadron in at a clear, revealed point | The craft are hidden and invulnerable for 34 frames, visible from frame 35, rest on the point at frame 150 and take orders from then; the Rebel population goes from 0 to 1. |
| PC-06 | Bring a unit in within 2000 of the enemy station | Dropped; the unit stays in the pool. |

## Unknowns and fidelity list

- PU-G1: the station's level-up (`Next_Level_Base`, the level-2 to level-5 menus, corvettes and
  frigates) is not in M2.
- PU-G2: the station upgrade objects (shielding, weapons, reactors, armor) are not in M2.
- PU-G3: build pads, mining facilities and the income they add are not in M2 (SK-32 keeps the
  pads inert).
- PU-G4: heroes (`Han_Solo_Team_Space_MP` and the other MP heroes of the higher levels).
- PU-G5: the arrival lane is an end-point test, not FoC's 200-unit sweep over the arrival frames.
- PU-G6: a squadron's craft arrive at their formation offsets, not at FoC's free-space search
  within 2500 of the point.
- PU-G7: whether an arriving unit fires or accepts orders before frame 150 (the remake: no).
- PU-G8: `Skirmish_Reinforcement_Delay_Frames` (90) is read by the debug build for the
  reinforcement bar; its effect on a skirmish placement is not established and not modelled.
- PU-G9: PU-22 (starting forces outside the population count) rests on one HUD capture.
- PU-G10: the arrival's hyperspace effect (the flash and streak particles) is presentation; its
  data source is not established here.
- PU-G11 is implemented: station hardpoint repair (HR-01 to HR-05, WSL-40 to WSL-42)
  uses replayable commands and an ordered credit reservation with partitioned per-frame service.
- PU-G13: the command bar's hover texts (the production tooltips: queue full, population cap,
  insufficient funds) and the button sounds are not drawn or played.
- PU-G14: presentation flourishes not modelled: the queue slots' alpha flicker (PU-64) and the
  arriving model's fade-in (PU-69).
- PU-G15: placement shows no ghost models and no valid-area overlay; an invalid point is refused by
  the simulation (PU-33) with no on-screen feedback.
- PU-G16 closed by the reinforcement-button visibility fix (legacy EAWR-981): PU-70 sources the authored sidebar anchor, alternate-only idle icon,
  layered states and empty-pool disable from the debug build and data. The previous production
  panel had a hit target but no button draw. The 2026-09-29 retail still did not settle the
  button's position; the owner's 2026-10-01 clarification identifies it beside the minimap.
  A retail capture comparing idle, hover, press, disabled and completion flash appearance is
  still requested; this is visual verification of the sourced behavior, not a placement guess.
- PU-G17: retail draws each build button on a bronze pad, and the price in a larger bold face;
  the remake draws the icon alone. The pad is probably the slot's faction `Blank_Texture_Name`
  (`i_button_pad_rebel.tga`, `i_button_pad_empire.tga`), but when retail draws it is not
  established.
- PU-G18: a replay does not record whether it runs with the economy. `sim_headless` and the
  viewer's replay path apply it when the M2 start rebuilt from the game data has the replay's
  players and units, so an M2 replay recorded before the station purchasing and reinforcements work were implemented now replays with credits and different
  hashes. The content identity covers the production data but not the on/off switch, the bounds or
  the queue length; a header flag belongs with a stable replay format.
- PU-G19: the elevated vulnerability (PU-38) lasts at most to the end of the arrival (frame 150);
  a `Space_Elevated_Vulnerability_Duration` above 5 s is cut there. FoC applies the same modifier
  outside arrivals too, which M2 does not reach.
- PU-G20: scripted damage (the damage command) is not blocked on a hidden arrival; PU-37 holds for
  projectile hits only. No M2 script issues it.
- PU-G21: the playable bounds of PU-31 are the map's declared extents about the origin; that they
  equal FoC's playable bounds is not verified.
- PU-G22: income fields read but not modelled for mining facilities in the space build-pad work (legacy EAWR-541):
  `Income_Multiplier`, `Interval_Multiplier`, `Reverse_Application_Logic` and
  `Affects_All_Allied_Sources` on the bonuses; `Split_Income_With_Allies`, `Full_Amount_To_Everyone`
  and `Split_Favors_Owner` (the recipient rule is fixed at the full amount to the owner and its
  allies) and `Allow_Reinforcement_Percentage_Normalized` (rounding up is fixed).
- PU-G23: build limits and prerequisites are not checked (PU-11: the four M2 types author none), a
  completed upgrade or structure has no effect yet, and the human queue length is the built-in 5,
  not a `Max_Build_Queue` a mod may set. The station-upgrade and space build-pad work owns these gaps (legacy EAWR-540, EAWR-541).
- PU-G24: the pane's population text (PU-65) has no place to draw: FoC's data defines `r_pop_text`
  but its `i_main_reinforce.alo` has no bone of that name, so where retail shows it (if anywhere)
  is not established. The retail HUD shows "used/cap" in the planet panel (`Text_Pop_Cap`) next to
  the credits; the remake's planet panel draws neither the population nor retail's credit symbol.
- PU-G26 (unverified): that FoC's reinforcement creation call sets the `Layer_Z_Adjust` raise flag is read from
  LZ-01's list of creation paths (production and reinforcements among them), not from the reinforcement call itself; confirm
  it in the debug build. The remake raises a bought ship; no buyable M2 type has a height, so nothing visible differs today.
- PU-G25: the viewer composes every model at the start, so a bought unit is drawn in a slot made
  for it then: per buyable unit type up to min(population cap / its population, 10). A slot whose
  unit has left the simulation (and whose death clone has finished) goes to the next unit of its type,
  so the limit is what stands at once, not how many were ever bought; a slot count that still runs out
  (ten of one type alive) leaves the next unit simulated but not drawn.
- PU-G27: fresh retail eye confirmation of the reinforcement pane with one and several ready
  types remains pending (legacy EAWR-1045). Row alternates, close-button geometry and the localized
  label follow the GUI data and debug build; the first two capture attempts did not show the pool.
