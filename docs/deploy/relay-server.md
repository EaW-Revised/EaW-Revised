# Self-hosting the introduction and relay service

The introduction binary is a small native TLS server. Coturn is an optional
companion for ICE relay traffic; the introduction service alone cannot make a
connection work through every NAT. These instructions provision nothing and
change no firewall or router settings automatically. The owner and third-party
operators follow the same steps with their own runtime configuration.

## Build

The standalone build reads no game data and has no GNS, simulation or Godot
dependency. Use CMake 3.25+, a C++20 compiler and Ninja or Visual Studio 2022.
On Debian/Ubuntu install `cmake ninja-build g++ libssl-dev`. Then:

```sh
cmake -S apps/relay_server -B out/relay -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/relay --parallel 2
```

On Windows use a Visual Studio 2022 developer shell:

```powershell
cmake -S apps/relay_server -B out/relay -G "Visual Studio 17 2022" -A x64
cmake --build out/relay --config Release --parallel 2
```

Windows fetches and statically builds **Mbed TLS 3.6.7**, from the official
complete release archive with SHA-256
`a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`.
No OpenSSL installation or Mbed TLS DLL is required on Windows. The standalone
Windows build selects the static C runtime. Enabled binary distributions must
retain the Mbed TLS Apache-2.0 licence and its bundled third-party notices
(the release contains `LICENSE` and per-file notices). Linux uses installed
OpenSSL 1.1.1+ (`libssl3` in the container); retain that package's licence notices.
The dependency source pin is not a promise of automatic security updates;
upgrade it after reviewing upstream security releases.

Root/game and independently configured viewer builds opt in with
`-DEAWR_ENABLE_RELAY=ON`; the default is OFF and performs no TLS discovery or
download. GNS remains a separate opt-in. The optional `linux-x64-gcc-relay` and
`windows-msvc-ninja-relay` presets enable both and build the relay and net tests.

## Owner's native Windows laptop

Build or copy the standalone executable onto the owner's Windows laptop. It
does not require game files, a Steam account, a public CA certificate or a
domain. Start with loopback so configuration can be checked before exposing it.

1. Copy `tools/relay/relay.example.conf` to `out/relay/relay.local.conf`, or
   a protected directory outside the repository. Secrets belong under ignored
   `out/` or outside the repository; `tools/relay/*.local.*` is additionally
   ignored as a safeguard for older local configurations. Restrict the directory
   to the operator account. The Windows generator creates the private key with
   a protected, owner-only ACL at file creation, without inheriting directory ACEs.
2. From `out/relay`, generate a self-signed certificate and a private access
   token with the executable (substitute its relative path if installed elsewhere):

   ```powershell
   ./Release/eawr_relay_server.exe --generate-certificate --certificate-file relay-cert.local.pem --key-file relay-key.local.pem
   ./Release/eawr_relay_server.exe --generate-token
   ```

   The certificate command prints its 64-digit lowercase SHA-256 fingerprint.
   It generates an EC P-256 key and a SHA-256 self-signed certificate, valid for
   one year, for localhost. It refuses to overwrite either file. A certificate
   for a public service name can instead come from the operator's CA.
3. Public guest access defaults to `require_access_token=0`. For a private service, set `require_access_token=1` and `access_token` to the generated token. Give authorized publishers the operator-controlled publisher token over
   a trusted channel. Joiners receive only the service's numeric address/port,
   trusted certificate fingerprint and a host's single-use join code; they need
   no access token and cannot publish new sessions.
   For remote use of the generated certificate select fingerprint verification;
   it authenticates the exact certificate regardless of its localhost name.
   Certificate validity dates still apply. Join codes are consumed by the first authorized join. No credential or address is embedded in a build.
4. Double-click `tools/relay/start.cmd` to run the native executable. It calls
   `start.ps1`, keeps the process in the foreground, and appends sanitized status
   and error messages to `out/relay/logs/relay.log`. Ctrl-C stops it. There is no
   scheduled logon task. A shortcut may target `start.cmd`; set its working
   directory to `tools/relay`.
