# Two-host Galactic Conquest acceptance

The GC case of `tools/rig/Invoke-LanAcceptance.ps1` stages the same viewer
artifact on two remote GPU hosts and uses their normal rig queues and private
desktops. Supply the current agent hold file; an unknown reservation state is
an error. The source and artifact commits must match.

Use `-Case galactic -Ticks 9000 -DryRun -TimeoutSeconds 1800` for the short
rehearsal, then `-Case galactic -Ticks 81000 -TimeoutSeconds 7200` for acceptance.
Both commands also require `-HostMachine`, `-JoinMachine`, `-HoldFile`,
`-ViewerArtifact`, and preferably `-OutRoot`. `-Campaign auto` chooses the first
installed multiplayer campaign; a named campaign must occur in that list.
No mod root is mounted for this stock-content qualification.

Both human seats occupy opposing factions. Unheld factions retain their agreed
AI controllers. Each local script submits ordinary pause, resume, speed and
fleet movement commands through the lockstep session. Movement follows the
installed campaign's graph towards an opposing human planet. Battle choices
and result acknowledgements use ordinary seat commands. In a child battle each
peer sends attack/approach orders for its own units through the retained tactical
feed, using its published seat binding and snapshots. No conflict or force
is injected. If the selected campaign or its ordinary orders do not produce
the required battle or auto-resolve, the run fails coverage.

Playable MP AI factions select their authored `Basic_AI` using the one globally
agreed difficulty and that player's XML difficulty mappings (GCMP-AI-01..04 in
[campaign setup](behaviour/campaign-setup.md#multiplayer-ai-project-policy)). This
is project policy, **not retail parity**: the private evidence receipt
`out/mp4481/retail-rule.md` distinguishes retail's authored campaign-pair path
from this shared-seat extension. Ambient controller absence is preserved;
unsupported playable controller inputs refuse before agreement. Per-seat AI
difficulty is a possible follow-up and is not present in the current agreement.

Each seat restores 1× and requests 2× after its pause/resume and the 45-second
LAN clock admission window. Movement starts after these clock exercises, so
both speed changes target the strategic clock. Both the stamped requests
and the published clock-change events must agree; selecting an unchanged 1×
speed cannot satisfy the shared-clock gate.

`Ticks` measures simulation ticks at 30 ticks per second, including strategic
and child play and excluding player pauses. The setup offer fixes its final
session tick at `Ticks + 3000`, allowing for pauses and transitions. Both peers
must finish the existing final-hash agreement before the viewer exits. The
extra allowance is not credited as gameplay unless the owner clocks advanced.
A rehearsal can report `DRY_RUN_PASS`, never an acceptance `PASS`.

Each peer exports `hashes.csv`, `costs.csv`, `trace.csv`, `gc.json`,
`match.eawr-replay`, a viewer report and a process receipt. The verifier requires
complete identical hash rows, equal nonempty replay bytes, a returned human
space battle, auto-resolve, movement by each seat, shared clock commands,
installed native AI activity, an agreed end and clean task cleanup. Missing
evidence or a network/checkpoint diagnostic fails the run.

Costs are wall milliseconds around the successful lockstep pump: simulation,
hash/monitor work and transport polling for that completed tick, excluding
the recorder's file writes. Wait-only pumps have no cost row. Stalls count
transitions into a completeness wait while a simulation tick is due. Latency
uses originating monotonic timestamps locally and measured clock-offset bounds
across hosts, without subtracting the configured delay.
Inputs stamped from `barrier_tick - input_delay` through `barrier_tick` that
execute on neither peer are reported as `unexecuted_tail_orders`, with a
separate count, and excluded from latency statistics. This allowance requires
equal agreed barriers and matching configured delays, and is capped at 64
orders per originating peer. Earlier missing executions, one-sided executions,
or an exceeded cap fail verification. Tail orders cannot satisfy movement or
latency coverage.

The launcher enables `--eawr-lan-acceptance` together with
`--eawr-gc-lan-acceptance <campaign>`, `--eawr-gc-lan-out <directory>` and
`--eawr-gc-lan-final-tick <tick>`. Partial GC argument sets are refused. The
qualification recorder and script exist only with this explicit opt-in.
Their counters are not world state and do not enter hashes
or replay bytes. Ordinary launches retain the default session cap and driver.
The GC acceptance lobby logs its transport phase, seat readiness, identity and
start/refusal reasons every five seconds, plus its ordinary LAN diagnostics.
Ready and Start automation each makes at most three attempts per lobby revision,
at least five seconds apart; a new revision restores its budget immediately.
Paired queue acquisition yields partial leases on a queue deadline and retries
within the overall deadline. Transport and configuration exceptions fail after
partial leases are released.
If setup cannot launch within 300 seconds, it writes a failing lobby
`report.json` and exits with status 3; this cannot earn gameplay credit.
