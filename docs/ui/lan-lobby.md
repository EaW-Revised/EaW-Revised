<a id="lan-lobby-and-staging-2940"></a>

# LAN lobby, host options, staging and direct connect (legacy EAWR-2940)

The LAN part of the multiplayer flow of the retail game, rebuilt from the dialog resources of the loaded
data. Catalogue rows: MNU-25 to MNU-29 in [menu-screens.md](menu-screens.md). The screens sit on the existing
LAN match (host by port, join by address, setup offer, accept, start); nothing here changes the lockstep, the
replay or a world hash. The game list, lobby players and lobby chat come from a `LobbyGameSource`, the seam a
discovery layer plugs into (rules LLB-10 to LLB-12). Evidence keys are those of the catalogue: **E-DBG** is the FoC debug build (read-only,
2026-10-11), **E-RC** the retail dialog resource script, **E-XML** the effective XML.

## Flow (rules LLB-01 to LLB-09)

| ID | Rule | Evidence | Status |
|---|---|---|---|
| LLB-01 | The LAN lobby is a full-screen opaque dialog. Cancel and Esc shut the LAN session down and return to the **main menu**. | E-DBG | done |
| LLB-02 | Host opens the host options page in its LAN mode (the page the internet lobby also uses). The game name edit takes 24 characters, the password edit 16 and shows as a password; the clan combo and its caption are hidden. An empty name refocuses the name edit. Create stores the last game name and opens the staging as host. | E-DBG | done; password disabled (D-b), clan hidden |
| LLB-03 | The host options list the four retail game types (campaign, land, space, control points) with a description each; this build can play the space one. | E-DBG, E-XML | done; the other three are listed disabled |
| LLB-04 | Join, or a double click on a game, checks in this order: a game is selected, the version matches, the game is not full, the player's name is not already in the game (compared without case), the password. Each failure is a **red line in the lobby's chat list**, not a box. A good join leaves the lobby and opens the staging as joiner. | E-DBG | done; the password step is a refusal (D-b) |
| LLB-05 | The game list rebuilds once a second and has five columns at fixed fractions of the list's width: name, game type, players as `n/max`, map, ping in ms (`?` when unknown). Games with a password are yellow, a game of another version dark orange. | E-DBG | done; colours approximate (unverified) |
| LLB-06 | The filter page sets the game type, a minimum and maximum player count (2 to 9) and a maximum ping (100 to 1000 in steps of 100); both player limits compare the game's slot count, and a ping limit hides a game with an unknown ping. | E-DBG | done |
| LLB-07 | The name edit takes 20 characters, the colour combo lists the nine multiplayer colours, the chat edit takes 128. Changing the name or colour writes the profile and tells the others. While the profile still has its default name the name edit has the focus. | E-DBG | done; the standalone mode opens from the profile's network name and colour and writes both back when they change (`profile_changed`, `--eawr-profile-file` names a test's own file) |
| LLB-08 | The staging has one row per `IDC_PLAYER_NAME_n` gadget of the dialog (nine in retail). The host's row choices are none, open and three AI levels; a joiner sees blanks. The host sees the map list, the preview and the options button; the joiner sees none of them and the start button reads Accept. Esc and Cancel leave the game and return to the lobby. | E-DBG, E-RC | done; see the gaps |
| LLB-09 | Start (host): a map is listed, at least **two humans** are in the game (AI do not count), then the teams: two or more assigned teams, one faction per team. Accept (joiner): the map is available, then the same team checks. Each failure is a **red line in the staging's chat list**: `TEXT_LAN_HOST_MIN_PLAYERS`, `TEXT_LAN_HOST_NOT_ENOUGH_TEAMS`, `TEXT_LAN_HOST_MISMATCHED_FACTIONS`, `TEXT_MAP_NOT_AVAILABLE_TO_PLAY`. The host's Start button is enabled while two humans are in the game and all have accepted. | E-DBG, [WSS-16 to WSS-18](../behaviour/walks/skirmish-setup.md) | done; the mismatched-teams key of the legacy map branch is not modelled (WSS-19) |

## What the trace settled

- **The two 620 by 520 dialogs (MNU-28a) are not used.** The debug build never names the LAN host or LAN join
  dialog, so there is no caller to match. Hosting goes through the host options page (MNU-27) and the staging
  (MNU-28). They stay unbuilt.