5. Probe it using the same binary:

   ```powershell
   ./Release/eawr_relay_server.exe --health --address 127.0.0.1 --port 27020 --certificate-sha256 <fingerprint>
   ```

   For a publicly issued certificate (such as Let's Encrypt), use
   `eawr_relay_server --health --address <numeric endpoint> --port 27020 --certificate-name <service DNS name>`.
   Self-signed deployments use `--certificate-sha256` or `--ca-file`.
   No `--ca-file` or fingerprint is needed for a publicly issued certificate: the system CA store verifies the chain
   and the supplied name. An explicit `--certificate-name` on the command line prevents the local
   mounted-certificate health fallback from silently selecting a pin.

   The command returns 0 and `healthy` only after verified TLS and the protocol
   health exchange. It has no HTTP endpoint and does not test coturn.
6. When ready for external use, set `listen_address=0.0.0.0` (dual stack where
   supported), then the operator opens TCP 27020 inbound on Windows Firewall
   and forwards that TCP port on the router if the laptop is behind NAT. Use
   the actual configured port if changed. A healthy loopback probe does not
   prove the router mapping works.

The same private configuration supports Windows Docker Desktop via the Linux
container below. Native operation requires fewer moving parts for the laptop.

## Linux and container startup

Copy the example config to `out/relay/relay.local.conf` or a protected directory
outside the repository, then create the certificate/token with `out/relay/eawr_relay_server`,
and protect the files with `umask 077` and owner-only permissions. Run:

```sh
sh tools/relay/start.sh
```

The script accepts an optional config path and appends to
`out/relay/logs/relay.log`. Both scripts launch from the config
directory so relative certificate/key paths resolve beside that file. Override `EAWR_RELAY_BINARY` or `EAWR_RELAY_LOG_DIR` for
an installed executable or a different log directory. It runs in the foreground.
On Windows `start.ps1` accepts `-Config`, `-Binary` and `-LogDirectory`.

Build the container from the repository root:

```sh
docker build -f apps/relay_server/Dockerfile -t eawr-relay .
docker run --rm --name eawr-relay -p 27020:27020/tcp --mount type=bind,source="$(pwd)/out/relay",target=/config,readonly eawr-relay --config /config/relay.local.conf --listen-address 0.0.0.0
```

Probe the container with `docker exec eawr-relay eawr_relay_server --health --config /config/relay.local.conf`;
exit 0 and `healthy` confirm the local verified-TLS exchange. The Docker
HEALTHCHECK runs that command every 30 seconds. If no explicit health CA or pin
is configured and no `--certificate-name` is supplied on the command line, the CLI derives the pin from the protected mounted certificate;
this never disables peer verification. A `certificate_name=` config key alone
does not suppress this fallback; supply the command-line flag to select system trust.

The container runs as UID/GID 10001; the operator must grant that user read
access to the mounted config, certificate and key without granting other users
access. It builds/copies only the engine-free relay sources. Logs go to stdout
and stderr (`docker logs eawr-relay`); no credentials, signals, join codes or
peer addresses are logged. Restrict container network access and resource
limits as appropriate for the host. This task has not built or deployed an image.

## Coturn route and firewall

Use a separately maintained coturn installation or container. A minimal private
configuration uses these keys (fill the private secret and external address
locally):

```text
listening-port=3478
realm=private-relay
use-auth-secret
static-auth-secret=<operator-generated-secret>
fingerprint
min-port=49160
max-port=49200
user-quota=2
total-quota=128
max-bps=1048576
bps-capacity=16777216
no-multicast-peers
no-loopback-peers
no-tcp-relay
denied-peer-ip=0.0.0.0-0.255.255.255
denied-peer-ip=10.0.0.0-10.255.255.255
denied-peer-ip=100.64.0.0-100.127.255.255
denied-peer-ip=127.0.0.0-127.255.255.255
denied-peer-ip=169.254.0.0-169.254.255.255
denied-peer-ip=172.16.0.0-172.31.255.255
denied-peer-ip=192.168.0.0-192.168.255.255
denied-peer-ip=::1
denied-peer-ip=fc00::-fdff:ffff:ffff:ffff:ffff:ffff:ffff:ffff
denied-peer-ip=fe80::-febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff
# Behind NAT: external-ip=<public-address>/<private-address>
```

Use a coturn secret separate from the introduction access token; generate it
with `--generate-token`. Set the introduction config's `turn_secret` to the
same private value and `turn_uri=turn:<numeric-address>:3478`. The native GNS
TURN backend takes numeric server addresses. The client resolves a hostname
before handing it to GNS, but a numeric address is simpler and avoids a lookup. Do not set `no-auth`. Keep coturn's admin CLI disabled or bound privately.
Add `denied-peer-ip` ranges for private, loopback, link-local and other internal
destinations so public allocations cannot reach the operator's internal network;
the operator must choose the appropriate IPv4/IPv6 ranges. Coturn's own quotas
and bandwidth caps enforce relay abuse bounds; the introduction limits do not
cover UDP game bytes.

Open/forward **UDP 3478** and **UDP 49160..49200** to coturn, in addition to the
introduction's TCP port. The native GNS relay route in this slice uses UDP TURN;
allowing TCP 3478 or TLS 5349 is not a claim of TCP/TLS TURN support in native
ICE. Configure the external/private address mapping when coturn is behind NAT.
If the host has CGNAT and cannot receive these ports, a reachable host under the
operator's control is needed; this work provisions none.

Paired clients receive distinct coturn REST credentials over verified TLS.
Their passwords are Base64 HMAC-SHA1 over the expiring username. The shared
secret stays on the introduction server and coturn. GNS applies the URI, username
and password when creating its introduced sockets and enables relay candidates.
`GnsOptions::turn_only` requires TURN candidates for later route qualification;
it fails if credentials are missing. This slice has not qualified live symmetric
NAT, public relay allocation, bandwidth or router reachability.

## Cloudflare Realtime TURN instead of coturn

The introduction service can ask **Cloudflare Realtime TURN** for relay credentials instead of
computing coturn REST credentials. This is the public server's choice (see
[public-server-setup.md](public-server-setup.md)); coturn stays fully supported and is the default.

