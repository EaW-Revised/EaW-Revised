# Space battle audio and speech

## Scope and evidence

This walk inventories tactical space sound dispatch, command responses, combat warnings,
announcements, attached ambience, music and mixing. It describes audible behaviour rather
than the original implementation. The simulation supplies immutable facts; presentation
owns playback. Audio must not change replay hashes.

`AU-01..37` already identify evidence in [battle audio](../battle-audio.md), so this walk
uses **SND** rule IDs and **SND-E** private evidence IDs. Original symbols, addresses and
raw results live only in ignored `out/research/audio/`. `SND-X01` is an effective XML
inventory; `SND-X02` is the tag-status snapshot. XML proves authored values, not that a
runtime branch executes. A claim marked **unverified** is an explicit research gap.

Comparisons initially use the walk's base revision `35568aeae490761fd8f2a70d0512d5ff0688c3c4`.
The gap table identifies concurrent implementation work separately; it is not treated as
merged behaviour. The standalone-announcement audit below updates SND-35/38/41/42/44/48/49/61/62/64
against main `300246a9678da2c949abde869990a41558f89446`; other comparisons retain their
stated base. File abbreviations below are:

- **core**: `src/presentation/audio/sfx.cpp`.
- **core policies**: `include/eawr/presentation/audio/announcements.hpp`.
- **prepare**: `apps/viewer/src/battle_audio_prepare.cpp`.
- **events**: `apps/viewer/src/battle_audio_events.cpp`.
- **mix**: `apps/viewer/src/battle_audio.cpp`.

## Service order and dispatch

The outer loop services the audio backend, SFX, music, speech, speech conversations,
SFX conversations and weather in that order, before the logical-frame gate
([battle flow](battle-flow.md), WBF-13). Object updates and input enqueue cues; they do
not imply one global per-frame ordering of every audible cue. The SFX manager sorts active
events by spatial distance, updates playing samples, advances waiting event states, then
reclaims completed events. Delay counters use elapsed milliseconds; combat music and
base-attack announcement timers use logical frames. Pause has explicit category hooks
(WBF-53), so a stopped simulation clock alone is insufficient.

For an ordinary attached request, replacement detachment precedes admission. Admission
then checks preset/play-count refusal, cinematic negative feedback, disabled categories,
demo/dialog and HUD-stream rules, instances/duplicate attached loops, overlap, probability,
delete-pending attachment and the optional separate fog-test object. Sequential-list
synchronization and event enqueue follow those gates. Backend sample allocation happens
later. SND-E21/E33 establish this order; the tables group related contracts under stable IDs.

| ID | FoC rule, in evaluation order | Source / earlier note | Ours / gap owner |
|---|---|---|---|
| SND-01 | Load `SFXEventFiles` in file order, presets first. Defaults: 3D, priority 3, probability 100, play count 1, max instances 1, volume/pitch 100, pan 50, saturation distance 300, zero pre/postdelay and loop fades; all classification flags false except 3D. | Debug build AU-01..02; SND-E01; BA-01..02 same for their covered fields. | **Same** defaults used by core; pan and delay execution are SND-08 gaps. |
| SND-02 | Apply children in document order. `Use_Preset` copies the preset, retaining the new name and clearing preset status; skip empty and `TBD`. Priority clamps to 1..5, probability 0..100. Initial pitch normalization clamps the minimum to 50..200 and maximum to 0..200; the subsequent range normalization can lower that minimum again. Volume and pan clamp to 0..100; loop fades/delays clamp nonnegative and saturation distance to at least 0. Inverted volume/pitch/pan ranges lower the minimum to the maximum, then raise the maximum if needed. Integer fields accept a leading integer; one-byte storage precedes these clamps. | AU-01..02; BA-01..02 same. | **Differs**, core `SfxRegistry::add` matches normal authored ranges and volume ceiling but raises the maximum on inverted ranges, and parses wide integers before clamping. Existing loader audit owns malformed-range/byte and duplicate policy. |
| SND-66 | Initial loading uses case-normalized event-name keys, reports duplicate definitions and retains the first inserted event. The distinct changed-file reload path replaces existing event data in place, preserving its index; it must not be generalized to initial duplicate loading. | Debug build SND-E45; BA lacks duplicate contract. | **Differs**, core `SfxRegistry::add` uses `insert_or_assign`, replacing an earlier same-name initial definition; existing loader audit. |
| SND-16 | Before instance/overlap checks, reject the configured GUI-negative-feedback event during a cinematic, then disabled localized, unit-response, HUD or ambient VO categories in that order. Ordinary HUD VO is refused while the speech-stream list is nonempty; tactical tutorial HUD VO is also suppressed. Demo/dialog dispatch admits GUI events only and excludes negative feedback. Unit-response VO has no corresponding speech-stream exclusion. | Debug build SND-E21/E24; AU-03 mentions gates without full predicates. | **Same** category and mode admission in core `Voices::start`; viewer supplies retained MP3 activity and otherwise uses ordinary skirmish defaults. Disabled-category and cinematic/tutorial/demo facts have an explicit caller interface; settings UI remains separate. |
| SND-03 | Presets and play count 0 cannot start. Ordinary admission rejects max instances 0 and a 2D event at its instance limit; counts include queued/delayed events and exclude those fading to silence. A loop already attached to the same object is refused. The 2D backend also refuses a sample whose starting gain is zero. | Debug build SND-E21/E23/E34; AU-03; BA-03..04 omit queued/attached-loop and backend-gain details. | **Same** event admission counts queued/delayed instances, excludes fades and rejects same-source loops; backend zero-gain refusal remains distinct. |
| SND-04 | Ordinary admission refuses an active matching `Overlap_Test`, then draws 1..100 for probability below 100 and accepts only a roll at most that value. Reject a delete-pending attachment. An explicit visibility-test object is fog-tested after admission, except story cinematics. The attached-event overload does not provide that separate visibility object; it uses SND-11's ongoing gain instead. | Debug build SND-E21/E33; AU-03; BA-05..07 miss the distinct object roles. | **Same** queue admission retains overlap/probability and distinct explicit visibility/attachment roles; attached fog feeds ongoing gain. |
| SND-17 | A forced request bypasses instance/attached-loop, overlap, probability, demo/dialog and HUD stream/tutorial admission checks. It still rejects presets, play count 0, disabled categories, cinematic negative feedback, a delete-pending attachment and a fogged explicit visibility object. Force does not mean every gate is disabled. | Debug build SND-E21; missing from BA. | **Partial**, core requests expose forced admission with these bypass boundaries; the viewer currently dispatches ordinary requests. General queued/attached-event lifecycle remains separate. |
| SND-05 | Each event loop draws volume, pitch and 2D pan uniformly from its min/max ranges. Main, pre and post sample lists have sequential cursors when `Play_Sequentially`; otherwise choose a sample uniformly. At admission, equal-sized pre/main or post/main lists synchronize that stage's cursor with the main cursor. Pitch percent scales playback rate. | Debug build SND-E02/E21; AU-04; BA-08 covers main selection/volume/pitch only. | **Same** loop draws and independent sequential stage cursors, synchronized at admission; backend panning law remains unverified. |
| SND-06 | A 3D event at `Max_Instances` can replace its farthest active instance only if the new sound is no farther. With all 32 spatial slots occupied, choose the highest priority number, then farthest at equal priority; the stored candidate distance is not refreshed when priority changes. Steal only when candidate priority is no more important and candidate is farther. | AU-05..08; BA-05, BA-09 same. | **Same**, core `Voices::start`, 32 spatial slots. |
| SND-07 | 2D and 3D use separate finite pools, 16 versus 32. At a full 2D pool, choose the highest priority number (least important), then oldest start time at equal priority; equal timestamps retain the first encountered slot. Replace it if its priority number is at least the incoming number; equal priority can interrupt. This occurs at sample allocation, after event admission. | Debug build SND-E22/E23; BA-09 has pool sizes, missing 2D stealing. | **Same** bounded 16-slot replacement in core `Voices::start`, using backend timestamps from the viewer. Reports distinguish requests, admitted events, allocated slots, refusals, steals and decoded starts; the general deferred queue remains SND-18 work. |
| SND-18 | A successful ordinary event request queues an event and returns its identifier, before file decoding or sample-slot allocation. Queued 2D events enter at the list head, 3D at the tail, then service uses the distance ordering. Later missing samples or unavailable slots can end an accepted event without audible playback. Caller success, event admission and backend playback are distinct observations. | Debug build SND-E21/E23 plus SND-E02; missing from BA. | **Same** event identifiers precede decoding/allocation, with 2D head insertion and spatial distance ordering; reports separate admission/allocation/decoded starts. |
| SND-08 | Event loop: draw loop values and pre/post delays; predelay, optional pre sample, main sample, optional post sample, postdelay, then increment completed-loop count. `Play_Count=-1` repeats; positive counts repeat until satisfied. Main playback count is one for finite events and continuous for `Play_Count=-1`; finite completed event loops enforce the authored count. | Debug build SND-E02; missing complete lifecycle in BA. | **Same** engine-free event stages execute elapsed-ms delays and finite completed loops; infinite main playback uses a continuous backend sample. |
| SND-09 | At completed event end, an explicit runtime chained 2D event takes precedence over `Chained_SFXEvent`; the authored chain runs only if no runtime chain ran. Both source and destination must be 2D. Chaining re-enters normal admission. | SND-E02; BA fidelity list only. | **Same** runtime 2D chains precede authored chains at full event completion and re-enter admission, including stolen/failed samples; explicit cancellation removes chains. |
| SND-10 | Spatial sample minimum distance is `max(1, Volume_Saturation_Distance * 1.5)` in stock space, maximum distance 15000, rolloff factor 2.0. The factors come from `Audio_Space_3D_Saturation_Distance_Mod` and `Audio_Space_3D_Rolloff_Distance_Mod`. Miles supplies attenuation; the inverse-distance formula and behaviour beyond 15000 remain a library assumption requiring a measurement. | AU-06, AU-09; XML SND-X01; BA-10..11 same, including its uncertainty. | **Same** configured factors and the documented approximation, core `falloff_gain`; SND-U02 retains library uncertainty. |
| SND-11 | Attached 3D audio follows the object's height-adjusted target position. Its visibility multiplier is zero if attached audio is silenced, the model is hidden, or the local player fogs it; story cinematics bypass that fog test. Fixed-position and ordinary 2D events do not use this attached-object multiplier. | Debug build SND-E03; BA-13 incomplete distinction. | **Partial** all live spatial attachments refresh logical fog/arrival hiding and visible height-adjusted poses, with explicit silence/story-cinematic interface facts; hidden pose interpolation remains unverified. |
| SND-12 | Space listener projects the view line toward the plane at `Audio_Space_3D_Listener_Z_Pullback_Dist` (60), clamping projection distance to absolute camera height over the plane; flattened forward supplies orientation. A cinematic uses camera position. | AU-10..12; BA-12 same. | **Same** normal-camera path, core `listener_for_camera`; cinematic listener is outside current viewer modes. |
| SND-13 | Pause/resume explicitly pauses positional SFX, looping 2D SFX, speech, movies and weather; outer audio service remains reachable. | WBF-53, debug build BF-E97/E102; same earlier time-control rule. | **Same** covered categories: events `update_pause` holds spatial voices, infinite 2D loops and retained speech; preserves slots, chains, fade progress and start grace; resumes through the same players. Ordinary 2D one-shots and music continue. Movies/weather are outside this viewer. |

