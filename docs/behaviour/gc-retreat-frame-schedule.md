# Campaign space retreat frame schedule

## Applicability

FoC campaign space retreat, researched on 2026-10-09. This is the GC supplement
to the space tactical `frame-service-schedule.md` maintained on the Skirmish
branch; fold these separately numbered rows into that schedule at branch sync.
The [frame-order walk](walks/frame-order.md),
[retreat walk](walks/reinforcements.md) and
[time controls](tactical-time-controls.md) retain their other boundaries.
WR-42 excludes skirmish and multiplayer tactical retreat.

Evidence is read-only **debug build** reading, EFS-R01..11, and effective XML.
Private receipts and the evidence map stay in ignored
`out/research/frame-service/fsm-retreat-cadence/`. The earlier WR-56 recording
establishes the visible sequence, not native frame numbers. No new recording
or measured per-ship screen-disappearance frame is claimed here.

The comparison base is `c18759d3e23cb9476e56ff2216da363627cc3147`.
The inspected tactical step and viewer do not implement this campaign escape
sequence; galactic retreat return is a separate consumer. The cases below are
implementation acceptance contracts, not tests passed by that base.

## Interface

Let **t** be the active tactical mode's gameplay frame at service entry, and
**k** the cinematic's elapsed gameplay frames since initialization. These are
distinct from synchronization frames, render frames and video timestamps.
Let **n** be a zero-based member of the final ordered cinematic roster.
Ordinary consecutive admission gives one service call per gameplay frame.
Pause prevents logical admission while render/UI work may continue.

Outputs include countdown cancellation or expiry, staged ship positions,
engine presentation and sound, group flight completion, station lethal state,
and eventual victory/remainder cleanup. A ship accelerating, disappearing from
the camera, completing group flight and surviving in the campaign are separate
events; only the sourced boundaries below may be treated as native frame gates.

## Service order

| ID | Rule | Evidence |
|---|---|---|
| FS-R01 | An admitted logical space frame services an already playing cinematic before battlefield modifiers, pathfinding and shared mode services. The cinematic therefore samples this frame's mode timer before either object manager runs. | debug build EFS-R01/02; WFO-02/06 |
| FS-R02 | Shared mode services synchronized objects and their deletion tail, then presentation objects and their deletion tail, followed by timeline, collision and movement coordination. Setup support follows; retreat runs next outside the editor, before victory countdown and tactical income. Retreat reads the mode frame, not the caller's synchronization-frame argument. | debug build EFS-R03; WFO-11/26..29 |
| FS-R03 | Countdown eligibility is checked at that retreat turn before countdown subtraction. A prevention entry changed earlier in object or movement service is available to this check in the same frame. Escape and station wait do not revisit that countdown eligibility branch. | debug build EFS-R04; WR-53 |
| FS-R04 | Countdown expiry filters disabled engines, initializes the cinematic, stages the remaining roster, resets flight progress, grants retreat immunity, disables targeting/weapons and clears movement coordination. It does not also execute flight movement that call. The prefix has already run: a new cinematic starts at k=0 and is first sampled by the next admitted frame's prefix. | debug build EFS-R04/05; WR-45/46 |
| FS-R05 | While flight remains active, the retreat suffix consumes the cinematic frame sampled by the earlier prefix. At k>=90 it finishes group flight without another movement step and selects station wait if playback continues. Station wait begins on a later retreat call; no individual off-map crossing ends flight. | debug build EFS-R04/06; WR-50 |
| FS-R06 | A station wait at k>=181 creates each living owned station's authored explosion and applies lethal damage, after that frame's object service/deletion passes. That call remains incomplete. When the prefix later ends cinematic playback, the retreat suffix can mark escaped, test enemies-retreated victory and clean up the remainder before the following victory countdown. | debug build EFS-R03/04/07; WR-47/52 |

## Cadence and retained state

