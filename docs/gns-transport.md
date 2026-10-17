# GameNetworkingSockets integration

ADR-022 option A was accepted by the owner on 2026-10-13. The project will run
the introduction and relay service; peers use the BSD-3-Clause standalone
[GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets)
library, without a Steam account or SDK. Service deployment and the viewer join
code flow belong to slice 2. Slice 1 supplies the optional two-peer adapter and
introduction interface; it does not promise relay connectivity.

## Build

`EAWR_ENABLE_GNS` defaults to **OFF** in both the engine and separately configured
viewer. Disabled builds perform no package discovery, download, or C language
enablement, and publish `EAWR_HAS_GNS=0`. The default stays OFF until all our
presets have enabled coverage. Build-system changes require full CI before merge.

The explicit **FETCH** provider uses FetchContent to obtain GNS v1.6.1 at
`d534c19aa760df3fb75fd20db13ba1932b8a5463`, with archive SHA-256
`099d4b0ff1837f0f27c83d4ce05869ec1d32136866f69a3336cc137e951189ab`, and protobuf 3.21.12 at
`f502b8e9c831bda0bea57d9cbeefca3eb76e4254` (protobuf tag `v21.12`, C++ version
3.21.12). Isolated CMake external projects
build static libraries and a matching `protoc`, with two compiler jobs each.
Build/install directories include both source revisions, so changing a pin in a
reused build tree cannot retain an older installed library. The GNS build step
also runs its incremental native build each time, honoring source-cache edits.
Third-party warning flags and output paths do not enter EAWR targets. Protobuf
3.21 avoids an additional Abseil dependency. Unused protobuf zlib support is
disabled, so the static runtime cannot acquire an optional gzip link dependency.
The GNS native ICE implementation
is enabled; WebRTC and its submodules, examples, tests and certificate tools are
excluded. The standalone dependency smoke harness is a cheap enabled CI path:

```sh
cmake -S tests/net/gns_dependency -B out/gns-smoke -G Ninja -DEAWR_ENABLE_GNS=ON
cmake --build out/gns-smoke --parallel 2
ctest --test-dir out/gns-smoke --output-on-failure
```

Run it in an MSVC or clang-cl developer environment on Windows, or select
`CMAKE_CXX_COMPILER` and `CMAKE_C_COMPILER` for GCC/Clang on Linux x64. The full
root/viewer configure accepts the same enable/provider options. Linux requires
OpenSSL development headers/libraries (`libssl-dev` on Debian/Ubuntu). FETCH needs Git and network access at
configure; FetchContent source overrides support an existing source cache.
The pinned GNS listen API requires a nonzero port. The smoke harness probes an
ephemeral OS UDP port, releases the probe and retries GNS bind at most eight
times if another process wins the port. No fixed testing port is reserved.
This path is intended for native builds: cross-compiled `protoc` is not supported.
ARM64 is a future qualification target, with no enabled-build claim here.