## Command responses

Unit responses and advisor/HUD acknowledgements may be **WAV SFX events**. They are not
automatically MP3 `SpeechEvent` streams. Ownership belongs to each caller's rule;
`Is_Unit_Response_VO`, `Is_HUD_VO`, `Is_Ambient_VO`, `Is_GUI` and `Localize` also influence
dispatch. Do not infer owner/allies/all from the sound's 2D/3D classification.

| ID | FoC rule | Source / earlier note | Ours / gap owner |
|---|---|---|---|
| SND-14 | One ranked selected speaker: first matching category in `Unit_Command_Rankings_By_Category`, then `Ranking_In_Category` (default 25). Preserve the traced loop's lower-ranking takeover even from a worse category. Resolve team members through their current leader for ranking/select/move/attack getters. | AU-19..21, AU-32; BA-20, BA-24 same. | **Same**, core `speaker`, events `respond` and `live.squadron_members()`. The BA-24 tick-zero-only caveat is a stale-note audit, not a new established runtime gap. |
| SND-15 | Select acknowledgement uses that ranked speaker's `SFXEvent_Select`; single-unit selection checks local ownership and retreat before entering group acknowledgement. A retreating ranked speaker is silent. Click/box/type/control-group selection callers request the acknowledgement. | AU-19, AU-23; SND-E04; BA-21 incomplete retreat/owner detail. | **Differs**, events `respond` has select routing but no matching retreat silence. |
| SND-20 | Group attack takes an optional supplied speaker, otherwise the ranked selection. Try speaker's `SFXEvent_Attack_Hardpoint` indexed by target hardpoint **Type**, then `SFXEvent_Group_Attack` when selected count exceeds one, then target-type override, then ordinary attack. Missing hardpoint line falls through; a found line whose start is refused does not fall through. | Debug build SND-E05; BA-22 incomplete; XML SND-X01. | **Missing** hardpoint/override branches in events `respond` and prepare; command-response gap. |
| SND-21 | Single-unit attack requires the attacker's local ownership and non-null attacker/target. Try hardpoint-type line, target-type override, ordinary attack; no group or random-assist branch. | Debug build SND-E06; missing there. | **Missing** distinct presentation contract; current events `respond` consumes local UI order summaries. |
| SND-22 | After an admitted group move/attack primary (SND-18), draw a random other selected object. Chain its `SFXEvent_Assist_Move`/`SFXEvent_Assist_Attack` only if it is not the speaker, is a different unit type, and lacks vehicle-thief ejection ability. A refused primary yields no assist; a failed eligibility test does not retry the draw. | Debug build SND-E05/E07; BA fidelity list incomplete conditions. | **Same** for local space selections: events `respond` tests the one random candidate after successful primary start and chains its 2D assist at completion (BA-29). |
| SND-23 | Stop and guard select the highest-ranked speaker and start its `SFXEvent_Stop` or `SFXEvent_Guard`; a missing entry is silent. Neither handler requests a group variant or assist. | Debug build SND-E08/E09; BA fidelity list. | **Missing**, events `respond` does not route these response tags. |
| SND-24 | For a group space move with a destination, test asteroid-field containment first; otherwise test nebula containment. Try the corresponding `SFXEvent_Move_Into_Asteroid_Field`/`SFXEvent_Move_Into_Nebula`; absent chosen line falls back to group move if count >1, then ordinary move. A point inside an asteroid field does not also test nebula when its line is absent. Chain an eligible assist only after start succeeds (SND-22). | Debug build SND-E07; BA-22 incomplete; hazard containment is [hazards](hazards.md). | **Same** for local space UI destinations: events `respond` queries bound live static hazard footprints, preserves asteroid-first fallback and retains the admitted assist chain (BA-29). |
| SND-25 | Single-unit space move checks local ownership and destination hazards with the same asteroid-before-nebula test, then ordinary move. It does not select group move or assist. Fleet-move fallback belongs to galactic mode. | Debug build SND-E10; missing there. | **Same** for one-unit local space UI orders: the environmental response precedes ordinary move and adds no assist; galactic fleet movement remains separate. |
| SND-26 | Ability command-button/key voice uses the first pressed eligible unit's `SFXEvent_GUI_Unit_Ability_Activated`/`Deactivated`; it is a plain 2D start, bypassing speaker ranking and assist. Script/AI switches have toggle sounds but no command-button voice. Timed natural expiry and relationship-sensitive faction toggle sounds follow BA-50..52. | AU-33..37; BA-50..52 same; [abilities](abilities.md) owns activation internals. | **Same**, prepare ability voice lookup and events `voice_ability`/toggle route for modelled abilities. |
| SND-27 | `Preset_UR` is 2D, unit-response VO, priority 1, one instance, volume 90. `Preset_URC` is priority 2, probability 50, volume 60, pitch 100..110. Type-specific `Overlap_Test` can block a second line. There is no universal response cooldown or single-global-unit-speaker gate in event admission: distinct response events may overlap unless event/overlap rules or sample-pool allocation prevent it. WAV responses do not enter the MP3 speech-stream list and do not activate SND-61 alone. | XML SND-X01; debug build SND-E21..24; BA-23 incomplete global distinction. | **Same** authored probability/instance/overlap in core; missing category gates and 2D priority allocation are SND-16/07. |
| SND-19 | Starting an attached event with `Kills_Previous_Object_SFX` detaches that object before new-event admission. Detachment clears any runtime chain sourced by the object, immediately ends playing attached 2D/3D samples without a fade, and removes all matching attached events, including queued ones. Refused replacement does not undo this. Fixed-position/plain 2D overloads skip replacement. | Debug build SND-E33/E48; BA fidelity list lacks ordering/lifecycle. | **Same** attached replacement detaches queued/playing events and object-sourced runtime chains before admission. |

