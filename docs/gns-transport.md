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
before opening the stream. The existing `SetupSession` still compares content,
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
the listener available for a valid retry. The adapter accepts only `peers == 2`;
other counts return `EAWR-NET-1501` rather than silently creating a two-peer link.

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

Only EAWR-NET-1500..1529 are used: 1500 disabled, 1501 invalid options/address,
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
