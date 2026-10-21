# Multiplayer match HUD

This note scopes the readability and player-overview work reported during multiplayer
playtesting (legacy EAWR-4484, EAWR-4485). It changes no viewer code. Retail observations,
source-audit findings and proposed EAWR behaviour are recorded separately below.

## Evidence and limits

The private capture index and original PNGs stay outside the published repository.
Retail images must never be committed. A still proves the visible state at its capture
time; it does not establish update cadence or departure behaviour.

The existing two-peer release-gate capture shows our Coruscant space match with the
tactical command bar and minimap, and no visible player-name overview. It is a normal
running state, with no connection incident visible. It cannot establish how an incident
message or its buttons look. The sealed player runtime predates the galactic shared-clock
overlay, so the latter is assessed separately from source.

Fresh synthetic recovery-state captures with the sealed **v0.2.6** player DLL
reproduce the reported appearance in all three inspected states: reconnect/grace,
desync-paused and recovered. White message lines sit directly over the lit space
scene. Quit Game, Save and Okay appear as plain captions, without visible button
strips or a surrounding recovery frame. The recovered state's incident header and
Save caption are likewise unbacked. These are production-renderer observations with
injected network state, not real connection-failure measurements. The normal two-peer
capture and these synthetic captures serve different purposes.

The tested galactic multiplayer harness cannot enter the stock campaign because
observer ownership is still being repaired (legacy EAWR-4470, EAWR-4475, EAWR-4480, EAWR-4481).
There is no reachable galactic notice capture in that build. The coordinator therefore
considers tactical skirmish the likely origin of the owner's report; that is an
inference about the report, not an observed retail or galactic rendering fact.

Retail release capture, 2026-10-21: Coruscant Siege, single-player space skirmish,
local Empire against Rebel Easy AI, 1280 by 720. The map name was checked in battle.
The ordinary starting HUD has no visible player-name/colour overview at bottom right;
the command bar ends before the right edge and the remaining area shows the world.
The session used the Highest graphics pin, capture-mod no-fog and a pinned environment.
This is a UI/layout observation, not a lighting or pixel-parity comparison.

The owner's recollection of a bottom-right names and colours panel in multiplayer
remains unverified until a capture demonstrates it. Fields, row order, team grouping,
visibility trigger, update timing, leave and defeat states must not be inferred from
the lobby or from this single-player screenshot. The paired retail LAN attempt was
refused before its second game launched because prior capture tasks remained on that
host. Both owned leases were released and the one started session was stopped; no
multiplayer player-overview, chat or departure behaviour is claimed from that attempt.

Retail release land capture on the same date: Naboo Incursion, local Empire against
Rebel Easy AI, 1280 by 720, Highest pin, capture-mod no-fog and pinned Noon_Clear.
The in-battle map-name check passed. Its normal HUD likewise shows no player-name
overview. Escape opens a centred translucent gold panel over the battle. Its dark
horizontal button strips have yellow rims and white captions, in this order:
Save Game, Load Game, Options, then a larger gap, Quit Game, Resume Game.
This capture establishes their idle appearance only; hover, pressed, disabled and
multiplayer-specific menu differences were not measured by this single-player run.

## Audited presentation paths

The source audit below is at `2af9ab2bc8abfdc6d1d0aac3125d0da8c1abb1a7`.
Later skin fixes do not invalidate the sealed v0.2.6 observations above.