The authored hardpoint-response keys are `HARD_POINT_WEAPON_LASER`,
`HARD_POINT_WEAPON_MISSILE`, `HARD_POINT_WEAPON_TORPEDO`,
`HARD_POINT_WEAPON_ION_CANNON`, `HARD_POINT_SHIELD_GENERATOR`, `HARD_POINT_ENGINE`,
`HARD_POINT_FIGHTER_BAY` and `HARD_POINT_GRAVITY_WELL` (SND-X01).
The mapping belongs to the **speaker's unit type**, keyed by the **target hardpoint's
Type**. Hardpoint name and slot number do not define the callout. A speaker without an authored hangar line
uses SND-20 fallback; the presence of that key on another unit does not grant it this line.
The effective XML inventory contains 64 definitions with hardpoint callout pairs and no
authored `SFXEvent_Attack_Override` entry. This is a data observation: the target-override
getter still belongs in the sourced fallback contract for custom data.
That override list belongs to the speaker's type, scans entries in source order and matches
the target's resolved unit type exactly, then falls back to the ordinary attack getter
(SND-E32). A broad target category is not the lookup key.

Concrete authored examples (SND-X01; these are definition rows before inheritance):

| Target hardpoint Type | `Calamari_Cruiser` event | `Generic_Star_Destroyer` event |
|---|---|---|
| `HARD_POINT_WEAPON_LASER` | `Unit_HP_LASER_Calamari` | `Unit_HP_LASER_Star_Destroyer` |
| `HARD_POINT_WEAPON_MISSILE` | `Unit_HP_MISSILE_Calamari` | `Unit_HP_MISSILE_Star_Destroyer` |
| `HARD_POINT_WEAPON_TORPEDO` | `Unit_HP_TORP_Calamari` | `Unit_HP_TORP_Star_Destroyer` |
| `HARD_POINT_WEAPON_ION_CANNON` | `Unit_HP_ION_Calamari` | `Unit_HP_ION_Star_Destroyer` |
| `HARD_POINT_SHIELD_GENERATOR` | `Unit_HP_SHIELDS_Calamari` | `Unit_HP_SHIELDS_Star_Destroyer` |
| `HARD_POINT_ENGINE` | `Unit_HP_ENGINES_Calamari` | `Unit_HP_ENGINES_Star_Destroyer` |
| `HARD_POINT_FIGHTER_BAY` | No pair authored | No pair authored |
| `HARD_POINT_GRAVITY_WELL` | No pair authored | No pair authored |

Of the scanned definitions, only `Arc_Hammer` authors a fighter-bay pair,
`Unit_HP_BAY_Arc_Hammer`. A hangar-target command can therefore legitimately use the
ordinary/group fallback for other speakers; an absent pair is not evidence of a missing
retail sound. The inventory parses 557 top-level XML files; one malformed land-story
file is excluded and contains no hardpoint-response tag.

## Combat, warnings and arrival

| ID | FoC rule | Source / earlier note | Ours / gap owner |
|---|---|---|---|
| SND-30 | Weapon-fire, impact, ordinary death, spin death and hardpoint-explosion audio retain BA-14..17. Hardpoint fire uses `Fire_SFXEvent`, else parent `SFXEvent_Fire`; immediate route requires appearance delay <1. Armor multiplier <=0.75 selects authored reduced-by-armor detonation, otherwise ordinary detonation; a fully absorbed shield hit is ordinary detonation. | AU-14..18; BA-14..17 same; [weapons](weapons.md) and [capital combat](capital-combat.md) own shot/damage rules. | **Same** covered event routes, prepare/events; delayed fire audio is an existing audit gap. |
| SND-31 | Hardpoint loss warns the local owner only while parent lives and loss is not self-destruct. If no weapon hardpoint stands, use `SFXEvent_Hardpoint_All_Weapons_Destroyed`; otherwise if none of the destroyed type stands, use its `SFXEvent_Hardpoint_Destroyed` entry. Explosion audio is a separate spatial cue. | WCC-63, debug build WC-06; BA-17 misses warning. | **Missing**, events hardpoint destruction plays explosion only; combat-warning gap. |
| SND-32 | Killing a unit plays `SFXEvent_Unit_Lost` for its local owner after the kill/death sequence; it does not require selection. | WCC-70, debug build WC-09; BA-16 misses warning. | **Missing**, events death branch plays death effects only. |
| SND-33 | A hit crossing displayed health thresholds plays local owner's `SFXEvent_Health_Low_Warning`/`Critical_Warning`, or local attacker's type's enemy-damaged equivalents. Stock `Health_Low_Percent_Threshold=0.33`, `Health_Critical_Percent_Threshold=0.10`; displayed health is WCC-16, including combined destroyable-hardpoint health where applicable. | WCC-16, WCC-54; WC-04/WC-18; XML SND-X01. | **Missing**, events has no warning consumer; thresholds and repeat crossing must use the combat contract, not hull-only guesses. |
| SND-34 | Frame-35 hyperspace arrival starts the arriving object's **owner faction** `SFXEvent_Arrive_From_Hyperspace` on the object when authored, including AI-owned objects. Visibility/attached silence follows arrival reveal and SND-11; placement/en-route acknowledgements are separate. | WR-37, WR-E17/E18; WPR-40 same. | **Missing** at base; production/arrival implementation owns immutable frame-35 notifications. |
| SND-35 | Accepted buy and completion use type `Build_Speech_Underway`/`Build_Speech_Completed` first, then tactical SFX, then generic build SFX, for local owner only. Upgrade objects follow the same completion contract. | WPR-22, WPR-30; WP-09/WP-11/WP-20; same earlier walk. | **Partial**, BA-60/73/74: local ordinary space production uses retained accepted-type/completion notifications and queues authored English speech before tactical/generic SFX; campaign/land callers remain separate. |
| SND-36 | Station replacement chooses local faction starbase-upgraded, ally-upgraded or enemy-upgraded event from relationship to upgraded owner. Reinforcement-cap refusal chooses faction tactical-unit-cap cue; pane opening, placement and en-route use their own events. | WPR-52; WR-07, WR-11, WR-16; sourced earlier walks. | **Differs**, events has upgraded cue but reinforcement UI/cap routes remain separate gaps. |
| SND-37 | Retreat start prefers local type `SFXEvent_Retreat_Start`, then faction begin sound; enemy begin uses its separate faction event. Hyperspace exit has its separate SND-70 flight-counter contract. | WR-44 and reinforcement walk's retreat interface; SND-E64/E65 source exit. | **Missing**, current battle audio has no retreat/exit consumer. |
| SND-70 | In simple-space and fighter hyperspace-away service, when remaining movement delay equals **90 frames**, try the type-specific retreat engine-jump cue, else owner faction `SFXEvent_Exit_Into_Hyperspace`. Request it at the current fixed position, before that call moves the craft and decrements delay. The type getter resolves a distinct event reference (SND-E75); no literal corresponding type tag is authored in the effective XML inventory. No local-owner or explicit fog-test object is supplied, so SND-11's attached-object fog multiplier does not apply. This is not arrival frame 35 or the retreat countdown start. | Debug build SND-E64/E65; WR-44 supplies retreat orchestration; missing from BA. | **Missing** fixed-position exit notification in events; retreat/arrival sound interface. |
| SND-38 | Tactical build-queue cancellation for the local owner tries type `Build_Speech_Stopped`, then `SFXEvent_Tactical_Build_Cancelled`, then generic `SFXEvent_Build_Cancelled`. The separate generic production front-cancel route queues authored stopped speech before its local-owner check; its tactical SFX branch has no generic fallback. These caller contracts must not be collapsed. | Debug build SND-E15/E16; extends WPR cancellation interface. | **Partial**, BA-61/73/74: local tactical queue cancellation queues authored English stopped speech before tactical/generic SFX. The separate generic campaign production caller remains outside M2. |
| SND-39 | Local-player service in tactical mode asks whether any reinforcement room remains, with no particular unit type. If its retained full flag is false and room is unavailable, try faction `SFXEvent_Tactical_Pop_Cap_Reached`, then retain the new full/not-full state. This differs from `SFXEvent_Tactical_Unit_Cap_Reached` on a rejected particular reinforcement request (SND-36); neither is a periodic ambient warning. | Debug build SND-E17/E18; XML SND-X01. | **Missing**, events has no tactical population transition consumer at base; production/cap sound gap. |

## Advisor, announcer and ambience

