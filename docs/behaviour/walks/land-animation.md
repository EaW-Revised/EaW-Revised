# Land locomotion animation

These rules describe observations from the debug build. The presentation policy is independent of the simulation; the live viewer integration follows separately.

| Rule | Behaviour |
|---|---|
| WLAN-01 | Ordinary locomotion recalculates the clip rate each service from the larger of the speed fraction and turn fraction, multiplied by maximum speed divided by the authored animation speed. A zero animation speed uses multiplier one. Scale and clip frame rate do not enter this multiplier. |
| WLAN-02 | Clip advance uses elapsed time, clip frame rate and playback rate. Loops wrap; non-looping clips finish two frames before the stored end. |
| WLAN-03 | Without Walk_Transition, moving members choose WALKMOVE below WalkAnimationCutoff when available, MOVE otherwise, and CROUCHMOVE when spread out. Walk and crouch have their own authored speeds. A walk/move switch preserves normalized phase and uses MoveBlendTime. |
| WLAN-04 | Walk_Transition starts with MOVESTART at rest, continues with MOVE at the carried frame without blending, and selects a MOVE_END clip by stride region on stopping. The start clip follows the simulation start clock's maximum-speed to animation-speed multiplier (LR13-17). |
| WLAN-05 | A standing member with Has_Pre_Turn_Anim uses a signed quarter, half or full turn with begin and end clips. Its rate is Stopped_Rate_Of_Turn divided by Rotation_Animation_Speed when both are positive, otherwise one. Other standing members use TURNL or TURNR at rate one and blend for the shorter of InfantryTurnBlendTime and turn duration. Translation does not request a turn clip. |
| WLAN-06 | Authored blend times cover movement, crouch movement, squad movement, squad crouch movement, infantry turns and idle/walk transitions. The debug build uses four frames for a walker turn, six frames for pre-turn and start/stop requests, and fifteen frames for bike movement transitions. |
| WLAN-10 | A bike starts with MOVESTART, moves at fixed rate one and requests MOVE_ENDONE below 0.55 of maximum speed. |
| WLAN-11 | A moving bike uses TURNL or TURNR as a lean above ten degrees of heading error, otherwise MOVE. A direct side swap passes through MOVE. In-place turning itself requests no clip; the rotation step uses a zero threshold and retains the active clip at zero error. |
| WLAN-12 | Bike roll moves toward the active lean side by Max_Rate_Of_Roll per service, or toward zero otherwise, clamped by Bank_Turn_Angle. |

The playback clock and start-frame choice are presentation-owned except for the simulation's start-clip clock. Deviations D-1 through D-6 are a 30 Hz fixed-point presentation clock, a member-id hash for random start frames, presentation-owned clip phase except for the simulation start clock, linear blend weights until the blend curve is established, a bike heading estimate from committed facing changes, and bike roll applied to the presentation root. The simulation publishes its member-local signed heading error from the movement proposal. D-8 still derives begin/end turn phases in presentation (legacy EAWR-6444). The blend curve, loop period, time source, transition edges and bike heading source remain under investigation. Ground-contact adaptation was not established; no procedural foot placement is asserted.

The simulation publishes the fraction of its moving path turn budget consumed (LR13-24) for generic-team members only; formation members and single objects publish the heading error but no fraction, so the viewer keeps its facing-delta estimate (D-9) for them until the rule is settled for them. A generic member held by the body-pair guard publishes zero. The heading error is measured from the facing before the step that turned toward it. The simulation's own steering and corner path remain deviations from retail. These transient outputs are not saved: immediately after restore, until the next movement step, the viewer falls back to the prior path-heading and facing-delta estimates (D-8/D-9).

The land simulation does not yet publish a spread-out state or produce the stop-point gate (U-LR13-02). The presentation motion view supplies false for both until those producers exist; crouch selection and walker stop-clip admission consequently remain fidelity seams.

When a clip blend needs an untracked bind transform that cannot be decomposed as a proper transform, the viewer samples the incoming clip without the blend. This keeps the unit visible while that unusual asset case remains a fidelity seam.