```text
turn_provider=cloudflare
# from the environment or a protected config file. The server refuses them on the command line
# (EAWR-NET-1555), because arguments show up in process listings and shell history:
#   EAWR_RELAY_CLOUDFLARE_TURN_KEY_ID=<Turn Token ID>
#   EAWR_RELAY_CLOUDFLARE_TURN_API_TOKEN=<API Token>
```

- `turn_provider=coturn` (default) uses `turn_uri` and `turn_secret` exactly as before. `cloudflare`
  uses the two Cloudflare values and must not be combined with `turn_uri` or `turn_secret`. An unknown
  provider name, a missing or malformed Cloudflare value, or a mixed configuration fails startup with
  `EAWR-NET-1555`; no value is ever printed.
- When a guest joins, the server makes **one** HTTPS POST to
  `https://rtc.live.cloudflare.com/v1/turn/keys/<key id>/credentials/generate-ice-servers` with
  `Authorization: Bearer <API token>` and `{"ttl": N}`. `N` is `session_lifetime` (rounded up to
  whole seconds) plus 120 seconds. `session_lifetime` is capped at 24 hours, so `N` is at most 86,520;
  the server also clamps it to 172,800 (48 hours), which stays inside every limit we know of. A longer
  credential lifetime needs the refresh in ticket E. It takes the UDP `turn:` URLs from the answer (at most two,
  normally ports 3478 and 443; `turns:` and TCP entries are skipped) and sends them with the
  username and credential to **both** players in the usual Paired message. The two players share the
  one credential pair.
- The call runs on one bounded worker thread, never on the relay's I/O thread, with a request timeout
  (`cloudflare_turn_timeout_ms`, default 4000), at most `cloudflare_turn_calls_per_minute` calls in any
  rolling minute (default 30) and no retries. The connection verifies the certificate chain against the
  operating system's trust store (the container installs `ca-certificates`) and checks the name
  `rtc.live.cloudflare.com`; there is no insecure mode. On Windows the roots come from the CurrentUser and LocalMachine `ROOT` stores. Name
  resolution for the call is bounded by the same timeout and by shutdown: a lookup that outlives it is
  abandoned (the operating system finishes it in the background on a helper that owns only copied
  data). The number of such helpers is bounded by the per-minute call cap.
- If Cloudflare is unreachable, slow, refuses the request or answers with something unusable, the
  pairing still succeeds, without relay candidates, and the log gets one warning (never the token, the
  key ID or the response body). Players who can connect directly are unaffected.

  | Code | Meaning |
  | --- | --- |
  | `EAWR-NET-1555` | Startup: invalid or mismatched TURN provider configuration |
  | `EAWR-NET-1556` | Cloudflare call failed: unreachable, TLS verification failed, timed out, or not ready in time |
  | `EAWR-NET-1557` | Cloudflare refused the request (HTTP status other than 2xx; the status number is in the text) |
  | `EAWR-NET-1558` | Cloudflare's answer was malformed or contained no UDP `turn:` URL with credentials |
  | `EAWR-NET-1559` | The per-minute call cap or the worker queue was full; no call was made |