| ID | FoC rule | Source / earlier note | Ours / gap owner |
|---|---|---|---|
| SND-40 | A hostile attack notification on an object with starbase or orbital-structure behaviour warns the local owner, or an ally only for community property. One space-mode timer must be zero. Reset it to `int(Delay_Between_Space_Base_Attack_Announcement_Seconds * logical FPS)` before trying local faction `SFXEvent_Space_Base_Under_Attack_Announcement`: stock 60 s, 1800 frames. If authored, also request a 4 s radar blip. Mode service decrements the timer each logical frame. Neither selection nor successful damage is required by this notification handler. | Debug build SND-E11/E12; XML SND-X01; missing from BA. | **Partial**, events observes published weapon-fire notifications, applies the global logical cooldown and faction WAV request, and presents the authored four-second radar marker; attack paths without a published fire notification remain outside this observer. |
| SND-41 | Tactical briefing runs once. Disabled briefing or no parent mode marks it done without speech: standalone skirmish does not automatically play these campaign-intro tags. With a parent, wait until no cinematic plays, frame >=10*logical FPS (300 stock), and the speech-event queue is empty; mark done before resolving local attacker/defender and conflict records. Try conditional speech, then space attacker/defender default. Missing event does not retry. Authored `Tactical_Intro_Command_Bar_Movie_Name` may accompany accepted speech. | Debug build SND-E35; XML SND-X01; WBF-09's cinematic intro is a separate prebattle interface. | **No standalone target**, BA-72; parented campaign speech/movie remains outside M2. |
| SND-49 | Conditional space briefing scans role-specific `_Conditional_Or` rows first, then `_Conditional_And`, each in source order. Query persisted objects available for this space battle: OR requires A or B present and C absent; AND requires A and B present and C absent. Return the first qualifying authored speech. `None` yields no object; C is an exclusion, not a third positive operand. Persistence/availability is the campaign interface. | Debug build SND-E36; XML SND-X01. | **No standalone target**, BA-72; campaign persisted-object conditions remain outside M2. |
| SND-42 | While logically visible in fog service, try type-sighting first when `Play_SFXEvent_On_Sighting`. Retain a type once per mode before testing that no active 2D event exists. Resolve local faction exact-type `SFXEvent_Unit_Type_Spotted`, otherwise generic spotted cue. Busy, absent or refused playback does not restore type eligibility. Return true after requesting an authored cue, irrespective of backend playback; that return suppresses SND-64 for this service call. Logical visibility, not a new reveal edge or model opacity, is the caller condition. | Debug build SND-E37/E38/E54; XML SND-X01; BA-70. | **Partial**, BA-70: prepare/events and core policies retain type eligibility before busy/lookup checks and preserve exact-type/blank fallback. Per-object fog-service timing remains below. |
| SND-64 | If the visible-object type handler returned false, fog service tries first-enemy sighting for a non-projectile local enemy. The mode handler requires ownership by current attacker or defender and an unset first-enemy flag. Set the flag before resolving/starting local faction `SFXEvent_Enemy_Spotted`; missing/refused cue does not retry. This handler has no all-2D-events busy test. A previous type cue suppresses it for that call, not permanently; later visible service can call it again. | Debug build SND-E39/E54; XML SND-X01. | **Partial**, BA-71: events/core policies retain first-enemy eligibility before lookup, with battle-party ownership and no whole-pool busy gate. Later still-visible service is covered; per-object fog-service timing remains below. |
| SND-65 | Overwhelming-odds service requires enabled tactical mode, **network multiplayer** (LAN/internet/online platform, excluding solo and skirmish) and positive `Tactical_Overrun_Multiple`; sample every 15 logical frames after `Minimum_Tactical_Overrun_Time_In_Secs` (30 s, 900 frames stock). If stronger power is strictly greater than weaker*3.0 (stock), retain that side's one-shot flag and request `SFXEvent_HUD_Tactical_Victory_Near` only when that side is exactly local. Map reveal and combat-power calculation belong to battle-flow/visibility. This announcement is not enabled by ordinary standalone skirmish. | Debug build SND-E40/E63; XML game constants; missing from BA. | **Missing** network-mode victory-near cue consumer in events; announcement gap outside current standalone M2. |
| SND-43 | Adding a nonempty story objective retains its text/status/order and rebuilds the command-bar objectives; that helper has no direct speech dispatch. Mission/tutorial speech is a separate scripted interface. Direct credit assignment has no audio dispatch; low-credit and generic research-complete announcements have no sourced universal tactical route here; ordinary unit/upgrade completion remains SND-35, construction unlock SND-68. Do not generate a universal advisor line from objective or resource mutation alone. | Debug build SND-E56/E71; WPR-22; XML SND-X01; low-credit route remains SND-U04. | **Missing** scripted advisor/presentation interface in events; campaign/story consumers retain their mode scope. |
| SND-68 | A tech-level update requests local faction `SFXEvent_New_Construction_Options_Available` only when unlock processing and notification are requested and at least one affiliated, initially locked, slicer-unlockable type below the resulting tech level is newly unlocked. An ordinary level change without a new unlock is silent here. Other slicer/black-market callers hand off to the ability walk. | Debug build SND-E55 and getter xrefs; missing from BA. | **Deferred** to galactic unlock-state implementation (legacy EAWR-1644); ordinary skirmish station/tech upgrades must stay silent on this route. |
| SND-69 | Associated speech text exists only for a now-playing front event with a valid selected-file index and equal file/Text_ID list lengths; return text at that index, otherwise none/assert for malformed data. The external command-bar hologram path displays text only when the current event matches its pending event and no text is displayed; when the tracked event is no longer queued/playing, stop the movie and remove its remembered text. This does not give every WAV VO or MP3 event automatic subtitles. | Debug build SND-E60/E47, rechecked by the speech text/movie scope audit below; effective XML. | **No stock standalone target**; indexed text and command-bar movie consumers remain campaign/story work, not an M2 announcement gap. |
| SND-44 | `SpeechEvent` queues append FIFO without priority sorting or deduplication. Unpaused service processes only the front: uniformly choose a file, localize for non-English, start at event `Volume_Percent`, normal speech priority, zero fade and centered pan 0.5. Failed start or completed/interrupted stream notifies completion callbacks and removes the front without retry. SND-69 defines associated text. | Debug build SND-E25/E26; XML SND-X01. | **Partial**, BA-73/74: events `production_cue`/`update_speech` and core policy `SpeechQueue` implement the English production FIFO, duplicates and front removal. Scripted/campaign callers, text/movie and other languages remain separate. |
| SND-48 | The speech backend permits one MP3 stream, separate from 16 WAV 2D slots. Priority 1..5 is lower-more-important; refuse a less important newcomer. Equal/more important closes the old stream before file open, so failed replacement cannot restore it. Unpaused backend service closes/removes completed streams; system pause skips this cleanup. Ducking/HUD gating tests a nonempty list, including retained paused streams. Outer service runs SFX before speech-event cleanup, so admission and later queue removal are separate observations. | Debug build SND-E24/E27/E61; WBF-13. | **Partial**, BA-73: core policy `SpeechStream` and events `update_speech` provide single-stream priority, replacement and paused retention. General scripted callers and completion callbacks remain separate. |
| SND-45 | Engine idle/moving switches ignore delete-pending or engine-audio-disabled objects, resolve a team through its current leader type and require an authored loop. Only state transitions start/stop attached cues with loop fades; active flags change even if unauthored/refused. Cinematic focus uses zero fades and requests `SFXEvent_Ambient_Moving` on both enable and disable. Preview switches disable the previous focus then enable the new; preview initialization admits a local-owned chosen unit. | Debug build SND-E29..31/E52/E53; XML SND-X01: stock engine presets often use 0.5 s in / 1 s out. | **Partial**, BA-82/83 implement ordinary-space engine switches, attached loops and authored fades; cinematic focus remains separate. Ordinary service predicate is SND-67. |
| SND-67 | After an ordinary simple-space locomotor state service reports an update, compare walk speed >= `SpaceIdleMovementSpeed * 1.1`: enable moving then disable idle; otherwise disable moving then enable idle. Stock speed scalar is **0.0**, so stationary speed alone must not be assumed to select idle. Engine disable explicitly switches both loops off; enable requests idle unless already online, before later service may switch it again. Locomotor state/engine simulation is a handoff. | Debug build SND-E49..51; XML game constants; missing from BA. | **Same** ordinary-space comparison through published walk speed, existing worker snapshot slots and presentation-only source state (BA-82). |
| SND-46 | Ambient service ignores non-leader children of a team container. Init schedules a uniformly drawn min*FPS..max*FPS delay from the mode's current frame. When due (<= current frame), ask locomotor whether moving; a buzz-droid object without locomotor also qualifies. Fighter movement means a nonempty remaining movement path: idle orbiting has none, and directed combat clears paths on reaching the combat cell or strafe range. If moving and authored, request attached `SFXEvent_Ambient_Moving`; then reschedule from the current frame even if stationary, unauthored or refused. Min/max moving-delay seconds must be positive and ordered; type defaults are 5/10 s (150..300 frames stock), overridden by XML. Shutdown adds no stop hook here; object detachment is separate. | Debug build SND-E41/E42/E46, freshly cross-checked fighter path, idle and directed-combat services; XML SND-X01. | **Partial**, BA-80/81: prepare reads event/delays, events owns bounded logical-frame timers, resolves the first live team member, and requests finite attached cues for movement presence. Snapshot publication uses existing partitioned work, outside canonical bytes. Buzz-droid/no-locomotor exception, hidden-at-start admission (SND-04) and cinematic bypass remain gaps. |
| SND-47 | `Ambient_Map_Sounds` maps environments to beds; `Ambient` entries supply event, inner/outer surround distances and min/max delay. Effective `audio.xml` authors seven land environments and **no Space row**. Therefore generic map-bed absence is not evidence of a missing stock space bed. | XML SND-X01 (`audio.xml`, `ambientmapsounds.xml`); BA fidelity list overbroad. | **Same** stock space has no configured row; custom space maps/beds require a separate sourced consumer. |

