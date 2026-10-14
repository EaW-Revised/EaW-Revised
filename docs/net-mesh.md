# Direct TCP full mesh

The [online lobby decision](architecture-decisions.md#adr-023-one-lobby-with-private-online-invitations)
supports two to nine independent human command seats. All seats may control one faction
against AI, or split between up to three opposing human owners, including 3v3v3.
A fourth human owner is refused before simulation construction. The seat is the command issuer,
while the faction player owns assets and supplies fog, credits and reinforcement pools.
The local faction dialog's eight rows are separate from command-seat capacity. The production
lobby's three-owner rows and side selector remain a presentation integration gap.

`TcpOptions::peers` selects the agreed transport roster size. The host occupies
index zero; a joining client's nonzero `seat` selects its stable roster position.
Zero requests the first vacant position. Setup binds those positions to stable
seat IDs and owners. Every participant selects the same count before connecting.
The default two-seat configuration retains one direct reliable stream.

For three or more seats, each client opens a listener. The host learns that
listener's port and numeric observed address, assigns its seat, and distributes
the complete endpoint roster with fresh secrets for each pair. Each client
receives only the secrets of pairs containing its own seat. `mesh_advertise` may
override the observed address; `mesh_listen` and `mesh_port` select the local
listener. Each higher-index client connects to each lower-index client. The
existing transport version/build handshake precedes a pair handshake containing
the pair's secret and both seat indices. Duplicate seats, duplicate pairs,
wrong-session pairs, malformed controls and unbound application data are refused.
Unbound attempts must provide mesh identity within the handshake deadline;
TCP keepalives do not extend it. Refused unbound streams also have a bounded drain,
so a mistaken two-seat join cannot occupy the waiting host's admission slots forever.
The setup host is trusted: it generates and can read every pair secret. A pair
that arrives before its recipient's roster stays in the bounded admission queue
until the roster supplies its secret or the handshake deadline expires.

The host opens the match only after every client confirms all its pair streams.
Application data travels directly on the sender's stream to its destination;
the host never forwards inputs, hashes or departure votes. Pair streams continue
after the host leaves. Queues and I/O work stay bounded per edge, with at most
eight edges and nine pending admissions per participant. RTT measurements remain
per link. Application polling rotates across edges to prevent a busy sender from
starving another. Cross-stream arrival order does not define command order:
setup holds early peer hash frames, and lockstep uses the canonical seat order.

Every client listener must be reachable by the other participants. This direct
adapter supplies no port mapping, NAT traversal, encryption or authenticated
account identity. Pair secrets bind streams to their assigned seats and session;
they are not encrypted account credentials. A traversal or relay-capable
transport supplies that external routing policy independently of lockstep.

A departed shared-control seat leaves its faction playing under the remaining
seats. The last seat still enters the existing faction departure policy. Real AI
attachment belongs to the adapter's `set_takeover_handler` integration seam and
the separate departed-faction AI work; mesh setup does not attach an AI itself.

`net_mesh_contracts` covers direct ordered delivery, stable seat assignments,
per-edge RTT and host-free client communication for three, four, eight and nine peers.
It runs three-, four- and nine-peer coop and two/three-owner setup/battles with mixed 1/2/4/8
simulation workers, compares every survivor tick hash, and agrees one disconnected
shared controller without removing its owner's command stream.

The viewer's Host options choose the number of humans and either coop against AI
or a split between two human owners; three-owner selection/validation remains
a presentation integration gap. Staging rows describe the owning factions;
their captions show how many human seats share each row. A discovered game's
presence advertises its human seat count, which Join uses automatically. Direct
Join asks for the same count as the host. Start waits for the whole mesh, and
each joining scene derives its local player from its assigned setup seat before
loading. A leaving shared controller exits locally while the other controllers
continue the faction's battle.
