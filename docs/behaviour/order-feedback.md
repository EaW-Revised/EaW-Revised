# Tactical destination feedback

Scope: the local player's admitted move, attack-move and guard gestures in the
space scene and minimap. Presentation reads published snapshot data and never changes commands or canonical state.

| Rule | Visible behavior | Evidence |
| --- | --- | --- |
| OF-01 | One acknowledgement is created at the clicked destination for the whole order, including a minimap order. It is local presentation, outside synchronized objects. Move uses `GUI_Move_Command_Ack_Effect`, attack-move uses `GUI_Attack_Move_Command_Ack_Effect`, guard uses `GUI_Guard_Move_Command_Ack_Effect`. | debug build: destination acknowledgement and minimap input; effective `gameconstants.xml` |
| OF-02 | The scene effect uses the particle object's `Space_Model_Name`, its authored colors and `Particle_Lifetime_Frames`. `GUI_Move_Acknowledge_Scale_Space` (5 in FoC) scales the emitter frame, without multiplying the particle size tracks. The ordinary particle geometry uses world dimensions and normal camera projection. The three ordinary types live for 30 logical frames, then their particle systems detach and drain. | debug build: destination acknowledgement, model transform, particle size and geometry submission, particle expiry; delayed retail capture at the default space camera; effective `particles.xml` |
| OF-03 | A double move uses `GUI_Double_Click_Move_Command_Ack_Effect` and replaces the nearest ordinary scene move effect. Attack-move and guard take precedence over double move for the scene. The radar removes its youngest ordinary click before a double click; attack-move takes precedence on the radar, guard otherwise uses the ordinary click event. | debug build: destination acknowledgement |
| OF-04 | The radar event comes from `GUI_Movement_Click_Radar_Event_Name`, `GUI_Attack_Movement_Click_Radar_Event_Name`, or `GUI_Movement_Double_Click_Radar_Event_Name`. Its `Event_Model_Name` is animated with its first clip, without player tint; `Event_Model_Scale` scales it against the radar rectangle. `Event_Duration` expires it; `Event_Single_Instance` replaces an existing event of the same type. `Use_Event_System` enables events. FoC's three click events use scale 0.001, last 0.8 seconds and allow multiple instances. | debug build: radar event admission, placement and servicing; effective `radarmap.xml` |
| OF-05 | Ordinary clicks draw no waypoint path. Alt or Waypoint mode shows only locally owned selected formations; selected craft address their team and duplicate references draw once. The chronological authored queued points start at the formation centre. A repeated segment closes at its earliest repeated point; omit the centre stub if its planar distance from the closing line is strictly below four units. Each destination receives the flag model at identity scale/orientation plus translation. Path lines are a pass-always, unculled line list, sampling the ordinary authored texture at UV (0,0) with RGB 128/255 and full alpha. Loops dash the entire display with the XML dash/gap lengths and velocity; the remake derives phase from elapsed presented logical frames and resets when display mode ends, so pause freezes it and redraw rate does not change speed. Space keeps authored Z rather than a terrain lift. | debug build: waypoint display service; effective `WaypointFlagModelName`, `WaypointLineTextureName`, `LoopWaypointLineTextureName`, `MaxWaypointsPerPath` |

The scene renderer reuses the existing particle backend. The acknowledgement's
scale multiplies the emitter basis, affecting emission positions and velocities;
particle size and tail size retain their authored dimensions. The debug build's
model-scale path changes the transform, while ordinary particle corners use the
size track separately from the transformed position. There is no acknowledgement
specific screen-size or camera-distance multiplier in these inspected paths.

A delayed retail move capture at 1280×720, Highest graphics, fog off and a pinned
map environment measures a 19×14-pixel footprint (18×13 coordinate span) near
0.3 seconds. A 60 Hz recording confirms a 19×13 span 0.3 seconds after the first
visible frame and a peak of 22×15. The cursor was parked away from the destination.
The still measurement uses RGB differences of at least 25 against the pre-order
frame at screen position (740,345). Its opening camera is near the authored maximum
distance 1900, with pitch 50 degrees and FOV 55 degrees: the minimap's near camera
edge spans about 2059 world units, versus 1072 at distance 1000. The captured opening
therefore differs from the XML `Distance_Default` of 1000. The covering GPU case
uses distance 1900 and the spawn target, preserves the same screen position, and
bounds each coordinate span within three pixels of the still measurement for
capture age and edge rasterization. This is a capture-specific regression band.
The project live camera normally starts at distance 1200.

Radar models and their
animation geometry are sampled at load time at 30 Hz, then drawn from cached
arrays. The local radar pool holds 32 overlapping click markers, replacing the
oldest if full; this bounded project limit prevents an unbounded click history.
Animation sampling retains material color and texture and the authored skin.
The additive radar material uses its `Color` RGB; its authored fourth component
is zero and does not hide the event. The shipped click texture has opaque alpha.
The ordinary radar model's corner coordinates expand from approximately
±9.84252 to ±19.68504 over its 30-frame, 30 Hz idle clip. At 0.3 seconds they are
approximately ±12.7952: scale 0.001 yields about 4.25 pixels of geometry on a
166-pixel radar. Its small early footprint follows this authored model and scale.

The space-only scope leaves `GUI_Move_Acknowledge_Scale_Land` and the
terrain lift to land. Despite their names, `WaypointLineLandDashLength`,
`WaypointLineLandGapLength` and `WaypointLineLandDashVelocity` also control the
space loop display. `LoopWaypointLineTextureName` is not read by the inspected display service;
loop lines use `WaypointLineTextureName` instead. Consumers outside this
service remain unverified, so its existing deferred registry row is retained. The authored flag's visible surfaces use the
production model/material renderer without replacing the scene camera,
environment or light; its shadow-volume surface is not displayed. Other radar event types (unit death, control
point transitions, beacons and persistent ability events) remain deferred to
their own consumers. The clock follows presented battle time, including pause
and speed changes; exact paused retail acknowledgement timing remains unverified.

The flag translation uses the renderer basis `(x, z, -y)` while retaining
identity scale and orientation, because the model upload already changes the
authored vertices into that basis. Sending source Y as render height displaces
the beacon away from its route. The stock beacon spans about 6.5 by 5.6 by 35.6
world units. Its additive rings bind the authored `Color` RGB (the fourth
component does not modulate additive RGB), and the detached overlay inherits
scene lighting without replacing the camera or environment. Dash velocity
remains the XML distance per logical frame; presented-tick sampling is a
remake presentation policy consistent with the radar clock, not a claim about
retail paused display servicing.
