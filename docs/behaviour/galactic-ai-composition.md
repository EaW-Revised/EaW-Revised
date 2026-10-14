# Galactic native AI composition

The campaign owner retains six native contributors at a completed owner barrier:
the lifecycle manager, perception, goals/plans, funding, execution, and production.
Each emits its own versioned component into the canonical native envelope
(WGA-56/139/195). Missing, extra, or incorrectly owned components refuse
composition; installing the state does not enable missing gameplay services.

Fresh creation and checkpoint reconstruction use the same factory. It receives
the actual retained parent world after world, child, and Lua reconstruction, so
producer and credit callbacks cannot bind a detached campaign copy. The factory
recreates runtime handlers. Native restoration then publishes lifecycle,
perception, goal identities, funding, execution reservations and derived routes,
and production billing identities, in that order. Funding with live goals requires
a validator against restored player/goal identities. Dead obligations remain
retained after their goal disappears. Execution's owning contributor validates
reservations and rebuilds its routes without resetting its saved deadline.

A refused or throwing contributor triggers restoration of the previous complete
envelope in the same dependency order. If rollback fails, the composed owner
refuses capture, hashing and further restoration. The checkpoint loader constructs
a fresh replacement owner before this process; a failed load keeps the running
session. Rollback does not replay gameplay service or battle callbacks.

The digest cache uses exact canonical bytes as its invalidation rule. Every hash
request captures the authoritative components and compares the encoded envelope
with the preceding request. Only changed bytes generate a new SHA-256 digest.
Policy writes, perception-query cache changes, RNG, goal/plan histories,
reservations, producer billing, battle listeners and restores are covered by
their actual encoded values. Derived matrices and diagnostic counters are excluded
by their owning codecs. This approach still serializes all retained components;
incremental owner revisions require a separate measured implementation. Digest
request and generation counters are deterministic diagnostics, never saved state.

The world hash reuses the existing per-section dirty cache. Native effects admitted
after ordinary world service refresh the affected sections before the script
bridge composes its final hash. A hash read leaves dirty flags available to the
next world service's population preparation. `EAWR_VERIFY_GALACTIC_HASH=1`, or
the test setter, independently recomputes every world section on every cached-hash
request and refuses a mismatch. This option is runtime test configuration and
does not change saved bytes or simulation decisions.

Fiscal callbacks carry the original gross aggregate after account crediting
(WGEP-112, WGA-13..15); the selected multiplier and credit cap affect the account,
not the callback's income rate. Funding reconciles its capital against the actual
credited balance. Explicit windfalls retain the actual clamped account delta
(WGA-170), including signed deductions.

Battle listeners consume the current finalized `notice.result` at committed
return (WGA-50..56/196..210). The session publishes its latest-result accessor
after that callback. Native restoration never emits an entered/returned notice.

This infrastructure's stock test loads the actual selected Normal Empire XML,
stock plan definitions and prepared Equal Footing campaign, retains positive
authored goal allocations, measures unchanged native hash requests, and checks
bounded world progression with independent hash verification on 1/2/4/8 workers.
Its lifecycle is unserviced, funding has no runtime goal reservations, and
execution/production have no active tasks. It proves the composed boundary and
its current serialization cost, not a running opponent, tactical credit transfer,
land simulation, or terminal campaign victory. Those remain explicit runtime
acceptance gaps.
