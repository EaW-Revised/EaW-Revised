# Skirmish battle loading screen

What the local skirmish shows between the setup screen's Start and the battle's first frame, and how
the viewer builds it. Evidence: a retail recording of a Coruscant space skirmish (Rebel against Empire,
fresh launch, 1280x720), the effective FoC XML and text database, and the debug build for the
dialog choice, the data each gadget reads and the loop behind the bar. The engine-free model is
`include/eawr/presentation/ui/loading_screen.hpp`; the viewer draws it in
`apps/viewer/src/loading_screen.cpp` and drives it from the battle's preparation (`map_mode_*`) and
the setup screen's Start (`viewer_host_process.cpp`). Contract cases BLC-01 to BLC-08 are in
`tests/ui/loading_screen_tests.cpp`; the viewer's cases in `tests/presentation/renderer/test_loading_screen.py`.

| Rule | Behaviour | Evidence | Ours |
|---|---|---|---|
| BLD-01 | A skirmish battle uses the dialog `IDD_SKIRMISH_BATTLE_LOAD_DIALOG` of the dialog resources (`guidialogs.rc`), placed full screen, with gadgets stretched on each axis as in UI-L3, whatever the map. Its frame background is the dialog's own. | debug build; retail recording | Built by the UI kit's dialog builder from the installed resources. |
| BLD-02 | The title (`IDC_STATIC_PLANET_NAME`) is the battle text (`TEXT_PREPARE_FOR_SPACE_BATTLE`, land: `TEXT_PREPARE_FOR_LAND_BATTLE`), a space and the display name (`Text_ID`) of the planet the map's root names (map root field 0x09, a `Planet` object). | debug build; recording: "Space Battle at Coruscant" | Applied. A campaign battle's dialog formats the title the same way (the two texts through one `%s %s`, battle text first; a mission has no battle text and shows only the name). |
| BLD-03 | The history list (`IDC_LIST_REFERENCE`) shows the planet's `Describe_History`; the weather, terrain and population captions show `Describe_Weather`, `Describe_Terrain`, `Describe_Population`, all through the text database. The caption headings are the dialog's own texts. | debug build; recording | Applied. |
| BLD-04 | The population picture (`IDC_NATIVE_ICON`) is the planet's `Native_Icon_Name`. The weather picture (`IDC_WEATHER_ICON`) is the planet's `Encyclopedia_Weather_Icon` with a leading `i_pa` replaced by `load` (`i_pa_weather_sun.tga` -> `load_weather_sun.tga`); any other name is used as written. `IDC_PLANET_OVERLAY` shows `load_overlay.tga`. | debug build; recording | Applied. Textures resolve under Data/Art/Textures by the written name, then the stem as `.tga` and `.dds`. |
| BLD-05 | The history wraps at word boundaries to the list's width; a word wider than the line stands alone. A skirmish has only the history list (no tactical or advantage lists). | debug build; recording | Applied; lines are not selectable. |
| BLD-06 | The map picture (`IDC_MAP_ICON`) is `Data/Art/Textures/<map file stem>.dds` when that file exists, framed by the small frame's rim; without it the gadget stays hidden. | debug build; recording | Applied. |
| BLD-07 | `IDC_PLANET_ICON` draws the planet's 3D model behind the pictures, lit by the `Battle_Load_Planet_*` game constants. | debug build; recording; WGP-32/51/52 | Independent loading viewport and model upload; alternate 1, looping first idle clip, 150° source-Z rotation, authored direction/ambient/viewport. Current-model identity takes precedence over the authored location fallback. |
| BLD-08 | The player-name, faction, credit and other-progress gadgets of the dialog stay hidden in a skirmish. | recording | Applied. |
| BLD-09 | A map whose root names no planet the data describe: unverified in retail. | none | Project choice, least visible: the title takes the map's display name, the texts and planet pictures stay empty, a warning is reported. |
| BLD-10 | The bar (`IDC_PROGRESS_SOLO`) follows the load's real work, reported as a fraction; the screen redraws while the load runs (about twenty times a second). | debug build | The preparation reports its stages in run order (map, session, effects, interface, scene; the scene by placements composed), weighted by their measured preparation cost (below). The fraction never decreases and reads full only when the load is done. No timer drives it. |
| BLD-11 | When the load is done the bar is full, Begin (`IDC_BEGIN_BUTTON`) appears and `GUI_Text_Hint_SFX` plays. | debug build; recording | Applied (the sound through battle audio, WBF-10). |
| BLD-12 | The battle's simulation does not advance until Begin: a click on Begin, Enter, Space or Escape. The screen holds indefinitely (still up 50 s after the load in the recording). | debug build; recording | Applied: the live session stays in its ready phase at tick 0; the screen takes the pointer, so nothing reaches the battle behind it. |
| BLD-13 | Begin starts the battle and closes the dialog; the next frame is the battle with its HUD, no cinematic. | recording | Applied in the same frame. |
| BLD-14 | Project choice: the window's 3D view is off while the screen loads (nothing would show under it) and on again when Begin appears, so the battle's first frame does not compile pipelines. | project | Applied. |
| BLD-15 | Begin appears for a solo battle only: a skirmish or a campaign battle whose game mode is not a multiplayer one (LAN, internet, Steam). A multiplayer load dismisses the screen by itself when it ends. Begin and Escape each resume the battle and close the dialog (Escape is not a cancel; Enter and Space are the same press, WGP-114). During the load the loop pumps the window's messages with the input-safe flag clear (the same load that draws the bar), so a key or click made before the load ends is dropped and cannot reach Begin. | debug build (the dialog's completion, its message callback and the load loop) | Applied for both dialogs: the load holds the main thread, so the screen dispatches the window's queued messages once, with Begin unarmed, and swallows them (`drained_inputs` in the report) before Begin shows. The dialog has no AI-only, observer or replay branch; whether a replay load shows the dialog at all is unverified. |