| ID | Interval and source | Initialization, resets and gates | Evidence |
|---|---|---|---|
| FC-R01 | One retreat call per admitted mode service; no per-retreat modulo gate or catch-up loop. Countdown subtraction uses the difference between current and last serviced gameplay frames. | Initialization records its supplied starting frame. Non-multiplayer service records the current frame even while idle or in initial lockout. Initial lockout starts at the supplied frame plus trunc(`Space_Retreat_Allowed_Countdown_Seconds` times logical FPS); ordinary frame-zero startup gives 900 at 30 FPS. Positive lockout subtracts elapsed frames, returns while still positive and clamps at zero. An accepted start loads trunc(faction `Space_Retreat_Countdown_Seconds` times logical FPS) without resetting the last-service frame; a pre-mode start can therefore consume this frame's elapsed count. Countdown rechecks eligibility before subtraction and clamps at zero on expiry. Accepted cancellation clears countdown/roster/flight progress and returns idle, preserving the last-service frame and initial-lockout state. Multiplayer tactical skips this progression; editor skips the caller; paused/unadmitted frames supply no call. | debug build EFS-R03/04/08/10; WR-42..45 |
| FC-R02 | Cinematic k comes from an active-mode frame countdown timer. Duration in frames is trunc(seconds times logical FPS), and completion is due at start plus duration. The logical prefix samples elapsed frames; rendering does not increment this counter. | Initialization records the current mode frame and sets k=0. Retreat duration is 5 seconds without a station and 15 with a station. A station-only cinematic uses 10 seconds. At 30 logical FPS these durations are 150, 450 and 300 mode frames. Pause holds them. Enabled `In_Game_Cinematics` and successful initialization are required for playback; absent playback takes WR-50/52 completion paths. | debug build EFS-R01/02/05/08; WR-50/52 |
| FC-R03 | Active cinematic playback overrides ordinary standalone speed and fast forward to target 30 logical frames per wall-clock second, with a whole-millisecond minimum wait of 33 ms. This is pacing, not a promise of exactly 30 rendered/captured frames or real-time completion under load. | Ordinary synchronization/admission gates still apply. Pause refuses admission; render admission is independent. | debug build EFS-R09; WBF-26/28; TM-02/03/07 |
| FC-R04 | Flight moves each roster member once per flight-service call, rather than multiplying displacement by elapsed frames. Below k=30 every member moves 8.125 units along departure direction; from k=30 an inclusive roster prefix uses faction `Space_Retreat_Flight_Move_Increment`, with the remainder still using 8.125. | Expiry initializes the accelerated prefix to member 0 and wait to 0. Movement uses the existing prefix before reducing wait by elapsed gameplay frames; an expired wait advances the prefix by at most one member and reloads `Space_Retreat_Unit_Increment_Wait_Frames`, discarding overshoot. Prefix is capped at the last member. No movement at k>=90 or without playback. | debug build EFS-R04/06; WR-51 |
| FC-R05 | From k>=20, engine brightness advances through the roster by the elapsed gameplay-frame count. Under consecutive frames member n brightens at k=20+n, while k<90. From k>=30 a departing prefix member can emit its jump sound, preferring type `SFXEvent_Retreat_Engines_Jump` over faction `SFXEvent_Exit_Into_Hyperspace`. | Expiry clears engine-brightening and sound progress. Null models omit emitter work; dead/death-clone members omit the jump sound. Engine brightness rises to 1 over 0.2 seconds. These presentation states do not remove a member or declare it escaped. | debug build EFS-R04/06; WR-51 |

For a positive authored wait **w** and consecutive native frames, member 0
accelerates at k=30, member 1 at k=31, and member n>=1 at
**k=31+(n-1)w**, provided that frame is below 90. The immediate first advance
comes from the initial zero wait. Stock wait is 1, so this reduces to **30+n**.
This is the first accelerated displacement, not a model-hide or removal frame.
A modded long wait or large roster can leave later members unaccelerated when
the shared frame-90 gate completes. An elapsed-frame jump does not replay all
missed displacement or all missed prefix advances.

## Camera, flight and authored data