- **Direct connect (MNU-29) has no retail caller either.** The debug build never names its dialog or its
  gadgets. We keep it because the lobby's discovery cannot cross every network: a small extra button on the lobby
  opens it. It is an EAWR addition, not retail behaviour.
- **Validation lines are chat lines.** The catalogue rows MNU-21 and MNU-28 said "a message box". For the LAN
  staging the debug build writes the lines to the chat list in red. The shared message box is used for the cases
  retail uses a box for: a refused or dropped connection here.
- **Chat sounds.** The lobby plays the send sound on a sent line and the receive sound on a received one. The
  debug build asks for `GUI_Common_Chat_Receive`, the effective sound data spells the event
  `GUI_Common_Chat_Recieve`. The screen takes the name from the data: an event with the asked-for name, else the
  one event within one edit of it (here a swapped pair of letters). It emits `sound_requested(<resolved name>)`
  and neither spelling is in the code.

## The game source seam (rules LLB-10 to LLB-12)

Retail's LAN lobby is built on UDP broadcasts from each player. The remake's transport has host-by-port and
join-by-address only, so the screens read what a lobby lists from one small interface,
`LobbyGameSource` ([lan_lobby.hpp](../../include/eawr/presentation/ui/lan_lobby.hpp)). The discovery layer
below implements it.

| ID | Rule |
|---|---|
| LLB-10 | The lobby tells its source its **presence** whenever it changes (in the lobby, hosting a game with its name, map, game type, TCP port, slot count and password flag, or joining a game) and reads the games, the players and the chat lines back once a frame. A source never blocks the frame. |
| LLB-11 | A game in the list carries its name, map, game type, address (`<ip>:<port>`), players and slot count, a password flag and whether its version matches (a game of another version is listed and refused at Join). A locked game (the host left the staging for the battle) is not listed. |
| LLB-12 | A chat line carries a scope, the lobby or one game's staging, and the sender's name. A line of another scope or another game is not shown. The lobby's own lines are shown at once and sent to the source. |

Without a source (none given) the list stays empty and a typed address still joins: the direct-connect page
(MNU-29, an EAWR addition) is the way in. `FixedLobbySource` lists a fixed set of games and players, replays
queued chat and remembers what was announced and sent; it feeds the contract tests and the standalone mode's
`--eawr-lan-game` and `--eawr-lan-player` fixtures, so the screens' list, filter, sort and join are tested
without a network.

## Discovery over UDP (rules LLB-13 to LLB-17)

Retail's LAN lobby is built on UDP broadcasts from each player. The remake had no discovery, only host-by-port
and join-by-address, so there is one: small UDP datagrams, [lan_directory.hpp](../../include/eawr/net/lan_directory.hpp)
and [lan_broadcast.hpp](../../include/eawr/net/lan_broadcast.hpp), put behind the `LobbyGameSource` seam by
`BroadcastLobbySource` (`apps/viewer/src/lan_broadcast_source.hpp`). The format is our own; no claim is made
that it matches retail's datagrams, so a retail client and this lobby do not see each other.

| ID | Rule |
|---|---|
| LLB-13 | A viewer sends its **presence** about once a second to the broadcast address, to the loopback address (a second viewer on the same machine) and to any extra port: name, colour, where it is (in the lobby, hosting a game, joining a game) and, for a host, the game's name, map, game type, TCP port, slot count and password flag. A player not heard for five seconds is gone. |
| LLB-14 | A game in the list is a hosting presence at the sender's address and the announced TCP port. Its player count is the host plus the presences joining the game by name. The version matches when the protocol number and the build identity are equal; a game from another build is listed and refused at Join. |
| LLB-15 | **Chat** is a datagram with a scope: the lobby or one game's staging. A line carries a per-sender sequence number, so one that arrives twice (a sender with several targets) shows once; a viewer's own datagrams are ignored. |
| LLB-16 | The match itself is unchanged: the joiner connects over the TCP transport to the address in the list, and the transport's setup offer, accept and start run as before. The UDP datagrams never enter the lockstep, the replay or a hash. |
| LLB-17 | The directory bounds what the network can make it hold: at most 256 players, 256 chat senders remembered (for 60 seconds), 64 listed games (the rest are counted, not listed) and 256 queued chat lines. A datagram from a new sender past a bound is dropped; entries expire, so the room frees up again. |

