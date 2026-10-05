# Space skirmish setup

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
  rows carrying faction, team and controller. WSS-08 bounds local rows by the
  lesser of authored capacity and eight; the host stays in row one and other
  rows offer AI or Open. Open rows have no faction/team/colour record (WSS-14/15).
  Map changes remove excess records, relocate surviving out-of-range rows and
  clamp excessive teams to the last authored start (WSS-08/09). Teams are
  displayed from one while stored from zero. Start requires at least two
  occupied teams, each with one faction; opposing teams may use the same
  faction (WSS-16–18). Network humans remain a separate staging path.
  Each occupied teammate needs a distinct valid spawn marker on its chosen
  side (WSS-54); insufficient markers disable Start with a diagnostic.
  Stations are shared team structures owned by the first occupied teammate,
  rather than one station per player (WSS-61). Authored player capacity and
  team/start count remain separate limits.
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
  Credits, tech, timer, random events, autoresolve and victory fields are retained
  in the copy; their additional controls/consumers remain with G6/G7. The
  established space-station construction path remains qualified by SS-U3.
  AI
  difficulty is likewise omitted pending purchasing-capable skirmish AI setup (legacy EAWR-603); the debug build's local lobby
  opponent defaults to Easy. Difficulty application remains with that setup
  work and AI difficulty multipliers (legacy EAWR-735); local row controls expose
  Open/AI until selected levels reach battle. Underworld is disabled on teams without its
  starting station. Installed-data contracts validate its start and session
  content on all 24 stock space maps.

The panel, preview, map-list, first two row positions
and Start button follow the measured primary retail capture at 1280 by 720
(roughly two-pixel measurement tolerance). Player and faction columns are
narrower to fit separate colour and control selectors. Unsupported game-type controls
and rows beyond authored capacity are omitted. Text metrics and the animated background
remain adaptations; this is not a pixel-identical reproduction. The native
fallback capture is 1920 by 1061, not a verified 1080p layout. Official/custom
filtering follows WSS-07; saved profiles and multiplayer are deferred.