## Music and mix

| ID | FoC rule | Source / earlier note | Ours / gap owner |
|---|---|---|---|
| SND-50 | Choose local faction's ambient/battle `Music_Event_List_*` entry for Space randomly. Scan present faction-superweapon objects for local faction's space override first; a missing override falls back to ordinary list. This is presence, not camera distance. | AU-27; SND-E13; BA-41 same ordinary branch, override missing there. | **Differs**, prepare/core choose ordinary faction lists only; superweapon audio gap. |
| SND-51 | Attack notification starts battle music and resets quiet counter when attacker or target belongs to local player, or is allied **community property**. Ordinary allied ownership alone does not satisfy this predicate. Fog is not a predicate in this handler. | Debug build SND-E11; AU-25/26. | **Same**, events `frame` checks both sides using retained owner/type metadata, independently of fog and camera. |
| SND-52 | Each logical frame increments quiet counter; in battle mode, >= `Music_Space_Battle_To_Ambient_Peace_Seconds * logical FPS` returns to ambient (15 s, 450 frames stock). SND-71 establishes the separate opening/activation reevaluation path. | AU-24; SND-E12; BA-43 same. **Differs from WBF-17 prose:** its own BF-E01 also tests threshold <= counter, so equality is sufficient; its claimed strictly-greater gap is not supported. | **Same** quiet transition, core `MusicDirector::tick`; opening policy is core `begin`. |
| SND-71 | Initialization, begin and activation request ambient reevaluation. Mode service, outside battle-end screen, dispatches current-mode ambient then clears the request. Normal tactical dispatch preserves scripted, superweapon attack-sequence, demo-finale and demo-attract modes and avoids restarting tactical ambient; force bypasses these policy gates. Space selects its Space environment playlist and starts ambient mode. | Debug build SND-E47/E68/E69; BA lacks opening caller/gates. | **Same** normal standalone opening in core `MusicDirector::begin` and events; protected scripted/demo modes are outside current viewer modes. |
| SND-53 | Music files use per-event sequential index, volume percent, `Loop`; a new event fades in using its `Fade_In_Seconds`, previous out using the new event's `Fade_Out_Previous_Seconds`. Choosing the already playing event need not restart it. | AU-28; BA-40/44 same. | **Same**, core `MusicDirector`, events `cue_music`/`update_music`. |
| SND-72 | Music has one active stream plus one ending stream. Before opening a replacement file, retire active to ending and immediately close an older ending stream; failed open does not restore the active track. For positive fade-out time T, fade from the current stream level C to zero at C/T per second, so a partly faded-in track still takes T. Stream service updates gain in elapsed milliseconds and retains a silent ending stream until later replacement/teardown. | Debug build SND-E66/E67/E73. | **Same** positive-duration transition, events `cue_music`/`update_music` and core `MusicFade`; zero-duration/zero-level acoustic parity stays SND-U02. |
| SND-54 | Tactical result music requires winner, loser and local faction. Exact local winner selects local faction win versus loser faction; otherwise select lose versus winner faction. Resolve the first exact-faction override in source order, else default event; a matching blank override returns blank without default fallback. An already-selected result music mode suppresses restart. This differs from allied-win/enemy-loss HUD predicates in WBF-38/BE-01..02. | Debug build SND-E28/E58/E59. | **Same** space outcome selection, prepare retains ordered faction fields, events resolves opposing faction, core `MusicDirector::result` suppresses duplicate starts; summary remains a separate cue. |
| SND-55 | Summary screen separately uses `Music_Event_Battle_End_Summary_Screen_Win`/`Lose`. Effective stock `audio.xml` leaves both blank; a blank must not invent a track or silence the prior one by assumption. | WBF-42..47; XML SND-X01; BA note missing distinct state. | **Same** authored summary resolution, prepare/events; broader end lifecycle remains battle-flow ownership. |
| SND-60 | Master/music/speech/SFX slider defaults are 0.75. Effective category gain multiplies its slider by master (0.5625); persisted profile may override defaults. Both spatial and 2D sample starts and updates choose the speech slider when the event's `Localize` flag is true, otherwise SFX; VO admission flags do not substitute for this test. | AU-30..31; debug build SND-E19/E20/E23/E43 plus SND-E02; BA-45 same defaults, missing flag distinction. | **Same** WAV category routing: events `play` selects the bus from `Localize` for both pools; mix `set_mix_levels` applies independent category sliders into master. Settings UI and profile persistence remain separate. |
| SND-61 | While the MP3 speech-stream list is nonempty and no GUI dialog is active, starting SFX gain is capped at **0.3**, after drawn-volume/visibility; lower gains stay lower. Playing-event update computes the same ceiling: ordinary gain writes cover 2D and object-attached 3D; fixed-position 3D gain is written there only during fade to silence. This is an absolute ceiling before backend category/master gain, not multiplication by 0.3. WAV unit-response/HUD events alone do not activate it. | Debug build SND-E14/E23/E24/E26; AU-13; BA-13 omits dialog condition, update distinction and exact cap. | **Partial**, events `play`/`update_voices` apply core policy `speech_sfx_gain` at start and ongoing 2D/attached updates while a speech stream is retained, with the dialog exception. WAV responses alone do not trigger the cap; fixed-position voices retain start gain, with generic fade-to-silence lifecycle outside this path (SND-08/45). |
| SND-62 | WAV SFX name localization requires `Localize` and non-English language; MP3 speech uses its queued language. The localization helper uppercases a copy, finds the English WAV or MP3 suffix and substitutes that language's corresponding suffix; a missing recognized suffix asserts. Speech paths use the language subdirectory. There is no missing-file English fallback in the traced SFX/stream starts; missing-file admission is separate from archive registration. | Debug build SND-E02/E23/E26/E44; XML SND-X01; BA-30 lacks language routing. | **Partial**, BA-74: English production speech resolves its speech-directory files and streamed route. Non-English localization and selected-language archive registration remain separate (legacy EAWR-1498). |
| SND-63 | The mix hard limiter at -0.3 dBFS is the viewer's choice; no FoC limiter parity is established. Category/pool caps, fades and clipping must be measured separately. | BA-45 explicitly project policy; SND-U02. | **Same** documented project policy in mix; not evidence of retail loudness parity. |

### Stock faction music data

SND-X01 joins `factions.xml`, `expansion_factions.xml` and `musicevents.xml`.
The faction chooses the event; a playlist name does not prove exclusive faction tracks.

| Faction | Space ambient / battle | Default tactical win / lose |
|---|---|---|
| Rebel | `Space_Map_Rebel_Ambient_Music_Event` / `Space_Map_Rebel_Battle_Music_Event` | `Rebel_Win_Tactical_Event` / `Rebel_Lose_Tactical_Event` |
| Empire | `Space_Map_Empire_Ambient_Music_Event` / `Space_Map_Empire_Battle_Music_Event` | `Empire_Win_Tactical_Event` / `Empire_Lose_Tactical_Event` |
| Underworld | The Rebel ambient/battle events | `Underworld_Win_Tactical_Event` / `Rebel_Lose_Tactical_Event` |

The Rebel and Empire ordinary ambient events have identical six-file lists, volume 55,
2-second fade in and 2-second previous fade out, looping. Their ordinary battle events
have identical ten-file lists, volume 55, 0.5-second fade in and 2-second previous fade out,
looping. The space superweapon battle overrides have two files and the same battle fade
values; Rebel/Underworld choose the Rebel override, Empire the Empire override. All three
space ambient superweapon overrides are blank, so normal ambient fallback applies.
`Music_Event_Tactical_*_Vs_Faction` rows can override the default result events; notably
loss against an opposing faction need not use the local faction's default lose track.
These are data values, not constants to bake into the remake.

## Gaps and verification

### Standalone announcement implementation

The remaining intro, hero-respawn and spotted-announcement inventory was checked against
the debug build and effective XML again after the speech and sightings implementation.
Both default space intros and all four attacker/defender OR/AND condition fields remain
campaign-only: SND-41 rejects a missing parent before considering those fields, and
SND-49 queries persisted battle objects rather than the ordinary skirmish roster.
The existing registry marks these six fields `land-or-galactic`, meaning no standalone
space-skirmish target; it does not claim that a space campaign intro is a land cue.
`SFXEvent_Hero_Respawned` likewise follows successful planetary placement for the local
owner (SND-E57), while WHE-38 rejects automatic respawn in skirmish. A paid replacement
or normal hyperspace arrival must not request it. These exclusions are BA-72.