Wire format (little-endian, at most 1200 bytes): magic `EWLN`, version byte (1), kind byte (1 presence, 2 chat),
sender id (u64, random per running viewer). Presence: protocol (u32), location (u8), colour (u8), flags (u8: bit 0
password, bit 1 locked), TCP port (u16), slot count (u8), then the build, name, game, map and game type as length
(u8) and UTF-8 bytes. Chat: scope (u8), sequence (u32), game and name as above, then the line as length (u16)
and UTF-8 bytes. A datagram with another magic or version, truncated or with trailing bytes is dropped.

Text from the network is untrusted (legacy EAWR-3014). The decoder reads each field whole, so the next one stays aligned, then replaces every invalid UTF-8 sequence (truncated, overlong, a stray continuation byte, a surrogate, past U+10FFFF) and every NUL with one U+FFFD and cuts the field at its limit (255 bytes, chat 400) on a character boundary. The encoder applies the same rule, so what is sent is valid too. Nothing the lobby lists or shows from a datagram is invalid UTF-8.

The default port is 47625. A viewer listens on its own port and sends to the ports in its list, so two lobbies on
one machine use different ports and name each other's (the retail `BroadcastPort2..4` idea). A broadcast does
not cross a router and may meet a host firewall's prompt; the loopback path and the unit tests do not need the
network. A cross-host check is the owner's.

## Authoritative LAN staging

The staging screen uses `net::LobbySession` over the admitted reliable LAN streams.
Discovery still lists games and carries lobby-wide chat; it cannot edit a room's roster,
readiness or chat. The host publishes revisioned map/options and individual player rows; players
request their own name, colour, faction, team and an unoccupied human row. Loaded map/team/palette
validation applies before publication. A repaired map and its player assignments commit
as one revision, so a joiner never sees an invalid intermediate configuration.

Every human accepts the current choices explicitly. The host's existing load-game gadget
is reused as an Accept control; its Start remains separate. A changed map, options,
assignment or colour clears all readiness, while names and chat keep it. The host can
remove a remote seat with the localized Kick entry in that owner row's name dropdown.
A ban follows the admitted connection generation. A fresh two-human LAN connection
can refill the vacant transport index with a new stable seat ID; it does not inherit
readiness. The old edge flushes its notice and close in a bounded room-only retirement.
The listener is reopened only by the host while staging has a vacancy; mesh and
confidential recovery admission keep their separate owners.
The local name edit and owner-slot dropdown share the loaded name-cell rectangle.
These are EAWR staging additions following [the lobby design](../net-online-lobby.md),
not newly claimed retail controls. The room's chat is host-sequenced and bounded by the
core history/rate limits; UDP game chat is ignored once the authoritative room exists.

Accept prepares the selected setup from local installed data on a background worker.
The same `prepare_skirmish` and `lan_recording_setup` helpers build the live scene's
canonical setup. Start freezes those bytes and enters the existing `SetupSession`;
no tick-zero state is published until the live adapter binds its actual context, and
no simulation tick advances through the loading barrier. A kicked player or a lost host
gets the distinct localized terminal notice. This slice covers LAN sources only and
keeps the existing transport/owner capacity; online room admission and nine-seat
presentation remain separate work. Leaving during the live loading handover is a
transport drop; a canceled Start ends that room through the existing terminal flow.
The core's deferred peer closes are flushed by the current I/O owner in both staging
and the live driver.

## Online half of the same screen (ticket I, slice 2)

The `Online` button on the selector swaps the screen's `LobbyGameSource` for an `OnlineRoomSource`
(`include/eawr/net/online_lobby_source.hpp`); the dialog, the sort headers, the name and colour
edits, the chat list and the staging are the same controls. The viewer takes the source through
`EawrLanLobby::Options::online_rooms`; null leaves Online listed as "not available in this build
or service configuration". The standalone mode feeds a fixed source from `--eawr-online-room`,
`--eawr-online-password` and `--eawr-online-list-failed` (no network).