| ID | Rule | Evidence |
|---|---|---|
| FS-R07 | The flight/camera setup uses the map's `ATTACKER ENTRY POSITION` marker, including when the defender retreats. Departure direction is the marker's local negative X rotated by its pitch and yaw. Staging faces ships opposite the marker yaw and searches free space within 2500 units of its position, placing higher maximum-tactical-health ships first. The final departure roster is ordered by increasing distance to a point 10000 units along that staging facing; equal distances preserve staging order. Only members with a model enter this staged cinematic roster. | debug build EFS-R05/06; WR-54 |
| FS-R08 | Ship and station camera keys are constructed by the debug build, not loaded from a retreat camera XML or ship animation file in these paths. The ship shot begins at k=0 with a fixed camera and target derived from the attacker entry geometry: camera beyond the destination by a quarter of the retained entry/destination span, perpendicular offset 150 and height offset 70; target on the other side of the destination by the same quarter span. A station adds attached target/camera keys at k=150, camera offset (0,-2000,2000), and a further camera key at k=210 with frequency 1000 and ease-in/out 0.5. The station-only path starts its attached keys at 0 and its further camera key at 60. Ship translation is FC-R04, not an authored ship animation driving tactical escape. | debug build EFS-R05/09/11 |
| FS-R09 | The ship shot holds until a cut to the attached station shot at k=150, then holds that offset through k=210. Rotation starts strictly after the last key: at k=211 it begins an eased Z-axis orbit around the target, with a 1000-gameplay-frame period and ease-in/out 0.5. The station-only shot analogously holds through 60 and rotates after 60. Attachment offsets ignore station facing; the ordinary model path includes `Ranged_Target_Z_Adjust`. A removed attachment can use its last cached transform. Rendering applies these keys plus the unlocked camera's clip/fade settings; it does not advance k. | debug build EFS-R10/11 |

Read the faction movement/wait tags and station `Retreat_Self_Destruct_Explosion`
through their data interfaces. Rebel and Underworld author the movement
increment 500 and wait 1. Empire authors the separate
`Space_Retreat_Flight_Increment`, already recorded as `foc-ignores` in the tag
registry; do not treat that spelling as an alias for the consumed movement tag.
The debug-build faction default for the consumed increment is **250** and for
wait is **2** (EFS-R10). Stock Empire lacks the consumed increment tag and uses
250, while its authored wait 1 overrides that default. Reading its ignored
500 spelling would incorrectly double accelerated displacement.
`Space_Retreat_Off_Map_Dest_Pos` belongs to countdown facing, not the flight
completion predicate or this camera's marker selection. No registry row is
newly applied by this research note.

The `Unlocked` entry in `TacticalCameras.xml` supplies `Near_Clip` 1,
`Far_Clip` 60000, `Object_Fade_Begin` 10000 and `Object_Fade_End` 11000.
The cinematic camera applies these clip/fade/rejection distances (EFS-R11).
They affect rendered departure alongside projected bounds; they do not replace
the native frame-90 completion predicate with a per-ship distance test.

## Same-frame dependencies

| ID | Writer -> reader | Original order and remake verdict | Synthetic observable |
|---|---|---|---|
| FD-R01 | Earlier prevention supplier -> retreat eligibility | Object/movement service precedes retreat; inspected campaign escape consumer **missing**. | A prevention entry enabled in frame t's earlier service cancels a due countdown at t instead of starting escape; FSC-R01. |
| FD-R02 | Cinematic timer sample -> flight/station gate | Logical prefix precedes retreat suffix; inspected consumer **missing**. | k changes from 89 to 90 before flight reads it: no final movement at 90; 180 to 181 permits station lethal state that frame; FSC-R02/03. |
| FD-R03 | Earlier engine destruction/deletion -> expiry roster | Both manager turns/deletion tails precede retreat filtering; inspected consumer **missing**. | A sole engine destroyed at t before a countdown expires excludes that ship from the staged escape roster at t; FSC-R04. |
| FD-R04 | Retreat station lethal state -> later victory/income consumers | Retreat follows objects but precedes victory countdown/income; inspected escape consumer **missing**. | Station is alive during its object turn at k=181, becomes dead in retreat's suffix, and later consumers see the lethal result; no second object-service pass is implied; FSC-R03. |

