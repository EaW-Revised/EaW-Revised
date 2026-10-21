# Space skirmish setup

- **SSOWN-01:** `Is_Playable` separates lobby factions from appended map-owner
  factions. Append only nonplayable factions that request
  `Create_Player_In_Multiplayer_Games`. Delete map objects assigned to playable
  factions; assign nonplayable objects to their faction's player, retaining a
  nondiscardable decoration without such a player only through the Neutral
  fallback. Evidence: existing [map-object ownership note](../skirmish-start.md),
  debug-build OW-E1 to OW-E8 and effective faction XML; WSS-47. The remake's
  separate teams for appended players remain the documented project deviation.

The applied match switches (WSS-27 to WSS-30) accompany non-default tactical replays in a
tagged header policy record. Playback rebuilds purchase content with the recorded flags
and rejects a different bound policy before ticking. The legacy all-enabled policy retains
its existing encoding; see [replay format](../replay-format.md#tagged-tactical-header-extensions-versions-45).

Evidence: the skirmish setup walk (walk 13), retail lobby and faction dropdown
captures at 1280 by 720, the debug build's map-list,
preview, map-header and team validation paths, and the installed lobby dialog
and skin data. Private research extracts are kept outside the published tree.
Existing starts continue to apply SK-01 to SK-24 and SC-01/SC-02.

- **R-SETUP-01:** The space lobby offers a map list with a preview and player
  rows carrying faction, team and controller. The project bounds local rows by
  the lesser of authored capacity and the shared engine seat limit; the host
  stays in row one and other
  rows offer AI or Open. Open rows have no faction/team/colour record (WSS-14/15),
  and their faction and team captions are blank in the retail lobby capture.
  Map changes remove excess records, relocate surviving out-of-range rows and
  clamp excessive teams to the last authored start (WSS-08/09). Teams are
  displayed from one while stored from zero. Start requires at least two
  occupied teams, each with one faction; opposing teams may use the same
  faction (WSS-16–18). Network humans remain a separate staging path.
  The owner-requested local ninth seat extends retail WSS-08, whose local
  dialog stops at eight. Missing row controls extend the last authored row
  geometry and retain its skin. When a map needs the additional controls, its
  rows fit within the authored rows area, preserving the status band. Smaller
  maps restore the authored rectangles and hide the additional idle controls.
  Uneven splits within total map capacity are allowed; the lobby does not
  limit teammates to their side's spawn-marker count (WSS-54). A teammate
  beyond that count gets no starting fleet or marker-derived camera/facing;
  no marker is reused and no fallback spawn is chosen. The player retains
  its credits and population account. Coruscant's six-player map has three
  spawn markers per team, so a 2v4 match starts with no fleet for the fourth
  Team 2 player. Evidence: effective map data SS-OV01 and debug build
  SS-OV02–04, recorded in the [setup walk](walks/skirmish-setup.md#uneven-teams-and-spawn-overflow).
  Stations are shared team structures owned by the first occupied teammate,
  rather than one station per player (WSS-61). Authored player capacity and
  team/start count remain separate limits.
  A modded zero-spawn team follows the same overflow rule for every member.
  Standalone special-weapon sources remain per playable player, independently
  of spawn selection (WSW-01, debug build SS-OV05). The remake's zero yaw for
  overflow is a project mapping of retail's zero facing vector; equal
  reinforcement behavior is unverified. Project viewer defaults prefer a
  free-marker team when adding a LAN AI row, then allow opposing-team overflow;
  an overflow human camera uses the team's spawn centroid, then its first
  station in record order, then the declared-extents centre. Retail assigns
  no starting camera there. These defaults do not add a lobby refusal.
  Felucia, Kamino, Ryloth and Saleucami provide three independent starts;
  the other stock space maps provide two. A four-team free-for-all needs a
  custom map with four starts. Different teams remain hostile even when they
  choose the same faction, and each AI row gets its own controller (WSS-62).
  Distant opponents keep their team's fog until contact reveals them (WSS-60).
- **R-SETUP-02:** Map names come from TED root field 8, a UTF-16 localisation
  key, resolved in the installed text database. Missing text retains its key;
  absent keys use the filename. Root field 9 is the planet context, not the
  lobby title. Effective VFS enumeration supplies stock space maps and mod
  overrides/additions. The project sorts displayed names and initially selects
  the established default map. Remembering the last map is deferred.
  WSS-03–07 supplies header-based space eligibility: capacity at least two,
  levels at least zero and the selected official/custom flag. Filename stems
  and mounted layers do not classify maps. Localized labels include authored
  capacity; absent names use the map stem. Equal displayed names sort by
  logical path as a deterministic project tie rule pending SS-U2.
- **R-SETUP-03:** The preview first resolves the map stem as an installed
  texture, then uses embedded top-level TED chunk 19, then the generic space
  texture. All art and fonts come from the player's installation and existing
  UI skin/font loaders. No retail art or extracted fonts ship in the package.
  Authored top-level start positions are decoded independently of player
  capacity. Team selection admits compatible station/spawn teams only within
  that authored range. Numbered preview icons use centred space coordinates
  divided by map dimensions with preview Y pointing down (WSS-10). A project
  inset keeps labels readable near the preview border.
  Legacy or malformed start metadata produces a diagnostic instead of a
  guessed start count; legacy support remains outside the stock release gate.
  WSS-20/21 offers the nine authored multiplayer colours in selector order.
  An occupied row choosing a colour used by another row moves to the first
  unused entry. Start carries the selected entry to runtime players, HUD
  identity and unit colourization (WSS-47/48), independently of faction colour.
  Preview starts use their first occupied teammate's selected colour.
- **R-SETUP-04:** Start maps occupied rows in row order to explicit skirmish
  fixture options, with empty extra fleets. It uses the same map hash, start,
  live session and replay preparation as explicit CLI slots. The screen never
  adds the M2 demonstration fleet. Its battle uses SC-02's generated camera.
  Command-line performance trace and replay output paths apply to the selected
  battle without an explicit live-session flag, including flush on close.
  Battle completion or the battle's Quit action returns to setup; closing the
  application quits. Invalid maps, teams, controllers and missing faction
  station candidates cannot start. A preparation failure returns to setup
  with a diagnostic, rather than terminating the launcher.
- **R-SETUP-05:** Walk 13 verifies the tactical space options as starting
  credits, heroes, starting units, superweapons and win condition. XML credits
  range from 2000 to 8000 in steps of 1000. Start/max tech sliders belong to
  campaign/custom setup, not this tactical dialog. The space prebuilt-base
  control is hidden; its underlying authored value is retained (WSS-27).
  Advanced options edits a copied record: Accept commits, Cancel/Escape discards
  and Defaults reads the authored record and clears custom state (WSS-22/24).
  Heroes and superweapons off remove named heroes and tagged superweapon types
  from the shared human/AI purchase menus and authoritative build admission
  (WSS-29/30). Free starting units off omits the faction default roster for
  human and AI players, while explicit fixture fleets remain separate (WSS-28).
  Credits, tech, timer, random events and autoresolve fields are retained
  in the copy; their consumers remain with G6. The credits slider bounds are the
  data's `Min_Skirmish_Credits` and `Max_Skirmish_Credits`; the slider shows the
  copy's authored credits clamped into those bounds without rounding to an edit bucket,
  including in-range off-grid mod values, and stays disabled, with its reason as a tooltip, until the
  start applies the selected credits (WSS-25). The win-condition list offers the
  two space conditions (WSS-31) and the chosen one is the start's victory
  condition (`FixtureOptions::victory_condition`); text naming neither leaves the
  data's default. The debug build fills the list in order: all enemy units, then
  enemy starbase, skipping inappropriate entries (WSS-31). The default's caption is the
  database's "Destroy Enemy Space Station", as the retail capture shows. The
  established space-station construction path remains qualified by SS-U3.
  AI
  difficulty is likewise omitted pending purchasing-capable skirmish AI setup (legacy EAWR-603); the debug build's local lobby
  opponent defaults to Easy. Difficulty application remains with that setup
  work and AI difficulty multipliers (legacy EAWR-735); local row controls expose
  Open/AI until selected levels reach battle. Underworld is disabled on teams without its
  starting station. Installed-data contracts validate its start and session
  content on all 24 stock space maps.

- **R-SETUP-06:** The staging screen and its advanced options are built from the
  installed dialog resource (the skirmish staging dialog and the skirmish advanced
  options dialog), placed under the retail layout rules at 1280 by 720: the
  catalogue's rectangles stand for the screen's own hand placement, which the retail
  capture's frame, preview, rows and buttons already match; the game-type list is the
  one known difference (its rows need more room than the gap above the map list gives, so
  it scrolls). Every caption of the setup and its advanced dialog is a text-database
  key, from the resource or from one table the screen owns: the game types (land, space
  and land control), the Open and AI row names, the team word, the three start-check
  messages and the LAN panel's buttons. A key the database lacks shows as written, and a
  mod's text replaces any of them. Still English (no retail key): the land reason shown as
  a tooltip, the map's unavailable reason, the LAN status and summary text and the peer's
  default name. The game-type list shows land and
  land control disabled, with the database's "Unavailable" and the project's reason as
  the tooltip; space is selected. All supported rows stay on screen: rows beyond the map's
  capacity are blank and disabled, as in the retail capture. The name column is the row's
  controller (the local player's name in row one, Open or AI below), as in the retail
  dialog. The colour column shows its swatch only. Faction names come from each
  faction's `Text_ID`, in the order of the faction registries. A refused Start shows
  the retail message for too few players, too few teams or teammates of different
  factions; any other refusal keeps its own diagnostic. Every combo keeps the width its
  resource rectangle gives it (its text clips; it never grows to its longest item), so
  the faction, colour and team columns keep the retail split and gaps. The colour box
  shows its colour as a bar centred in its text area. A combo's drop-down list is the
  list skin as the retail capture shows it: the small frame's rim around the skin's
  translucent backing, the entries in the list font and left aligned, no mark beside
  the chosen one. The popup contents scale with their owning combo. The advanced
  dialog hides the prebuilt-base check in space (WSS-27); its credits slider and
  win-condition list are described under R-SETUP-05. The staging's Cancel button ends
  the screen: it quits the stand-alone screen, and a shell that owns it takes the
  request instead, and the shell's Back and Escape are that same Cancel. The screen opens
  with the player profile's choices (PRF-03): the remembered space map, the local
  faction, colour and team and the game type, each ignored when the data no longer has
  it, and the profile's name for the local player; Start writes them back to the whole
  record (credits, timer, game name and the land choices stay as loaded), Cancel does not.
  Test, capture, report and LAN smoke runs leave the user's profile alone unless
  `--eawr-setup-profile <file>` names one. Unverified: which of the two start-check
  messages shows first when several apply.

The preview, map list and row geometry follow the dialog resource through the dialog
builder and match the measured primary retail capture at 1280 by 720. The LAN panel
below the frame is this project's own and keeps its hand placement. Text metrics and
the animated background remain adaptations; this is not a pixel-identical
reproduction. The native fallback capture is 1920 by 1061, not a verified 1080p
layout. Official/custom filtering follows WSS-07; multiplayer is deferred.
