# Online play for up to nine peers: GNS mesh and relay rooms

Design for the online half of the nine-slot release gate (owner answer, point 1)
(legacy EAWR-3461). Written 2026-10-14 against main `4dc96f30` and the open long-session branch,
ticket E (legacy EAWR-3661). This document proposes protocol and code changes; it changes
no code and claims no deployed connectivity; its wire IDs are allocated (section 2). The implemented
wire details stay in [the relay service note](net-relay-server.md) and
[the GNS note](gns-transport.md) once the slices below land.

Status quo: LAN supports nine command seats through the [TCP full mesh](net-mesh.md)
(tickets G and H). Online is two-peer: `GnsTransport::peer_count()` is 2 and its options
refuse other counts with 1501; the relay pairs exactly two connections per session and
consumes the code; a password/directory admission hands the room ID to the two-peer
bridge as if it were an invitation code ([lobby note](ui/lan-lobby.md#fidelity-list-gaps-this-note-does-not-close));
TURN credentials are issued per pairing. The [online lobby plan](net-online-lobby.md)
lists "The N-peer GNS roster/relay integration" as pending; this is that design.

## 1. Topology: full GNS mesh

**Recommendation: a full mesh of GNS connections, one per pair of peers (36 at nine),
each with its own ICE negotiation and its own TURN fallback. The service introduces and
signals; it never carries game frames. No host star, no service-forwarded data.**

| Option | Latency for the slowest pair | Who carries the traffic | Failure of one machine | Fit with existing code |
| --- | --- | --- | --- | --- |
| **Full mesh (chosen)** | one hop (direct or one TURN traversal) | each peer sends 8 small streams; TURN only for the pairs that need it | only that peer's links; pairs among the others continue | same contract as the LAN TCP mesh: peer index = stable roster position, host is 0, one reliable ordered stream per pair, host never forwards |
| Host star | two hops for every non-host pair, so input delay must cover two RTTs | host uplink carries 8 x 8 forwarded streams | host loss ends every link; owner decision 4 rules out migration, so the match ends | new forwarding path; lockstep would trust the host to relay other seats' sealed batches unmodified |
| Service-forwarded data | two hops via the service | project server pays all game bandwidth for every match | service outage ends every match | a bespoke data relay was already rejected in favour of coturn ([relay note](net-relay-server.md)) |

Reasons:

- **Lockstep needs every seat's sealed batch for every tick.** In a star, a tick
  completes only after the host has received and re-sent each batch, so the worst pair
  sees roughly twice the RTT and `adaptive_input_delay` picks a larger delay for everyone.
  The mesh keeps one hop per pair.
- **ICE is per pair anyway.** GNS P2P connections each run their own ICE session with
  the remote peer; there is no shared multi-party ICE. A mesh is 36 ordinary GNS
  connections, which is exactly what the adapter already knows how to make one of.
- **TURN cost scales with the pairs that fail, not with the room.** In a mesh, two peers
  on open networks talk directly even when a third needs a relay. In a star the host's
  reachability decides everyone's path; with a forwarding service everyone is relayed.
- **The lockstep, setup, desync, departure and held-seat code already runs on an
  N-peer `MessageTransport`** (TCP mesh, 3/4/8/9-peer `net_mesh_contracts`). Keeping
  the same transport semantics means `LockstepSession`, `SetupSession`, `LobbySession`,
  the departure votes and the reconnect proofs are shared unchanged; only the adapter
  and the introduction service are new.
- **Host loss.** Pair streams among the survivors continue without the host, the same
  as the TCP mesh. That is what makes "held seat, then departure, no host migration"
  (owner decision 4) workable in battle.

Semantics kept identical to the LAN mesh: `peer_count()` is the agreed roster size
(2..`max_match_seats`), `self()` is the local roster position, host is 0, `link(p)`
reports each pair, the match opens only after the whole mesh is confirmed, cross-pair
arrival order never defines command order (lockstep uses canonical seat order),
per-edge bounded queues, polling rotates across edges.

One internal difference: the TCP mesh distributes its roster once every seat has joined.
Online, the mesh is built **incrementally**: each admitted newcomer connects to every
member already present. ICE takes seconds per pair and can fail per pair, so a failing
pair must be detected when its newcomer arrives, while everyone can still be told who
could not connect, rather than at Start. The exposed contract (complete mesh before the
match opens) is unchanged.

## 2. Introduction service: room-scoped roster

The service gains a **room scope**: the signalling counterpart of a directory room.
It replaces two-peer pairing for N-peer rooms. Two-peer sessions (Kind 1..12) stay for
direct invitations until the slices below replace their callers.

### Room model (service owner thread)

```
Room
  id               128-bit, the directory room ID (one record, two views)
  capacity         2..9 (max_match_seats), fixed at publish; directory cap 64 stays a listing bound
  state            open | locked (Start) | closed
  owner            directory principal + ownership capability (unchanged from ticket I)
  slots[0..8]      vacant | reserved(deadline) | attached | held(deadline) | left
    member         identity (<= 31 printable bytes), member capability (128-bit, random),
                   connection id, TURN username, reservation source (directory, alias)
  revision         u32, bumped on every slot change, sent with each roster push
  lease            ticket E lease, renewed by any attached member
  budget           per-room operation/byte window (section 4)
```

The host (directory owner) is slot 0. The service assigns slots 1..capacity-1: the
directory's `Password join` reply gains the reserved **slot** (and the member capability);
the slot is the transport `PeerIndex`. The directory's own "seat" stays a never-reused chat
label (N3 note: slots are reused when seats empty and refill, so only the slot identifies a
signalling member). One reservation table per room serves
directory admission and alias admission, committed on the owner thread before any
reply, so the last vacant slot has exactly one winner (the existing ticket I rule,
extended to the alias path).

### Operations (proposal)

| Operation | From | Effect |
| --- | --- | --- |
| Room open | host | ownership capability + identity; creates the scope for its published room, slot 0 attached; reply carries slot 0's member capability, TURN credentials and lease |
| Room attach | guest | room ID + member capability from the directory admission (or the alias reservation) + identity; binds the reserved slot to this TLS connection; reply as above for that slot |
| Roster | service to all | revision, capacity, state, and per slot: state + identity. No addresses, names or capabilities |
| Room signal | member | destination slot + opaque GNS blob (1..65,536 bytes); allowed only between attached members; the service never parses it |
| Room forwarded | service | source slot + source identity (set by the service, never by the sender) + blob |
| Leave | member | frees the slot before Start; marks it left after Start |
| Release / kick | host | release frees a slot (failed mesh, no ban); kick also denies that identity and its admission principal for the room's lifetime (ticket C rule) |
| Lock | host | Start: no new reservations or attachments; the directory hides the room |
| Reattach | member | member capability + identity for a **held** slot after Lock: binds a new connection for in-battle repair, issues fresh TURN credentials |
| Room closed | service | typed reason; all member connections close |

Signals between two members are the per-pair ICE exchange: the dialing peer's GNS
connection object sends its blobs addressed to the remote slot; the service forwards
with the authoritative source slot. The receiving adapter checks that the GNS identity
inside the connect request equals the roster identity of that source slot and rejects a
mismatch. A member cannot signal a slot outside its room, a vacant slot or itself.

The blobs are not end-to-end encrypted: inside TLS to the service they are plain GNS
rendezvous messages. The service does not parse them, but whoever runs it can read every
pair's ICE candidates, including members' local network addresses, and their relay
addresses. It already sees each member's public address as a TLS peer. This is the same
exposure as the two-peer path today; the service must not log blobs (existing rule: no
addresses in logs). Encrypting blobs to the remote peer would need a key before the
first pair exists, and the host pair is formed through these same signals, so it is not
proposed.

Lease and renewal (ticket E) move from the pairing to the room: Expiring goes to every
attached member, any one may Renew, Renewed goes to all. Kinds 13..16 are reused
unchanged on a room member connection; they do not need new IDs.

### Join, leave and kick before Start

1. **Admission.** Password rooms: directory `Password join` (ticket I) reserves a slot
   and now also returns the slot's member capability. Public rooms: a four-character
   alias resolves to the room and reserves a slot the same way (guess limits as today).
   Password rooms publish no alias (ticket I2 decision). A reservation expires after
   30 s if not attached; expiry frees it. Against seat squatting in public rooms: at
   most two outstanding (unattached) reservations per room, reservations count toward
   the prefix's `unpaired_sessions_per_ip` (2), and a reservation that expires
   unattached counts as a failed guess for its prefix. Two outstanding reservations
   per prefix expire at most four times a minute, below the five-guess cooldown, so
   N3 gives expiries a window of their own: the third unattached expiry in a rolling
   minute starts the same 60 s cooldown.
2. **Attach.** The guest opens its room member connection with the capability, gets its
   slot, TURN credentials and the roster. Every member gets the new roster revision.
3. **Host pair first.** The newcomer connects to slot 0. Today's GNS hello is `'EAWR'`,
   protocol, build identity, frame limit and role; the codec checks magic, version
   (1502), build (1503), frame limit (1504) and role, and `accept` must carry exactly
   `u32 1` (the two-peer client index). N2 changes the pair handshake: the hello adds the
   sender's slot and the roster size (`peers`) and the GNS hello revision is bumped on purpose
   (`gns_protocol_version` = 2; the LAN/TCP protocol stays 1), so the N2 decoder tells an
   old-layout hello 1502 and never a malformed-hello error (a build from before revision 2 is
   unchanged and answers an N2 hello with its own 1518); `accept` carries the accepted slot
   instead of the constant 1, and `GnsTransport` option validation accepts
   `2..max_match_seats` peers instead of exactly 2. A roster-size or slot mismatch is
   refused before any application frame. The host cross-checks the newcomer's slot and
   identity against its own roster copy, then runs `LobbySession` admission (names,
   bans, capacity).
4. **Pair grants.** The host generates a random secret for each new pair (k, j) and sends
   it to j and k over their host streams with j's or k's slot (same trust model as the
   TCP mesh: the setup host generates and can read every pair secret).
5. **Other pairs.** The newcomer dials every present member; a member accepts a
   connection from slot k only after the host's grant for (k, j) arrives (held in a
   bounded admission queue until then, like the TCP mesh). Each pair verifies its secret
   and both slots before any application frame.
6. **Ready.** The newcomer reports all its pairs open to the host; staging shows the seat
   as connected. Start requires the room full to the host's chosen capacity with every
   slot connected, then sends Lock.

**Capacity is the human seat count the host chose, fixed at publish.** This is the LAN
rule ("Room capacity is fixed while connected; changing required human count rebuilds
admission", lobby plan): every peer's `GnsOptions::peers` and every pair hello carry the
same roster size, so it cannot change in place. A refused or departed newcomer leaves a
vacant slot; the room keeps its members and waits for another newcomer to fill it. To
play with fewer humans the host changes the human count, which closes this room and
publishes a new one (new room ID, fresh admissions), as on LAN.

Leave before Start frees the slot: the service pushes a roster without it, the host
tells every member to close the pair, and a later newcomer gets fresh secrets. A pair
between nonhost members gets up to two regrants before Start, including a newcomer's
first failed ICE attempt. Each regrant replaces only that pair's native ICE streams,
uses a fresh secret and the latest credentials already issued to each room member,
and lets the later-admitted end redial within the pair deadline. Opaque room signalling carries the immutable formation grant, and the adapter rejects
superseded requests before GNS can create a new connection from them. Failure reports
echo the failing link's grant secret so a delayed report cannot consume another attempt. The peer protocol
revision is 3; mixed revisions refuse through the hello version check. Lobby retries
never reattach the room and leave healthy streams alone. After the third failed attempt,
the later-admitted of the two (by the host's admission order, recorded from the roster
revision at which each slot attached) is refused exactly as a failing newcomer
(section 3), and its slot is released for a refill. The host notice includes both seat
numbers and the pair-unreachable diagnostic. A lost host pair
refuses that member; there is no regrant for it. Kick is
the host's `LobbySession` kick plus the service kick. Host connection loss before Lock
closes the room for everyone (owner decision 4).

### Build hash and setup agreement for N peers

Every pair hello compares the exact `core::build_identity()`, so all 36 pairs check it,
not only the host pairs. A mismatch on the host pair fails the join with 1503 and the
existing curated text. Slice N2 compares the build on every pair hello in the codec
(tested with a non-host pair); through the adapter a mismatch on a pair that does not
include the host cannot be reached, because that peer already matched the host's build on
its own host pair and every admitted seat shares it. After admission, `SetupSession` already binds the build into
`SetupOffer::identity.build` and requires every peer's Accept for the frozen digest;
that is N-peer today. The directory row's build tag stays a hint.

### Per-peer TURN credentials

One credential per **member**, issued at Attach and at Reattach, valid for the room's
hard maximum (ticket E decision F2: pairing credentials cover `max_session_lifetime`,
24 h by default) plus the allocation margin. coturn issues one HMAC username per member
(current code already makes one per peer). For Cloudflare, one generate call per member
(9 per full room) instead of one per pairing, so a kicked or departed member's
credential can be revoked on its own. The member uses the same credential for all of its
8 pair connections.

Cloudflare's per-minute cap counts revokes as well as generates. A full room costs up to
9 generates at attach, 9 revokes at its end, and one generate plus one revoke per kick,
refused newcomer or Reattach: about 20 calls over its life. The default 30 calls per
minute therefore serves roughly one to one and a half full rooms filling per minute;
operators running busy servers raise `cloudflare_turn_calls_per_minute`.

**coturn has no revocation.** REST credentials are HMAC usernames with an embedded
expiry; nothing on the coturn side withdraws one early. A kicked or refused member keeps
the right to allocate on the operator's coturn until that expiry (up to `max_session_lifetime` plus the
margin, clamped to 48 h). Re-issuing credentials on lease renewal does not shorten it: the renewal can
only hand out new credentials, and live GNS connections cannot switch to them (ticket E,
"Long sessions"), which is why pairing credentials cover the hard maximum in the first
place. The residual is quota and bandwidth, not game access (a kicked member gets no
pair secrets and cannot join the mesh), and it is bounded per username by coturn's
`user-quota` (10 allocations, section 4) and `max-bps`. Operators who want a shorter
window lower `max_session_lifetime`, which also caps match length. Accepted as a
documented residual; rotating coturn's shared secret would end every live allocation.

### IDs (allocated by the lane coordinator, 2026-10-14)

| Range | Count | Use |
| --- | --- | --- |
| Relay `Kind` 34..49 | 16 (14 used, 48..49 spare) | room open/opened, attach/attached, roster, signal, forwarded, leave, release, kick, lock, reattach, closed, ack. 1..12 base, 13..19 ticket E, 20..33 ticket I are taken. The server's request routing needs an explicit range check for these (ticket I review found a `kind >= 20` predicate). |
| `TcpMessage` 15..18 | 4 | GNS mesh controls on host streams: pair grant, member vacated, pair report (open/failed/lost), mesh ready. The existing `mesh_pair` (12) is reused for the pair handshake. GNS-only bodies; TCP refuses them. |
| Lockstep app kinds | 4 | 82 link report (63 bytes: per remote open/lost and smoothed RTT, plus the sender's active set; feeds both the pair-loss decision and adaptive timing), 83 pair-loss proposal, 84 pledge, 85 confirmation (section 3; slice N4 split the sealed decision into proposal, pledge and confirmation). 80..81 are taken by the LAN credential delivery (`lan_credentials_kind`). |
| `EAWR-NET-1778..1789` | 12 | slice N4, pair loss in battle. Emitted: 1778 you left (excluded or isolated), 1780 no majority/decider, 1781 decision without confirmation, 1782 overlapping second loss, 1783 malformed or unexpected mesh control, 1784 invalid mesh options. Declared in `mesh_errors` and not emitted by N4 (the survivors' notice, the flapping hold-down and the timing freeze are not failures): 1779, 1785, 1786. 1787 is the online match's "the service issues no room codes" (a host that can only be joined by code, such as the online test kit, fails at once on a `room_aliases=0` service; the lobby screen joins through the room list and does not require a code). 1788..1789 are spare. The staging refusals reuse 1762 (pair unreachable) and 1764 (mesh incomplete) through two new `LobbyReason` values. |
| EAWR-NET-1760..1779 | 20 | room service: unknown/closed room, capability, slot reserved/expired/full, locked, not a member, kicked, room budget, reattach refused, roster mismatch; client: pair unreachable direct and relay, pair handshake timeout, mesh incomplete at Start, pair lost in battle, repair failed, departure selected for pair loss, invalid room size. `LobbyReason` values for the new staging refusals take numbers from this range. |

## 3. Client: GnsTransport with N peers

### Shape

`GnsTransport` keeps its public class and gains a room factory:

```
host_room(std::shared_ptr<GnsRoomEndpoint>, GnsOptions)   // options.peers = room capacity
join_room(std::shared_ptr<GnsRoomEndpoint>, GnsOptions)
```

`GnsRoomEndpoint` is a new introduction interface (its own header, so it does not
collide with ticket E's edits to `gns_signalling.hpp`): `local_slot()`, `roster()`
(revision + slot states/identities), `send(slot, blob)` (may run on a GNS thread; copies
and bounds like today), `poll()` returning `{from_slot, blob}`, `turn_credentials()`,
`lease()`, `status()`, and the host's `release/kick/lock`. An in-process fixture
implements it for tests; `NetworkGnsRoom` implements it over the TLS service.

Internally the single `stream` becomes one **pair link** per remote slot, each with its
own GNS connection handle, frame decoder, inbound/outbound queues, deadlines, close
state and RTT sampler. The one-identity-per-process rule stays: one adapter, one GNS
identity, up to 8 connections. The runtime's connection map points at (adapter, slot).
Per-poll work stays bounded in total, not per edge: at most 64 data calls and 32
signalling receives per `poll`, rotated across pair links so a busy edge cannot starve
another.

### Pair state

| State | Meaning | Exit |
| --- | --- | --- |
| absent | slot vacant or not yet granted | grant arrives |
| signalling | GNS connecting, ICE exchange through room signals | native connected, or pair deadline (20 s default, includes TURN) |
| handshaking | transport hello/accept, then `mesh_pair` secret check | verified, or handshake timeout / refusal |
| open | application frames allowed | close, native timeout, refusal |
| failed | typed cause: unreachable (direct and relay), refused (build/protocol), timeout | reported to host |
| closed | ordered close drained or aborted | regrant (before Start) or repair (after Start) |

`link(p)` maps these to the existing `connecting/open/closed`. A diagnostic per pair
records only the cause and whether a relay candidate was offered, never addresses.
Direct/relayed path is shown only if GNS reports it reliably (lobby plan rule).

### Completeness before Start

The mesh is complete when every occupied slot has reported all its pairs open and the
host's own pairs are open. A later pair loss clears that seat's ready flag. Start is
refused (host only, local, new 1760..1779 reason) while incomplete. After Lock, `SetupSession`
and tick zero run over the complete mesh exactly as on LAN.

### Partial-mesh failure policy

**The newcomer is refused, not the room.** When pair (k, j) cannot connect, directly or
via TURN, after the bounded attempts and their pair deadlines, where k is the later admission:

| Who | Is told | Effect |
| --- | --- | --- |
| newcomer k | `Could not connect to <name of j>. Your network or theirs blocks a direct connection and the relay could not help.` (or `…no relay is configured on this server.`) | join attempt ends terminally; profile, room and name kept; one manual retry allowed |
| member j | notice in room chat: `<k> could not connect to you and was not admitted.` | no action; its other pairs unaffected |
| host | the same notice plus the diagnostic ID | host's `LobbySession` refuses k with the pair-unreachable reason, sends Release (not Kick: no ban) |
| everyone else | `<k> could not join.` | roster revision without k |

Reasons: the members already in the room are mutually connected; the newcomer is the
only new variable, and refusing it keeps the room playable. If several pairs fail, the
notice names up to three players. A failure on the host pair (0, k) is an ordinary join
failure (`Could not connect to this game…`, existing text). Retry after a failure is
manual after the two automatic nonhost-pair retries have been exhausted, so a blocked
pair cannot loop through the service budgets. The later-admitted of the two is then
refused with the same texts (section 2, "Join, leave and
kick before Start").

### Departures in battle

All of a seat's links closing at every survivor is the existing case: held seat if
negotiated, then the agreed departure (ticket F/G behaviour). Bounds stay as defined
(legacy EAWR-3465), with `remote_peer_count = 8` at nine peers (section 4).

**Repair over the room.** A returning seat Reattaches with its member capability, gets
fresh TURN credentials and re-dials every survivor through room signals. Survivors accept
a new connection for a held slot only in repair mode and only after the existing
nonce/HMAC proof with the pair key K(X, Z), the semantics of the authenticated
rejoin (legacy EAWR-3386) and the TCP `repair_closed_links` contract, ported. The service capability only opens the
signalling path; it never proves the seat.

**Asymmetric pair loss.** If pair (k, j) closes while both still reach everyone else, the
match stalls (k cannot complete a tick without j's batch). Both ends first try a pair
repair through the room within the existing initial grace (30 s), trying the TURN
relay path before removal. When both endpoint reports can reach the decider, both
must report failed relay repair. Across a cut the inside endpoint must report its
failed relay attempt; an unreachable outside report is never invented or forwarded. If that fails, the
choice of who leaves must not depend on what each seat happens to see: a seat cut off
from part of the room cannot receive every report, so seats computing the selection
locally could pick different victims and split the match. Only one seat decides, and
everyone else follows its announcement or leaves.

1. **Link reports.** From the first lost pair until the incident settles, every seat
   sends a link report (unsealed app kind) to every peer it still reaches, on every
   change and once a second: per remote seat, open or lost, plus its smoothed RTT.
   Reports are never forwarded.
2. **One decider.** The decider is the lowest roster index whose own pairs are all
   open. Such a seat reaches every other seat, so it receives every report and the
   whole loss set; it also receives the report of any other all-open seat, so two
   all-open seats cannot both decide. An all-open seat decides only after it holds a
   current report from every other seat, no report has changed for 2 s, and no lower
   index has reported itself all-open.
   If there is no all-open seat, the fallback is the largest reciprocal-open clique
   forming a majority of the original active set. Equal-size clique ties use the
   lexicographically lowest sorted seat list; its lowest seat decides. The decision
   carries an explicit mask of the current reports it used. A seat can use only
   reports delivered over its own open pairs, so unknown outside rows stay absent.
   Receivers defer to a larger or earlier equal-size clique they already know.
3. **Selection.** On the complete, agreed loss set the decider repeatedly removes the seat with
   fewer reachable peers among the remaining seats, ties to the higher roster index, until the remaining seats form
   a complete mesh. The decider has no lost pairs, so it always survives; the survivor
   set is a clique.
   With no all-open decider, selection is the majority clique above. With every
   reported pair repaired and the full mesh open, selection is a **no-removal
   restoration proposal**, with the lowest active seat deciding. A locally all-open
   view never permits continuation by itself; restoration uses the same pledges and
   confirmation as removal. Once complete fresh reports stay all-open for 2 s,
   restoration may be proposed before the repair grace ends, avoiding a full 30 s
   wait after a short repaired blip. Removals still wait for the agreed grace end.
4. **Announcement.** The decider sends the decision (survivor set and the loss set it
   used; the sealed-departure app kind) to every seat. A receiver accepts it only if the
   sender's latest report to it was all-open and no lower index reported all-open to it
   (or it is the validated lowest seat of the selected majority clique);
   anything else fails closed. This is a proposal: no seat leaves or closes a pair
   yet. Each active seat, including a prospective victim, pledges at most one exact
   topology snapshot per incident and echoes it to the decider. RTT samples are
   excluded. Before the agreed pledge cutoff, the decider confirms only after every
   active seat has echoed the same snapshot. At cutoff a majority of the original
   active set may confirm it, excluding every missing pledger from the survivor set;
   later pledges cannot change that result. Without a majority the decider leaves.
   The final survivor set must contain at least two seats. A fallback clique must
   still be a majority after missing pledgers are excluded; otherwise nobody confirms.
   One pledge per seat and intersecting majorities prevent competing proposals,
   including proposals with disjoint survivors, from both becoming effective.
   The first issued confirmation is frozen immediately; late or backdated echoes
   cannot change it. Opening an additional pair while agreement is pending does
   not alter the pledged snapshot or abort its surviving clique. Only a loss
   between proposed survivors invalidates it.
5. **Agreement and settle tick.** After the decider's confirmation, each survivor closes its links to the removed seats and
   runs the existing departure protocol for each of them over survivor pairs (proposal
   48 with a new pair-loss value in its u8 reason field, recovery 49, ready 50, acknowledgement 51). A pair-loss
   proposal without a matching accepted decision is refused. The **settle tick** for a
   removed seat is that protocol's agreed cutoff: the first tick missing from the union
   of the survivors' receive frontiers for that seat. Its batches before the cutoff
   count, none after; a second removal waits for the first, as today.
   The session bounds this existing departure protocol: `settling` itself owns no
   timer in the pure policy. A confirmed restoration with no removals completes the
   incident directly. Any verified authenticated exclusion stops an excluded seat,
   even if it did not pledge or held a stale all-open view.
6. **Everyone else leaves.** A seat named as removed leaves after the confirmation. A seat
   that has not pledged by the agreed cutoff (10 s after the agreed repair grace ends) leaves
   locally with the pair-loss reason. If no majority clique exists (for example
   pairs a-b and c-d lost among four seats, or a 4-4 split at eight), nobody can decide
   and the match ends. An isolated fourth seat leaves while the other three continue;
   in a 5-4 split the five continue. Unreachable outsiders leave at the shared cutoff
   and may rejoin; they cannot receive live game controls across the cut. A further pair
   loss after a decision is sent and before its departures settle also fails closed for
   everyone (the existing overlapping-departure rule).

The session supplies a common incident-relative time base and an agreed pledge
deadline covering the repair grace plus its 10 s decision window. Pledged seats allow
another bounded 10 s for the decider's confirmation to arrive; without it they leave.
Receivers add a fixed 500 ms tolerance to their give-up timers. A majority
confirmation is accepted even if the receiver's clock has not reached the decider's
cutoff: the decider alone fixes the pledge set. Policy calls clamp time to a monotonic
high-water mark, so message receipt stamps processed after a timer cannot reopen it.
The decider resends its unchanged proposal once a second until cutoff; a receiver
may refuse a stale-view proposal and accept the same proposal later after its reports
catch up. Echo only an accepted proposal, only to its decider, as a distinct pledge
control. Proposal broadcasts must never be routed as pledges. The frozen confirmation
is resent once a second through cutoff + 10 s, then resending stops.
The confirmation carries the original proposal, its pledge set and the final survivor
set, so every continuing seat excludes exactly the same missing members. Each seat may detect
the loss at a different time. Election uses the agreed grace end (pledge deadline
minus 10 s); local detection never restarts that grace or creates a private acceptance
deadline. A valid proposal may arrive during a receiver's repair grace because
both relay-failure reports already prove that repair was attempted. Local link truth
does not age out; remote report freshness and the 2 s quiet period still gate election.

Continuing therefore always requires the single decider's announcement, and every
survivor runs the same agreed departure on it, so the two sides of a partition cannot
both continue. With one lost pair among three or more seats a decider always exists and
the seat of the pair with the higher roster index leaves, which is owner question 2's case.

The removed seat may rejoin through the existing departure/rejoin flow if its links recover.
It leaves through the normal departure path (shared seats keep their
faction; the last seat of a faction goes to the departure policy) and sees `Connection
to <name> could not be restored. You have left the match.`; survivors see `<name> left:
their connection to <other> failed.` Forwarding j's batches through a third peer in
steady play is rejected: sealed batches are not signed per sender, so a forwarder could
alter another seat's commands. (The existing departure recovery already relays a
departed seat's retained batches among survivors once, inside its bounded protocol.)

### Host loss (owner decision 4: no host migration)

- **Before Lock:** host loss closes staging for everyone; the service closes the room
  when the host's member connection ends.
- **In battle:** seat 0 is held and departs like any seat; the pairs among survivors
  continue. The host's only remaining duty is adaptive timing (MP-2): with the host held
  or departed, **timing freezes** at the last agreed delay; changes already agreed for
  future ticks still apply; a returning host resumes the policy. Nothing is transferred
  or elected. The service room survives the host's connection loss after Lock (any member
  renews the lease); host-only operations (kick) are unavailable for the rest of the match.
- **Adaptive timing at N peers.** The host can measure only its own 8 links. Each peer
  sends the host its per-remote smoothed RTTs in the same link report (unsealed,
  non-authoritative, as `AdaptiveTiming::rtt_ms` already is). Reports are not
  authenticated, so one peer could claim a huge RTT and push everyone's delay to the
  lead cap. The host therefore takes, for each pair (k, j), the **smaller** of k's and
  j's report (for its own pairs, its own measurement), then the maximum over pairs. A
  single lying peer can only lower its own pairs' values, which at worst makes those two
  players stall more; raising the delay for everyone needs both ends of a pair to lie.
  The timing change itself is still proposed by the host and agreed by every peer.

### Slice N4 implementation notes

Landed in `include/eawr/net/mesh_incident.hpp`, `src/net/mesh_incident.cpp`, `LockstepSession` (`SessionOptions::mesh`), `LobbySession`
(`LobbyHooks::mesh_complete`, `lock_room`, `refuse_unreachable`) and `src/platform/net/online_match.*` + `lan_match.*`.

- **Online bridge.** The host's room source publishes a plain directory room (no bridge invitation, protocol tag
  `online-room-v1`); the host's match waits for it (`OnlineMatchOptions::hosted_room`), opens the signalling scope with
  `NetworkGnsRoom::open` and builds `GnsTransport::host_room`. A guest attaches with its directory admission
  (`OnlineRoomAdmission::slot` + `member_capability`; the room source keeps the directory connection, so the reservation outlives
  the attach) or with the four-character alias, then `GnsTransport::join_room`. The match options carry the room's capacity
  (`GnsOptions::peers`); the host's seat count must equal it.
- **Staging.** `LobbySession` admits a seat when its host pair verifies. A newcomer the mesh refuses (`seat_refused`) is refused
  with `mesh_newcomer_refusal_text` (1762) and a room notice; a closed or locked mesh refuses Start with 1764. Start calls
  `mesh_complete` first. Skirmish then polls `lock_room_progress` for the roster-lock confirmation before the committed
  broadcast; a pending reply stays frozen and a refused comparison aborts Start with its named reason. Campaigns retain
  the existing full-capacity lock hook. See LLB-18 in [the staging rules](ui/lan-lobby.md).
- **Incident driver (`MeshIncident`).** One per `LockstepSession` with three or more active seats. The incident ID is "incidents
  completed + 1"; the time base is the earliest start heard, in the sender's elapsed milliseconds, so seats that noticed the loss
  at different moments still share the grace and the pledge cutoff. A pair lost three times within two minutes is held lost for
  the rest of the incident from this end's own history (flapping hold-down). Each end proves its own relay failure: a remote
  report never turns an open local link into a failed row (no mirror), so a half-open pair whose open end still sends successfully
  removes nobody and fails closed at the agreed deadline with 1780. A send that fails on a link which still reads open is that end's
  own evidence: during an incident it is reported as a failed pair (inside the incident driver it is never a fatal service error, and
  the remaining seats still get the report), so the pair is lost at both ends and decided like any other. The session's own batch,
  departure and timing sends use the same mesh ownership rule, so their failed sends do not bypass the incident.
- **The agreed active set.** The incident runs over the seats that are not leaving by an ordinary departure. A seat whose quit
  batch arrived, whose timeout/disconnect agreement is running or that is held leaves the set at once (`LockstepSession::mesh_active`),
  not when its quit executes. Those exclusions are learned at different moments, so every link report carries the sender's active set
  (two bytes) and a seat begins or feeds an incident only from reports of exactly its own set; a seat with a different set waits, the
  sender resends every second, and the sets converge through the departure agreement itself. A running incident whose set changes
  (shrinks on a departure, grows when an agreement is abandoned) restarts its policy over the new set while nothing is proposed or
  pledged (same incident ID and time base; the report revision keeps counting), and a closed link starts an incident only after the
  receive queue drained, so a queued quit batch is read first. The closed link of a seat outside the set stays with the ordinary
  departure scan. Tests: `ordinary_quit_never_starts_an_incident` (3 and 4 seats, a lagging survivor),
  `ordinary_timeout_ends_without_an_invented_departure` (a frozen application with open links ends the running seats without
  inventing a departure), `sets_that_differ_neither_begin_nor_feed`, `staggered_restart_still_converges`. Not covered here, handed to N5: a set
  change after a proposal was made or pledged is not applied (the incident then fails closed), a quitter that crashes before its quit
  batch reached every seat leaves the seats that never saw it with a closed link they read as a pair loss; they converge through the
  ordinary departure's proposal and restart over the smaller set, and only a proposal that stays deferred behind another running
  agreement for the whole incident ends with 1780, and an ordinary departure that runs
  alongside an incident pauses its timeout through the unconfirmed repair/decision window; confirmed settlement keeps its bounded timeout.
- **Decider loss.** The policy has one decider and one pledge per seat, so nobody continues on a different survivor set. If the
  decider drops after seats pledged, nobody receives a confirmation. A pledged survivor whose own pair to the decider closes leaves
  at once (1781); a pledged victim, who does not read the survivors' pairs, gives up at the agreed cutoff plus the 10 s decision
  window (and the 500 ms tolerance) with 1781; a seat that never saw a proposal leaves at the cutoff with 1780; 1778 is only for a
  seat a confirmation named. A decider that goes silent without closing its links ends the same way at the same deadlines. Every
  wait is bounded, so no seat hangs. `mesh_session_tests` (`decider_loss_ends_the_match`) pins the codes through `LockstepSession`.
- **Relay churn (legacy EAWR-4021).** On a relay that keeps dropping pairs, a pair goes silent while still reading open, its watchdog
  closes it 15 s later and the redial takes a few seconds, so an incident easily runs the whole 40 s window. Three rules keep that
  from ending the match for everyone (`mesh_cascade_tests`: nine seats, silent pairs, a model of the transport's repair deadline):
  (1) a *restoration* removes nobody, so a pair lost while it is pending does not break it (the loss opens the next incident), and a
  seat whose own view of some pair is older than the decider's snapshot (that pair is silent towards it) still pledges it;
  a *departure* still fails closed when a further pair between its survivors is lost; (2) a seat that pledged to nothing and was
  confirmed nothing does not leave at the end of a window: while the pairs are being repaired the next window (20 s, the decision
  window twice) starts for every seat together, ten and a half seconds after the previous deadline, three windows in all
  (`MeshIncidentOptions::rounds`); a seat that pledged still leaves at its own deadline (1781), because its pledge may be
  effective; (3) the transport's repair deadline belongs to one stretch of repair and starts afresh once every pair is open again.
  A pledge that is not the exact proposal is ignored (one bad pledge cannot end the decider's incident); a restoration
  confirmation is resent once a second until the pledge window closes, like a removal.
- **Hand-off.** A confirmed decision queues `DepartureReason::pair_loss` (3) departures at the survivors; the existing protocol
  (48..51) agrees the cutoff and the quit. A pair-loss proposal without a matching confirmed decision is refused.
- **Adaptive timing.** The host's `mesh_rtt_ms` is the maximum over pairs of the smaller of the two ends' reports (its own pairs
  from its own transport); an unmeasured pair counts as 600000 ms so it can never lower the maximum. With the host held,
  excluded or departed, timing freezes at the last agreed delay (`LockstepSession::timing_frozen`).
- **Fallbacks at session level.** `mesh_session_tests` and `mesh_session_workers_tests` (1/2/4/8 sim workers, equal cutoff ticks
  and state) cover the all-open decider, a crashed seat among four (the majority clique {0, 1, 2}) and three lost pairs among five
  (clique {1, 2, 4} over {1, 3, 4} by the lowest seat list) with their exact leave codes; no majority is covered by
  `no_decider_ends_the_match`.
- **Accepted deviations from this document.** One room chat line for every member, naming the newcomer and the player it could not
  reach, replaces the per-audience refusal lines and "up to three players" (a room chat is a broadcast; per-audience lines need a
  new lobby frame, with the nine-seat texts in N6). Timing also pauses while an incident runs (the header documents it). The
  `mesh_complete` check does not read the locked flag: Lock is only issued after it. `require_short_code` stays off in the lobby
  screen (its host is joined through the room list, so no code is awaited); only a code-only host such as the online test kit sets
  it. Adaptive timing is configured by no app yet, so the per-pair minimum is reached only through tests until MP-2 turns it on.
- **Lock.** Skirmish uses roster-lock protocol 1 (room kinds 48/49): an exact service roster revision and attached-seat mask,
  including sparse members below capacity. The service refuses reservations, incomplete attaches, a changed revision or a
  different mask without changing the room. It pushes the locked roster before confirming the request; only that confirmation
  lets the host commit. Older relays reject the operation and the client names the unsupported protocol and required update.
  Existing `lock_room` remains the full-capacity campaign/test path.
- **Repair before departure.** The adapter attempts an authenticated replacement during the incident grace. A successful
  repair uses the existing restoration agreement; a failed repair waits for the same decision and departure cutoff.
- **What the minority side sees.** A seat cut off from the decider receives no confirmation. It cannot continue alone: at the
  agreed cutoff it ends with 1780 (no majority) or 1781 (a decision but no confirmation), and a seat a confirmation named ends with
  1778. Neither claims a departure for the other side. The majority's survivors see one line per removed seat,
  `<name> left: their connection to <other> failed.` (`LanView::mesh_notice`, bounded to eight lines); a confirmed removed seat sees
  `Connection to <name> could not be restored. You have left the match.`; the other codes have curated texts. The nine-seat
  battle texts are slice N6's. The viewer reaches these paths only with three or more seats, which the lobby screen does not
  offer before N6 (its two-seat limit and join paths are N6's and are untouched here).

Contracts of the follow-up (legacy EAWR-3697) in this slice: incident ID, shared incident time base, flapping-pair policy, decider loss mid-incident
(fail closed, above) and the minority-side note are done. Left: automatic decider failover (a second decider could confirm a different survivor set); wiring the GNS per-peer teardown into the viewer (the adapter supplies `close_peer`, and the viewer still needs to call it);
and the STUN list (`GnsOptions::stun_servers` stays empty because the room service does not hand out STUN URIs yet).

### Slice N5 repair contract

The I/O owner arms `GnsTransport::repair_closed_links` with the agreed incident or held-seat epoch and the locally retained
MP-5 match identity and pair keys. Repeated calls for the same epoch are idempotent; older epochs or changed keys are refused.
`poll` then requests a nonblocking `GnsRoomEndpoint::repair_attachment` on the TLS worker. The existing Reattach request takes
over an attached or held member and issues fresh provider credentials. No relay message kind or server change is required.
Existing peer data paths survive a signalling reattachment. The new credentials are installed on each new dial and accepted
connection; an unavailable provider never falls back to the original credentials.

A replacement uses deterministic pair roles (higher seat dials), including a request arriving while the other end still sees
its old link open. Every replacement proves its pair key with the MP-5 nonce/HMAC primitives in a separate GNS pair domain,
bound to both seats, match identity and epoch. Both nonce challenges must finish before the link opens or any application
frame reaches the session. The original pair grant and the room capability alone cannot authenticate it. Each pair has one
bounded attempt in the epoch, including credential refresh, ICE and proof. The transport's `repair_grace` defaults to the
agreed 30-second grace and is distinct from its 20-second per-connection `pair_timeout`. The higher seat still dials, so
lower-seat-first detection allows the higher seat's connected timeout and refresh within that full grace. Signals arriving
before the receiving core arms are retained in the bounded 32-signal buffer; arming does not discard them. Expiry produces
final failure evidence for N4. This is one deadline per epoch, shared by its pairs; it does not restart for a second pair.
The owner arms a new agreed epoch for a new incident.
`restart_links` re-dials a returning seat's surviving edges; `close_peer` retires one excluded edge without ending the room.

The session retains its own sealed tail, bounded by the input horizon and an 8 MiB budget. Pending departure/timing
controls have a separate 1 MiB queue per peer; a replacement rebuilds those controls from the existing agreement state,
including the most recently completed departure acknowledgement. Queue progress is retained across backpressure, and only
identical settled-departure replays on a replacement are ignored; contradictory input digests or cutoffs still fail. When `edge_identity` changes
after authentication, it resends that original tail in tick order before sending later batches. Receivers use the existing
identical-duplicate checks; another seat never manufactures or forwards it. The monitor's retained current and previous hash
summaries (and diagnostic rows when needed) are resent through `SessionMonitor::on_link_repair(peer, ownership)`; identical retry frames are
idempotent, while contradictory hashes still fail. The viewer's monitor wrapper must forward this hook and its ownership predicate; an unsupported monitor fails before play. If retention exceeded the byte budget, the candidate
is closed with 1794 and the existing pair-loss departure protocol resolves it. Ordinary departures and pending timing votes pause their own timeout during
the shared repair/decision window; confirmed settlement retains its ordinary timeout. Normal tick advancement waits for the incident's restoration confirmation even when repaired links have already filled
the sealed-input gap; receive, control service and ordinary departure convergence keep running. No networking work enters
the simulation.

Monitor replay retains the current and previous payload windows. One additional retired-window digest receipt per peer
validates ordered overlap: a slower sender can retry its previous window after the receiver has already agreed the sender's
current one. Identical summaries or digest-matching detail rows are harmless; changed hashes, frontiers or end flags still
fail. This receipt is overwritten on the next agreement and cleared at reconnect; it retains no older rows or payloads.

**Data for the viewer slice.** `GnsPairPhase` adds `refreshing`, `repairing` and `authenticating`; `open` still means proved.
`relay_failed` becomes true only when the attempt is exhausted. Diagnostic codes are 1790 (invalid repair input/epoch),
1791 (fresh room attachment or credentials unavailable), 1792 (pair proof), 1793 (repair deadline), and 1794 (retained tail budget).
Codes 1791..1794 are pair-diagnostic evidence for N6; the session outcome is still N4's typed result (for example 1778 for a
confirmed removed seat, or 1780/1781 when agreement cannot be completed). They are not substitutes for that session outcome.
The viewer owner supplies the already negotiated MP-5 keys/epoch, continues pumping through these states, forwards
`edge_identity` and `fail_repair` through its transport wrapper, consumes the existing mesh decision for player departures,
and calls per-peer teardown for confirmed exclusions. These are state data, not display text.

**Deployed-server alias limitation.** Alias-only admission returns a seat and capability, but no canonical room ID. The current
server's Reattach request requires the canonical ID, so that client fails promptly with 1791 and proceeds to the agreed typed
departure if it cannot use a directory admission. It never waits forever or reuses cached TURN. Directory-admitted members and
the host retain their canonical ID and can repair. Supplying a canonical ID after alias admission remains a server follow-up.

The policy retains its existing flapping hold-down, incident identity, shared clock, clique selection and fail-closed decider-loss
rule. Automatic decider failover and service-provided STUN URIs remain separate follow-ups. Cross-network Cloudflare/NAT
qualification still needs the deployed-service cases in the test plan; loopback checks cannot establish those properties.

## 4. Bounds at nine peers

### Client

| Item | Value at nine peers | Note |
| --- | --- | --- |
| Pair links per peer / per room | 8 / 36 | `max_match_seats` = 9 |
| Departure frame cap | `max(1024, (2h + 4) * 8)` with `h = sealed_input_horizon(D) = 2D + 2`: 1,024 for D <= 30, 2,112 at the MP-05 lead cap D = 64 | 1 MiB departure byte budget unchanged (legacy EAWR-3465) |
| Lockstep receive rate | 8 batches per tick = 240 frames/s | `max_frames_per_pump` 256 is ample |
| Per-edge queues | `queue_bytes` 8 MiB, `queue_frames` 4,096 (shared TCP defaults) | worst case 64 MiB inbound across 8 edges |
| GNS send buffer | `SendBufferSize` = `queue_bytes` per connection today | 8 x 8 MiB = 64 MiB worst case; the slice sets it per connection to 2 MiB (still above the 1 MiB default frame), 16 MiB total |
| Poll work | 64 data calls + 32 signalling receives per poll, total | rotated across links |
| Desync monitor | `max_retained_bytes` 16 MiB, hash exchange every 30 ticks to 8 peers | unchanged |

### Service

| Item | Default proposal | Why |
| --- | --- | --- |
| Room capacity | 2..9 for signalling rooms | the transport bound, separate from the directory's listing cap of 64 |
| Connections per member | 2 (directory + room member), or 1 if the slice moves room membership onto the directory connection | `max_connections` 128 holds 7 full rooms at two each; operators expecting more raise it (bound 4,096) |
| Live connections per IP / IPv6 /64 | default `connections_per_ip` raised from 8 to **18** (2 x `max_match_seats`) | the live cap is enforced at accept; nine members behind one home NAT (three 3-human sides in one house, or the several-processes-per-machine test) need 18 live connections at two per member, and even 9 at one. The per-prefix operation, unpaired and guess budgets still bound abuse from that prefix |
| Signals | provisional room budget: 480 operations and 2 MiB per member per minute, room = 9 x that; separate from the legacy 120 ops / 1 MiB session budget and from the per-prefix legacy budget (players can share a NAT) | GNS sends several blobs per connection while ICE settles and each member sets up 8 connections; the slice measures blobs and bytes per pair in the introduced-path test and sets the default to 4 x the measured maximum |
| Memory per room | about 10 KiB of room/member records; queues are per connection (256 KiB / 64 frames each), so the worst case at two connections per member is 18 x 256 KiB = 4.5 MiB plus TLS state | total output queue memory stays bounded by `max_connections x queue_bytes` (32 MiB at defaults) |
| Reservations | 30 s attach deadline, then freed; at most 2 outstanding per room; counted toward `unpaired_sessions_per_ip`; an expired unattached reservation counts as a failed guess | stops a reserved-but-absent guest, or one client in rolling 30 s windows, holding a public room's seats |
| Roster pushes | one snapshot per revision to each member, under 1 KiB | no deltas (lobby plan rule) |

### TURN traffic estimate (relayed nine-peer match)

Per directed pair per tick, an empty sealed batch is 25 bytes, plus 1 transport kind byte
and the adapter's 8-byte fragment header: 34 bytes of GNS payload. Assumed overheads, to
be measured in slice N2: about 40 bytes of GNS packet header and encryption tag, 28
bytes IPv4 + UDP, 4 bytes TURN ChannelData: about **106 bytes per packet, 30 packets per
second** (about 3.2 KB/s). The rest of the 3.6 KB/s below is an allowance of about
400 B/s: one hash exchange per second (every 30 ticks, about 100 B), one keepalive per
second (about 100 B on the wire) and about 200 B/s of commands in busy play. ACKs mostly
ride on the reverse stream's packets.

| Scope | Rate | Per match-hour |
| --- | --- | --- |
| One directed pair | about 3.6 KB/s (29 kbit/s) | 13 MB |
| One peer with all 8 pairs relayed | about 29 KB/s each way (230 kbit/s) | 104 MB each way |
| Whole room, all 36 pairs relayed (72 directed streams) | about 260 KB/s into TURN and the same out (2.1 Mbit/s each way) | about 0.93 GB egress |
| Same, both ends of every pair relayed (relay-to-relay) | twice the above | about 1.9 GB egress |

Real rooms relay only the pairs that fail direct ICE, so cost scales with that
fraction. With Cloudflare's pricing note quoted in the deployment guide ($0.05/GB after
the first 1,000 GB a month) (legacy EAWR-3627), a fully relayed nine-peer match-hour costs
about 5 to 10 cents beyond the free tier. The free 1,000 GB would cover roughly 1,000
such hours, but it is shared with the account's other Cloudflare Realtime use, so the
real headroom is smaller.

**Allocations, not bandwidth, are the binding coturn limit.** If GNS creates a TURN
allocation per connection whenever TURN servers are configured (expected from its
per-connection ICE sessions; slice N2 confirmed this by reading the pinned source, not by a
capture: no allocation count was measured),
a nine-peer room holds 9 x 8 = **72 allocations even when every pair is direct**. The
sample coturn config in the deployment guide (`user-quota=2`, `total-quota=128`, relay
ports 49160..49200 = 41 ports) cannot serve one full room. Slice N3 updates the guide:
`user-quota` >= 10 (8 pairs + 2 during repair), `total-quota` and the relay port range
at least 72 per concurrent full room. `max-bps=1048576` per allocation and
`bps-capacity=16777216` total are ample (about 60 fully relayed rooms by bandwidth).
A later option is "direct first, relay on retry" per pair (no TURN credentials on the
first attempt), which removes most allocations at the cost of a slower relay-only pair;
not proposed for the first slices.

**STUN.** The adapter configures TURN but no STUN server list. Whether GNS derives
server-reflexive candidates from the TURN server's responses decides how many pairs
behind ordinary NAT connect directly; slice N2 read the pinned source (not measured): GNS
makes none, so the adapter takes a STUN list (`GnsOptions::stun_servers`) and the service
hands out a STUN URI with the credentials (coturn answers STUN on the same port).

## 5. Test plan

| Layer | Fixture | Cases |
| --- | --- | --- |
| Mesh policy (no GNS) | pure state machine over fake pair links, in process | 3/4/9 peers: incremental formation, completeness, newcomer refusal for every failing (k, j), pre-Start failure of a newcomer or admitted pair (two regrants, then the later-admitted seat refused), leave/kick/refill before Lock, host-loss timing freeze, RTT per-pair minimum against a lying reporter |
| Pair-loss agreement (no GNS) | same, but each seat sees only the reports its open pairs deliver (no global view in the fixture) | **partitioned reports:** 4 seats a<b<c<d, pairs ab, ac, bc lost: d is the only all-open seat and decides; survivors {a, d}, b and c leave, a and d hold the same decision and a never computes its own; **no decider:** ab and cd lost, every seat leaves at the deadline and nobody steps past the stall tick; **two all-open seats:** five seats, ab lost, only c announces and d/e defer; **forged decision** from a seat that is not the lowest all-open seat is refused; **second loss** during the settle fails closed for everyone; all loss sets up to 3 pairs at nine seats end with every continuing seat holding the identical survivor set |
| Lockstep integration (no GNS) | fake N-peer transport + `LockstepSession`/`SetupSession`, as `net_mesh_contracts`; the fake can cut single pairs | nine-peer coop, 3v3v3 and shared-control battles with mixed 1/2/4/8 sim workers, equal survivor tick hashes; partitioned pair loss (the ab/ac/bc case) through the decision and the existing departure protocol, survivors agreeing the cutoff tick and equal hashes after it; host departure with frozen timing |
| Room service | loopback TLS `RelayServer`, single process | open/attach/roster/signal routing, source slot set by the service, forbidden destinations, leave/release/kick/lock/reattach, last-seat race between directory and alias admission, reservation expiry, per-member TURN credentials distinct, room budgets, ticket E renewal on room members, memory at max rooms |
| GNS mesh | N child processes (each its own GNS identity) + loopback room broker (in-process fixture or loopback `RelayServer`), Python launcher like `net_gns_introduced_paths` | 3, 4 and 9 processes: complete mesh, whole frames per pair, 1 MiB ordered tail, host leaves and survivors' pairs keep delivering, build mismatch on a non-host pair |
| Partial mesh | same, with a broker filter that drops signals between slots k and j | newcomer refused with the pair reason, notices at j/host/others, slot released, a second newcomer fills the slot and the room then starts; in battle a test seam closes one pair: repair, then the agreed departure, equal hashes |
| Forced relay | same, `turn_only`, with a local coturn | not on the per-PR path (owner rule 10-13: no Linux job per PR): a coordinator-approved one-off or nightly job on a Linux host with coturn installed, or a Windows job with a local coturn binary; skips with 77 when coturn is absent; checks 8 allocations per member under the new quota |
| Sim-worker equality | the lockstep integration row, plus a 4-process GNS battle with different worker counts per process | 1/2/4/8 workers |

Nine GNS processes on one CI host is a functional test, not a performance claim.
Owner policy defers performance measurement (lobby plan, decision 1). Per PR, slices run
their own rows on the Windows pool (the 3- and 4-process GNS cases and the in-process
rows); the nine-process case runs once per slice at its final head, and the forced-relay
row as above.

**Needs the deployed service across real networks** (cannot be shown on loopback):
symmetric NAT on both ends of a pair, UDP-blocked networks, real TURN allocation counts
and quota exhaustion, relayed pairs surviving lease renewal past 2 h (ticket E), the
Cloudflare provider with per-member credentials, IPv4/IPv6 mixes, service outage after
Lock, host loss in battle with relayed pairs, a nine-peer 3v3v3 across at least three
networks (several processes per machine are fine; they share that machine's NAT, which
also exercises the raised per-IP connection cap of 18). The
cross-network test kit (legacy EAWR-3659) is the vehicle; its runbook gains a nine-peer page.

## 6. Slices

Calibration as in the [lobby plan](net-online-lobby.md#calibration): UTC PR creation to
merge, nearest-rank p80 (sorted element `ceil(0.8 n)`), read 2026-10-14. The sample
window is 2026-10-01 to 2026-10-14, one day longer than the lobby plan's, because it adds
PRs merged on 10-14. These are delivery-cycle hours (review and
queue included, coding before PR creation excluded), not effort promises.

| Cohort | Merged PRs (legacy numbers) and hours | n | Median | p80 |
| --- | --- | ---: | ---: | ---: |
| Network/session | 3386 1.503, 3391 2.096, 2122 2.314, 3344 3.171, 2130 3.186, 3602 3.314, 3340 5.549, 3465 5.854, 3364 7.999, 3369 9.310 (legacy EAWR-3386, EAWR-3391, EAWR-2122, EAWR-3344, EAWR-2130, EAWR-3602, EAWR-3340, EAWR-3465, EAWR-3364, EAWR-3369) | 10 | 3.250 | 5.854 |
| Relay service | 3647 2.039, 3622 3.583, 3603 5.765, 3468 20.973 (legacy EAWR-3647, EAWR-3622, EAWR-3603, EAWR-3468) | 4 | 4.674 | 20.973 |
| Viewer/lobby UI | 3652 2.014, 3038 3.460, 3620 3.961, 3570 4.773, 3370 7.666, 2978 7.753, 2965 9.909, 2134 9.967 (legacy EAWR-3652, EAWR-3038, EAWR-3620, EAWR-3570, EAWR-3370, EAWR-2978, EAWR-2965, EAWR-2134) | 8 | 6.220 | 9.909 |

The relay cohort is small (n = 4) and its p80 is the TLS service's first PR; treat it as
wide.

| Slice | Scope and acceptance | Main files | Depends on / conflicts | Cohort: median / p80 h |
| --- | --- | --- | --- | --- |
| **N1** room interface and mesh policy | `GnsRoomEndpoint` + in-process room fixture; pure mesh policy (formation, completeness, newcomer refusal, asymmetric-loss selection, host-loss timing freeze) with the first two test-plan rows | new `include/eawr/net/gns_room.hpp`, `src/net/gns_room_fixture.cpp`, `include/eawr/net/mesh_policy.hpp`, `src/net/mesh_policy.cpp`, `tests/net/mesh_policy_tests.cpp`; shared `src/net/sources.cmake`, `tests/net/CMakeLists.txt` | none on code; ticket E also adds lines to `tests/net/CMakeLists.txt` (additive, the second to merge rebases). Can start now | network/session: 3.250 / 5.854 |
| **N2** GNS N-peer adapter | pair links, room factories, pair grants and reports (`TcpMessage` 15..18), completeness, partial-mesh reports, per-connection send buffer, signal counts and TURN-allocation/STUN findings measured; GNS mesh + partial-mesh rows with the in-process broker | `include/eawr/net/gns_transport.hpp`, `src/net/gns_transport_signalling.cpp` (option validation `2..max_match_seats`), `src/net/gns_transport_impl.cpp` (hello with slot and roster size, `accept` with the accepted slot), `src/net/gns_transport_formation.cpp` (room formation and signalling), `src/net/gns_frame_codec.*`, `tests/net/gns_frame_codec_tests.cpp` and `tests/net/gns_transport_tests.cpp` (new hello/accept layouts and mismatches), `tests/net/gns_mesh_tests.cpp`, `tests/net/gns_mesh_processes.py`, `docs/gns-transport.md` | N1. Touches `docs/gns-transport.md` after E's "Long sessions" section; additive | network/session: 3.250 / 5.854 |
| **N3** room service + native client | room scope (Kind 34..49), shared reservation table and member capability in the directory admission reply, alias reservation, per-member TURN credentials (coturn and Cloudflare), room lease on E's renewal, room budgets; `NetworkGnsRoom`; deployment guide quotas | `src/net/relay_room.{hpp,cpp}` (new, keeps `relay_server.cpp` edits to dispatch hooks), `relay_protocol.hpp`, `relay_server.cpp`, `relay_directory.cpp`, `relay_client.cpp`, `include/eawr/net/relay_server.hpp`, `network_introduction.hpp`, `src/net/relay_sources.cmake`, `tests/net/relay_room_tests.cpp`, `docs/net-relay-server.md`, `docs/deploy/relay-server.md` | N1 interface; **after ticket E merges** (same files: `relay_server.cpp`, `relay_client.cpp`, `relay_protocol.hpp`, both docs). Parallel with N2 | relay service: 4.674 / 20.973 |
| **N4** session integration | online bridge creates a room endpoint instead of a two-peer pairing; `LobbySession` pair-unreachable refusal and notices, Start gated on completeness + Lock; link reports, the single-decider pair-loss decision and its hand-off to the existing departure protocol; per-pair RTT minimum at the host; timing freeze | `src/platform/net/online_match.*`, `include/eawr/net/lobby_session.hpp`, `src/net/lobby_session.cpp`, shared `include/eawr/net/lockstep_session.hpp` + `src/net/lockstep_session_mesh.cpp`, `tests/net/viewer_online_tests.cpp`, lockstep/departure tests | N2 + N3; ticket E touches `lan_match.*` (merge after E) | network/session: 3.250 / 5.854 |
| **N5** in-battle repair over the room | Reattach, re-dial survivors, nonce/HMAC proof per pair before data, refreshed TURN credentials for repaired connections, asymmetric pair repair before the departure fallback | `src/net/gns_transport_mesh.cpp` (repair mode), `include/eawr/net/reconnect.hpp` if a seam is needed, reconnect and GNS mesh tests | N2 + N3 + N4; reuses the MP-5 held-seat core | network/session: 3.250 / 5.854 |
| **N6** nine-seat online in the viewer | lift the two-seat online limits (seat chooser `seat_limit`, `direct_seats = 2` in the join paths), pair-failure texts, staging connected state per seat, nine-peer page in the cross-network kit runbook; GPU case for a nine-seat online staging fixture | `apps/viewer/src/lan_lobby_screen.cpp`, `lan_match.*`, `tests/net/viewer_lan_tests.cpp`, `tests/presentation/...` case, `docs/ui/lan-lobby.md` | N4 (N5 for the recovery texts) | viewer/lobby UI: 6.220 / 9.909 |

Totals, adding each slice's cohort numbers with no multiplier: medians 23.894 h, p80s
54.298 h. Critical path N1, then N3 (the longer of N2/N3 when E has merged), N4, N5, N6
(N6's recovery texts need N5): medians 3.250 + 4.674 + 3.250 + 3.250 + 6.220 =
**20.644 h**, p80s 5.854 + 20.973 + 5.854 + 5.854 + 9.909 = **48.444 h**. The cross-network qualification (section 5, last paragraph) is
outside these PR-cycle measurements.

## Owner decisions

Decided 2026-10-14 (legacy EAWR-3669):

1. **A newcomer who cannot reach one player.** Refuse the newcomer with a curated
   message naming that player; the room keeps the seat open for a refill.
2. **Asymmetric pair loss in battle.** Try relay repair first. After repair fails,
   remove the player with fewer reachable peers, ties to the later seat, using the
   single decider and agreement protocol above. Tell every peer the reason; the
   removed player may rejoin if its links recover. With no all-open decider,
   the largest connected majority clique continues; without a majority clique,
   everyone leaves at the deadline.

### Unconfirmed application timeout

A required input batch or hash voter that is still missing after the 30-second completeness deadline ends the local match as **Connection lost: the match could not continue**, with no winner. Every still-running peer reaches its own bounded loss deadline. The viewer names the missing peer and diagnostic reason, logs the loss, and saves the completed replay prefix with a companion `.connection-loss.json` reason record; the binary replay format is unchanged. A received quit, a confirmed mesh membership decision, or a closed-stream departure backed by received matching votes from multiple survivors retains its normal departure result. A lone survivor cannot confirm the other peer departed just by voting for itself. The retail observations MP-34 and MP-36 allow timeout-triggered quit processing; the deliberate project deviation is to withhold a winner when membership was not confirmed, so two isolated peers cannot each award themselves the match. Application silence on an otherwise open transport is not a mesh pair-loss report: feeding that evidence into recovery is a separate follow-up.

This application-timeout rule also applies to three or more seats; it does not start a mesh membership incident. An already agreed reconnect policy still holds a closed-stream seat for its existing grace, progress and hard-cap deadlines before an unconfirmed lone-survivor loss ends neutrally. Confirmed mesh failures retain their specific agreement-failure messages. Loss recordings publish complete temporary files by rename in the log directory: the companion is published first and the final replay is the pair's commit marker.


### TCP capacity staging

Skirmish LAN rooms opt into capacity staging: the verified host hello carries the
room capacity and mode, so a direct join or a browser vacancy count cannot select
a different native mesh size. Verified host links carry staging snapshots before
vacant seats connect. Start compares the admitted native identities and mask with
attached streams and pending joins, freezes that sparse roster, and distributes
pair secrets only for its active guests. Live tick-zero publication waits for the
native readiness barrier, including flushing the host Open markers before a
synchronous scene handover. A lost guest during that barrier fails with
EAWR-NET-1301 and cannot produce a half-committed match; late joins get
EAWR-NET-1305. Incompatible lobby bootstrap formats get EAWR-NET-1306.

This mode is opt-in. Legacy fixed-roster TCP and campaign setup retain their
original full-mesh barrier and recovery route; the lobby bootstrap negotiates that
host mode before admission. Map selection checks each occupied human/AI row before
repairing defaults: rows 1 and 7 require a map exposing row 7, even with only two
humans, and a refused map never renumbers either player.