- **Revocation.** When a paired session ends, the server posts to Cloudflare's revoke endpoint
  (`.../credentials/<username>/revoke`) once, best effort and within the same call cap. If that call is
  skipped or fails, the credential simply expires with its TTL (session lifetime plus 120 s).
- **Hostnames.** Cloudflare's URLs are hostnames (`turn.cloudflare.com`). GNS only takes numeric
  addresses, so each player's client resolves them (IPv4 preferred, at most two addresses per URL) on its
  introduction worker thread before handing them to GNS. If nothing resolves, that player continues
  without relay candidates. coturn's numeric `turn:` URIs pass through untouched.
- Cloudflare bills relay traffic; at the time of writing the owner's pricing note (legacy EAWR-3627) gives $0.05/GB to
  players after the first 1,000 GB each month, shared with other Realtime products. Check Cloudflare's
  current pricing. Treat the API token like a password: it can mint unlimited credentials.
  Rotate it by creating a new key in the dashboard, updating the environment and restarting.

## Configuration keys and operations

Precedence is defaults, `--config` file, `EAWR_RELAY_<UPPERCASE_KEY>` environment,
then CLI. CLI accepts underscore or hyphen spelling (`--listen-address`). Files
use `key=value`, blank lines and whole-line `#` comments. Unknown keys, invalid
numbers, invalid limits and mismatched TURN URI/secret fail startup. Avoid passing
secrets as CLI arguments; keep them in the protected config or environment.

| Keys | Default / meaning |
| --- | --- |
| `listen_address`, `port` | `127.0.0.1`, `27020`; numeric IPv4/IPv6; 0 selects an ephemeral port for tests |
| `certificate_file`, `key_file` | Required server PEM paths; private key must be unencrypted for unattended parsing |
| `require_access_token` | 0 (OFF) allows guest publishers; 1 requires the operator secret. Public slot-availability trade-off is described below. |
| `room_aliases`, `failed_guesses_per_minute` | 0 (aliases OFF), 100 alias misses per rolling minute globally; full codes retain the per-prefix guess limit |
| `access_token` | Optional publisher-only operator secret when `require_access_token=1`, 32..256 bytes; generate 256 random bits with `--generate-token` |
| `turn_uri`, `turn_secret` | Empty together disables TURN credentials; otherwise `turn:` URI and 32..256-byte shared secret (coturn provider only) |
| `turn_provider` | `coturn` (default) or `cloudflare`; mismatched combinations fail startup with `EAWR-NET-1555` |
| `cloudflare_turn_key_id`, `cloudflare_turn_api_token` | Secrets of the Cloudflare provider; use `EAWR_RELAY_CLOUDFLARE_TURN_KEY_ID` / `EAWR_RELAY_CLOUDFLARE_TURN_API_TOKEN` or a protected config file |
| `cloudflare_turn_calls_per_minute`, `cloudflare_turn_timeout_ms` | 30 outbound calls per rolling minute (1..600), 4000 ms request timeout (500..30000) |
| `max_connections`, `max_sessions` | 128, 64 (caps 4096 / 2048) |
| `connections_per_ip`, `accepts_per_minute`, `unpaired_sessions_per_ip` | 8 live connections, 30 accepts/minute, 2 unpaired sessions per IPv4 address or IPv6 /64; mapped IPv4 shares the IPv4 key |
| `directory_operations_per_minute`, `directory_bytes_per_minute` | 2,048 operations / 8,388,608 bytes per prefix for directory traffic; independent of legacy session budgets |
| `operations_per_minute`, `bytes_per_minute` | 120 operations / 1,048,576 bytes per IP and per session; IP bytes include incoming TLS records, session bytes count protocol bodies |
| `queue_bytes`, `queue_frames` | 262,144 / 64 outgoing bytes/frames per connection |
| `admission_timeout_ms`, `max_join_lifetime_ms` | 10,000 before publish/join, 900,000 maximum accepted join lifetime; longer requests are capped |
| `idle_timeout_ms`, `session_lifetime_ms` | 60,000 admitted idle / 7,200,000 paired lease length; each renewal adds one lease |
| `max_session_lifetime_ms`, `renewal_window_ms` | 86,400,000 (24 h, cap 7 days) absolute paired session maximum counted from pairing; pairing TURN TTL is this maximum + 60 s, clamped to 48 h / 600,000 renewal window before a lease ends (at most half a lease) |
| `address`, `certificate_name`, `ca_file`, `certificate_sha256` | Health/client numeric endpoint; name with no CA file or pin selects system CA trust; explicit pin takes precedence over CA/name; no insecure mode |