The viewer now consumes SND-42/64 from logical snapshot visibility: type eligibility is
consumed before WAV 2D admission, and first-enemy eligibility before event lookup. Missing,
busy and refused cues do not retry. The observer shares a 30-frame stable-service phase;
original per-object scheduling and service acceleration while fading remain a timing gap.
No fog ghost, model-opacity threshold or radar reveal is used as a sighting predicate.
Hiding and re-revealing an object never resets either retained eligibility. A later
service of an object that stayed visible can request the first-enemy cue after its type
cue suppressed that handler on an earlier call. Both routes remain WAV SFX, as sourced
by SND-42/64; they do not acquire the streamed-speech queue or its ducking trigger.
Core `announcements()` assertions cover busy/missing cues, retention and that later
first-enemy call; the renderer's `test_sighting_announcements_are_once_per_type_and_battle`
checks both logged starts and a matching blank type override.

The precise remaining sighting difference is the shared 30-frame observer phase versus
the original per-object, fade-accelerated fog service (SND-42/64, SND-E54; WSU-03).
That can change when a visible object is serviced and whether the 2D pool is busy then;
cue retention alone does not establish timing parity. It requires a separate visibility
and presentation change. Other sourced announcement gaps remain the base-under-attack
timer/cue and authored radar blip (SND-40) and qualified construction-unlock notification
(SND-68). Tracked speech text/movie lifecycle (SND-69) is campaign/story scope,
as established by the audit below. Campaign/story and
network-only callers keep their respective scopes under SND-41/43/49/65.

English tactical production speech now uses the SND-44 FIFO and SND-48 single-stream
admission contract, including duplicate requests, front-only service and failed/completed
front removal. Reachable files are preloaded, with fixed queue storage and reported
overflow. SND-61 caps SFX at 0.3 without a modal dialog; WAV unit responses alone do not
activate the cap. Authored speech WAV files also go through the streamed route: the
production speech data and sample headers establish PCM WAV authoring (SA-E01), while
SND-E26 establishes their speech-backend dispatch. Text/movie consumers remain
campaign/story work; non-English playback is a separate gap. The generic spotted
tag is present in the debug-build strings
(SA-E02) but has no effective stock XML row, so it adds no stock registry claim.

SND-41/49 and WHE-38/SND-E57 exclude automatic intro and hero-respawn announcements from
standalone skirmish. Campaign conditions and network-only victory-near retain their
separate scope. The additional music start/update and shared gain-getter traces (SA-E03)
apply music fade/slider gain without a speech predicate or SFX ceiling. See
[battle audio](../battle-audio.md) BA-70..75 for the runtime contract.

Tracking: battle audio and speech walk (legacy EAWR-1508). Existing gap issues are reused;
new lifecycle, pause, music selection/stream transition, 2D allocation and category-admission issues are children of the tracking issue. Estimates
describe implementation scope, not an assertion that acoustic parity is already proved.

| Gap owner | Rules | Size / owner-visible effect |
|---|---|---|
| Hardpoint/order response inventory (legacy EAWR-1500) | SND-15, SND-20..25 | M; **owner-visible** missing authored hardpoint callouts, stop/guard and second speaker. |
| Production and arrival sounds (legacy EAWR-725) | SND-34..36, SND-38..39, SND-70 | M; **owner-visible** silent purchases/completions, cancellations, cap transitions and arrivals; concurrent implementation, not counted as merged at base. |
| Combat warning sounds (legacy EAWR-706) | SND-31..33 | S; **owner-visible** unit/hardpoint loss and low-health lines. |
| Attached ambience (legacy EAWR-1501) | SND-45..47, SND-67; SND-11 | M; **owner-visible** engine/ambient loops and fly-bys; stock Space map has no ambient-bed row. |
| Reinforcement UI sounds (legacy EAWR-1502) | SND-36 | S; **owner-visible** pane opening/en-route feedback. |
| Button and refusal feedback (legacy EAWR-1503) | BA-52; command-bar interface | S; **owner-visible** invalid/recharging ability feedback. |
| Tactical announcements/streams (legacy EAWR-1504) | SND-40..44, SND-48..49, SND-64..65, SND-68..69; SND-61..62 | Standalone FIFO, priority, ducking and type/first-enemy cues are implemented. Remaining sourced work: sighting service timing, SND-40 base warning and SND-68 qualified unlock; SND-69 text/movie and intro/hero respawn are campaign/story scope, and victory-near is network-only. |
| Ability effect and superweapon audio (legacy EAWR-1505) | SND-26, SND-50 | M; **owner-visible** separate spatial effect cues and music override. |
| Complete SFX lifecycle (legacy EAWR-1509) | SND-03..05, SND-08..09, SND-11, SND-18 | M; **owner-visible** authored loops/chains and attachment lifecycle; pre/post and pan support also affect custom data. |
| Full 2D pool priority allocation (legacy EAWR-1520) | SND-07 | S; **owner-visible** important responses refused when 16 slots fill; equal-priority oldest replacement. |
| Category admission and localized gain (legacy EAWR-1521) | SND-16..17, SND-60 | S; **owner-visible** wrong speech/SFX channel for 2D effects or localized spatial sounds; HUD suppression during MP3 streams. |
| Explicit audio-category pause (legacy EAWR-1510) | SND-13 | S; **owner-visible** sound continuing through pause. |
| Tactical result music and community-property triggers (legacy EAWR-1511) | SND-51, SND-54 | S; **owner-visible** absent win/lose transition; shared allied structures should trigger combat music. |
| Music stream replacement and fade timing (legacy EAWR-1530) | SND-72 | S; **owner-visible** rapid-transition fade shortening and different behaviour when a replacement file is missing; two-player capacity already matches. |
| Existing audio audits (legacy EAWR-801, EAWR-802, EAWR-803, EAWR-804, EAWR-806, EAWR-807) | SND-02, SND-08, SND-19, SND-30, SND-61..63, SND-66 | XS..M; delayed fire, ducking predicate, object replacement, loader normalization/duplicates, acoustic tests and stale baseline notes. Source corrections below must precede fixes based on the audit prose. |
| Presentation tag registry (legacy EAWR-653) | All registry discrepancies below | S; correct applied-versus-parsed metadata alongside the owning implementation; no code change in this walk. |

Highest impact on the current battle: hardpoint-target callouts; ordinary production and
frame-35 arrival audio; loss/health warnings; base-under-attack announcements; engine/ambient
loops. Result music and pause are independently visible gaps. Selection-only evidence must
not be used to suppress unsolicited combat warnings.

### XML and tag-status audit

The following is a **base-revision snapshot** of `docs/tag-coverage/statuses.json` (SND-X02),
not its current truth after concurrent implementations. A grouped row preserves each
literal tag's status; where classes differ they are named. The full private XML/tag joins
remain ignored. No status is promoted by this docs-only change.