## Load stages (BLD-10)

The viewer prepares a battle on the main thread, so the bar is painted through the rendering server
and the screen is drawn on the spot between work items (at most every 50 ms, and at every stage
boundary). The screen is built a frame or two before the work starts: a Control's first draw needs a
normal frame.

| Stage | Work | Weight |
|---|---|---|
| map | the map read and decoded | 0.05 |
| session | the live session prepared (unit tables, Lua, the start) | 0.20 |
| effects | breakoff props, projectile models, battle input, live fog, battle audio | 0.10 |
| interface | the tactical HUD | 0.05 |
| scene | the placements composed and uploaded, then the view's own preparation | 0.60 |

The report's `loading_screen.stage_ms` gives the time between preparation reports. A warm-cache
Coruscant setup run measured session 1233 ms, effects 528 ms, interface 279 ms and scene 3851 ms.
The bar rounds those shares to 20%, 10%, 5% and 60%, keeping 5% for the map decode that precedes
the first report. These are our preparation costs, not retail timing measurements. A cold cache can
change each stage's cost; the fraction still advances only when preparation work completes.

## Data read

| Tag | Object | Where we apply it |
|---|---|---|
| `Text_ID` | Planet | `loading_screen.cpp#loading_screen_content` (BLD-02) |
| `Describe_History`, `Describe_Weather`, `Describe_Terrain`, `Describe_Population` | Planet | same (BLD-03) |
| `Native_Icon_Name`, `Encyclopedia_Weather_Icon` | Planet | same (BLD-04) |

The `Battle_Load_Planet_Direction`, `Battle_Load_Planet_Ambient` and `Battle_Load_Planet_Viewport`
game constants light and place the independent 3D planet of BLD-07.

## Unknowns

- WGP-51/52: the loading handoff copies the resident viewer's current model and
  instance transform, composing its source-Z rotation without scaling twice.
  Without a resident model it uses the platform's authored model/scale or the
  location type. Clone animation starts at reset (UA-01/02 first IDLE clip);
  complete retail cold-load timing remains U-11 in the galactic presentation walk.

The preview owns one gadget-sized LDR viewport without MSAA or a shadow atlas,
and uploads only alternate-1 surfaces. Textures use the shared GM-04 adaptive
budget (including 4 GB devices), rather than a second galaxy scene or screenshot.
Dismissal, failure and exit release the renderer's mesh/material/texture leases,
detach the viewport texture and retire the viewport; resize replaces that owner.
Missing or unsupported models leave the authored overlay and a diagnostic, a
project fallback rather than a claim of native missing-model recovery.

WGP-32/51 camera/bounds follow-up (debug build): box extents are half-sizes.
The preview's initial camera setup stores `atan(0.5)` as its full horizontal
FOV, and projection uses its half-angle tangent. Framing therefore uses
`0.5 * length(half_extents) / tan(atan(0.5) / 2)`. The final square projection
has equal X/Y scale `1 / tan(atan(0.5) / 2)`, approximately 4.236068, despite
the authored frustum's ±0.25 shorthand. A square render target is mapped into
the authored gadget rectangle, preserving retail's stretch at a wide viewport;
its canvas underlay borrows the authored dialog frame textures rather than the
shared renderer's inspection backdrop. ONE/ONE atmosphere surfaces render over
that underlay before the foreground overlay: their fragment alpha is one, so
flattening them to a transparent RGBA picture would make the quad hide the dialog.
The underlay reuses UI textures and is released with the private viewport.
- BLD-09's retail behaviour for a map that names no planet is unverified.
- A cold-cache first load was not recorded; the recording's load took under a second.
- Land battles use the same dialog in our model (the title's battle text differs); only space
  battles reach the screen at present.