Public token-free publishing trades easy guest access for slot availability:
with the defaults, thirty-two IPv6 /64 prefixes can hold all sixty-four unpaired
session slots by publishing two invitations per prefix and republishing at expiry.
Admission and byte quotas bound the work but do not establish player identities.
Operators can enable `require_access_token=1` for controlled publisher access, or
tune `max_sessions`, `unpaired_sessions_per_ip` and admission lifetime to their
community's capacity. This is the documented cost of the public token-OFF policy.

The same client fields appear in `NetworkIntroductionOptions`; its access token
is used only by publish, and join sends the code and identity alone; application code
constructs `NetworkGnsIntroduction`, publishes or joins, waits for the host's
paired rendezvous, then supplies that endpoint to `GnsTransport`.
Identities are unauthenticated, self-asserted claims from publishers and
join-code holders, bound to their TLS connection. They do not prove players or
accounts. The server can observe signals and
owns TURN credential issuance; trust the chosen operator. Pin rotation requires
distributing the new fingerprint over a trusted channel. CA mode requires a
certificate with the correct DNS/IP name. Server restart or a signalling TCP
disconnect releases the pair; established GNS data connections follow the
adapter's existing signalling-outage behavior. Reconnect UI is outside this slice.

Upgrade by stopping the process, replacing the executable/container with a
validated build, retaining the private config/certificate/key, and starting it
again. Active introductions are in memory and are lost on restart. Rotate the
access token and TURN secret if leaked; outstanding TURN credentials may remain
usable until expiry unless coturn is restarted or explicitly revokes them.
Rotate/renew the certificate before its one-year expiry. Archive/rotate the
sanitized text log with the operator's normal tools; the binary does not rotate
logs itself. No scheduled task, DNS record, purchase or firewall change is part
of startup.


## Directory and chat limits

The same optional TLS service hosts the always-on room directory. No accounts
or global chat are configured. Directory rooms use the `max_sessions` cap
independently of paired introductions; principal history is bounded to four
entries per configured connection slot, with at most four new principals per
prefix per minute and two concurrent rooms per prefix. Resumes and listings do
not consume that creation quota. Disconnected history lasts 60 seconds,
and rooms expire unless their owner renews the 30-second lease (client cadence:
10 seconds). Pages contain at most 50 rooms, queried at most once per second per
principal. Starting/closed rooms cannot admit new guests.

Private admission stores salted PBKDF2-HMAC-SHA256 hashes with 120,000 iterations,
never plaintext. Passwords are limited to 128 bytes. Five failed attempts per
prefix per rolling minute trigger a 60-second cooldown shared with aliases;
every hash (including successful publish/join) counts toward a four/minute prefix
cap, per-room verification is capped at 30/minute, and all hashes at 128/minute.
The fixed iteration count is a deliberate bound for short-lived room passwords.
PBKDF2 runs on one worker with at most 16 outstanding copied jobs; the relay
owner remains available for I/O and alone commits current-state admission.
Room IDs retain their long-form exemption from the global alias guess cap.

Room chat keeps 200 sanitized lines, at most 128 UTF-8 bytes each, with one send
per second per principal. Member names are capped at 20 bytes and room rosters
at 64 seats. The host may remove a guest from chat; operator integrations may
invoke `RelayServer::remove_chat_member` on the poll owner thread. Both deny
chat reads/writes while leaving the seat reserved until leave/disconnect.
Directory operation/byte budgets default to 2,048 requests and 8 MiB/minute per
prefix, independent of legacy signalling's 120 requests and 1 MiB/minute. At
default capacity a distributed fill needs at least 32 prefixes for all 64 rooms,
and 128 at sustained creation rates (64 in a boundary burst) for 512 principal
history slots; a prefix is not a verified account. Use directory budgets to control egress,
and coturn limits separately for game traffic. Do not add passwords, principal
or ownership capabilities, peer addresses, or rejoin secrets to logs/metrics.
The [service contract](../net-relay-server.md#room-directory-private-admission-and-room-chat)
documents directory framing, diagnostics, resources and client/source APIs.
