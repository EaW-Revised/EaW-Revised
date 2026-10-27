# Galactic LAN clock and notices

The LAN clock is session input. Any active human may request pause, resume, or a strategic speed through the
same sealed command stream as other orders. Input is stamped by the lockstep core and executes at the agreed
session tick; the HUD reads the published state. Waiting notices never pause or advance the simulation.

`GalacticClockPolicy::limited` applies `ClockThrottle` ((legacy EAWR-4574), the one place the numbers live). Pause and resume
spend no budget: resume is always allowed, and a pause is only debounced one session second after the same seat's
previous pause or resume (double clicks). Strategic and tactical speed changes are limited to six per seat in a
rolling window of 1,350 session ticks (45 seconds at 30 ticks per second). The refusal names the limit and the
whole session seconds until the next speed change is allowed. Refused requests do not terminate the match.
The window uses session ticks, including paused and child-battle ticks, rather than local wall time or
strategic calendar time. `unrestricted` reverses this project policy without changing single-player rules.
Tactical LAN battles use the same constants: the pause clock applies the debounce at execution, and the local
speed request window (`MatchSpeedWindow`) allows six requests per 45 session seconds (1,350 session ticks) before they are sent.
The adapter includes policy and retained per-seat request ticks in its canonical state and agreed hash: peers
must agree on the history that decides future admission, as well as on the campaign state.

Strategic speed controls are separate from the authored fast-forward rule. The existing restriction on 4x
fast-forward with multiple active humans remains in force. LAN pause submits one command and does not enqueue
an additional local speed reset. The single-player nested pause and fast-forward behaviour is unchanged.

The HUD displays the driver's waiting peers after its stall threshold and clears that line on progress. It
resolves peer and seat identities through lobby display data. Departures describe the configured faction fallback,
which may be AI, passive control, or another co-op human; names never decide control. A two-player split is
explained as separate campaigns, and the notice states that Galactic rejoin is disabled. Ended and desynchronized
matches retain their reason. Notices pass pointer input through to the galaxy; clock controls occupy their own row.

Clock action and refusal lines expire after five seconds on the presentation clock, including when the shared frontier
is stopped. An agreed unpaused state clears a paused line immediately. Driver refusal receipts remain available for
pending-input cleanup after their text expires; presentation lifetime never changes clock admission or state hashes.