| Surface | Audited implementation | Reuse direction |
|---|---|---|
| Tactical LAN status and recovery actions | `apps/viewer/src/map_mode_hud.cpp`: `LanStatus`, `LanLeave`, `LanRejoin`, `LanSaveDiagnostic` use `EawrUiLabel`, `EawrUiButton` and `EawrUiFrame`; the overlay is assigned `hud->hud()->get_theme()`. The v0.2.6 synthetic eye-check still shows captions without visible skin. | Preserve these components; trace whether this separate canvas branch receives the effective mounted HUD theme, then verify nonempty skin textures and font roles. Class names alone do not prove styling is active. |
| Tactical incident header | The same file creates `LanIncidentText` and `LanIncidentSave`; the text has no separate backing frame. | Use the shared frame/text treatment where contrast needs it, while preserving the distinction between a nonblocking notice and a blocking recovery panel. |
| Galactic LAN notice and clock controls | `apps/viewer/src/galactic/galactic_lan_hud.cpp`: a plain Godot label at fixed coordinates has a one-pixel shadow; four plain Godot buttons expose pause/resume and speed. | Inherit the mounted HUD theme and use the styled kit, with layout in the HUD reference space. Keep existing callbacks and gameplay gates. |
| Tactical pause, Begin and results | `src/presentation/godot/ui/battle_overlay.cpp` mounts the pause shell and result dialog; `EawrOverlayButton` is an action wrapper with caption/optional rollover. | Preserve the mounted shell. The action wrapper alone does not provide a framed push-button skin. |

The reusable kit is in `src/presentation/godot/ui/kit.hpp`: `EawrUiButton` draws the
loaded left/middle/right button pieces in normal, hover, pressed and disabled states;
`EawrUiLabel` uses role-based gradient, emboss and outline; `EawrUiFrame` supplies the
shared frame; `EawrUiList` supplies a framed list and skinned scrollbar. These are the
same controls used by the lobby. `apps/viewer/src/lan_lobby_screen_chat.cpp` shows how
chat kinds select row colours. Use loaded theme values and localized captions rather
than copying retail textures or choosing a new fixed palette.

`src/presentation/godot/ui/tactical_hud_build.cpp` already loads the dialog catalogue,
atlas, fonts and reference-space theme model for the result dialog, then assigns the
built Godot theme to that dialog's frame. Reuse that construction path for network
overlays instead of assuming the tactical command-bar control itself owns the UI-kit
theme. Verify this diagnosis in the fix by inspecting resolved button/frame textures
and repeating the same three synthetic states with the mounted skin visible.

## Implementation work and acceptance

### Readable multiplayer messages and recognizable buttons (legacy EAWR-4484, EAWR-4493)

Scope the fix from captures of each affected state: running, waiting, recovered,
departure, shared pause/speed and refusal. Distinguish tactical and galactic surfaces;
the current tactical recovery panel already uses the kit. Apply the loaded font roles,
contrast treatment and button skin consistently, reusing the components above.
The v0.2.6 capture establishes a visible skin failure; theme propagation is the
source-backed investigation target, not a proven root cause or a completed fix.

Acceptance:

- Show the message over both bright and dark lit battle backgrounds at the supported
  UI scales; captions remain legible, wrapped and inside their frame.
- Every actionable button has visible normal, hover, pressed and disabled states,
  with its displayed rectangle matching its input rectangle.
- Capture the actual tactical recovery/incident and galactic clock controls. Retain
  current pause authority, recovery actions, refusal lifetime and input gates.
- Decorative text and empty panel areas preserve world input; blocking recovery
  controls prevent accidental orders where the existing policy requires it.

### Player names and colours during a match (legacy EAWR-4485, EAWR-4496)

First establish the retail presentation from an in-match capture. The requested
bottom-right location is an owner preference while its retail provenance is unverified.
The implementation should consume the admitted match roster and its resolved lobby
palette. Do not derive identity from transport index, faction colour or current unit
ownership, and do not add a second simulation roster.

Acceptance:

- Show every admitted active seat with its lobby name and resolved seat colour,
  including AI and human seats, without inventing extra retail columns.
- Keep stable seat identity through defeat, leave and reconnect. Match retail row
  retention and status styling only once captured; record an explicit EAWR choice for
  any state still unverified.
- Fit the maximum supported roster and long localized names without covering the
  minimap, command slots, reinforcement controls or message actions at supported scales.
- Supply normal, full-roster, leave/defeat and reconnect eye-checks beside the retail
  reference. A source audit alone does not satisfy visual acceptance.
