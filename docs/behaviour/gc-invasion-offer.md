# Prepared campaign invasion selection

The campaign return prepares surviving transport identities under WGP-88.
Preparation is separate from visibility: WGP-BS-U05 has not qualified a visible
post-space-win offer. `ObservedInvasionOffer::visible_admitted` therefore stays
false in the live producer. Synthetic presentation fixtures can exercise the
button without claiming retail evidence.

The observer publication retains the result conflict, current winner-owned
fleet selection and current direct cargo. It suppresses candidates for a surface
that is inaccessible, destroyed or currently owned by the observer. A non-allied
observer receives no selection. Allied winners can supply prepared candidates;
authority to command another player's transports is not inferred from alliance.
The producer reuses its immutable publication while the result/selection
revision, observer input generation, shared cargo identity and permission keys
remain unchanged. Quiet and unchanged paused steps allocate no offer storage
and perform no candidate gather. Ordered input changes refresh the fact.

WGP-89 acceptance uses a conflict-scoped `LiveGameOrder::accept_invasion` intent.
The simulation thread resolves retained identities again, skips missing or empty
transports, and calls the native invasion owner for every eligible fleet. It
does not substitute an ordinary fleet-landing command. Newly added transports
are excluded. Successful fleet admissions consume their prepared selection so
an immediate retry cannot duplicate them. The native owner keeps queued
requests until completion; presentation releases its unused receipt reader.
WGP-136 limits acceptance to the first 127 resolved identities across the
prepared fleet list, before current ownership filtering, with no backfill.

The native invasion journal records selected IDs with action 4 of its existing
effect kind. The pending-input checkpoint section is version 2 and retains
conflict-scoped intents independently of their unused command payload. The
galaxy checkpoint section is version 11 because saved summary candidates retain
transport identities alongside per-mode timeline state. Older project save
formats are not supported.

The summary consumes only its observer's explicit visible admission and current
accept permission. Continue remains presentation acknowledgement. Neither
action resumes an independently paused session or reapplies a battle outcome.
The existing battle-end resource, fonts and button artwork remain the visual
source; no new GUI tags are applied.