| Tags / class | Registry status at base | Walk finding / owner |
|---|---|---|
| `Audio_Space_3D_Saturation_Distance_Mod`, `Audio_Space_3D_Rolloff_Distance_Mod`, `Audio_Space_3D_Listener_Z_Pullback_Dist` / Audio | todo | **Stale:** prepare applies all three; SND-10/12, registry correction (legacy EAWR-653). |
| `Music_Space_Battle_To_Ambient_Peace_Seconds` / Audio | todo | **Stale:** prepare/core apply it; SND-52. |
| `Chained_SFXEvent`, `Pre_Samples`, `Post_Samples`, `Min_Predelay`, `Max_Predelay`, `Play_Count` / SFXEvent | applied | Event queue executes all sample stages, delays, completed loops and authored chains (SND-05/08/09/18). |
| `Loop_Fade_In_Seconds`, `Loop_Fade_Out_Seconds` / SFXEvent | todo | Parsed without general runtime fades; SND-08/45 (legacy EAWR-1509, EAWR-1501). |
| `Is_Unit_Response_VO`, `Is_Ambient_VO`, `Is_HUD_VO`, `Is_GUI`, `Localize` / SFXEvent | applied / partial | SND-16/17 admission and SND-60 WAV gain routing apply in both spatial modes. Non-English sample localization remains SND-62 work (legacy EAWR-1498). WAV classification alone does not activate stream ducking. |
| `Kills_Previous_Object_SFX` / SFXEvent | todo | Parsed, unconsumed replacement policy (legacy EAWR-803). |
| `Files`, `Text_ID`, `Volume_Percent` / MusicEvent and/or SpeechEvent | applied or partial; see registry per class | Music execution does not prove SpeechEvent application. English production stream execution applies speech files/volume (SND-44); associated speech text is an explicit campaign/story consumer (SND-69), absent from stock M2. |
| `MSS_Internal_Loop` / MusicEvent | todo | SND-E66/E77 forwards this to backend same-file infinite repeat, separately from event playlist `Loop`; stock effective authoring is the loading-loop event. Loading presentation owns it (legacy EAWR-653); no stock space ambient/battle event authors it. |
| `SFXEvent_Attack_Hardpoint` / SpaceUnit, StarBase, UniqueUnit | todo | SND-20/21 (legacy EAWR-1500). |
| `SFXEvent_Assist_Move`, `SFXEvent_Assist_Attack`, `SFXEvent_Move_Into_Asteroid_Field`, `SFXEvent_Move_Into_Nebula` / SpaceUnit, UniqueUnit | partial | Local space responses consume these fields through BA-29 and SND-22/24/25; unsupported land/galactic callers remain separate (legacy EAWR-1500). |
| `SFXEvent_Hardpoint_All_Weapons_Destroyed`, `SFXEvent_Hardpoint_Destroyed`; local/enemy `SFXEvent_*Health_Low_Warning`, `SFXEvent_*Health_Critical_Warning` / applicable space types | todo | SND-31/33 (legacy EAWR-706); health thresholds also follow WCC-54. |
| `SFXEvent_Unit_Lost` / Mobile_Defense_Unit, SecondaryStructure, SpecialStructure, UniqueUnit | todo | WCC-70 traces space kill playback; SND-32 (legacy EAWR-706). GroundBuildable/GroundVehicle and Props_Story rows separately remain land-or-galactic. |
| `SFXEvent_Arrive_From_Hyperspace` / Faction | todo | SND-34; production/arrival work (legacy EAWR-725). |
| `SFXEvent_Exit_Into_Hyperspace` / Faction | land-or-galactic | **Stale scope:** SND-70 sources fixed-position space exit at 90 remaining flight frames; retreat/arrival interface owns implementation. |
| `Build_Speech_Underway`, `Build_Speech_Completed`, `Build_Speech_Stopped`, `Build_Speech_Countdowns`, `Build_Music_Completed` / SpaceUnit | todo | Production speech precedes SFX; superweapon countdown branches separate; SND-35/43 (legacy EAWR-725, EAWR-1504, EAWR-1505). |
| `SFXEvent_Build_Started`, `SFXEvent_Build_Complete`, `SFXEvent_Build_Cancelled`, `SFXEvent_Tactical_Build_Started`, `SFXEvent_Tactical_Build_Complete`, `SFXEvent_Tactical_Build_Cancelled` / ordinary units, squadrons, station/upgrade types | todo | Build-pad `SpaceBuildable` start/complete rows are separately applied; they do not qualify ordinary station production (legacy EAWR-725). |
| `SFXEvent_Tactical_Pop_Cap_Reached`, `SFXEvent_Tactical_Unit_Cap_Reached` / Faction | todo | Population-full transition versus particular-unit reinforcement refusal; SND-39 versus SND-36, production implementation (legacy EAWR-725). |
| `Reinforcements_Selection_SFXEvent`, `Reinforcements_Pick_Landing_Zone_SFXEvent`, `Reinforcements_Enroute_SFXEvent`, `Reinforcements_Cancelled_SFXEvent` / Faction | partial | WR-07/11/16 and BA-84 cover local space pane opening, placement, submission, failed drops and explicit cancellation; land and campaign callers remain outside this path (legacy EAWR-1502). |
| `Reinforcements_Ready_SFXEvent`, `Reinforcements_Requesting_SFXEvent` / Faction | todo | Ready services pending garrison, outside the ordinary space pool path; Requesting runtime use remains unverified (legacy EAWR-1502). |
| `SFXEvent_Command_Bar_Attack`, `SFXEvent_Command_Bar_Attack_Move`, `SFXEvent_Command_Bar_Move`, `SFXEvent_Command_Bar_Stop`, `SFXEvent_Command_Bar_Guard`, `SFXEvent_GUI_Negative_Feedback` / Audio | partial | Local space mode arming/stop and ability refusals consume BA-27/28; stock mode cues are blank. Waypoint semantics remain deferred, as do unsupported land/galactic refusal callers (legacy EAWR-1503). |
| `SFXEvent_Space_Base_Under_Attack_Announcement` / Faction; `SFXEvent_Unit_Under_Attack` / StarBase and structures; `Delay_Between_Space_Base_Attack_Announcement_Seconds` / Audio | todo | SND-40 sources mode base warning; distinct unit-warning tag has a real event-reference parser row (SND-E72), but neither traced land nor space mode warning handler consumes it (SND-E74/E11); a type-level caller remains unverified (legacy EAWR-1504). |
| `SpeechEvent_Tactical_Intro_Space_Attacker`, `SpeechEvent_Tactical_Intro_Space_Defender`, each `_Conditional_And`/`_Conditional_Or` / Faction | todo | Parented campaign briefing SND-41/49; no automatic standalone-skirmish line (legacy EAWR-1504). |
| `SFXEvent_New_Construction_Options_Available` / Faction; `SpaceIdleMovementSpeed` / GameConstants | todo | Unlock cue SND-68 (legacy EAWR-1504) and engine switch scalar SND-67 (legacy EAWR-1501); actual unlock/motion internals retain their owning walks. |
| `Tactical_Intro_Command_Bar_Movie_Name` / Faction | presentation-later, menu evidence | SND-41 proves a tactical parented briefing consumer; current mode/presentation support remains missing (legacy EAWR-1504). |
| `SFXEvent_HUD_Tactical_Victory_Near` / Faction; `Tactical_Overrun_Multiple`, `Minimum_Tactical_Overrun_Time_In_Secs` / GameConstants | todo | SND-65 network-mode-only audio handoff; ordinary solo/skirmish excluded; battle-flow/visibility owns power and reveal (legacy EAWR-1504). |
| `Play_SFXEvent_On_Sighting`, `SFXEvent_Enemy_Spotted`, `SFXEvent_Unit_Type_Spotted`, `SFXEvent_Hero_Respawned` / applicable types/faction | todo or land-or-galactic for the hero-respawn row | Sighting caller/order is sourced by SND-42/64. Hero-respawn cue is local-owner planetary spawn after successful production placement (SND-E57), not a generic tactical arrival cue (legacy EAWR-1504). |
| `SFXEvent_Ambient_Moving`, moving min/max delay seconds / applicable space types | partial | SND-46, BA-80/81: live space movement timers and finite attached flybys; no-locomotor exception and cinematic visibility remain outside this path. |
| `SFXEvent_Engine_Idle_Loop`, `SFXEvent_Engine_Moving_Loop`, `SFXEvent_Engine_Cinematic_Focus_Loop`, `SFXEvent_Ambient_Loop` / applicable space types | todo | SND-45/67 (legacy EAWR-1501); continuous ambient-loop getter xrefs lead to parse-time duplicate ambient-versus-engine loop warnings (SND-E62/E76), runtime attachment remains SND-U05. Stock fighters author flybys and cinematic-focus loops, not ordinary engine idle/moving loops. |
| `Ambient_Map_Sounds` / Audio; ambient-bed name/entry fields | foc-ignores for Audio mapping; todo for bed entries, with malformed `Ambient_Map_Sound` node foc-ignores | No authored Space mapping; SND-47. The mapping lacks a literal parser row in the registry evidence; do not treat XML authoring as execution proof (legacy EAWR-1501). |
| `Music_Event_Tactical_Win`, `Music_Event_Tactical_Lose`, each `_Vs_Faction` / Faction | todo | SND-54 (legacy EAWR-1511). |
| `Music_Event_Battle_End_Summary_Screen_Win`, `Music_Event_Battle_End_Summary_Screen_Lose` / Audio | partial | prepare resolves authored values, effective values blank; SND-55, battle-flow summary lifecycle owns remaining classes. |
| `Music_Event_Space_Ambient_Super_Weapon`, `Music_Event_Space_Battle_Super_Weapon`, `TSW_Post_Music_Wait_Frames` and TSW SFX / Faction or SpaceUnit | todo | Separate superweapon state/callers; SND-50 (legacy EAWR-1505). |
| `Unit_Abilities_Data/Unit_Ability/SFXEvent_Special_Ability_Loop`, `SFXEvent_Target_Ability`; ability-specific activate/zap/target-affected events | todo | SND-26 handoff to ability consumers (legacy EAWR-1505); no stock starting-roster special-ability loop authored in BA evidence. |
| `SFXEvent_Retreat_Start`; faction `Space_Retreat_*`, `Space_Surrender_SFXEvent` | No literal local type retreat row; todo for faction rows | WR-44 and SND-37; caller scope must follow retreat evidence, not registry category alone. |