- **Rows.** Name, host (in the type column; the header reads `Host` and sorts by host), occupancy over
  capacity, map and ping (`?`: the service round trip is not the game's). A password room is drawn
  in the retail yellow with a small padlock before its name. Ten rooms make a page; `<` and `>` step
  pages, `Refresh` asks the source for a fresh list now, and the line under the list says why it is
  empty or unavailable.
- **Join.** Join or a double click checks the room like LAN does (other build, full, taken name), then
  a password room opens a modal prompt (secret edit, OK, Cancel, Enter submits). A wrong password
  shows `The password was not accepted.` and a cooldown shows `Too many password attempts. Wait a
  minute and try again.`; both keep the prompt open, any other refusal closes it. The attempt is
  one at a time and cancellable. The password goes to the source only: not to the chat, the report or
  a log. An admitted join attaches the room's signalling scope with the directory admission
  (`LanMatch::join_admitted`; the source keeps its directory connection until the attach returns) and opens
  the shared staging.
- **Room code.** The field under the list takes the optional four-character code and attaches the room by
  its alias the same way. A pasted long invitation of the earlier two-player bridge is no alias: the room service refuses it.
- **Host.** `Host Online` opens the host options with the password edit enabled: empty lists a public
  room, a password makes it private. **Human players** offers two humans up to the chosen map's
  player-row count (up to nine), just as LAN does. Each human has a separate row,
  faction, colour and Team; the create dialog has no shared-sides Teams selector. Set alliances
  with the Team selector on each staging row (LLB-08/09), with one faction per team. Shared sides
  are reserved for galactic conquest. Capacity is fixed while connected; changing it creates a new room.
  The published state (`Listing your room…`, `Your room is listed.`, `Could not list your room.
  Trying again…`) shows as a notice line in the staging chat.
- **Chat.** The window shows the service's room chat. Slice 1's service has room chat only, so before
  a room is joined or hosted a typed line is answered with `Join or host a room to chat.`
- **Private rooms** show and publish no invitation code. A typed code always takes the invitation path
  (public/direct invites); a password room is joined only through the directory with its password.
- **Failures** are classified from the diagnostic code alone into curated text; no id, code, address or
  password is shown or logged.

## Joiner setup review

The shared LAN/online staging shows the joiner the selected map's local name and preview,
plus a read-only summary of the received space match options, before Accept. The host's
map controls remain hidden for the joiner; the map-list area displays disabled settings
rows. An options-only revision refreshes that summary without requiring a map or roster
change. This follows the owner's joined-game review requirement (legacy EAWR-3759); the host-only
visibility described in the earlier LLB-08 implementation no longer describes this screen.

## Nine-seat online staging

Directory and room-code joins obtain the agreed capacity from the attached room roster before
creating the mesh. Every skirmish human has a separate command seat and player row. A newcomer fills the next
free human row with that row's own faction and unused default colour. Joined members appear by
name on both host and joiner staging screens; AI rows keep their existing controls. The local
skirmish screen keeps its retail eight-row layout; a nine-start map enables the ninth network row.
The **My connections** button opens a scrollable list of all room seats, including vacancies. It shows
your own named links as connecting, checking, direct, relayed or failed; a failed link says whether
relay was available. Closing the list restores the map/settings view. The host can Start only after
every pair in the room is connected and every human accepts the current setup. Its tooltip explains
an incomplete room mesh even when all the host's own links are open; the link count is explicitly local.

Edits remain available while staging. The team and faction checks run at Ready and Start, with
their reason shown once for the failed attempt. A newcomer who cannot reach an existing player
is refused with that player's name and the room keeps the seat available for another joiner.
Connection-state refreshes do not add chat lines. Confirmed battle departures name both affected
players in a banner that expires after ten seconds, and a removed player sees why they left once,
in the terminal dialog. See the [nine-player crew runbook](../net-online-playtest.md)
for the cross-network dry run and the logs to keep.

## Fidelity list: gaps this note does not close

- **No room-less online chat channel yet.** The lobby window can only carry chat for a joined or hosted room until the service gets a lobby channel (reserved relay kinds 20..33).
- **Eight rows, not nine.** The shared setup model caps a map's rows at eight (WSS-08 sourced from the skirmish
  dialog); the staging's ninth row is never shown. The retail LAN staging has nine.
- **One AI level.** Easy and hard are listed disabled; there is no AI difficulty yet.
- **Match options** (the options button) stay with the skirmish setup screen; the staging's button is disabled.
- **Ping needs an established link.** The joined endpoint can display its measured smoothed TCP RTT. Discovery-only games stay `?`; the lobby does not open probing connections.
- **The profile** name and colour are stored by the standalone mode (network name, lobby colour); the last game name is not kept by this screen.
- **The connect dialog** shown between Start and the battle load is not built.
- **Sort direction** of a repeated press is not traced; every press sorts ascending.
- **Direct-connect addresses** are kept for the session only.