On Windows FETCH selects **BCrypt** for AES/SHA and GNS's bundled reference
curve/signature implementation, avoiding a separate OpenSSL installation. It
matches the parent's static/dynamic CRT choice, including the viewer's static
CRT. On Linux it selects **OpenSSL 1.1.1 or later**, including curve operations.
**libsodium** is an upstream alternative, but its AES hardware restrictions make
it unsuitable as the default portable backend. These choices follow the pinned
[build source](https://github.com/ValveSoftware/GameNetworkingSockets/tree/v1.6.1)
and [upstream build guide](https://github.com/ValveSoftware/GameNetworkingSockets/blob/v1.6.1/BUILDING.md).

The initial v1.4.1 pin passed direct-IP tests but its native ICE address
enumeration was Windows-only, preventing introduced connections on Linux.
Version 1.6.1 provides POSIX `getifaddrs` enumeration and a revised native ICE
client. The public custom-signalling API used here remains compatible; the
adapter requires no application wire changes. GNS 1.6 uses CMake's
`CMAKE_MSVC_RUNTIME_LIBRARY` in place of `MSVC_CRT_STATIC`; the external build
explicitly receives the parent's dynamic/static runtime selection. The existing protobuf 3.21.12
bootstrap remains isolated and supplies matching runtime and compiler versions.

The **SYSTEM** provider accepts `find_package(GameNetworkingSockets CONFIG)` and
links `GameNetworkingSockets::static`. This supports vcpkg or a separately built
installation through `CMAKE_TOOLCHAIN_FILE`/`CMAKE_PREFIX_PATH`. The package
must provide compatible crypto, protobuf runtime and `protoc`; the upstream
vcpkg default disables P2P, so it needs an ICE-enabled overlay for the later
custom-signalling adapter. Package versions and CRT selection are the package
manager's responsibility. It never silently falls back to another provider.

## Transport and introduction shape

### Dependency licences

The fetched source and any enabled binary distribution must retain upstream
copyright/licence notices. The pinned tree contains these components:

| Component | Licence / notice source | Selected build use |
| --- | --- | --- |
| GNS, including Valve modifications to bundled wrappers | BSD-3-Clause; GNS `LICENSE` and individual file headers | All enabled builds |
| Protobuf 3.21.12 | BSD-3-Clause; protobuf `LICENSE` at tag `v21.12` | Static runtime and matching protoc |
| curve25519-donna | Public domain or MIT for original code; `src/external/curve25519-donna/README.md` and file headers; Valve-modified files retain separate notices | Windows BCrypt curve backend |
| ed25519-donna original code | Public domain by Andrew M.; `src/external/ed25519-donna/ed25519.c` and `ed25519-donna.h` headers | Windows BCrypt signature backend |
| ed25519 Valve wrappers | Valve copyright in `ed25519_VALVE.c` and `ed25519_VALVE_sse2.c`; distributed under GNS BSD-3-Clause `LICENSE` | Windows BCrypt signature backend |
| sha1-wpa | BSD-3-Clause; `src/external/sha1-wpa/README` and source headers | Fetched source; compiled only with the alternative libsodium backend |
| OpenSSL | Installed version's licence: 3.x Apache-2.0, 1.1.1 OpenSSL/SSLeay notices | Linux package dependency |

Both `src/external/curve25519-donna/readme_VALVE.txt` and
`src/external/ed25519-donna/readme_VALVE.txt` distinguish Valve-modified files
from the original public-domain code; individual file headers retain their
notices. The entire fetched tree is not described as public domain.
The WebRTC, Abseil and vjson gitlink submodules are neither fetched nor built by this
provider. SYSTEM packages retain their package manager's dependency notices.


`GnsTransport` implements the same two-peer contract as `TcpTransport`, with
host peer 0 and client peer 1. `host`/`connect` accept numeric IPv4/IPv6 addresses
and a port; host port 0 probes an OS UDP ephemeral port, then retries a raced
GNS bind at most eight times. GNS owns UDP retransmission
and encryption. All adapters in a process share one standalone GNS runtime and
one I/O owner thread. Destroy transports before process/static teardown. The
runtime lasts until process exit; closed adapters
release their own connection/listener handles after their drain.

The adapter uses existing transport control kinds (`TcpMessage` hello, accept,
keepalive, close and data), without adding application kinds. Both sides check
protocol version, exact caller-supplied build identity, frame limit and role
before opening the stream. GNS has its own hello revision, `gns_protocol_version` (2, the
LAN/TCP protocol stays 1), bumped when the sender slot and roster size joined the hello; the
decoder reads magic, version and build before the revision-specific tail. The N2 decoder
therefore reports 1502 for an old-layout hello and 1503 for a different build. A build from
before revision 2 is unchanged: it checks the hello length first and answers an N2 hello with
its own 1518. The existing `SetupSession` still compares content,
setup and tick-zero state. A GNS data message contains little-endian u32 total
frame bytes and u32 fragment offset, followed by up to 64 KiB of application
data. Fragments stay contiguous on one reliable lane; the receiver validates
size/offset and emits only a complete frame. This preserves empty frames,
arbitrary first bytes and the default 1 MiB application limit despite GNS's
512 KiB send-message limit. The configurable maximum remains 16 MiB.

`poll` services callbacks, signalling and at most 64 data send/receive calls,
plus at most 32 signalling receives. The adapter's frame/byte quotas and GNS's
separate send-buffer quota bound each queue. Send backpressure is retryable;
receive overflow closes with a typed reason. Application keepalive/timeout
requires both peers to pump even though GNS maintains UDP in background threads.
`close` queues an ordered close after accepted messages, exchanges close
acknowledgements and drains reliable GNS sends; pump both sides until `closed`.
Before native teardown, a one-byte, out-of-band keepalive packet flushes GNS's
receiver ACKs, and its pending unreliable bytes must drain too. This prevents
the final reliable close from waiting on a receiver ACK after its ICE route is
destroyed when P2P's native close signalling is unavailable. The probe uses an
existing kind, carries no game frame and cannot alter fragment assembly; only
this exact one-byte control is permitted unreliably after admission. All game
frames and other control envelopes remain reliable.
Loss of the final best-effort probe can still produce a bounded close timeout
on a lossy route.
The close timeout bounds a stalled drain: accepting queued fragments or a new
low-water mark in pending reliable bytes extends it. Retransmit fluctuations
or unrelated keepalives do not extend it. This lets a large ordered tail finish
under GNS's bandwidth cap without granting an unresponsive peer unlimited time.
Destruction/cancel abort immediately. A failed pre-open host attempt leaves
the listener available for a valid retry. The direct and paired factories accept only `peers == 2`;
other counts return `EAWR-NET-1501` rather than silently creating a two-peer link. A roster of
2..`max_match_seats` peers is created only through the room factories ([N-peer room mesh](#n-peer-room-mesh-slice-n2)).

`poll` samples the connected remote peer's native ping estimate at a one-second
monotonic-clock cadence. `link_stats` and `connection_rtt` only return cached
observations; reading them cannot sample or send application ping/pong traffic.
Stats remain unknown until the first valid nonnegative estimate, and are absent
for self, invalid or closed peers. Stop, retry and close clear the observations.
The SDK estimate is quantized to integer milliseconds, converted to microseconds
for the common `LinkStats` interface. Every valid cadence observation counts,
including repeated values and zero; `sample_count` counts local observations of
the SDK estimate, not native probes. A polling gap yields one new observation,
without inventing samples for the missed intervals. The first sample initializes
smoothed RTT; subsequent samples use the common 1/8 RTT and 1/4 absolute consecutive
RTT-delta jitter updates. GNS's packet-jitter high-water field is not this jitter.
Lockstep admission and desync state remain in their existing modules.

The introduction interface maps an expiring join code to a rendezvous containing
two opaque peer identities and a session identity. It forwards bounded
opaque GNS signalling blobs between those identities. GNS owns candidate
encoding and ICE negotiation; the service must not reinterpret candidates as
new game message kinds. A thread-safe in-process implementation qualifies
this boundary without deploying a service. Outbound signalling callbacks may
run on GNS threads, so queues must own their payloads and must not hold locks
while entering GNS. The receiving game thread drains signals into
`ReceivedP2PCustomSignal`; initiation uses `ConnectP2PCustomSignaling`.

`host_introduced`/`connect_introduced` require a paired endpoint and use generic
GNS identities (at most 31 bytes, matching the pinned API's 32-byte capacity
including its terminator). Changing the process identity with another active adapter is
rejected because GNS would close its connections. Signal objects retain their
endpoint until GNS releases them, without retaining or calling a dead adapter.
The in-process service uses predictable test codes, an injected monotonic clock,
64 session slots and explicit signal byte/count budgets. Publish/join serialize
their session map and code counter; endpoint operations separately serialize
their queues. Join-code expiry gates admission only: a paired endpoint remains
usable after that deadline. Once the application handshake is accepted,
signalling-service failure cannot abort a healthy data path or its ordered close;
GNS can still time out if the underlying data route fails. Failure before
acceptance remains fatal. A pre-ACK departure resets admission on the retained
host listener; a later attempt must supply its own verified hello. Polling
resumes after retry so a retained signalling outage fails closed before that
new admission. Retained atomic signalling state handles outbound
callbacks without accessing a destroyed adapter. Additional IP joiners receive
GNS native close reason 1508 because they have no established adapter data path.
On an established native route, a host-side pre-admission failure instead sends
one reliable, non-Nagle `TcpMessage::refuse` envelope before teardown. A pending
native connection may finish readiness during that same linger; it is never
admitted. The envelope reuses TCP's
length-prefixed diagnostic code (13 bytes) and message (at most 1024 bytes),
restricted to EAWR-NET-1500..1529. It replaces queued acceptance, ignores later
application messages and closes after a fixed 250 ms linger driven by polling;
neither send retries nor ACK progress extends that bound. Incoming signalling
continues during the refusal drain so a pending route can receive its remaining
negotiation. Further endpoint failures keep the original diagnostic and deadline.
The native deadline close also preserves the original typed refusal reason.
Owners keep polling through the linger; explicit cancellation tears it down immediately.
Delivery is best effort if the ICE route itself
fails or the joiner is not pumping. Before a native route exists, only the native
close reason is available. A joiner accepts a reliable host refusal before its
admission (including before hello), surfaces that diagnostic and closes terminally; a malformed,
unreliable, post-admission or client-originated refusal fails with 1518.

The fixture is not an authenticated service. A two-process test exercises real
GNS custom signalling and native ICE, carrying only opaque signals through a
loopback test broker whose service is `InProcessGnsIntroduction`. It injects
service failure after acceptance, checks whole messages and drains an ordered
1 MiB tail. A second run uses a native SDK client to withhold its application
ACK until the host endpoint has returned an injected expiry failure after the
host queued acceptance. A third run disconnects before ACK, reconnects the same
identity without sending its new hello, injects endpoint failure and verifies
the host closes locally with 1517 without admitting or delivering game frames.
The raw peer requires the second native route, the injected expiry observation,
the in-band host diagnostic 1517 with its message, and broker telemetry published
only after those host assertions, then closes its SDK; this retry leg runs ten
times. IPC sockets use exclusive bindings; the broker verifies duplicate binding
is rejected and every endpoint has a distinct port. LAN discovery retains its
port-sharing behavior. The failed signalling route cannot promise the native reason to the joiner;
the established ICE route carries the structured refusal (legacy EAWR-3388).
An additional healthy-signalling refusal leg verifies input is still consumed
within the linger and the native close carries 1517 alongside the in-band reason.
This deterministically tests the admission boundaries;
the fake service's admission-only deadline is not claimed as a paired-endpoint
outage. Public NAT/relay qualification
remains server-slice work.

## Diagnostics and tests

EAWR-NET-1500..1529 are the two-peer adapter's (the room mesh adds 1766..1771, listed with it below): 1500 disabled, 1501 invalid options/address,
1502 protocol mismatch, 1503 build mismatch, 1504 frame limit, 1505 queue full,
1506 backend/disconnect, 1507 application/handshake/close timeout, 1508 occupied
peer slot, 1509 missing/invalid/conflicting rendezvous identity, 1510 expiry,
1511 unpaired introduction, 1512 signal size, 1513 service capacity, 1514 service
options/identity, 1515 unknown code, 1516 occupied code/duplicate identity,
1517 failed/unauthorized signalling and 1518 malformed transport envelope.
GNS's native application close-reason range carries the same 1500-series code;
its raw address-bearing backend debug text is not shown as a player diagnostic.

`net_gns_transport_contracts` covers framing, mismatch/retry, capacity, timeout,
RTT and ordered close. `net_gns_signalling_contracts` covers rendezvous,
identity/admission expiry, copied signals, bounded queues, concurrent admission
and concurrent GNS-thread sends. The GNS-independent frame decoder is always
compiled and `net_gns_frame_codec_contracts` covers hostile receive envelopes
in default builds: malformed/duplicate handshake, unknown or unreliable
envelopes, early/late data, oversize frames, bad fragments, quota overflow and
invalid controls. `net_gns_introduced_paths` exercises the two-process path.
The existing setup, scripted lockstep/departure and injected-desync integration
fixtures are also compiled against GNS. The setup/battle compares final state
hashes on 1/2/4/8 workers. Dependency/adapter integration tests return CTest skip
code 77 when GNS is disabled; the service protocol contracts run in both modes.

The open-source library does not grant access to Valve's Steam Datagram Relay.
The optional [project introduction server](net-relay-server.md) uses verified TLS,
runtime publisher-only access tokens, random single-use join codes and bounded forwarding.
`NetworkGnsIntroduction` implements the existing introduction interface over
the native socket layer. Introduced endpoints expose short-lived coturn REST
credentials; the adapter applies the TURN server/user/password lists when
creating native ICE sockets. `GnsOptions::turn_only` requires a relay candidate
for qualification and rejects endpoints without credentials. See the
[deployment guide](deploy/relay-server.md) for the standalone Windows/Linux
server, certificate pinning and coturn configuration. No service address or
credential is embedded in the build. Public internet and symmetric-NAT relay
qualification remain unverified; introduction alone is not that evidence.

## Long sessions (ticket E)

Paired peers keep their TLS connection to the introduction service open and renew its
lease automatically ([protocol](net-relay-server.md)). `GnsSignallingEndpoint::lease()`
reports the current end and the hard maximum; `take_refreshed_turn_credentials()` hands
over the credentials of the latest renewal once.

**Live-connection assumption.** This adapter assumes the GNS SDK cannot replace TURN
credentials for an existing ICE connection. The SDK is supplied externally and is not
version-pinned here; this is an adapter constraint to recheck against the chosen SDK,
not a verified claim covering every GNS version. The adapter configures TURN server,
user and password lists when creating ICE sockets. Refreshed credentials are delivered
to the application for new connections and are **not** applied to the live connection.

- At pairing, both providers issue credentials for `max_session_lifetime_ms` plus a
  60 s margin (24 h 1 min by default), clamped to **48 h**. Renewals keep the original
  pairing credentials alive and extend the service lease, without extending the live
  allocation's credential boundary. A maximum above 48 h can outlast relayed play.
- The view warns for matches holding relay credentials at 15 minutes and escalates
  at 2 minutes before pairing plus the advertised credential lifetime (EAWR-NET-1736),
  independently of successful service renewals. It keeps an expired-credential notice
  after that boundary. Direct connections can continue.
- At that boundary, this build assumes a relayed path can no longer refresh its TURN
  allocation. Packet loss then follows GNS's normal timeout/peer-left handling; the
  online view labels a peer loss after the credential boundary EAWR-NET-1737.
  This time-based classification does not prove which ICE path was active or which
  cause ended it. Public coturn/Cloudflare allocation-expiry behaviour remains unverified.
- To cover a longer relayed match, the operator raises `max_session_lifetime_ms`
  before pairing (within the 48 h credential clamp), accepting longer-lived credentials.
  Applying new credentials to a live allocation remains a follow-up SDK capability check.

Setting up a new connection (for example after an ICE restart or a future reconnect)
should read the refreshed credentials; nothing in this build restarts ICE by itself.

## TURN permission refresh (legacy EAWR-3912)

**Symptom.** Every relayed two-peer battle on the real route stopped about 298 s after the pair attached
(three runs on 2026-10-16: 04:19:24Z to 04:24:21Z, 04:35:40Z to 04:40:38Z, 13:17:10Z to 13:22:08Z), at ticks
4366, 4470 and 6330, so the cause is elapsed time and not game state. Both peers then failed 30 s later with
EAWR-NET-0106 while GNS still reported Connected with a 31 ms ping, the application queues empty, and
`unacked_reliable` (bytes in flight, `m_cbSentUnackedReliable`, not a message count) growing on both sides. LAN and
direct routes pass because they have no TURN permission to lose.

**Cause.** The pinned native ICE client (`steamnetworkingsockets_ice_client.cpp`, `Think_TURNMaintenance`) refreshes
the allocation at half its lifetime but sends CreatePermission only when the set of permitted peer addresses changes
(`m_nTURNPermissionRevision`, bumped in `AddPeerCandidate` for a new public or LAN address). RFC 5766, section 8,
gives a permission a fixed five minutes, so about 300 s after the first remote candidate the TURN server (Cloudflare
Realtime TURN on the public server, coturn in a private one) stops forwarding in both directions. GNS's own connection timeout (`TimeoutConnected`, 180 s in the viewer) is longer than the 30 s
lockstep wait, so the match ends as NET-0106 and not as a link loss. Upstream master (`d534c19a`, the pin) has no fix.

**Carried patch.** `cmake/EawrGnsTurnPermissionRefresh.cmake` is applied right after the FetchContent populate and
repeats CreatePermission for the whole permitted list every `min(120 s, allocation lifetime / 2)`. Each anchor must match
exactly the expected number of times, so a changed pin fails the configure instead of silently running unpatched; the
patch id is part of the build prefix. Drop the file when a pin bump contains an upstream fix. The SYSTEM provider cannot
be patched and warns at configure time. `net_gns_turn_permission` (tests/net/gns_turn_processes.py) runs two real GNS
processes through a small TURN server whose permissions and allocations expire after 6 s, for 20 s, and requires the
frames to keep flowing; it failed on the unpatched pin. The patch also repeats a Refresh or CreatePermission that
is answered with an error reply carrying a fresh nonce (438 Stale Nonce, or 401): the pinned client counted any reply
with a header as success, so one stale nonce ended the refresh chain and the allocation lapsed after its lifetime
(600 s on coturn). The new nonce (and a changed realm, with its long-term key) is adopted and the request is re-sent at
once, at most five times in a row; any other error reply on a Refresh re-arms it after 30 s. `net_gns_turn_nonce`
rotates the server nonce every 2 s so every request is answered with 438 first, and requires the frames to keep flowing.

**Loud failure.** `GnsOptions::ack_silence_timeout` (15 s): a Connected link whose reliable data stays unacknowledged
without a single acknowledgement for that long stops with EAWR-NET-1507, "GNS link silent N s while Connected (relay)",
sampled every 250 ms. GNS acknowledges on its own thread, so a peer that is busy loading does not trip it at lockstep
volumes (it stops acknowledging once the receiver buffers more than 1 MiB of reliable data). The sampled in-flight byte
count must fall or reach zero within the window; a level that stayed exactly constant for 15 s would count as silent.
This names the link instead of the lockstep wait, well before the 30 s completeness timeout. `net_gns_turn_silent` covers it with a
server that stops forwarding for good.

**Diagnostics in the log (legacy EAWR-4021).** The first silence of a relayed pair used to be unexplained, because GNS spew never reached godot.log. `GnsTransport::set_log_sink` (the viewer sets it with the LAN diagnostic logger) receives, as `EAWR net event=gns utc=HH:MM:SS.mmm seat=N [peer=M] ...` lines: every GNS warning or worse; the ICE/TURN messages, which the second carried patch (`eawr_gns_patch_turn_diagnostics`, one line per Refresh or CreatePermission reply, accepted or with its error code, and one for a CreatePermission that times out; plus the existing nonce repeat and refresh-timeout lines); and the reason every link stops, 1507 included, with this seat and the peer. A TURN line has the seat but no peer (the ICE session does not know the pair). Addresses become `<addr>` and control characters `?`; an identical GNS warning (one per failed send during an outage) passes once per ten seconds with the count held back. Every line has a `utc=` stamp. The sink belongs to the match that installed it and is cleared when that match is cancelled or destroyed. Healthy matches log a few lines per TURN refresh and nothing per packet; at most 256 lines wait for the next poll. Lines are queued from any thread and delivered by `poll` on the I/O owner, so the sink needs no locking. `net_gns_turn_permission`, `_nonce` and `_silent` assert it.

**Not covered.** A 438 on the first Allocate is not retried (the nonce was issued a moment earlier). A relayed match
longer than the allocation lifetime (600 s on coturn) is still the next thing to soak on the real route.

## N-peer room mesh (slice N2)

Design: [online play for up to nine peers](net-online-npeer.md). `GnsTransport::host_room` and `join_room` take a
`GnsRoomEndpoint` (`gns_room.hpp`: the introduction service's room, or `InProcessGnsRoom` in tests) and the agreed roster
size in `GnsOptions::peers`. The endpoint's slot is the transport's `PeerIndex` (the host is slot 0), `peer_count()` is
the roster size, and `link(p)` reports each pair. The match semantics are the LAN TCP mesh's: one reliable ordered stream
per pair, no forwarding, cross-pair arrival order never defines command order. The service only introduces: every
pair has its own GNS connection and its own ICE session, and `GnsRoomEndpoint::send` carries the bytes of one GNS signal
to one slot.

**Pair links.** A pair link is the two-peer connection machinery (hello, accept, fragments, ordered close, refusal drain)
bound to a roster position at each end. The adapter keeps up to 8 of them behind one listener and one GNS identity (the
roster identity of the local slot; one identity per process, as before). A connection is accepted only from a slot whose
roster identity equals the GNS identity in the connect request and whose room signal arrived from that same slot, and (for
a pair that does not include the host) only once the host has issued the pair's grant. The newcomer dials; a member never
dials a seat that dials it. Per `poll`, work is bounded in total, not per edge: at most 64 receive calls, 64 send calls
and 32 room signals, shared by all pair links and rotated by a moving start, so a busy edge cannot starve another.

**Wire changes to the pair handshake** (EAWR-NET-1766 on mismatch, before any application frame): `hello` is
`magic, protocol, build identity, frame limit, sender slot (u32), roster size (u32), role`; the receiver checks magic,
protocol (1502) and build (1503) first, then the layout and role (1518), the frame limit (1504), the
sender slot against the slot it expects at the other end and the roster size against its own (1766). `accept` carries the
dialer's slot instead of the two-peer constant 1 (the same value in both directions); the two-peer adapter uses slot 0
for the listener and 1 for the dialer. A listener may report 1766..1771 in the pre-admission `refuse` envelope.

**Pair secret.** For a pair that does not include the host, both ends hold a 16-byte secret the host generated for exactly
that pair. After `accept`, the dialer sends `mesh_pair` (`TcpMessage` 12: secret, own slot, remote slot) and the listener
answers with its own; the pair opens (`link(p)` becomes `open`, application frames are allowed) when both have matched.
Application data before that, or a wrong secret or seat, ends the pair with 1767. The host pair has no secret: the room's
roster identity and the checked hello bind it. A connection accepted before its grant has arrived is parked (at most
`gns_pending_pair_limit` = 8): the listener neither sends its hello nor reads until the grant arrives or the pair deadline
passes (1771).

**Host-stream controls** (`TcpMessage` 15..18, decoded by `GnsFrameDecoder` with fixed sizes, directions and seat ranges;
the TCP transport refuses them as unknown kinds). Each rides one reliable pair stream between the host and a member:

| Kind | Direction | Body | Meaning |
| --- | --- | --- | --- |
| 15 `mesh_grant` | host to member | remote slot, remote occupancy epoch, dial (u8), secret | introduce this pair: dial or accept, with this secret. A grant for a pair that already has a link replaces it (the regrant) |
| 16 `mesh_vacated` | host to member | slot, epoch, reason (0 left, 1 could not connect to `other`, 2 removed), other, code (u32), relay offered (u8) | close the pair to this slot. Naming the receiver itself ends its join (reason 1 carries the unreachable seat, the failing pair's own reason code and whether a relay was offered; both are zero for the other reasons). One older than the receiver's grant for that slot (epoch) is ignored, so a quick refill is not undone |
| 17 `mesh_report` | member to host | remote slot, status (0 open, 1 failed, 2 lost), relay offered (u8), code | a pair failed or was lost (the code must be 1500..1529, 1762 or 1766..1771, else the control is malformed); the host feeds it to `MeshFormation` (N1) and keeps the code for a refusal |
| 18 `mesh_ready_mask` | member to host | bit mask of its open pairs | what the member holds open; the host counts a pair open when both ends claim it |

**Formation at the host.** The host runs N1's `MeshFormation`: a newly attached roster slot is admitted at the roster
revision it attached at; when its host pair opens, the host introduces it to every seat whose host pair is open (the later
admission dials; the grant also tells the other end to accept). A failed or lost report from an end counts as relay
evidence for that end (`MeshRelayFailure`), because a closed GNS link has already tried every candidate it was offered;
when one end reports and the other stays silent for 5 s (or half the pair deadline if shorter), the silent end is counted
too. That rule is the policy's (`mesh_report_grace`, `mesh_silent_end_failed`, covered by `net_mesh_policy_contracts`);
the adapter only supplies the wait. Both ends failing refuses the later admission and releases its slot (`GnsRoomEndpoint::release`, no ban); the refusal
carries the failing pair's reason as reported (the newcomer's report, else the other end's; 1762 when neither reported)
and its relay flag; a pair
between two admitted seats gets one regrant first. A newcomer that never reaches the host, or whose host pair ends before
Start, has its slot released the same way. `mesh_complete()` is the formation's completeness (every slot of the roster
present and every pair claimed by both ends); `lock_room()` refuses Start with `EAWR-NET-1764` while it is false and
otherwise locks the room. After Lock the formation is not consulted again: pair loss in battle is the session's (N4)
and repair is N5. Before Lock, losing the host pair ends a member's join (EAWR-NET-1760 or the pair code).

**Observation.** `take_event()` yields `GnsMeshEvent`s (bounded to 64): `pair_open`, `pair_failed`, `pair_lost`,
`seat_refused` (host: newcomer and the seat it could not reach), `seat_vacated`, `left_room` (this transport can no longer
take part, with the code; for a refused newcomer the failing pair's reason (1762 when the pair never connected) and the
unreachable seat, or the host-pair code such as 1503). `seat_refused` and `left_room` carry the reported relay flag.
`pair(p)` is the pair's phase and cause (`signalling`, `handshaking`, `open`, `failed`, `lost`) with whether a relay
candidate was offered and, while open, that pair's own route (`GnsRoute`), never an address. `GnsTransport::observed_route()`
follows the two-peer adapters only; pair links do not write the process-wide cell. `signal_counts(p)` accumulates over every
link the slot's current occupant has had, so a regrant adds to the totals; a different occupant of the slot starts again.
One pair whose control queue overflows ends alone (1770, the normal pair-failure path); the room goes on. `connected_seats()` and `mesh_complete()` summarise it.

**Without TURN.** `GnsPairDiagnostic::relay_failed` is true for a failed or lost pair whether or not a relay was offered:
with none offered the direct failure is the only evidence and is reported at once, so N4 can set
`MeshLinkReport::relay_failed` from it and an asymmetric loss does not wait for a relay that does not exist (the follow-up
"relay evidence without TURN" (legacy EAWR-3697)). The adapter emits codes and the relay flag only; N4 chooses the player-facing
wording from `relay_offered` (`mesh_newcomer_refusal_text`, N1). The incident ID, the shared incident time base and the flapping-pair policy of that follow-up (legacy EAWR-3697) belong
to the session (N4). The adapter never redials a closed pair link by itself, so a pair that dropped stays failed or lost
until a new grant, which is the sticky loss the review asked for.

**Diagnostics.** EAWR-NET-1766 hello/accept roster size or seat mismatch; 1767 pair secret or identity mismatch, data before
the pair identity, or a connect request that does not match its roster identity; 1768 connected natively but the pair
handshake did not finish in time; 1769 an open pair closed or timed out; 1770 malformed, unexpected or over-quota mesh
control; 1771 no grant before the pair deadline. 1760..1765 are the room and policy IDs of N1 (1762 is "pair unreachable
directly and by relay", 1764 "mesh incomplete at Start", 1765 "room size or seat differs from the agreed roster").

**Buffers.** `SendBufferSize` is `min(queue_bytes, 2 MiB)` per pair connection (the two-peer adapter keeps `queue_bytes`),
16 MiB for 8 pairs. `queue_bytes` and `queue_frames` still bound each pair's inbound and outbound queue, so a nine-peer
receiver holds at most 8 x `queue_bytes` of inbound data (64 MiB at the defaults) and, with every pair's outbound queue
full, another 8 x `queue_bytes`; together with the 16 MiB of GNS send buffers that is 144 MiB worst case at the defaults.

**Pair deadline.** `GnsOptions::pair_timeout` (20 s) bounds one pair from its first signal (or its grant, for the
listening end) to a verified pair, ICE and any TURN allocation included. It replaces `handshake_timeout` for pair links, and
the clock is not restarted by the hello: it started when the link was created (the first signal or the grant). A pair that
never connected natively ends with 1762 whichever of GNS's own initial timeout and the adapter's check fires first; one
that connected but did not finish the pair handshake ends with 1768.

**STUN (source-read, not measured).** The pinned GNS (v1.6.1, `d534c19`) reads only the relayed address from a TURN allocation, so without a
`P2P_STUN_ServerList` it makes no server-reflexive candidate: two peers behind ordinary NATs reach each other only through
the relay, or after a peer-reflexive address is learned from checks that crossed it. `GnsOptions::stun_servers` (numeric
`stun:host:port` URIs) sets that list. coturn answers STUN on its TURN port, so the service can hand out the same host with
the credentials (slice N3); the adapter applies whatever the caller passes and invents nothing.

**TURN allocations (source-read, not measured).** Every connection has its own ICE session. When the
`Relay` candidate type is enabled and TURN servers are configured, the session sends a TURN Allocate on **every local
interface it binds** (non-loopback addresses; `Think_DiscoverRelayCandidate` skips only interfaces already done), whether
or not a direct pair would have worked, and refreshes it at half the lifetime. A member with `I` usable interfaces
therefore holds `8 x I` allocations in a nine-peer room and the room `72 x I` even when every pair ends up direct; a home
machine with one wired and one wireless address counts twice. The deployment guide's `user-quota`, `total-quota` and relay
port range must be sized with `I`, not 1 (slice N3 owns the guide). Counting the allocations a real host makes needs a
coturn instance and a capture; the loopback tests cannot, because they configure no TURN server.

### Tests

**Committed room handoff.** The host queues service Lock before publishing the validated lobby commitment on GNS.
These are independent streams: a guest can receive commitment before its TLS owner receives the pushed Lock roster.
The battle owner calls `commit_room()` after taking the validated lobby result and before advancing the first tick.
This latches the native mesh into battle mode, so an older open roster cannot reopen formation or defer repair arming.
This operation applies the already agreed commitment locally; the host still locks the service room. Reattachment
waits asynchronously for a live TLS owner's Lock roster; an outage may leave that cache stale, so reattachment then
lets the service check room state, identity and seat capability. `net_gns_transport_contracts` holds the guest endpoint's roster open across commitment
and verifies repair can arm and later open rosters cannot clear the lock. The nine-seat viewer contract checks native
Lock at the first battle tick and interrupts the final directory guest's host pair at tick three.

`net_gns_frame_codec_contracts` (always built): the new hello and accept layouts and mismatches, the hello revision in both
directions (an old-layout hello and a new-layout hello read as an older revision give 1502, never 1518, at the N2
decoder; a different build gives 1503), the extended refusal range, and every mesh control's size, direction, seat, phase and reason-code checks. `net_gns_transport_contracts`: the room factories'
contracts against an in-process room (roster size, seat, options, incomplete Start, closed room). `net_gns_mesh_paths`
(skips with 77 without GNS): each case starts a broker process that owns an `InProcessGnsRoom` and relays only the room's
opaque signals over loopback UDP, and one process per member with its own GNS identity and a real ICE data path.

| Case | Checks |
| --- | --- |
| 3 and 4 members | incremental formation, complete mesh and Lock, every pair open, whole ordered frames both ways on every pair, a 1 MiB ordered frame around the ring, the host leaves after Lock and the survivors' pairs keep delivering |
| newcomer cannot reach one member | the broker drops that pair's signals: the newcomer is refused with 1762 naming the unreachable seat, the host sees `seat_refused`, the others `seat_vacated`, the slot is released, a second newcomer fills it and the room completes |
| newcomer with another build | the host pair refuses with 1503, the slot is released and refilled, the room completes |
| nine members (`net_gns_mesh_nine`, once per slice at its final head) | the same checks at the roster size of the release gate, 36 pairs |

A mismatching build is always found on the host pair, which comes first, so through the adapter a mismatch on a pair that
does not include the host is unreachable; the pair hello still compares the build identity on every pair, and
`net_gns_frame_codec_contracts` tests it with a decoder bound to a non-host pair (a deviation from the design row's
"build mismatch on a non-host pair" process case, which cannot be staged without breaking the host-pair check). The
in-battle rows of the design (a seam that closes one pair, repair, the agreed departure) need the session and repair
slices (N4, N5) and are not covered here.
