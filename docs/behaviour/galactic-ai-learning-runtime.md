# Retained AI learning

The learning inventory follows [WGA-211..225](walks/gc-ai.md#learning-outcomes-retained-history-and-later-decisions).
Activation records admission. A completed or explicitly abandoned associated plan
records its retained Boolean result once, after successful task cleanup and before
detaching its goal and removing Lua. An unwritten result is false. A battle winner,
an admitted native action and a completed production task do not independently
become successful plan outcomes (WGA-212/213/215).

Four cumulative tables retain goal and plan successes/attempts, globally and per
target. Three recent tables retain goal outcomes, plan outcomes and activations.
The eighth table stores supplied unit-type survival observations. Zero tracking
duration omits recent insertion; positive duration uses the current synchronization
frame plus truncated single-precision duration times logical FPS; negative duration
stores −1 (WGA-214/215/221). Counters and expiries retain the project's signed 64-bit
representation; this is a project format choice, not a claim about the debug
build's signed 32-bit range or its unsourced overflow behavior (WGA-222).

Learning sweeps plan outcomes, goal outcomes, then activations. Each vector loses
the prefix before the first positive expiry strictly beyond the current frame;
the whole remaining suffix survives. A negative-only vector therefore clears,
while a negative expiry after a future positive entry survives that sweep. Empty
keys remain and cumulative counters do not decay (WGA-216).

The shared player archive includes all eight tables. The galactic component uses
`eawr-galactic-goals-v4` and payload version 14; the tactical archive uses
`eawr-tactical-ai-v10`. Restore checks the exact configuration/content closure and
retains target identities, results, order, expiries, counters and per-player synchronized RNG (GFS-A24).
No outcome or fresh expiry is manufactured by restore (WGA-223).

The battle adapter takes `GalacticAiServices::battle` notices directly. It validates
the current finalized return result and passes the notice's exact world/result to
the delivery callback. Space entry qualifies a retained movement actor against
the conflict's original force identity or parent/carrier with matching ownership;
end requires the saved conflict location. A caller-supplied predicate checks the
saved listener/block subscription; a finished flag alone cannot unsubscribe it
because conflict begin clears that flag (WGA-202). A matching destination alone cannot
subscribe a request. It neither services the frozen parent nor replaces member
loss with fleet loss, nor grants casualty-price credits (WGA-202/205/207/209).

The retained movement adapter checks the saved request/plan/TaskForce/block
association before delivering `Space_Conflict_Begin` or `Space_Conflict_End`.
It selects the TaskForce-named handler or the default handler, prepends the
TaskForce handle, and skips a missing or finished script/thread. Callback events
remain in the native engine archive until the next admitted parent barrier
assigns their scheduler tick and sequence. Delivery does not pump the parent or
drain pending attachments (WGA-202/204/209).

Campaign tactical children receive a reconstructed `Evaluate_In_Galactic_Context`
closure holding the retained parent manager, world and conflict planet. It reads
that parent's equation context without admitting a parent service. An absent
parent or equation produces no numeric value; standalone tactical setups retain
their existing binding set (WGA-80/81/209).

Remaining seams:

- WGA-65/80/81 child-cache publication: child equation reads respect ordinary
  synchronized-time expiry. A missing or expired getter computes its current
  value and discards cache/visibility publication, leaving the frozen parent's
  saved perception bytes and revisions unchanged. FoC would also refresh that
  parent cache; this implementation deliberately omits that write. The visible
  value follows the ordinary lookup rule for each read. Restricted runtime
  reconstruction uses archived cache values regardless of expiry instead.
- WGA-204: the scheduler has no synchronous handler-only entry point. The first
  adapter proof pins deferred saved-event delivery at the next admitted
  scheduler service, with host pacing preventing a coroutine pump. This is a
  timing difference from the sourced immediate handlers.
- WGA-202/203: campaign tactical entry is later than the moving actor's actual
  world-conflict-begin signal. Ground action completion and auto-resolve begin
  remain world-listener responsibilities. The adapter does not invent these
  signals from a generic battle return.
- WGA-205/206/217: the deletion helper removes all five target-keyed tables and
  preserves global totals/null-target rows, but still requires a real
  delete-pending producer. Ownership change and periodic target polling are not
  substitutes for that callback.
- WGA-221: the composed selector reads the galactic player's saved survival rate
  through the content-supplied type identity. Qualified survival observations and
  the installed ground-company constituent mapping remain producer/content
  integration; completed plan results do not create survival observations.
- Installed post-return completion, first resumed learning readers and the next
  learned weighted draw await the live stock selector and retained action/block
  binding pieces. Synthetic continuation and definition-loading proofs do not
  constitute that installed gameplay acceptance.