Implementations keep copied inputs, partitioned work and ordered commits. The
dependency rows require observable state at these boundaries, not a serial
per-entity copy of the original's service structure. Require equal outcomes on
1, 2, 4 and 8 workers when these cases become simulation tests.

## Cases

| ID | Setup and expected result |
|---|---|
| FSC-R01 | Countdown has one frame left entering t. Earlier object service enables prevention. At t's retreat turn cancel before subtraction; no cinematic, immunity or departure begins. Enabling the same entry at t+1 after escape began does not enter this countdown cancellation branch. |
| FSC-R02 | Two modeled eligible members, stock wait 1, consecutive frames: initialize at t with k=0 and no flight step. At t+1 prefix samples k=1. At k=29 both move 8.125; k=30 member 0 uses the faction increment and member 1 uses 8.125; k=31 both use the increment. At k=89 flight still moves; k=90 no flight movement and group enters station wait if playback remains. |
| FSC-R03 | Retreat with a living station: prefix samples k=180, retreat wait leaves it alive. At k=181 object service runs while it is alive, then retreat creates its authored explosion and applies lethal state but remains incomplete. At k=182 no duplicate living-station explosion is admitted. At k=450 prefix ends the 15-second playback and the suffix completes retreat, victory test, then remainder cleanup. |
| FSC-R04 | Sole engine hardpoint of a roster member is destroyed in t's object pass while the countdown has one frame left. Expiry filtering at t excludes it. A matching ship whose engine remains alive enters the staged roster. |
| FSC-R05 | Retain a modded wait 3 and three members: first accelerated steps are k=30,31,34. Compare one elapsed-frame jump of 3 with three consecutive calls: timers can advance equally, but the single call supplies one displacement and at most one prefix advance. These must not be silently treated as equivalent histories. |
| FSC-R06 | At k=29 render several times while logical admission is paused: k, prefix, wait and ship positions stay fixed. Resume one admitted frame to k=30: member 0 gets its first accelerated step. Changing rendered FPS alone does not change native 30+n acceleration gates. |
| FSC-R07 | With a station, camera/target use the ship shot at k=149, cut to the attached station shot at 150, hold the station offset at 209 and 210, then begin the eased orbit at 211. Station-only keys give the analogous hold at 59/60 and rotation at 61. A missing attachment after a prior valid sample uses cached attachment state when available. |

## Unknowns

| ID | Unsupported boundary and bounded plan |
|---|---|
| UFS-R01 | Exact native frame of each model's screen disappearance is not established by WR-56's video timestamps or by acceleration. For a fixed map, camera and viewport, record per-admitted-frame mode t, cinematic k, final roster order, positions, model visibility and projected bounds alongside the clip. Sample k=19..35 and 89..91; distinguish first acceleration, leaving the viewport and any actual visibility/removal change. A frame-aware campaign probe is required before an implementation promises a fixed disappearance frame. |
| UFS-R02 | FS-R08/09 source the cuts and orbit, but do not qualify the complete rendered effect/model sequence or the prior cinematic history supplying the retained hyperspace span. Constructor span is zero; retreat clears keys/attachment caches without resetting that span. Trace its producers only if a consumer needs an absolute ship-shot position for a particular startup history. A bounded lit campaign clip at k=149/150/180/181/209/210/211 and playback end, with attachment/cache state recorded, verifies the presented result. |
| UFS-R03 | Save/load during retreat, exceptional nonconsecutive service frames, missing entry markers and oversized engine-brightening batches remain runtime boundaries. The ordinary static path has no catch-up movement loop; that is not a measured scheduling exception. Use a separate frame-aware probe if a consumer needs one of these cases. Survivor/attrition statistics and forced triggers remain retreat U-8. |
