# Architecture decisions

Each decision below has a stable reference and a plain name. Accepted decisions
remain in force except for the parts explicitly superseded by a later decision.

## ADR-001: Engine-independent core before backend selection

Status: superseded by the [Godot presentation decision](#adr-011-godot-presentation).

Build the simulation, loaders, virtual file system and Lua host independently of
the presentation engine. Compare an SDL3/wgpu backend with Godot RenderingServer
through GDExtension against loaded assets before choosing. The lasting boundary
is a read-only simulation snapshot; simulation code includes no engine headers.

## ADR-002: Windows and Linux targets

Status: accepted.

Target Windows x64, Linux x64 and Linux ARM64. Qualify MSVC/Clang on Windows and
GCC/Clang on Linux. Cross-target replay comparison checks deterministic state;
cross-built packages alone do not establish native runtime or GPU support.
See [build](build.md) and the maintainer CI policy.

## ADR-003: Integer-only authoritative simulation

Status: accepted; refined by the [checked Q24 math decision](#adr-010-checked-q24-math-and-finite-durations).

Simulation state and arithmetic use fixed point. Floating values belong to
rendering, audio and UI, which convert at the snapshot boundary. Deterministic
geometry uses integers. The checked Q24 contract settles the binary point and
removes the earlier proposed strict-float fallback.

## ADR-004: Case-insensitive layered asset access

Status: accepted.

Route runtime asset I/O through a case-insensitive [VFS](vfs.md): mod
layers, expansion, base; loose files override active archives within each layer.

## ADR-005: Headless replay determinism

Status: accepted.

Use headless replay hashes as the cross-platform determinism oracle.
Compare actual per-tick outputs across targets and worker counts (CI evidence 1/2/4;
contract tests also 8 and the hardware count), including tests that perturb storage
order. See [simulation](simulation.md) and
[replay format](replay-format.md).

## ADR-006: C++20 and composed libraries

Status: accepted; the presentation composition is superseded by the
[Godot presentation decision](#adr-011-godot-presentation), and the entity-library
choice is settled by the [EnTT storage decision](#adr-009-entt-storage-and-stable-simulation-ids).

Use C++20 and CMake for the engine-independent simulation, VFS, loaders and Lua
host. The original library composition proposed SDL3, RmlUi, Dear ImGui and
miniaudio for presentation; Godot now owns that integration. Recast/Detour land
navigation remains subject to behaviour matching; GameNetworkingSockets and
Steamworks were proposed for networking. These proposals do not establish a
completed navigation or networking implementation. MIT reuse retains attribution.

## ADR-007: Original XML and Lua compatibility

Status: accepted.

Keep original XML tag and Lua API names and semantics; extensions are additive.
Inventory declarations do not prove runtime behaviour. Keep exact numeric
lexemes and report unsupported features instead of inventing semantics.

## ADR-008: Clean-room implementation and distribution

Status: accepted.

Ship new code only; users supply their own game/mod installation. Original
assets, restricted source and translated private shaders are not redistributed.
Require the [clean-room boundary](clean-room.md); MIT reuse retains
attribution. Original behaviour belongs in clean notes with independent fixtures.

## ADR-009: EnTT storage and stable simulation IDs

Status: accepted.

Use EnTT v3.15.0, commit
`d4014c74dc3793aba95ae354d6e23a026c2796db`, as private component storage.
Persistent nonzero uint64 EntityIds are separate from EnTT handles and are never
reused. Evaluation, commit, serialization and snapshots use ascending stable-ID
order. Workers read immutable copied inputs and write disjoint staging outputs;
ordered serial commit alone mutates storage. Per-entity work runs in named
phases over 64 fixed partitions on a persistent worker pool; the
[phase map](simulation.md#phase-map) says which steps are serial and why. Public
interfaces expose no ECS handles or references. EnTT avoids a second scheduler; this is not a benchmark
claim against Flecs.

## ADR-010: Checked Q24 math and finite durations

Status: accepted; refines the [integer-only simulation decision](#adr-003-integer-only-authoritative-simulation).

Require integer-only authoritative arithmetic: checked signed int64
Q24, nearest-even reductions and exact widened accumulation. There is no float
fallback. [Fixed-point](fixed-point.md) defines overflow, geometry and extended
finite-duration rules. Lua remains outside authoritative state until its numeric,
RNG, time, iteration and serialization policies are defined and qualified. The
selected [Lua profile](multiplayer-readiness.md) (owner decision on the Lua numeric policy) keeps
script numbers as IEEE binary64 computed by integer-only soft-float code, not
hardware floats, and converts them to and from Q24 with defined rounding at the
simulation boundary; Q24 stays the simulation's number type.

Retain exact authored large finite durations rather than treating them as unlimited.
The [fixed-point contract](fixed-point.md) specifies a separate whole-seconds and
fraction integer representation for duration consumers after conformance tests.
Unknown sentinel meanings remain unresolved; ordinary Fixed conversion fails
explicitly outside its domain.

## ADR-011: Godot presentation

Status: accepted; supersedes the [deferred backend choice](#adr-001-engine-independent-core-before-backend-selection)
and the presentation portion of the [composed-library stack](#adr-006-c20-and-composed-libraries).

Select Godot 4.7.2 with pinned godot-cpp 10.0.0 bindings through a C++
GDExtension and RenderingServer resource ownership. A bootstrap node is allowed;
simulation entities do not become Godot nodes. The choice reduces editor,
rendering, UI, audio and platform integration work; it is not an FPS ranking.
It supersedes the deferred backend choice and proposed SDL/RmlUi/ImGui/miniaudio
presentation stack. Prefer Godot UI/audio behind project-owned adapters for
original XML, atlas and sound-event data; this is not a completed UI/audio port.

Immutable snapshots are the only simulation-to-renderer state boundary. Godot
types, callbacks and floating presentation state stay outside authoritative
simulation. RenderingServer materials require Godot-language adaptation; raw
SPIR-V is not a drop-in material. RenderingDevice/compute needs a separate
integration. Preserve dependency caches and keep packages free of original
assets and translated private shaders. Cross-built ARM packages do not establish
native runtime or GPU support. Revisit the backend if material fidelity, platform
viability, representative RTS performance or maintenance cost requires it.

## ADR-012: Galactic simulation layer

Status: accepted for M3; written with the GC architecture (legacy EAWR-1958).

The galaxy is its own deterministic simulation, `GalacticSession` in `eawr_sim_galactic`
(`src/sim/galactic`, own source list), with no Lua and no tactical session inside it. It
shares the core contracts: stable IDs and EnTT storage (ADR-009), checked Q24 (ADR-010), the
phase map and worker pool, ordered events, immutable snapshots (ADR-011), the off-thread state
hasher and typed replay commands. Randomness is keyed counter-based draws, so a checkpoint
needs only the seed. The galactic state hash is SHA-256 over versioned section digests, each
re-encoded only in ticks that change it. A `GameSession` in a new `eawr_campaign` library
composes the scripted galaxy with an optional tactical child; it is the only component that
knows both layers. See [galactic architecture](galactic-architecture.md#1-the-galactic-simulation-layer).

## ADR-013: Galactic time

Status: accepted for M3.

One galactic tick is one FoC logical frame, 1/30 s of galactic time, the tactical rational.
The session tick (commands, replays, hashes, lockstep), the galactic frame (advances only
when the galaxy is serviced) and the tactical frame are separate state counters. Pause is a
nested state counter changed by commands and by pending conflicts. Speed settings are state
changed by commands and change only the pacing until debug-build evidence shows a per-tick
effect. A fully paused session steps only to execute waiting commands. See
[galactic time](galactic-architecture.md#2-galactic-time).

## ADR-014: Tactical battles nest inside the galactic session

Status: accepted for M3.

One process, two simulation layers, one ID allocator, one seed, one session tick and one
worker pool. A session tick runs exactly one layer: the child battle while it exists, the
galaxy otherwise. The galaxy is frozen during a battle. A typed conflict record hands persistent
forces, station, defences, map and rules to a conflict entry builder that reuses the skirmish
start's placement; persistent objects keep their galactic IDs in the battle. A typed result
record returns winner, retreat and per-identity fates; auto-resolve is a bounded, checkpointable model producing
the same record, so one reconciliation path serves both. A child battle is an ordinary tactical
replay and can be re-run alone. See
[tactical nesting](galactic-architecture.md#3-tactical-nesting).

## ADR-015: Galactic commands are the only input

Status: accepted for M3.

Every external player action, AI order and ordinary script world intent is a typed
`GameCommand` (session, galactic or tactical layer) in canonical `(tick, seat, sequence)`
order in one replay stream. Story graph service derives typed effects from causal input
inside the session, preserving callback-before-reward and recursive activation order;
presentation completions remain recorded commands. Presentation state is never a command
and the viewer holds no session. Galactic opcodes are a separate,
coordinator-registered number space reserved in one block. A PR adding behaviour proves it is
multiplayer-safe with live-equals-replay, round-trip and refusal tests, the two-peer lockstep
loopback gate, 1/2/4/8-worker and scrambled-storage parity, and the boundary checks. See
[commands](galactic-architecture.md#4-commands).

## ADR-016: One checkpoint format for saves and resync

Status: accepted for M3; supersedes nothing (it is the format the
[multiplayer readiness](multiplayer-readiness.md) design asked for).

`EAWRCKP` is a canonical, uncompressed, sectioned format bound to format, rules, numeric, Lua
and content identities and to the sealed-input frontier. Restore validates everything into an
inactive session before swapping. Derived data is rebuilt from content. A battle in progress
is saved either as its entry record plus commands (re-simulated on load) or, once tactical
restore exists, as full state. Player saves wrap a checkpoint with unhashed metadata and replay
their load fix-ups as commands. In M3 a checkpoint loads only into an identical build; it is
never truncated. The remake reads only its own saves. See
[checkpoint format](galactic-architecture.md#5-checkpoint-format).

## ADR-017: Galactic data is tolerant and data-driven

Status: accepted for M3.

Galactic content loads through validated loaders in `src/data/galactic` and a galactic type
table beside the M2 unit tables. Bad or unknown data is a diagnostic row, not a failed
campaign, and no vanilla list is hard-coded; code literals are only walk-verified constants
with their rule IDs. Each applied tag flips its registry row in the same PR, in a galactic
status shard. See [data](galactic-architecture.md#6-data).

## ADR-018: Galactic Lua and AI hosting

Status: accepted for M3.

`ScriptedGalacticSession` reuses the authoritative scheduler, sandbox, numeric profile,
persistence and command routing around the galaxy; story plots and galactic AI plans are
instance sets with mode-scoped story hosts. AI service stops while paused or during a battle;
story service keeps WST-11/12/16 dispatch semantics with frozen elapsed accumulators, bounded
ordered callbacks and rewards. Only the active child's story is serviced during a battle (WST-11; WST-U03 resolved).
The galactic AI reuses the FoC AI engine through a mode-neutral world
interface with a galactic manager beside the tactical one, budgets its work deterministically,
and is accepted by the AI-vs-AI GC soak. See [Lua](galactic-architecture.md#7-lua) and
[AI](galactic-architecture.md#8-ai).

## ADR-019: Galactic viewer mode and in-process mode switches

Status: accepted for M3.

The viewer gains a galactic mode with its own source list; it consumes `GalacticSnapshot` from a
`LiveGameSession` on the simulation thread and sends only commands. A battle hand-off switches
to the existing tactical live presentation in the same process, with the galaxy scene kept
resident; the simulation never waits for presentation, the live pacer only delays stepping the
child until the scene is ready. Galactic per-entity phases are partitioned with 1/2/4/8-worker
parity and deterministic work budgets. See [viewer](galactic-architecture.md#9-viewer) and
[threading](galactic-architecture.md#10-threading).

## ADR-020: Seats command, players own (coop GC from the start)

Status: accepted for M3 (owner requirement 2026-10-07, coop GC (legacy EAWR-2013)).

Every command's issuer is a seat seated in one player; players own planets, fleets, credits,
tech, the pool, victory and defeat. A command is authorized when its seat is active and seated
in the owning player. The new galactic envelope uses an explicit `SeatId`, separate from
owning `PlayerId` (A1).
The original galactic scope allows one to eight humans sharing one side against AI or
forming two human sides. ADR-023 supersedes this capacity for online tactical matches:
up to nine command seats share up to three opposing human owners; a fourth is refused. Faction count is independent and data-driven. All seats
command their faction, last order wins in canonical seat order. In battle the
session forwards seat commands as the faction's tactical player, so the tactical session and
M2 replays need no seat concept. Selection, control groups and camera are per-seat presentation
state. A seat leaving is a session command; remaining seats retain control and the configured
AI/passive handover applies only when none remain. Setup, replay, checkpoint and loopback
carry the seat/side tables. Galactic validation retains its original capacity; tactical
seat routing and checkpoints use the nine-seat/three-owner limit. See
[seats and coop](galactic-architecture.md#seats-and-coop-galactic-conquest).

In versus battles against AI, the other team's lead assigns all its active seats to control
the AI units, share the fighting team's control as temporary allies, or spectate read-only;
the fighting team retains auto-resolve. The conflict-scoped session command, effective
battle assignment and observer perspective are replay/checkpoint state, with every peer
participating in lockstep. Tactical admission uses the battle mapping; reconciliation uses
original owners and clears temporary roles. Galactic ownership and diplomacy do not change.

## ADR-021: Galactic extension points are explicit contracts

Status: accepted for M3 (owner requirements 2026-10-07).

`AutoResolveModel` takes forces, strengths, planet, modifiers and conflict-scoped RNG,
supports bounded stepping and canonical restore, and returns one typed conflict result.
Its model ID, version and configuration are authoritative setup and replay/checkpoint identity;
FoC is the first implementation, with future replacement tracked separately (legacy EAWR-2033).
`GalacticCameraBehaviour` maps seat-local input and snapshot targets to pose using mod-overridable
parameters, entirely in presentation (legacy EAWR-2034). `FactionController` supplies commands and
canonical controller state for any data-defined faction, including non-playable owners; initial
kinds are human seats, AI and scripted/passive (legacy EAWR-2035). Future autonomy is separate work.

Pause admission is a setup/checkpoint policy indexed by command kind with the FoC default
(legacy EAWR-2038). Orbit fleet slot count is a pinned parameter N (FoC default three) consumed by
storage, commands, conflicts, checkpoint and viewer layout (legacy EAWR-2039). M3 includes the seams
and tests with non-default fixtures; exposing future options does not change M3's FoC defaults.
See [galactic architecture](galactic-architecture.md).

## ADR-022: Online play without port forwarding

Status: accepted: option A, project-run introduction and relay server
(owner, 2026-10-13). Integration: an optional `GnsTransport` implements
`MessageTransport` over Valve GameNetworkingSockets. Slice 1 adds the dependency
and adapter with direct IP and in-process signalling qualification; slice 2
provides the configurable server and viewer join codes. Deployment is a separate
owner decision. See [GNS integration](gns-transport.md) for dependency pins,
crypto, signalling and validation boundaries.

LAN play reaches peers directly over `TcpTransport`, so an Internet host must
forward a port. The goal is that hosting needs no router setup. Every option is
a `MessageTransport` adapter; lockstep, setup agreement, desync diagnostics and
the simulation do not change, and adapters can coexist.

- **Project-run introduction and relay server.** Peers connect outward only;
  the host gets a join code. The server introduces peers for a direct UDP path
  (for example GameNetworkingSockets ICE) and relays the pairs that cannot
  connect. Lockstep traffic is a few KB/s per player, so one small server is a
  workable start, with more relay regions only if relayed latency needs them.
  Costs hosting and upkeep; the address is configurable so others can run one.
- **Epic Online Services.** Free hosted lobbies, NAT traversal and relays,
  anonymous device sign-in, store-agnostic. Costs a closed SDK, an Epic product
  registration and its terms, and an external dependency.
- **Steamworks under the FoC App ID.** Overlay invites, Steam lobbies and Valve
  relays, with an ownership check, for Steam owners only. The App ID belongs to
  the rights holders and the SDK terms cover partner-distributed games, so this
  needs their permission first. An own App ID needs Valve to approve a Star Wars
  product; Spacewar (480) is for development only. Linux ARM64 support of the
  Steam API is unconfirmed.
- **Router port mapping (UPnP/NAT-PMP)** removes some port forwarding without a
  server, but does not work for everyone (UPnP disabled, carrier-grade NAT).

Rejected: user-installed VPN or virtual-LAN tools (Tailscale, ZeroTier, Hamachi);
players must not need third-party software to play online.

Identity (names, avatars) is separable from transport. With a project server,
Discord OAuth2 or Steam OpenID sign-in in the browser plus the public Steam Web
API supplies verified names and avatars without an SDK or App ID; it does not
prove game ownership. The Discord Social SDK could later add presence and
invites. Retail FoC lobbies and wire compatibility are out of scope: commands
and the simulation deliberately differ from the original. Work and the
permission request are tracked in the online multiplayer tracking issue.

## ADR-023: One lobby with private online invitations

Status: accepted product direction, 2026-10-13 (legacy EAWR-3461); supersedes the
private-code/two-seat-first proposal. The original heading remains for stable
links. Implementation and live route qualification are pending.
See [online lobby design](net-online-lobby.md) for the owner's decision table,
current limits and revised tickets A-I.

Use **one LAN/Online lobby screen with a game list and chat window**. Online
ships with **nine capable player slots**, including 3v3v3, and **public and
password-protected private rooms**. Two-peer development slices do not define
release capacity. Keep host-owned revisioned staging and existing canonical
setup/content/tick-zero agreement. Names are guest labels; no account hoops.
Compare a local compiled-build hash at peer admission and bind it through setup.
Optional invitation shortcuts use four letters/digits with server guess limits.

ADR-022 supplies introduction/relay; the same always-on service can host room
listing, password admission and bounded online lobby chat. Passwords do not
replace NAT introduction. An optional operator token protects service/relay
bandwidth, not identity; proposed public-community default is OFF, with separate
TURN quotas. Required publisher tokens and 32-hex pair codes in the pending
MP-9b service need follow-up in B; directory/password/chat need I, and a
nine-peer roster needs G. This decision edits no relay protocol or code.

Consequences: raise eight-peer setup/lockstep/TCP/lobby bounds and two-peer GNS
admission; qualify three human sides, nine seats and 36 mesh pair paths. The
tactical sim's `max_players = 64` is not that peer limit. More network/retained
traffic needs bounded queue/frame-cap checks, while 1/2/4/8 simulation-worker
equality tests remain; nine-peer performance testing is deferred by the owner.
Before battle, host loss closes staging. During battle, use held-seat return
then agreed departure; no initial host migration. Crash/relaunch return is a
nice-to-have, not a requirement for prolonged connections.

Hours-long connections are required. E refreshes TURN credentials and admitted
service lifetime, and lifts/qualifies `max_session_ticks = 1'000'000` (about
9.26 hours at 30 Hz) plus retention bounds; the current service's two-hour age
is another implementation limit, not a product limit. Online GC is outside this
space-skirmish delivery. Automatic matchmaking, ranked play and accounts remain
outside scope. The former two-seat/private-code estimates do not cover this
release. These are intentional EAWR product choices, not retail wire fidelity.

## Shader extensibility

Newly authored modern shaders must have a path independent of legacy effect names
and the FX parser. Describe supported languages, stages, resources and backend
constraints explicitly. Do not promise arbitrary HLSL/GLSL, compute, mesh shaders
or ray tracing can be dropped in unchanged. Legacy material semantics stay intact.
See [shader translation](shaders.md).