Land/galactic-only fiscal, bombardment, landing-zone, ground base-shield and weather callers
remain with their owning walks. Story/cinematic speech is an interface, not an automatic
skirmish announcement. Existing `foc-ignores` rows such as `SFXEvent_Barrage` are retained:
an absent parser entry is different from a parsed but unauthored or unconsumed event.

| ID | Unverified question | Discriminating evidence |
|---|---|---|
| SND-U01 | Audible timing of queued SFX/MP3 callbacks, fades and subtitle appearance. Admission, capacity, cleanup/pause and indexed hologram text are sourced. | Paired select/move/guard during an MP3 advisor line; record request/admission/backend start and visible text separately. |
| SND-U02 | Miles distance law at/after 15000, clipping and audible panning/mix parity. | Same event at controlled distances with music off, then paired speech/SFX recording at matched camera and sliders. Zero-duration/zero-current-level music fades also need an audible check; the backend source computes its fade slope by division. XML and cue counts cannot establish acoustic parity. |
| SND-U03 | Audible exit onset versus flight/camera framing; SND-70 now sources exact dispatch counter, type/faction fallback (SND-E75) and fixed-position fog distinction. | Retail retreat at a controlled camera, distinguishing flight delay from global countdown and request from audible onset. |
| SND-U04 | Remaining low-credit/research/scripted-objective announcement routes and type-specific unit-under-attack consumer. Sighting order, planetary hero spawn, construction-unlock gating, parented intro and network-only victory-near eligibility are sourced. **Sweep:** Still unverified: First-sighting latch is sourced. Low-credit/research/scripted-objective dispatch and type-specific unit-under-attack consumers remain separate; no mode-qualified consumer was resolved for all of them. | Mode-qualified caller traces and stock skirmish versus scripted campaign controls; do not infer a generic advisor trigger. Retained sweep boundary: EUS-24. |
| SND-U05 | Continuous `SFXEvent_Ambient_Loop` attachment, animation-driven audio and custom space map-bed consumption; ambient-loop parse-time warnings are not runtime playback proof. Engine switches, cinematic focus and moving timers are sourced. **Sweep:** Still unverified: Ambient parse-time acceptance and object sound teardown do not prove continuous-loop playback, animation audio or a custom map-bed reader. Those runtime attachment/bed consumers remain unresolved. | Continuous-loop/animation attachment traces and a staged custom bed; stationary/moving/cinematic-focus clips verify audible results. Retained sweep boundary: EUS-37. |

## Speech text and movie scope

This SND-69 audit rechecks the debug build and effective XML against main
`6ccb9a6df908336a2f9694572076ea1f5ead622c`. It corrects the earlier classification
of text/movie playback as a standalone announcement gap (legacy EAWR-1645).
Private receipts retain the text query, speech queue service/pause, external
hologram admission/service, mode service, campaign briefing and corruption
callers; no original symbols or code are needed by the implementation note.

| Stock data or caller | Applicability to standalone space skirmish |
|---|---|
| `SPACEUNITSSUPERS.XML`: `Death_Star` owns the only authored `Build_Speech_Underway`, `Build_Speech_Completed`, `Build_Speech_Stopped` and `Build_Speech_Countdowns` entries. `SPEECHEVENTS.XML` supplies their speech files and associated text. | `Tech_Level` 99 exceeds `GAMECONSTANTS.XML` `MP_Default_Max_Tech_Level` 5; stock M2 production does not reach them. The synthetic viewer production overlay proves stream execution, not stock subtitles. |
| Faction `Tactical_Intro_Command_Bar_Movie_Name`: Rebel, Empire and Underworld loops in `FACTIONS.XML`/`EXPANSION_FACTIONS.XML`. | SND-41's fresh debug-build check marks a parentless briefing done before speech/movie dispatch. Campaign-only despite Space intro event names. |
| Planet `Corruption_*_Success_Bink_Hologram_Name` and `Corruption_*_Failure_Bink_Hologram_Name`. | External hologram admission's direct callers are local galactic corruption initiation and campaign corruption results after mode transition. They are not ordinary space skirmish announcements. |
| Ordinary space selections, orders, sightings, warnings and production fallback. | WAV SFX routes do not query associated speech text or start a linked movie. A globally registered `SpeechEvent`/`Movie` alone creates no consumer. |

The resulting M2 change is documentation only. There is no stock text/movie
consumer to implement. Existing FIFO and single-stream execution are retained
for authored overlays; campaign/story presentation and other languages remain
outside this task. A scripted tactical mission is a distinct caller scope even
when it uses a space map.

### Sourced lifecycle for the campaign remainder

The following expands SND-69; it is a behaviour contract for future consumers,
not a claim that the viewer implements these modes.

| Transition | Debug-build behaviour |
|---|---|
| Associated text query | Only the now-playing front event supplies text. The chosen sample index must be valid and the file and text lists must have equal lengths. Return the text at that index; invalid index/list cardinality asserts and returns none. Pending events and failed starts have no associated text. This query does not display text. |
| External hologram request | Requires a speech event and movie name, with no pending request/name and no tracked command-bar movie event. An occupied request is refused, not replaced. Pending movie service additionally waits for no active GUI dialog. |
| External start | A conflicting tracked event name or a different playing speech event aborts the pending request. Otherwise, when no movie event is tracked or no speech event is playing, append the speech to the FIFO, request looping command-bar movie playback and retain its event-name identity. Movie playback starts on that request, without waiting for the selected speech file to become audible. This path retains identity without checking the movie start result. |
| Text appearance | With a tracked movie, display only when the playing event equals the pending external event and no text ID is remembered. Add the indexed text to the command bar's tutorial/message area with `Message_Text_Color` and indefinite duration. No per-sample timer, ordinary VO subtitle or generic comm portrait is established by this path. |
| Completion, failed start or interrupted stream | SND-44 removes the failed/completed front on unpaused queue service. Mode service stops the movie only when no queued or playing event has the tracked name, then clears the movie/request identity and removes any remembered tutorial text. Duplicate FIFO entries with the same event name can therefore keep the movie retained beyond one stream. |
| Pending conflict | The conflict branch clears the pending speech, movie name and remembered text ID; it does not immediately stop the movie or call text removal. The separate tracked-name cleanup owns movie shutdown. Do not turn this branch into an invented atomic movie/text replacement; a conflict after text display needs a campaign capture to establish the resulting visible cleanup. |
| Campaign briefing start | After SND-41's parent/time/cinematic/empty-queue gates, queue the intro, request its looping faction movie and retain the event name only if movie playback succeeds. This route does not populate the pending external hologram event, so the external consumer's equality gate alone does not subtitle the briefing. |
| Pause and resume | SND-13/WBF-53 pause the movie backend and retained speech separately. The speech queue's nested pause state prevents front start/completion service while paused; resume continues it. The indexed query checks the retained now-playing event, not an elapsed text duration. Pause is not completion or a new speech request. |

The campaign remainder comprises the indexed text accessor and cardinality
diagnostics, explicit text-area consumer, movie decoding/rendering and separate
pause hook, galactic corruption request callers and parented briefing movie
caller. Scripted/story consumers need their own caller evidence. Audible onset,
visible cleanup on pending conflict and subtitle layout remain SND-U01 capture
questions; XML and decompilation do not establish their appearance or mix.

## Voice allocation, category and pause implementation evidence

The debug-build allocation, ordinary-admission, 2D start, spatial-pause and infinite-2D-loop-pause paths were rechecked for SND-07/13/16/17/60. Their private receipts live in ignored research output. Equal timestamp replacement retains the first encountered slot, and a zero target-volume 2D start is refused before stealing; zero category/master sliders alone do not refuse it. Gain routing and admission are independent: a nonlocalized unit response uses SFX gain while its unit-response flag still participates in category admission.

Synthetic core contracts cover mixed priorities, oldest and equal-clock replacement, lower-priority refusal, overlap before allocation, all disabled categories, forced boundaries and independent master/category sliders in both spatial modes. The viewer pause case holds an attached spatial loop, spatial one-shot, infinite 2D loop and production speech across two resumes, then exits paused; playback positions, cue counts and headless replay hashes are checked. Associated text/movie consumers remain campaign/story work and other languages remain separate.

### Additional sweep boundaries

| ID | Remaining question | Source boundary |
|---|---|---|
| SND-05 | Backend audible panning law parity. | Retail capture required |
| SND-11 | Hidden attached-audio pose interpolation. | Retail capture required |
| Reinforcements_Requesting_SFXEvent | Runtime consumer of requesting notification. Still unverified: The requesting-notification entry-point search did not resolve a runtime consumer. Ordinary space request/arrival audio receipts do not establish which other mode or cancellation caller reads this tag. | EUS-37 |
