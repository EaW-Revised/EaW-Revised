# Online lobby, public/private rooms and match entry

Owner-approved product direction, 2026-10-13 (legacy EAWR-3461); implementation
audited at `7672ea1425f24b91bd79b1e28aa3e0861aba56c6`.
These are delivery requirements, not claims of implemented UI or retail fidelity.
[ADR-023](architecture-decisions.md#adr-023-one-lobby-with-private-online-invitations)
records the decision; its original anchor is retained for existing references.
ADR-022 selects the project-run introduction and relay service, configured at
runtime and usable without Steam accounts.

Ship online with **nine capable player slots**, including the requested 3v3v3
arrangement, **public and password-protected private lobbies**, and **one
LAN/Online lobby screen with a chat window**. Two-peer slices are development
milestones, not the online release capacity. Guest display names require no
accounts; service access tokens are optional and OFF by default for a public
community server. If invitation codes are offered, use four letters/digits with
server-side guess limits. Matchmaking and ranked play are outside this plan.
Reuse setup agreement, lockstep and recovery rather than another start path.

## Existing contracts and scope

| Boundary | Existing behavior | Design consequence |
| --- | --- | --- |
| [LAN UI](ui/lan-lobby.md), [model](../include/eawr/presentation/ui/lan_lobby.hpp) | `LobbyGameSource` supplies games, players and scoped chat without blocking a frame. Filtering, sorting, profile name/colour, Host/Join and staging checks exist. | Share the view and model. Online discovery is a separate source; discovery does not authorize a seat or make settings authoritative. Some fidelity-gap paragraphs in the older LAN note predate later setup/mesh work. |
| [SetupSession](../include/eawr/net/setup.hpp) | One canonical offer, digest-bound Preview/Accept/Start, locally computed content identity and tick-zero agreement; human review/Start have no network deadline. | Keep this final start barrier. Live editable staging is a new control protocol; Preview confirms delivery, not readiness. The existing offer is not an editable replicated lobby. |
| [TCP mesh](net-mesh.md) | Two to nine stable seats, each human bound to its own owner. Per-row Team numbers allow allied humans, including coop versus AI; the match needs opposing teams. | The earlier [eight-seat feasibility scope](research/multiplayer-feasibility.md#approved-eight-seat-shared-control) (legacy EAWR-1285) is superseded by the nine-slot release requirement. The 2026-10-15 owner decision requires nine separate owners for 3v3v3; loaded map rows bound human capacity. |
| [GNS](gns-transport.md), [introduction interface](../include/eawr/net/gns_signalling.hpp) | Adapter and rendezvous authorize exactly one remote peer. In-process codes are test fixtures; network service is slice 2a. | TCP nine-seat support does not imply GNS nine-seat support. A new GNS roster/pair-introduction slice is required. |
| [Recovery](multiplayer-readiness.md#join-in-progress-and-reconnect), [reconnect contract](../include/eawr/net/reconnect.hpp) | Authenticated held-seat catch-up, all-peer proof/resume and one desync recovery attempt per incident exist in core. | Surface existing phases and diagnostics. Viewer/TCP integration is still a separate active delivery, not proof that online return already works. |
| [Shared speed](../include/eawr/net/match_options.hpp) | Both tactical and strategic speed choices are sealed session inputs. | Initial lobby values enter setup; in-battle speed stays on its existing input path and permission/throttle policy. |

Backlog reviewed on 2026-10-13: online entry tracking (legacy EAWR-2733, EAWR-3334),
held-seat integration (legacy EAWR-3328), recovery presentation (legacy EAWR-3393),
departure edge cases (legacy EAWR-3419), departed-faction AI (legacy EAWR-2133), and GC
shared seats (legacy EAWR-2013). Reuse those owners. This proposal adds no GC/land
entry, spectators, accounts, clans, ranked play, retail wire interoperability,
or new game modes. Factions, maps, options and captions continue to come from
loaded data and validated setup, including mods; no retail file-name allowlist.

### Current capacity and relay integration gaps

The tactical simulation's `sim::tactical::max_players` is **64**, not eight;
`sim::galactic::galactic_max_players` is 64 and `galactic_max_seats` is 256.
These are owner/seat domains, not proof of online capacity. Setup, lockstep, TCP,
desync monitoring, credential delivery, lobby direct-seat validation and tactical
checkpoint admission share a nine-command-seat bound. Setup accepts up to three
opposing human owners using loaded faction/team rows. The viewer resolves offered
faction IDs through names from the mounted catalog, including mods.
`GnsTransport::peer_count()` remains **2** and its options require two peers.
`live_session_config.cpp` parses faction/player rows, not command seats; its
`skirmish::local_setup_rows = 8` bound describes the local dialog's hidden ninth
row and stays separate from online capacity. The production LAN lobby validates
up to three host-planned human owner rows and offers coop, two-side and three-side
rosters with up to nine shared command seats, bounded by the loaded map rows.
Owner rows show their admitted controllers and readiness; guests cannot claim AI
rows. Nine-seat admission, Start cancellation and commanded battles are covered
at 1/2/4/8 simulation workers. The N-peer GNS roster/relay integration remains pending;
its design (full GNS mesh, room-scoped relay roster, slices N1-N6) is in
[online play for up to nine peers](net-online-npeer.md).

Nine peers mean 36 pair paths in a full mesh, versus 28 at eight, and eight
remote destinations per peer rather than seven. More sealed inputs, votes,
retained history and relay traffic require checking bounded queues and the
departure controls' 1 MiB byte budget and frame cap
`max(1024, (2 * input_horizon + 4) * remote_peer_count)`
(legacy EAWR-3465). Keep 1/2/4/8 **simulation-worker** equality tests: nine is a
player/peer count, not a worker count. Functional nine-seat coverage is required;
the owner has deferred performance testing, not capacity support.

The pending MP-9b slice-2a relay design (legacy EAWR-3468), inspected from its
branch on 2026-10-13, has TLS verification, a required publisher access token,
two-peer pairing, consumed 128-bit codes (32 lowercase hex digits), a two-hour
paired-session age and expiring TURN credentials. Joiners use the code, not the
publisher token. Those are current integration inputs, not the approved release
flow. B changes token policy and optional code format; G adds roster/pair
admission; I adds room listing/password entry; E handles session age and refresh.
This document changes no relay code or wire IDs. Its eventual service document
remains authoritative for implemented wire details.

## Player flow and room lifecycle

Open one lobby screen with a LAN/Online source selector, a game list and a chat
window. LAN keeps broadcast discovery/direct join. Online loads the configured
community service without an account or token prompt by default. Host chooses
a room name, supported map/mode, up to nine player slots and either Public or
Private (password protected). Show password protection on the room row; a private
room can be found in the list but only joined with its password. Do not list or
log passwords, room ownership secrets, peer addresses or rejoin credentials.

Publishing, updating and removing rooms use an always-on listing service, which
the introduction/relay server can host. A password does not make a host behind
NAT reachable: the introduction service still finds and connects peers, and TURN
provides a route when direct connection fails. Password verification happens
inside TLS at admission, with bounded retries and atomic vacant-seat reservation;
the peer handshake and setup remain the final authority. Do not give a list
reader authority to mutate the room. Start hides/closes new-seat admission,
closing the host removes the listing, and rematch creates a fresh room identity.
Expired listing leases remove stale rooms without killing healthy game streams.

Service settings retain certificate trust/name and an optional operator token
for restricted servers. A token only limits who can use the operator's
introduction/relay bandwidth; it is not player identity, build compatibility or
cheat prevention. Public community servers default to token OFF. Operators may
enable it and distribute publisher access separately; TURN allocation/bandwidth
limits remain necessary in either mode. Protect credentials in platform storage,
outside invitations, profiles and logs. A certificate failure requires correcting
trust settings, with no insecure retry.

### Optional four-character invitations

Codes are an optional shortcut, not a replacement for public/private rooms.
If enabled, the server generates a unique active alias of four ASCII letters
and digits, canonical uppercase (`A-Z`, `0-9`). Clients may trim outer ASCII
whitespace and fold ASCII case; reject other lengths/characters and do not guess
nearby aliases or map lookalikes. Never truncate a 128-bit code on the client.
An alias identifies a room; a private room still requires its password. Internal
room IDs and ownership credentials remain long random secrets, separate from
the alias. Repeated slot admissions use fresh reservations rather than reusing
MP-9b's consumed pair capability. A proposed 15-minute alias admission lease is
separate from room listing lease, match duration and held-seat recovery; the
service must report the accepted deadline and confirm revoke/replacement.

Proposed guess budget: **five failed code/password attempts per minute per IP
(IPv6 /64), with a 60-second cooldown**, plus bounded per-room and global budgets.
Keep counters across reconnects, cap their storage and avoid treating a shared
NAT as a verified account. Use the same unavailable reply for absent/expired
aliases; honour server retry delays. These limits reduce casual guessing, not a
claim that a four-character alias is a strong admission secret. Ticket B owns
the MP-9b code-format/rate-limit follow-up; I shares password admission limits.

Join runs one cancellable attempt: `Finding game…` -> `Connecting…` ->
`Checking game files…` -> staging. Preserve room/name after a failed attempt;
cancel releases its reservation. Before content loading or seat authority,
compare a locally generated compiled-build hash in the GNS peer admission hello
(`TcpOptions::build_identity` and the GNS frame-codec hello path), then bind the
same identity through `SetupOffer::identity.build` and local `SetupProviders`
agreement. Do not trust a directory's advertised hash alone. Build matching is
cheap compatibility checking, not access control; local content/mod/tick-zero
agreement remains required. Names are guest labels, with existing
case-insensitive uniqueness checks and an editable refused name.

### Player-facing failures

Local checks appear beside the field and as an error line in the reused lobby
chat/notice pane. Async failure keeps the player in the lobby. Disconnected
staging/game uses the existing terminal notice flow with an explicit return
action. Texts below are proposed localized strings, not literal backend text.
Show a safe diagnostic ID in Details; never raw GNS address-bearing debug text,
codes, tokens, TLS private material or peer socket addresses.

| Condition | Text | Recovery / diagnostic mapping |
| --- | --- | --- |
| Empty/malformed input | `Enter four letters or digits.` | Fix field; no service request. |
| Unknown, expired, consumed or revoked code | `This invitation is no longer available. Ask the host for a new one.` | New invitation. Existing 1510/1515/1516 where applicable; slice 2a maps its own typed reasons. Do not infer expiry from a generic connection failure. |
| Occupied seat / full room | `This game is full.` | Return; no automatic retry (1508 or authoritative staging refusal). |
| Wrong build/protocol | `This game uses a different EAWR build. All players need the same build.` | Show safe build identifiers (1502/1503); don't download/execute anything automatically. |
| Content/map mismatch | `Your game files or mods do not match the host's game.` | Show validated differing content category/map; return to setup/content selection. `SetupRefusal` owns the cause. |
| Wrong room password | `The password was not accepted.` | Edit password; honour admission retry/cooldown limits. Never echo the password into chat or diagnostics. |
| Duplicate name | `That player name is already in this game. Choose another name.` | Edit name; new consumed invitation if necessary. |
| Service not configured / GNS disabled | `Online play is not available in this build or service configuration.` | Open settings; 1500 when disabled. |
| Trust failure / access denied | `Could not verify the online service.` / `The online service did not accept your access credentials.` | Fix configuration; no insecure fallback. Use the service's typed reason, not guessed new numbers. |
| Capacity/rate limit | `The online service is busy. Try again shortly.` | Respect server retry delay if supplied; bounded backoff otherwise (1513 or slice-2a limit reason). |
| Route failed or timed out | `Could not connect to this game. Check your connection and try again.` | One manual retry; setup remains. 1506/1507/1517 as reported. Health alone does not prove TURN works. |
| Kicked / host closed staging | `The host removed you from this game.` / `The host closed this game.` | Return to the same Online lobby. Distinct authoritative lobby reasons; no new wire IDs allocated here. |

All timeout/rate decisions run outside sim time. Inputs and buttons remain
responsive while I/O is in progress. Cancellation generations discard stale
publish/join callbacks so an old success cannot enter a cancelled room.

## Authoritative staging state

Discovery is a hint. The **host owns pre-game staging**, while all peers run the
agreed battle simulation after Start. Add a bounded, versioned lobby control
channel over admitted reliable peer streams, separate from sealed game inputs
and from the introduction service. No message kind/error number is reserved by
this document; implementation tickets must obtain the coordinator's ranges.

Use full snapshots first, not deltas. At this capacity a snapshot is small and
late admission/cancellation is easier to validate. Every snapshot includes a
session ID, monotonically increasing revision, host identity, stable seat IDs,
seat-to-owner assignments, bounded name/colour/connection state, owner rows
(faction/team/AI/open/closed), map identity and supported options. Seed and
initial session policies are fixed in the final canonical setup; reconnect,
resync, timing and credential-lifetime choices must also agree before tick zero.
Secrets are provisioned confidentially to their authorized peers outside sim,
replay, snapshots used for display, and chat.

Clients send requests bound to session/revision and their admitted seat. The host
validates each request against loaded map capacity and setup rules, then commits
one revision in host order. A stale request is rejected with a fresh snapshot;
never silently applied against another map or reassigned seat. Reject spoofed
seats, foreign sessions, oversized text/rosters, unknown options and unauthorized
host controls. Bound request queues, snapshot bytes and chat; sanitize UTF-8 at
the network boundary following the existing LAN model. No network arrival order
becomes simulation command order.

| Field/action | Authority and effect |
| --- | --- |
| Seat admission | Host assigns a stable vacant seat after transport checks. Room capacity is fixed while connected; changing required human count rebuilds admission, not an in-place transport mutation. Disconnected seats never silently become another player's commands. |
| Names/colours | Player requests their own name/colour; host commits after validation. Names are unverified labels. An observer cannot edit another seat. |
| Faction/team and ownership | Owner decision 2026-10-15: every local, LAN and online space/ground skirmish human has its own owner row (units, credits, base, faction, colour); alliances use the row Team number. Host accepts/overrides assignments. Shared-owner mappings are reserved for Galactic Conquest and refused for skirmish. Eight-row maps support eight humans; nine humans require nine rows. Faction IDs and map capacity come from loaded data. |
| Map, advanced options, AI/open/closed rows | Host only, through the existing validated skirmish builder. Joiners see a read-only map/preview/options summary and local availability. Unsupported choices remain disabled with a reason. Every human occupies a separate loaded owner row. |
| Ready | Each human explicitly accepts the current revision. A consequential change (map/options, faction/team/colour, seat membership or owner mapping) clears all ready flags. Name/chat-only changes do not. Host also explicitly readies its selected setup. |
| Start | Host only; every required seat is connected/ready, teams/map/options valid, and the exact revision is frozen. Run `SetupSession` with manual accept/start. Each peer's existing setup Accept must match that frozen offer digest; UI readiness cannot bypass content construction or tick-zero proof. The host's setup acceptance remains implicit in the core's host role. |
| Unready during Start | Before Start commitment, abort the proposed start and return everyone to staging; clear readiness. After the reliable Start commitment, controls are locked and leave uses the agreed session/departure path. Do not rewrite an in-flight offer. |
| Kick | Host may remove a non-host seat before commitment; send a typed reason, close its streams/reservation and clear readiness. Deny that admitted identity for this room lifetime, and cancel its admission reservation. Optional room aliases do not override this denial. No claim of account-level ban protection against a new identity. |
| Chat | The shared screen has a chat window. Room chat uses admitted streams, immutable session ID and host-assigned bounded sequence. Online lobby chat uses I's bounded service channel; LAN chat stays source-specific. MP-9b alone supplies neither room state nor online lobby chat. |

Lobby readiness and final setup acceptance are separate gates. The implementation
may reuse the same Accept control visually, but must display `Validating setup…`
while final acceptance is pending, and distinguish ready icons from delivered
Preview acknowledgements. Offer rejection returns a clear reason to the host;
rebuild staging/start on a new revision. This needs explicit cancellation/reset
integration: `SetupSession` currently takes one offer and is not a live editor.

### Leave and host loss

Before Start, a joiner's voluntary leave frees its seat and clears readiness.
Host leave or loss **closes staging for everyone**; preserve each player's local
profile/settings and return to the selected source with the host-closed notice.
No staging host election in the initial product. A temporary service outage after
pair admission does not itself close healthy GNS game streams; hide unavailable
invitation controls and continue current staging if its authoritative peer is
still connected. Before pairing/admission, failure is terminal for that attempt.

In battle, host is a command seat plus policy duties, not the only simulation.
Use held-seat reconnect first if negotiated. After expiry, apply the existing
agreed departure result. With two humans that result ends the match; shared-control
survivors may keep their faction when the transport/session policy permits it.
TCP peer-to-peer continuity after host departure does **not** qualify equivalent
GNS continuity or adaptive-timing authority transfer. Initial online product
does not promise host migration: terminally return when required policy authority
or route cannot survive, naming the cause. Nine-seat online acceptance must
explicitly decide/freeze timing duties on host loss or add a separately qualified
authority transfer. Never silently elect a host from discovery order.

## One screen, two sources

Keep `EawrLanLobby`/the LAN model as the shared presentation implementation;
rename/refactor only where needed for source-neutral responsibilities. Reuse
profile fields, sort/filter, rows, map preview, accepted icons, chat notices,
audio and cancel/return conventions. Build layout and row count from the loaded
dialog data; small online controls are EAWR additions with localized captions.

`BroadcastLobbySource` remains the LAN discovery adapter. The online source
loads public and password-protected room metadata from I's directory, with
loading/empty/unavailable states and manual refresh. Show occupancy out of the
host's chosen capacity, up to nine, compatibility and password status.
`players()` shows the relevant admitted room or service chat participants,
never an invented online population. Keep the chat window visible for the
selected source; I supplies bounded online lobby chat and C supplies room chat.

Replace implicit address parsing at the join boundary with a typed join target:
LAN endpoint versus online service/session/code. `LobbyGame::address` currently
holds an endpoint string; do not stuff credentials or a code into it and let
`TcpTransport` parse it. The discovery/source identity travels with selection.
Sources cannot leak chat or stale selections across a LAN/Online switch; switching
sources while staging requires the ordinary leave/cancel path first. Preserve
unknown ping as `?` until `link_stats` provides an established path measurement;
service RTT is not game RTT. Filters must retain their documented unknown-ping
behavior. Show direct/relayed path only if GNS reports it reliably.

### Viewer binding (ticket I, slice 2)

The shared screen takes the online directory through `net::OnlineRoomSource` (the slice 1 live source
and its fixed fixture both implement it): room rows with a padlock for password rooms, paging, a
manual refresh, a password prompt on join, public or password hosting with the published state, and the
chat window. See [the LAN lobby note](ui/lan-lobby.md#online-half-of-the-same-screen-ticket-i-slice-2).
Join failures map from the service diagnostic to the curated texts above
(1546 wrong password, 1547 cooldown, 1545 full, 1544 unavailable, 1553 duplicate name, 1548 removed,
1549/1542/1551 busy, 1552 timeout); an unknown code gets a generic line, never the diagnostic text.

## Reconnect/rejoin and desync UX

**Recommendation: retain credentials in memory for return within the running
process first; defer crash/relaunch persistence.** Reusing an invitation would
admit a new player, not prove ownership of a held seat. Online repair requires
authenticated replacement rendezvous/pair streams and the existing nonce/HMAC
proof at every survivor. Slice 2a consumes a code and cannot reopen a signalling
session; this is an explicit follow-up service seam, not a UI-only task.

Use the existing policies, not new lobby timers: default initial reconnect grace
30 seconds, progressing transfer timeout 30 seconds, incident cap 300 seconds
before READY, bounded retained log (64 MiB by default). Display negotiated values
from live status; these are agreed network policy limits, not sim/replay state.
No late join or spectator may occupy a held seat. Voluntary Leave abandons return
through the ordinary agreed expiry/departure policy. Rejoin after expiry says
`Your reserved seat is no longer available.` Do not make a new player mid-battle.

| Core/view state | Player sees | Permitted action |
| --- | --- | --- |
| Missing input / peer wait | `Waiting for <name>…` with measured connection status | Leave; keep I/O/rendering alive. A wait is not a desync. |
| Agreeing/held | `Connection lost. Reconnecting…` and initial grace remaining | Retry repair automatically with bounded existing policy; Leave ends this local attempt. Disable gameplay submission. |
| Catching up | `Rejoining game…` with verified completed-tick progress when available | Leave; no camera/audio side effects from headless replay. |
| Proving/barrier | `Checking synchronized state…` | Wait for all-peer proof/READY/resume; no command authority yet. |
| Rejoin expired/unavailable/auth failure | `Could not rejoin this game.` plus safe reason | Return after terminal agreement; never retry auth endlessly or consume a new public seat. |
| Desync detected | `Players' game states differ. Play is paused.` | Save local diagnostic details; if negotiated, show the single recovery attempt. Never offer an unsafe Continue that ignores hashes. |
| Resyncing | `Restoring synchronized game state…` | Existing bounded recovery; no gameplay input, keep rendering/I/O responsive. |
| Recovered | `Game synchronized.` with previous incident details retained | Resume only after core publication; all controllers/HUD use `active_adapter()`. Suppress replayed UI/audio. |
| Recovery failed / disabled | `Synchronization could not be restored. This match cannot continue.` | Save diagnostic bundle and return; no automatic second attempt for the same incident. |

Core recovery selects the most common completed-state hash; ties, including 1–1,
choose the lowest seat's hash and lowest matching source. Setup/input-history
must already agree; every active peer proves the result. Explain this as an
agreed repair policy, not a cheating verdict. Departed-peer diagnostic-only
divergence is a notice, not a reason to stop surviving peers. Retention exhaustion
warns that reconnect is unavailable while healthy play continues. File diagnostics
locally through the existing sink; sending a bundle anywhere needs a separate
player action and redaction. No automatic upload.

TURN/service lifetime is distinct from code expiry and recovery grace.
**Prolonged connections are a release requirement**, even though online GC is
not in this space-skirmish scope yet. No two-hour service age or expired TURN
credential should force a healthy match to end: E adds admitted heartbeats,
authenticated renewable sessions/credentials and renewal failure handling.
The current lockstep also enforces `max_session_ticks = 1'000'000` through
`SetupOffer::final_tick_count` and batch validation: about **9.26 hours at 30 Hz**,
with wall-clock duration depending on speed/waits. E must lift/qualify this finite
limit and its retention/resource bounds before promising unrestricted duration.
Crash/relaunch rejoin is a nice-to-have and deferred; connection longevity does
not depend on implementing persisted rejoin secrets.

### Long sessions: what E delivers and where the limits are

**Service lease.** Paired peers renew the relay lease over their TLS connection (Kinds
13..16, [protocol](net-relay-server.md)); the maximum is configurable and defaults to 24 h.
The client shows nothing while renewal works. It warns two minutes before a lease that
is still awaiting renewal (EAWR-NET-1735), as soon as a renewal fails (1733, bounded retries),
and 15 minutes before a final lease ends (1730). An expired lease or failed endpoint
shows that the service session has ended (1731 or 1730), even if a direct match continues.
Renewal does not extend the live relay allocation: pairing credentials cover the configured
maximum plus 60 s, clamped to 48 h (24 h 1 min by default). Matches holding relay
credentials show a separate notice at 15 and 2 minutes before pairing plus their lifetime
(1736); after that boundary direct connections can continue, while relayed allocations
may fail and time out (1737). See the [GNS SDK assumption](gns-transport.md#long-sessions-ticket-e).
Held-seat return keeps using the
confidential proof; no code or password is involved.

**Tick and retention caps at the 30 Hz logical rate** (`logical_frames_per_second`).
Wall clock follows the match speed; ticks do not.

| Limit | Value | After 1 h (108,000 ticks) | After 4 h (432,000) | After 12 h (1,296,000) |
| --- | --- | --- | --- | --- |
| `net::max_session_ticks` = `tactical::max_ticks` = `final_tick_count` | 1,000,000 ticks, 9 h 15 min 33 s | 10.8 %, no effect | 43 %, no effect | **reached at 9 h 15 min 33 s**; the match ends there like any match reaching its agreed final tick, and the online view warns 15 minutes ahead (1738; LAN retains its existing end behaviour) |
| Live TURN credentials issued at pairing | `max_session_lifetime` + 60 s, clamped to 48 h; default 24 h 1 min, renewed service leases do not change these credentials | valid | valid | valid by default, though the tick cap ends a normal-speed tactical match sooner; notices at 15 and 2 min before this separate boundary |
| Reconnect log (`max_log_bytes`) | 64 MiB default, 256 MiB maximum, about 20 bytes per idle tick plus 16 bytes and the payload per command | about 2 MB idle | about 8.6 MB idle | 20 MB idle at the tick cap; a heavy match (about 100 commands/s, assuming 40-byte command payloads plus the 16-byte record overhead) reaches 64 MiB in roughly 3 h, after which reconnect is disabled with a notice while play continues |
| Desync monitor (`max_retained_bytes` 16 MiB, `exchange_every` 30) | windowed: rows and inputs are dropped after each agreed window, two checkpoints kept | bounded, no growth | bounded, no growth | bounded, no growth |
| Replay recording / checkpoints / fog sidecar | all share the 1,000,000-tick format limit | unaffected | unaffected | stops at the same tick as the match |

**Why the tick cap is not lifted here.** The cap is the simulation's own limit: the
tactical session, replay format, checkpoint restore and fog sidecar all fail at 1,000,000
completed ticks, and the canonical-input exports used by desync diagnostics are written in
that format. Raising only the net constant would let a match run on while its diagnostics
and recording failed. Lifting it is a sim and format change (with the 76-replay comparison),
filed for the owner as a follow-up; it is not needed for any match shorter than 9 h 15 min.

## Public list and service bounds

Public and password-protected rooms are part of the release, not a later
matchmaking feature. The always-on directory needs publish/update/remove bound
to a long random room ownership credential, build/protocol/content/mod tags,
mode/map, occupancy/capacity and starting/closed status. A guest name is not an
ownership credential. Use leased heartbeats, stale expiry and atomic seat
reservations: list counts cannot resolve simultaneous joins for the last seat.
Exclude starting/running games from new-seat lists. Never expose passwords,
tokens, peer addresses or rejoin secrets in list rows or chat.

Proposed listing defaults: one room per host principal, 30-second lease,
heartbeat every 10 seconds, pages of at most 50 and queries at most once per
second per client. Bound connections, requests, bytes and IP history, including
the code/password guess limits above. Online lobby chat is a bounded,
source-scoped service channel with sanitized names/text, message throttles and
operator/host removal controls; no account or global cross-service chat is
required. Service limits do not replace coturn allocation/bandwidth and unsafe
destination restrictions. These are new I acceptance targets, not MP-9b defaults.

Automatic matchmaking and ranked play are not priorities and have no ticket in
this release plan. No accounts, deployment or service provisioning is authorized
by this docs change.

## Retail FoC comparison (presentation only)

The original online experience used GameSpy. The publisher-authored combined
manual describes sign-in/account creation, searching games, Host and a 1v1 Quick
Match route (base manual p.46). Its FoC supplement (p.26) documents online custom
games coloured blue for no password, orange for password and red for another
version, plus chat-lobby nickname hover showing clan, wins/losses, rank and
disconnects. This is a **manual description**, not a new retail capture:
[combined EaW/FoC manual transcription](https://pdfcoffee.com/star-wars-empire-at-war-manual-pdf-free.html).
The copy is third-party hosted; the underlying manual is the publisher's source.

The FoC lead designer's [2006 multiplayer diary](https://www.gamespot.com/articles/star-wars-empire-at-war-forces-of-corruption-designer-diary-4-multiplayer-improvements/1100-6159013/)
describes selectable Quick Match faction, clan-game flagging/ladder results,
mirror matches and selectable starting positions. The audited resource catalogue
[MNU-30 to MNU-32](ui/menu-screens.md#skirmish-and-multiplayer-mnu-20-to-mnu-32)
adds visible home counters/news, custom-room list/filter/chat, staging, connection
test, profile/buddy/message and clan controls; its online Host/Join callers remain
untraced. The [LAN rules LLB-02/08/09](ui/lan-lobby.md) establish the shared host
options/staging and validation presentation, not GameSpy's internal protocol.

These sources describe what players were offered. They do not prove service
availability today, retail reconnect, host migration or corrective resync. The
[multiplayer walk](behaviour/walks/multiplayer.md) leaves retail reconnect
unverified; its inspected desync Continue performs no corrective transfer.
EAWR's invitation codes, guest entry and recovery are intentional additions.
Reuse clear list/readiness/staging presentation without recreating obsolete
accounts, ladders, service branding or packets. This design includes no original
assets, screenshots, binary addresses, implementation symbols or decompiler text.

## Sliced implementation plan and measured estimates

Ticket names below are planning labels, not allocated issues or message IDs.
File lists name the main ownership seams; new files are proposed. The coordinator
should reuse existing trackers and reserve numeric protocol IDs at dispatch.
All slices keep networking outside sim, local-content validation before Start,
bounded queues, and 1/2/4/8-worker equality in covering network/session tests.

### Calibration

Sample: comparable merged PRs from 2026-10-01 through 2026-10-13, read on
2026-10-13. Duration is UTC **PR creation to merge**, including review/queue
delay but excluding coding before PR creation. It is elapsed delivery-cycle
evidence, not active engineering hours, a staffing promise or a statistically
validated forecast. Exclude unrelated GC work, research-only docs, tiny fixture
fixes and dependency-only builds. Samples and nearest-rank p80 are explicit:

| Cohort | Comparable change (maintainer lookup) | Hours |
| --- | --- | ---: |
| UI/discovery | Viewer host/join/HUD (legacy EAWR-2134) | 9.967 |
| UI/discovery | Lobby/model/staging screen (legacy EAWR-2965) | 9.909 |
| UI/discovery | UDP discovery/chat source (legacy EAWR-2978) | 7.753 |
| UI/discovery | Profile/network text polish (legacy EAWR-3038) | 3.460 |
| UI/discovery | Shared-seat lobby (legacy EAWR-3370) | 7.666 |
| Network/session | Direct TCP (legacy EAWR-2122) | 2.314 |
| Network/session | Setup/readiness (legacy EAWR-2130) | 3.186 |
| Network/session | Adaptive delay (legacy EAWR-3340) | 5.549 |
| Network/session | Held-seat catch-up (legacy EAWR-3344) | 3.171 |
| Network/session | Authenticated rejoin (legacy EAWR-3386) | 1.503 |
| Network/session | Full mesh (legacy EAWR-3364) | 7.999 |
| Network/session | GNS adapter/introduction (legacy EAWR-3369) | 9.310 |
| Network/session | Desync recovery (legacy EAWR-3391) | 2.096 |

UI/discovery: n=5, median **7.753 h**, p80 **9.909 h**. Network/session: n=8,
median **3.178 h**, p80 **7.999 h**. Compute each unrounded duration as
`(mergedAt-createdAt)/3600`; median averages the middle two for even n; nearest
rank p80 is sorted element `ceil(0.8*n)`. Each bounded ticket below uses one
cohort's measured median/p80 with no invented effort multiplier. Large or
unqualified work is split or explicitly excluded, not given false precision.

| Ticket / scope and acceptance | Main files / ownership seams | Dependencies and service follow-up |
| --- | --- | --- |
| A: source-neutral lobby contract/fixtures. Typed room/code targets, LAN/Online switch/cancel isolation, list/password/chat states; preserve LAN fixtures. | `include/eawr/presentation/ui/lan_lobby.hpp`, `src/presentation/ui/lan_lobby.cpp`, `apps/viewer/src/lan_lobby_screen.*`, `tests/ui/lan_lobby_tests.cpp` | None on deployed service; fake sources cover list, trust, optional-token and admission failures. |
| B: online session bridge and build identity. Async room/pair join/cancel; local compiled-build hash at GNS hello and setup; content/tick-zero agreement. Optional four-character aliases with guess limits; public token OFF, optional private operator gate. | proposed `src/platform/net/online_match.*`, `include/eawr/net/gns_signalling.hpp`, GNS frame codec, MP-9b client/server and tests | A + merged MP-9b/enabled GNS; MP-9b follow-up changes **32-hex code format and required token policy**, exposing accepted alias deadline/revoke. Room API comes from I, multi-peer admission from G. |
| C: authoritative staging and room chat. Revision snapshots, bounded requests, ready resets, kick/close, host controls, ordered chat and Start cancellation races. | proposed `include/eawr/net/lobby_session.hpp`, `src/net/lobby_session.cpp`, covering tests; setup reset seam if needed | A/B stream boundary; service introduces peers, never owns setup. Reserve kinds/errors and claim source/test CMake lists at dispatch. |
| D: shared LAN/Online lobby and staging UI. Public/private room rows, password prompt, chat window, optional four-character code field, safe failures and Ready/Start. | `apps/viewer/src/lan_lobby_screen.*`, `lan_lobby_mode.*`, online source and covering UI/viewer tests | A/B/C + I list/password/lobby-chat API; two-peer fixtures are development milestones, not release acceptance. |
| E: prolonged sessions and authenticated in-process repair. Heartbeats, renewable admitted service/TURN credentials, expiry warning/failure handling; lift/qualify tick-count and retention caps. Held-seat return uses confidential proof, never a code/password. | MP-9b service/client, `include/eawr/net/gns_signalling.hpp`, `lockstep_session.hpp`, `src/net/setup.cpp`, reconnect and covering tests | B/C + MP-9b; extend two-hour service age and TURN lifetime, audit `max_session_ticks`/`final_tick_count`. Reuse held-seat core (legacy EAWR-3328); crash/relaunch persistence deferred. |
| F: recovery presentation. Actual held/catch-up/proof/resync/failed states, adapter swap, Leave/cancel and local diagnostic action. | `src/platform/net/lan_match.*`, online match, `src/platform/live_session*`, `apps/viewer/src/live_session_view.cpp` and covering tests | E; reuse recovery viewer delivery (legacy EAWR-3393) and one-attempt incident policy. |
| G: nine-seat net/setup/owner support and N-peer GNS mesh. Audit eight-peer checks; nine stable seats with separate owners, three allied teams for 3v3v3, 36 confidential pair paths, completeness/departure/frame-cap contracts. | net setup/lockstep/TCP/GNS, sim/skirmish setup owner validation, MP-9b roster operations and mesh tests | B/C/E + MP-9b roster extension; functional 3/4/8/9-peer coverage on 1/2/4/8 sim workers. Check departure bounds (legacy EAWR-3465); no nine-worker or performance benchmark requirement. |
| H: nine-seat release lobby integration. Loaded-data owner/team rows, one human per owner, occupancy/readiness, vacant-seat admission and kick/leave/start races; qualify 3v3v3. | shared lobby UI, online match/source, slot parsing and covering viewer/GPU tests | G + D/F/I; no release at two/eight seats. Reuse departed-faction AI owner when promised (legacy EAWR-2133); preserve loaded map capacity validation. |
| I: always-on room list, password admission and online lobby chat. Leased public/private listing, atomic reservations, sanitized bounded chat, guess/query budgets and host removal. | MP-9b directory/admission/chat service extensions, online source, covering service/UI fixtures | A contracts + MP-9b TLS/ownership boundary; coordinate API with B/C. Extend the introduction/relay service, not an account/matchmaking service. |

The measured cohorts above remain historical comparators, not estimates for the
revised scope. A-D no longer describe a two-seat release: nine-seat support,
three-side setup, directory/password/chat and prolonged connections are release
requirements. The old 43.723 / 71.632-hour A-H envelope is superseded; do not
reuse it for A-I. Re-estimate bounded slices when their service contracts are
dispatched. Live internet/NAT qualification, deployment and performance work
remain outside those historical PR-cycle measurements.

For implementation, run only each slice's own contracts and covering UI/GPU
cases/builds under the current worker policy. A synthetic TLS/loopback success
is not NAT success. Release acceptance needs the configured deployed service,
direct and forced-relay routes across independent networks (including symmetric
NAT), nine-seat/3v3v3 admission, public/private list and chat, wrong passwords,
optional code expiry/guess limits, token OFF/ON, service outage after admission,
TURN refresh and tick-cap boundaries, cancellation, build/content mismatch and
recovery. Prolonged-session qualification is functional, not the deferred
nine-peer performance benchmark. Follow the normal rig/coordinator capacity rules;
this design task itself requires only clean-room/reference checks and no builds.

## Owner decisions

The owner's full answer on 2026-10-13 (legacy EAWR-3461) supersedes the earlier
recommendations. Implementation/service qualification remains pending.

| Decision | Owner answer | Delivery consequence |
| --- | --- | --- |
| 1. Release capacity | Nine capable player slots, including 3v3v3; no performance test yet. | G/H are release gates; audit eight-peer checks and three-side setup. Keep 1/2/4/8 sim-worker equality tests. |
| 2. Finding/joining a room | Prefer public and password-protected private lobbies. If codes exist, four letters/digits. | I supplies listing/password admission; B changes optional alias format/rate limits, not client truncation. |
| 3. Screen and staging | One LAN/Online lobby screen with a chat window like retail FoC. | A/C/D share source-aware list/staging/chat; I adds online lobby chat. Retain host-owned state and setup proof. |
| 4. Host leave | Accepted: close before battle; held seat then departure in battle; no host migration. | C closes staging; E/F/G qualify held-seat/departure and loss of required policy authority. |
| 5. Guest access | Guest names and a no-hoops flow; compiled-build hash should match. Token usefulness questioned. | B checks local build hash at peer hello and setup. Proposed operator policy: optional token, OFF for public community servers; only protects service bandwidth, never player identity. |
| 6. Recovery and longevity | Crash rejoin is a nice-to-have; hours-long connections required, with GC sessions in mind. | E qualifies refresh and lifts unintended service/tick limits. Crash persistence deferred; online GC remains outside this scope. |
| 7. Matchmaking/ranked | Neither is a priority. | Public list is in I; automatic matchmaking/ranked have no release ticket. |

## Current two-player game entry

The shared screen reads the online service from Network Options: an empty project default,
a host and port, explicit system/pin/CA-file trust, and the relay-only preference. The live
source performs directory calls on its I/O worker and drains off the frame thread when the
screen retires. Internet selects Online; the existing LAN menu route remains available.
This is the two-player development bridge, not the planned nine-peer online release.

A signalling publication returns a full invitation and a separate publisher-only ownership
capability over TLS. Directory publication proves that ownership; successful admission returns
only the invitation to one guest. Consumption belongs to the signalling session, so a room
lease cannot re-arm it. Removing or expiring a directory room revokes its unpaired invitation,
including any capability already delivered to a guest. An already-paired game continues.
Neither capability appears in the list, chat, profile, UI view or report.

Only public/direct publications may request a four-character alias. Password publications
request no alias from the start. The screen displays only a four-character alias; with aliases
disabled, a public room is still listed and has no displayed code. Room admission and direct
public-code admission share the existing staging and start barrier.
