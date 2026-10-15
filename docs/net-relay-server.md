# Project introduction and relay service

The standalone introduction server pairs two peers and forwards opaque GNS
signals. Coturn carries game traffic when ICE cannot establish a direct path,
including symmetric NAT. A bespoke data relay would duplicate TURN allocation,
authentication and ICE candidate handling. The pinned GNS native ICE backend
already implements those operations; select coturn as the smaller route.
See the [pinned GNS ICE source](https://github.com/ValveSoftware/GameNetworkingSockets/blob/d534c19aa760df3fb75fd20db13ba1932b8a5463/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_ice_client.cpp)
and [coturn authentication documentation](https://github.com/coturn/coturn/blob/master/README.turnserver).
No Valve SDR access is assumed.

Windows statically builds the official Mbed TLS 3.6.7 release, with archive
SHA-256 `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`.
This portable backend avoids a Windows OpenSSL installation and keeps the
client and standalone server on the same TLS interface. The Linux backend
uses its installed OpenSSL library. Relay support defaults OFF.

The default pinned Mbed TLS configuration has no PSA threading backend.
Native Mbed TLS calls therefore share a process-wide mutex, including TLS 1.3
PSA initialization and key state. Socket callbacks stay nonblocking; endpoint
workers own all these calls. Game/GNS send and poll only access copied queues
and never enter that mutex. Initial publish/join exchanges wait for a bounded
response and should run on the application's setup I/O worker.

Diagnostics 1519..1529 are respectively configuration/TLS, authorization,
malformed protocol, expired code, unknown code, occupied/unpaired session,
rate/byte limit, output queue limit, session capacity, connection/session end
and secure-random/credential generation failure. They are project service
diagnostics, separate from game messages and simulation rule IDs.

## Wire protocol v2

TCP is protected by TLS 1.2 or later: OpenSSL on Linux and pinned static
Mbed TLS 3.6.7 on Windows. Clients verify a certificate name against an explicit CA file, or the system CA
store when `certificate_name` is set to a DNS name and both `ca_file` and
`certificate_sha256` are empty. System mode rejects IPv4/IPv6 literal names
with curated configuration diagnostic `EAWR-NET-1519`; explicit CA-file and pin
modes retain their existing behavior. Windows reads the account and machine ROOT stores and cached CA
intermediates once per connection context on the calling I/O worker, with bounded certificate
counts and bytes; only ROOT certificates are trust anchors. User-installed and
enterprise/policy roots in those stores are trusted too. Lobby operational calls
block the caller, which must be a background I/O worker; `OnlineLobbyGameSource`
provides that worker. They must never run on a frame/UI thread. POSIX uses OpenSSL's
default verify paths. No certificate or revocation URLs are fetched.
Windows outbound HTTPS uses ROOT only and requires the server to send its
complete intermediate chain; the relay client can also use cached CA intermediates.
An explicitly configured DER SHA-256 certificate fingerprint takes precedence
over both the CA file and certificate name. All modes
check certificate validity dates. There is no insecure client mode.
Local diagnostics `EAWR-NET-1740` (untrusted certificate), `EAWR-NET-1741`
(certificate name mismatch), and `EAWR-NET-1742` (system trust unavailable)
contain curated text without certificate contents or endpoint addresses.
The `require_access_token` flag defaults OFF for public guest access. When ON,
publishers present a runtime access token inside TLS. It is an operator-controlled publisher secret, not an account system.
Possession of the random join code authorizes the second peer without giving
it publisher authority;
Peer identities are unauthenticated, self-asserted claims bound to the TLS
connection; they do not prove players or accounts. That connection can only
signal its code's admitted peer and cannot address another session.

Each TLS application frame starts with a big-endian u32 body length, then a
one-byte version (2) and one-byte operation. Strings are big-endian u16 length
plus bytes; integers are big-endian u32. Bodies are at most 66,560 bytes.
Signals remain uninterpreted bytes. No operation changes simulation state.

| Operation | Value | Fields after version and operation |
| --- | --- | --- |
| Publish | 1 | access token, identity, requested admission lifetime in ms |
| Join | 2 | join code, identity |
| Signal | 3 | remaining body is the opaque signal (1..65,536 bytes) |
| Health | 4 | empty; allowed only before admission |
| Published | 5 | full join code, local identity, optional four-character alias, accepted lifetime in ms |
| Paired | 6 | session, local identity, remote identity, TURN URI, username, password, credential lifetime in seconds |
| Forwarded | 7 | sender identity, remaining opaque signal bytes |
| Healthy | 8 | empty |
| Error | 9 | diagnostic number as u32 (1519..1539, or 1730..1731 for lease-ending reasons); connection closes |
| Renew | 13 | empty; paired sessions only |
| Renewed | 14 | remaining lease ms, remaining hard-maximum ms, TURN URI, username, password, credential lifetime in seconds |
| Renew refused | 15 | reason u32 (1732..1734), remaining lease ms; the connection stays open |
| Expiring | 16 | remaining lease ms, remaining hard-maximum ms, renewable u32 (0 or 1) |
| Ping | 10 | empty; admitted connections only |
| Revoke | 11 | empty; unpaired publisher only |
| Revoked | 12 | empty; confirms removal of the unused invitation |

Unknown versions, operations, trailing bytes and invalid sizes fail closed.
Connections receive at most one publish/join request. Pairing consumes the code.
Join codes contain 32 lowercase hexadecimal digits from 16 cryptographically
random bytes (128 bits); identities are printable ASCII with the existing GNS
31-byte limit. With `room_aliases=1`, each publication also receives a random four-character
`[A-Z0-9]` alias. Rejection sampling over secure random bytes avoids modulo bias;
live aliases are unique and resolve to the existing full session code. Both
forms share pairing, expiry and revocation. Clients never truncate full codes.
Unknown/expired/occupied attempts count against five failures per minute per
IPv4 or IPv6 /64 prefix, followed by a sixty-second cooldown from the fifth
failure. The global cap defaults to 100 alias-form misses per rolling minute. Full-code
misses count only against their prefix; valid full-code joins bypass the global
alias cap. Successful joins do not consume either budget; prefix cooldown
blocks both invitation forms. Expired and absent aliases both reply unavailable
(1523).
Publication responses report the accepted lifetime, capped by server policy
(default fifteen minutes). Revoke removes the invitation before acknowledging;
clients keep replacement disabled until acknowledgement or expiry.
Codes expire on a monotonic clock, at the exact admission deadline.
Paired connections have a separate maximum session lifetime and idle deadline.
Disconnect releases both peers and the session; this slice does not reconnect
signalling connections. Endpoints retain their own worker and TLS connection
after the service factory is destroyed. Outbound GNS callbacks copy into bounded
queues and never perform socket I/O or call GNS under a lock.

## Bounds and credentials

Defaults: 128 live connections, 64 sessions, 18 connections per IPv4 address or
IPv6 /64 prefix; at most 2 unpaired sessions per such prefix;
30 accepted connections per IP per minute; 120 protocol operations and 1 MiB
incoming TLS record bytes per IP per minute; 120 operations and 1 MiB per
session per minute. Each connection has 256 KiB / 64 frames of output capacity.
All connections have a 10-second admission deadline, including incomplete TLS
handshakes; every admitted connection has a 60-second idle timeout and maximum
a paired session holds a 2-hour lease that its peers renew (see "Session lease and renewal"), up to 24 hours from pairing.
Every IP limit uses that same aggregated key; mapped IPv4 shares its IPv4 key.
IP history is bounded and retains the current rate window after disconnect.
Live and unpaired counters are maintained on admission/pairing/removal, without
scanning all connections for every IP. Each stream reuses its read buffer.
Slow readers, oversized frames and quota overflow close the pair. The native
client uses the same bounded queue sizes and a 10-second initial request timeout.
Client heartbeat frames count toward the limits. Bounds are configurable within
finite validated caps.

The server generates coturn REST credentials as a Unix expiry timestamp followed
by `:` and a random per-peer identifier. The password is Base64 HMAC-SHA1 of that
username using the server-only TURN shared secret. This use of SHA-1 is the coturn
REST interoperability scheme, not certificate signing. Pairing credentials last for the
configured session maximum plus a 60 s margin (24 h 1 min by default), clamped to
48 h for both providers. They cover successive leases without replacing the live
allocation's credentials. A renewal over the admitted TLS connection issues fresh credentials
(never a consumed join code). Coturn must separately enforce
per-user/total allocation quotas, relay bandwidth limits and destination address
restrictions. The introduction service cannot measure or limit UDP relay bytes.
Neither a server address nor a secret is compiled into a client. Configuration
and generated credentials must never be logged.

## Session lease and renewal (ticket E)

A paired session holds a lease: `session_lifetime` after pairing, extended by one
`session_lifetime` for every accepted renewal, never past `max_session_lifetime`
counted from pairing (default 24 h). Kinds 13..16 carry it; 17..19 stay reserved.

- When the lease has `renewal_window` left (at most half a lease) the service sends
  **Expiring** to both peers, once per lease. The native client then sends **Renew**
  on its existing TLS connection. Any paired peer may renew; the second request
  after a renewal finds a full lease and gets a harmless **Renew refused**
  (1734, too early or already in progress).
- A renewal asks the TURN credential refresh (`TurnRefresh`, below) for credentials
  that cover the new lease plus a 60 s margin, extends the lease, and sends
  **Renewed** with each peer's own credentials. Coturn generates separate random
  usernames; Cloudflare uses two generate jobs, both charged to the provider call cap.
  These refreshed credentials are for new connections, not the existing ICE allocation.
- Refusals never close the session: 1732 the maximum is reached (final warning,
  `renewable=0`), 1733 the credential refresh failed or its budget refused the job
  (the lease keeps its end; the client asks again after `renewal_retry`, 5 s by
  default, validated in (0, 60 s], at most six requests per peer per lease), 1734 too
  early or in progress. Exhausted retries retain 1733 for the view and stop sending.
- Renew spends the ordinary per-session and per-address operation/byte budgets.
  An unpaired session (1524), a connection without a session (1521) or a body on
  Renew (1521) is refused and closed like any malformed operation.
- When the lease ends the service closes both connections with a typed error:
  **1730** the maximum was reached, **1731** the lease was not renewed in time. A
  Renew that arrives after the end gets the same error, not a refusal.
- Client and view only (never sent as service errors): 1735 the service lease is about
  to end, 1736 the separate live credential boundary is near or has passed, 1737 the
  relayed connection ended after its credentials expired, 1738 the match reached
  the maximum match length.

**Credential refresh step.** `src/net/relay_turn_refresh.hpp` defines the internal
`TurnRefresh::submit(job)` / `take()` pair the renewal flow uses. Both run on the poll
owner thread and must not block: the coturn implementation issues its HMAC credentials
inside `submit`; the Cloudflare provider (`TurnProvider::cloudflare`) queues one generate
job on a worker thread of its own in `submit` and returns the outcome from `take` on a later
poll, spending the same per-minute call cap as pairing. Each peer's job carries a unique
attempt key. Pending renewal work has a deadline (two configured Cloudflare timeouts plus
2 s, capped by the current lease end); expiry clears the attempt and sends 1733 so the
client can retry. Late completions cannot satisfy a retry and are revoked best effort.
The pairing credential remains alive across renewals. Pairing and the latest refreshed
credentials are revoked best effort at session end; older refresh credentials expire
naturally at their advertised TTL, so a later renewal does not revoke an allocation
using them. It is not a public interface: `RelayServer::listen` takes it only as a
test seam, as it does for the password hash. A provider with no relay configured returns
empty credentials; the lease still renews.

## Configuration and qualification

Server configuration is `key=value`: defaults, then config file, then
`EAWR_RELAY_<UPPERCASE_KEY>` environment variables, then `--key value` CLI options.
TLS certificate/key, access token and (when enabled) TURN URI/shared secret are
runtime inputs. The service defaults to loopback TCP port 27020. Health uses the
same TLS port and the Health/Healthy exchange; it reports service availability,
not TURN reachability. See [deployment](deploy/relay-server.md) for startup,
firewall and coturn configuration.

Loopback contracts qualify actual TLS client/server pairing and signal forwarding,
expiry, authorization, framing and bounded resource handling. Public internet,
symmetric NAT and TURN allocation/data connectivity remain unverified until a
separate multi-machine NAT qualification. Issuing credentials and configuring
GNS are not evidence that the deployed relay is reachable.

## Viewer online bridge

`make_online_match` takes runtime TLS/service options and a guest peer identity.
For a publicly issued certificate, populate `OnlineMatchOptions.service.address`
with the service host name (or a numeric endpoint), set `certificate_name` to the service's DNS name, and
leave `ca_file` and `certificate_sha256` empty to select system CA trust.
Service host resolution and connection run on the bounded I/O worker, preferring IPv4 and trying at most two addresses. Production Network Options supplies the host name for both connection and certificate verification.
It performs no I/O during construction. Host/join run on a registered bounded
worker; cancellation detaches that generation immediately and drains publication
closure without blocking frames. A single retiring generation blocks another
attempt until closure is known or its invitation expires. Service health is
observed without consuming opaque GNS signals during pairing.

The admitted GNS stream uses the same staging, setup providers and live driver
as LAN. Peer hello compares `core::build_identity()` before content building;
`SetupOffer::identity.build` and the locally computed agreement use that same
compiled identity. Content and tick-zero barriers stay on the existing setup
path. Online admission remains two-seat, opt-in, with recovery delegated to the
online repair slice. This bridge exposes a factory for the shared lobby UI;
public room lists, passwords and online chat discovery are separate slices.

Diagnostics added here: 1530 unavailable online/recovery capability, 1532 guess
cooldown/global budget, 1533 alias generation exhaustion, 1534 unconfirmed publication close, 1535 invalid online staging setup, and
1536 first-snapshot timeout. Invitation strings and service credentials are never copied
into diagnostic text.


## TURN credential providers

`turn_provider=coturn` (default) issues HMAC REST credentials from `turn_uri` / `turn_secret`; each peer
gets its own username. `turn_provider=cloudflare` asks Cloudflare Realtime TURN once per pairing on a
bounded worker thread and sends the same credentials to both peers; the Paired message waits for that
call (bounded by the request timeout), so a join takes one HTTPS round trip longer. Failures never fail
the pairing: it completes without relay candidates and the server logs `EAWR-NET-1556..1559`
(1555 is the startup error for an invalid or mixed provider configuration). The wire fields are
unchanged; the `uri` field may carry up to eight comma-separated `turn:` URIs and clients resolve
hostnames to numeric addresses before handing them to GNS. Operator details are in
[deploy/relay-server.md](deploy/relay-server.md#cloudflare-realtime-turn-instead-of-coturn).

## Room directory, private admission and room chat

The always-on directory shares the server's TLS listener and prefix budgets but
has independent state. Directory expiry never ends an admitted GNS stream.
Relay support remains OFF by default. `NetworkOnlineLobby` provides blocking,
bounded calls for an I/O worker; `OnlineLobbyGameSource` implements the shared
`LobbyGameSource` contract with copied snapshots and a bounded worker queue.
`FixedOnlineLobbySource` supplies public list/member/chat fixtures without I/O.
Online addresses in these rows are opaque room IDs, never TCP destinations.

A directory connection first creates or resumes a server-issued random 128-bit
principal capability. Publishing returns a separate random 128-bit ownership
capability and room ID. Both principal and ownership must match to update,
remove or moderate a room; guest names grant no authority. A principal may host
one room, including across resumed connections. Capabilities stay in memory or
protected caller storage and must not appear in profiles, diagnostics or chat.
Resume uses an ordered lookup of the full 128-bit bearer capability; this lookup
is not constant time. Subsequent ownership comparisons use constant-time secret
comparison for both principal and ownership. Publisher access-token policy applies to directory publishing too. Listing and
joining require no operator token and no account.

Rows contain name/host label, build/protocol/content/mod tags, mode/map,
occupancy/capacity and a password flag. These are discovery hints: peer build,
content and setup agreement remain required. Capacity is an implementation
bound of 64, independent of map/mod values; this slice does not claim a 64-peer
transport. Starting and closed rooms are hidden and refuse new admissions.
A publish/heartbeat grants a 30-second lease; the source renews every 10 seconds.
The service drops stale rooms at the deadline. Ownership cannot resurrect an
expired room. Host connection loss leaves the listing until expiry, allowing a
capability-authenticated resume; guest disconnection/leave frees its reservation.
Heartbeat occupancy is clamped to the host plus live reservations, so stale
advertised counts cannot reopen an occupied seat. Reservations are committed
on the server owner thread before any success reply; the last seat has one winner.
This is directory admission, not game transport/setup authority.

Private rooms store only a random 128-bit salt and PBKDF2-HMAC-SHA256 result
(120,000 iterations, 256-bit output). Password input is capped at 128 bytes and
is neither echoed nor logged. These are short-lived, low-value room secrets;
the fixed 120,000 iterations are a deliberate service bound. Verification is
bounded by four hashes per prefix per minute (including successful publication
and admission), 30 checks per room per minute, and 128 hashes globally per minute.
A bounded single-worker queue holds at most 16 requests, including running and
completed work. Only copied password/salt inputs enter that worker; it reads no
room, seat, prefix-budget or TLS state. The owner validates/quota-charges before
submission and revalidates/commits completion against current room/membership.
Signalling and directory I/O continue while PBKDF2 runs. Disconnect, expiry or
removal can invalidate a pending result without giving it seat authority.
Failed password/unknown-room guesses reuse the alias prefix history: five
failures in a rolling minute cause a 60-second cooldown, shared across reconnects
and IPv6 /64 clients. Long room IDs do not consume the global alias guess cap.
These are guest capabilities and abuse limits, not verified identities/accounts.

Directory defaults and hard bounds:

| Resource | Default/bound |
| --- | --- |
| Rooms | `max_sessions` (64 by default, at most 2,048), at most two concurrent rooms per prefix, separate from signalling sessions |
| Principal history | at most four times `max_connections`; at most four new principals per prefix per minute; disconnected entries retained 60 seconds after their last use and only removed when no room is hosted |
| Queries | at most one per second per principal, also across resumed connections |
| Page | at most 50 rows; keyset cursor is the last room ID when more rows exist |
| Room membership | at most advertised capacity (implementation cap 64), one room per connection |
| Chat history | 200 lines per room; at most 128 UTF-8 bytes per line, names at most 20 bytes |
| Chat sending | at most one message per second per principal |
| Source queues | 64 outgoing lines, 64 completion results, 200 incoming lines, 2,048 displayed rooms |
| Directory prefix budget | `directory_operations_per_minute` = 2,048 requests/minute and `directory_bytes_per_minute` = 8 MiB/minute of incoming TLS bytes plus response bodies, separate from the legacy 120 ops / 1 MiB budget; shared TLS admission caps still apply |

Resuming existing principals and listing are unaffected by the principal creation
quota. Global caps remain necessary: at default limits, filling 64 room slots
requires at least 32 prefixes; filling 512 principal-history entries requires
about 128 prefixes at sustained creation rates, or at least 64 prefixes in a
burst across a fixed-minute boundary. Prefixes are abuse keys, not accounts;
these costs do not claim protection against a distributed adversary.

Chat requires a reserved room membership. It exposes only bounded sequence,
room-local seat, sanitized guest label/text, and a seat/name roster. UTF-8 is
validated, controls and direction overrides removed, and truncation never splits
a character. A client cannot choose another sender name/seat. There is no global
chat. The room owner can mute a non-host member's reads and writes for that
admitted connection; an operator can call `RelayServer::remove_chat_member` on
the server owner thread. The seat remains reserved until leave/disconnect.
A new guest identity is not an account-level ban. No credential, password hash,
peer address or rejoin secret is serialized into directory rows/chat.

### Directory operations in wire v2

All fields use the framing/types above. `metadata` means eight bounded strings
(name, host name, build, protocol, content, mod, mode, map), then occupancy,
capacity, starting and closed u32s. Boolean u32s are exactly zero or one.
There is one outstanding request per connection; reply layout is selected by
that request. Semantic directory refusals use Error (9) and keep the connection;
shared resource/transport failures can close it.

| Operation | Value | Payload / response |
| --- | --- | --- |
| Open | 20 | empty or resume principal string; 30 returns principal string |
| List | 21 | cursor string, limit u32; 31 returns next cursor, count and rows (room ID, metadata, password flag) |
| Publish | 22 | publisher access token, metadata, password; 32 returns room ID, ownership and lease milliseconds |
| Heartbeat | 23 | room ID, ownership, metadata; 30 empty acknowledgement |
| Remove | 24 | room ID, ownership; 30 empty acknowledgement |
| Password join | 25 | room ID, password, guest name; 33 returns room ID, directory seat and invitation, then (room with an open scope) the reserved slot and member capability |
| Leave | 26 | empty; 30 empty acknowledgement |
| Chat send | 27 | text; 30 empty acknowledgement |
| Chat poll | 28 | last sequence; 31 returns through sequence, line count, lines (sequence, seat, name, text), roster count, members (seat, name) |
| Chat removal | 29 | room ID, ownership, non-host seat; 30 empty acknowledgement |

Diagnostics 1772..1777 belong to the room scope (see "N-peer rooms"). Diagnostics 1540..1553 cover malformed directory input, ownership/access,
query throttle, existing room/membership, unavailable room, full room, wrong
password, guess/hash cooldown, removed/nonmember chat, capacity, hash/random
failure, chat throttle, client response timeout and duplicate guest name respectively. IDs 1555..1559
remain reserved; 1554 refuses a password change while publishing/published.
The client resets its stream after transport failure/timeout and resumes its
principal on the next call. Only malformed service protocol is latched. The
principal accessor reads a separate snapshot and never waits for I/O. An expired
resume capability is cleared so a subsequent exchange can create a fresh one.

A transport reset ends a guest's connection-bound directory admission; guests
must rejoin. A resumed host principal automatically restores its reserved chat
seat when it polls/sends, preserving the room ID; lease and metadata changes
still require the ownership capability. If a publish commits but its reply is
lost, the caller lacks the returned ownership capability and a retry can receive
1543 until that unrenewed 30-second lease expires. No lost reply is treated as
permission to mutate or silently replace an existing room.

The source exposes idle/publishing/hosted/retrying hosting states. Failed initial
publication retries after five seconds; heartbeat 1540/1541 drops the old
publication and retries on the next worker tick. Display labels are normalized
once before both publish and heartbeat. Password changes while published are
refused with a typed notice, because this wire version has no password update.
Removed membership clears active chat/roster state, releases its service seat,
and queues a bounded notice. A host first reattaches a lost directory channel
with its existing publication; only an expired/refused heartbeat replaces it;
success clears the current error. Invalid begin_join requests still produce a
refused completion. The source discards cancelled join completions and releases a
late successful seat; it never blocks a frame on I/O. Shutdown may wait for the
configured bounded request timeout and a final removal/leave exchange.

## N-peer rooms (signalling scope)

Design: [Online play for up to nine peers](net-online-npeer.md), slice N3. The
room scope is the signalling counterpart of a directory room. It replaces
two-peer pairing for rooms of 2..9 members; invitations (Kind 1..12) keep working
unchanged. The service introduces and signals. It never carries game frames and
never parses a signal blob: the pairs among members are ordinary GNS connections.
Relay support stays OFF by default; nothing here starts with the viewer.

### Model

One room per published directory room, keyed by the same 128-bit room ID. The
**host** opens the scope on its own TLS connection (a room member connection is
separate from the directory connection) and holds slot 0. Slots `1..capacity-1`
belong to guests; **the slot number is the transport peer index**. Capacity is
the directory row's capacity at open time (2..9, the `max_match_seats` bound,
separate from the directory's listing cap of 64) and cannot change while the
scope is open. A heartbeat that names another capacity is refused (EAWR-NET-1540).

| Slot state | Meaning |
| --- | --- |
| vacant | free |
| reserved | admitted through the directory, not yet attached (30 s) |
| attached | bound to a live member connection |
| held | started match, connection lost, capability holder may reattach |
| left | started match, member left, was kicked or the hold ran out |

One reservation table serves the directory admission and the public alias. Every
change commits on the owner thread before any reply or push, so the last vacant
seat has exactly one winner whichever path asks. The directory's *Password join*
reply (value 33) appends the reserved slot and a 128-bit **member capability**
when the host has opened the scope; a room without a scope replies as before
(slot 0, empty capability). The capability only opens the signalling path: it
never proves the player to the other members (the pair secret and the reconnect
proof do).

### Operations (Kind 34..49)

| Kind | Name | Direction | Body |
| --- | --- | --- | --- |
| 34 | room_open | host to service | room ID, ownership, principal, identity, u32 alias flag |
| 35 | room_opened | service to host | slot, member capability, identity, alias, TURN URI/user/password, TURN lifetime s, lease ms, hard-maximum ms |
| 36 | room_attach | guest to service | room ID or 4-character alias, member capability (empty for an alias), identity |
| 37 | room_attached | service to member | as room_opened (also the Reattach reply) |
| 38 | room_roster | service to attached | revision, capacity, state, count, per slot: state and identity |
| 39 | room_signal | member to service | destination slot, blob (1..65,536 bytes) |
| 40 | room_forwarded | service to member | source slot, source identity, blob |
| 41 | room_leave | member to service | empty |
| 42 | room_release | host to service | slot |
| 43 | room_kick | host to service | slot |
| 44 | room_lock | host to service | empty |
| 45 | room_reattach | member to service | room ID, member capability, identity |
| 46 | room_closed | service to member | reason; the connection closes after it |
| 47 | room_ack | service to member | empty (release, kick, lock acknowledgements; leave closes with it) |

Kinds 48..49 stay spare. The request router has an explicit range check for
34..49; a room member connection answers only these plus ticket E's renewal
(13) and the client heartbeat (Kind 10), and a directory or invitation connection
refuses all of them.

* **Roster.** One snapshot per revision (under 400 bytes at nine seats) to every
  attached member on every slot change, including reservations. Identities appear
  once a member has attached; no addresses, names or capabilities are ever sent.
  The revision grows by one per change; a client ignores older revisions.
* **Signals.** Delivered only between two attached members. The service stamps the
  source slot and identity, and the receiving adapter checks that identity against
  the roster. Because the roster reaches a member before the newcomer can signal
  it, a forwarded signal from a slot the receiver does not know is malformed.
  A signal to oneself, to a seat outside the room, an empty or oversize blob or a
  malformed frame is a protocol violation and ends only the offender's connection.
  A signal to a seat that is vacant, reserved, attaching, held or left is a benign
  race (a member just left, a repair is under way, ICE retries): it is **dropped and
  counted, and the sender stays**. A receiver whose output queue is full (server) or
  receive queue is full (client) likewise loses the blob, never its connection.
* **Alias.** A public room (no password) whose host asked for one receives a
  four-character alias from the same namespace as invitation aliases, so a typed
  code names one thing. Alias attach reserves the lowest free seat and attaches in
  one step (no 30-second window to squat on); wrong aliases spend the prefix's
  failed-guess budget (five per minute, 60-second cooldown) and the global alias cap.
* **Leave, release, kick.** Before Start leave and release free the seat for a
  refill (release never bans). Kick additionally denies the identity and the
  directory principal for the room's lifetime (at most 64 of each) and works
  after Start, where it marks the seat left. Only slot 0 may release, kick or lock.
  Host races keep every connection: release or kick of a seat that just emptied is
  a plain acknowledgement; a repeated Start and a release after Start are
  acknowledged with a reason (Kind 47 carrying the u32 1775); a kick whose denylist
  entry does not fit the bounded list still removes the member and is acknowledged
  with 1777. Only a host naming itself or a seat outside the room is a violation.
  The denial is a courtesy, not an identity guarantee: it covers the identity string
  and the directory principal, both client-chosen, and an alias attach carries no
  principal.
* **Lock** is Start: pending reservations are dropped, nothing new may attach or
  reserve, and the directory hides the row (its heartbeat can no longer clear the
  flag). From then on a lost member connection **holds** its seat for
  `room_hold_timeout_ms` (default 5 minutes), then marks it left. A started room
  with nobody attached or held ends.
* **Reattach** (after Lock only) binds a new connection to a held seat when the
  member capability **and** identity match, issues fresh TURN credentials and
  revokes the replaced ones. A still-bound old connection (half open) is taken
  over and closed. The service capability opens the signalling path; the survivors
  still demand the pair proof before any data.
* **Host loss** before Start closes the room for everyone (reason EAWR-NET-1528)
  and hides the directory row. After Start the host is held like any seat and the
  room lives on; host-only operations are unavailable for the rest of the match.
  A staging room whose directory row expires (no heartbeat for 30 s) ends too,
  while a started match outlives its row.

### Typed reasons

| ID | Meaning |
| --- | --- |
| EAWR-NET-1772 | unknown or closed room, or not yours to open |
| EAWR-NET-1773 | capability, identity or membership does not match |
| EAWR-NET-1774 | seat unavailable: taken, reservation expired, duplicate identity, room already open |
| EAWR-NET-1775 | room started; no new attachments |
| EAWR-NET-1776 | released or kicked (also: a denied principal asks for a seat) |
| EAWR-NET-1777 | member or room budget exceeded |

Malformed room requests, forbidden signal destinations and an invalid room size
reuse EAWR-NET-1521; a full room reuses 1545; an unknown alias 1523; alias
exhaustion 1533; guess cooldowns 1532/1547; the prefix's outstanding reservations
1527. A room refuses with a typed error frame and closes only the offending
connection. Member and lease endings use room_closed with the reason.

### Reservations and abuse bounds

* At most two outstanding (unattached) reservations per room, a reservation
  lasts 30 seconds (`room_reservation_timeout_ms`), and each counts toward the
  reserving prefix's `unpaired_sessions_per_ip` (default 2) until it attaches,
  expires or its directory connection ends.
* A reservation that expires unattached spends one failed guess of its prefix and
  has a window of its own: with the default two outstanding reservations per prefix a
  squatter makes at most four expiries a minute, which alone would stay below the
  five-guess cooldown, so the **third unattached expiry in a rolling minute starts
  the same 60-second cooldown** (directory admissions from that prefix are refused
  with 1547). Raising `unpaired_sessions_per_ip` raises the squatting rate
  accordingly.
* One directory principal holds one seat per room.
* Room traffic has budgets of its own, separate from the legacy 120 operations /
  1 MiB session budget and from the per-prefix legacy budget (players share a NAT):
  `room_operations_per_member_per_minute` (480) and
  `room_bytes_per_member_per_minute` (2 MiB) per member, seat-count times that per
  room, and `connections_per_ip` times that per prefix. A renewal request costs one
  operation and each member may start a renewal at most every 30 seconds, so one seat
  cannot hammer a failing credential provider. A member over budget is closed
  with EAWR-NET-1777 and costs the others nothing. Churning members cannot launder
  operations through fresh seats because the room-wide budget is
  capacity times the member budget. The defaults are provisional until the
  introduced-path test measures blobs per pair.
* `connections_per_ip` now defaults to 18 (twice `max_match_seats`): nine members
  behind one NAT hold a directory and a room connection each. The per-prefix
  operation, unpaired and guess budgets still bound abuse from the prefix.
* Memory: about 10 KiB of room and member records per room; output queues stay
  per connection (256 KiB / 64 frames).

### TURN credentials per member

Each member gets its own credential at attach and at reattach, valid for the
rest of the room's hard maximum plus the 60-second allocation margin (clamped to
48 h), and uses it for all of its pair connections. coturn issues one HMAC
username per member. Cloudflare costs one generate call per member through the
same `TurnRefresh` provider seam that renewals use, so the cap
`cloudflare_turn_calls_per_minute` is shared; the attach reply waits for the call
(the member attaches without relay candidates after `cloudflare_turn_timeout_ms`
plus two seconds, and a credential that arrives late is revoked at once). A full
room costs up to nine generates at attach, nine revokes at its end and one of each
per kick, refused newcomer or reattach: about 20 calls over its life.

The service remembers **every** credential username it issued to a seat (attach,
each renewal, reattach; at most 64, the oldest revoked first) and revokes all of them
when the seat ends: leave, release, kick, a held seat that times out into left, a
reattach (the replaced connection's credentials) and room end. A revoke that finds the
shared Cloudflare call cap spent waits in a bounded queue and goes out as calls free
up. **coturn has no revocation**: a REST credential is an HMAC username with an
embedded expiry, so a kicked member keeps the right to allocate until it expires
(at most `max_session_lifetime_ms` plus the margin, clamped to 48 h). The residual is quota and bandwidth, not game access
(a kicked member gets no pair secrets), and coturn's `user-quota` and `max-bps`
bound it. Lower `max_session_lifetime_ms` for a shorter window. No relay
configured at all is a plain attach with empty credentials; the client's
`turn_credentials()` is then empty and members connect directly.

### Lease and renewal in rooms

The room carries ticket E's lease from the moment the host opens it:
`session_lifetime_ms` (2 h) renewed up to `max_session_lifetime_ms` (24 h). At the
renewal window every attached member receives Expiring (Kind 16); **any one**
member may Renew (Kind 13). The service asks the provider for one fresh credential
per attached member, then sends each its own Renewed (Kind 14); a second renewal
while one runs, or before the window, is refused harmlessly (1734). A failed or
late provider call refuses the renewal (1733) with the lease unchanged, and late
credentials are revoked. At the end of the lease, or at the maximum (1730), every
member receives room_closed with the typed reason. Live GNS connections cannot
switch credentials; the client keeps the refreshed credential for connections
dialed afterwards (`turn_credentials()`).

### Client

`NetworkGnsRoom` implements `GnsRoomEndpoint` over one worker thread and one TLS
connection per member:

* `NetworkGnsRoom::open` (host: the published room, its principal from
  `NetworkOnlineLobby::principal()`, the member's GNS identity),
* `attach` (guest with the directory admission: slot and member capability),
* `attach_alias` (guest with a four-character alias) and
* `reattach` (after Start, with the saved capability).

The factories block for the bounded admission exchange, including the member's
credential, so call them from an I/O worker; `send`, `poll`, `roster` and `lease`
only touch copied, bounded queues (64 frames, 256 KiB by default) and are safe on
GNS callbacks. Host-only operations refuse locally from any other slot. Hostnames in
a credential's TURN list become numeric addresses on the worker thread, as for
pairings.

### Seat and slot

The directory seat in an admission reply is the chat label of the *Password join*
protocol and is never reused; the **slot** is the transport peer index, reused as seats
empty and refill (guest A leaves, guest B gets directory seat 3 and slot 1). For a
scoped room the slot is the only identifier of a signalling member: `NetworkGnsRoom`
release and kick take slots, the directory chat kick (Kind 29) only mutes chat and
never removes a member from the room. A guest must keep its `NetworkOnlineLobby`
(the directory connection) until `NetworkGnsRoom::attach` returns, because the seat
reservation ends with the directory admission. `accepts_per_minute` stays at 30 per
prefix: nine members forming a room use 18 accepts, so a reconnect burst of a whole NAT
can exceed it; raise it together with `connections_per_ip` on busy home-NAT communities.
