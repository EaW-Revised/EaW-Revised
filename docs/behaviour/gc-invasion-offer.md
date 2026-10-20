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

WGP-89 acceptance is the session command `InvasionAccept` (session opcode 16), carrying only the
result's conflict. The sending seat's own presentation may hide the button, but no peer decides the
acceptance from a publication: every peer executes the command at its tick from the agreed session
state, refuses it identically (a refused command is still journaled), and replays it from the recorded
stream. The execution revalidates the retained result, resolves missing identities, skips missing or
empty transports, and calls the native invasion owner for every eligible fleet. It does not substitute
an ordinary fleet-landing command. Newly added transports are excluded. Successful fleet admissions
consume their prepared selection so an immediate retry cannot duplicate them. The native owner keeps
queued requests until completion; the command releases its unused receipt reader in the same
execution. WGP-136 limits acceptance to the first 127 resolved identities across the prepared fleet
list, before current ownership filtering, with no backfill.

The command is its own journal entry: its admission and reader release append no native invasion
effect, because replaying both the command and an effect would admit the selection twice. The AI's
own invasion admissions keep their native effects. When the accepting seat owns the open summary hold
of that conflict, the same execution ends the hold, as Continue does; any other seat leaves it open.
The pending-input checkpoint section is version 3: a deferred acceptance is an ordinary typed order
and no side metadata carries it. The galaxy checkpoint section is version 11 because saved summary
candidates retain transport identities alongside per-mode timeline state. Older project save formats
are not supported.

The summary consumes only its observer's explicit visible admission and current
accept permission. Continue remains presentation acknowledgement. Neither
action resumes an independently paused session or reapplies a battle outcome.
The existing battle-end resource, fonts and button artwork remain the visual
source; no new GUI tags are applied.
