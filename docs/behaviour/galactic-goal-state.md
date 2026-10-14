# Galactic goal runtime identities and state

The retained native Engine owns galactic goal allocation. Goal IDs are local to
one player. Potential and running plan IDs share one engine-wide allocator, so a
plan-to-goal lookup cannot confuse a potential allocation with another running
plan. Released IDs are not reused. Definition indices identify immutable catalog
entries and are never reservation IDs. These are project representation choices
supporting WGA-56.

Allocation reads an initialized selected player's authored function, application
and target context. Positive desire includes the learned failure adjustments.
Ordinary proposal/exclusion/blocking gates and materialized plan-name policy
filter the allocation. The synchronized choice uses success rate plus one under
WGA-19. Allocation retains an **unselected** potential plan: it does not imply
force selection, funding admission, an executing Lua instance or a paid purchase.
The composed goal-service selector refusal remains a named integration seam.

`GoalStateContributor` contributes `galactic-goals`, component version 3, to the existing
native AI envelope. Its domain is `eawr-galactic-goals-v4`. Scalars are explicit
little-endian integer words; Fixed and Number values retain their raw words.
Strings, vectors and maps are length framed, with strictly ascending map keys.
The component retains:

- initialization, seed and synchronized RNG; frame/event/order sequences and
  next goal, target, potential/running plan, TaskForce and instance IDs;
- selected player/type, raw difficulty values, policy priorities, ordered
  functions/targets, proposal cursor/quota/sleep/maintenance, proposal/exclusion
  counts and blockers, proposed/active goals and potential selections;
- three activation/goal/plan histories, four cumulative goal/plan success tables,
  unit-type survival counts, object/pool/credit/pad
  reservations, assignments and build tasks;
- target, running plan, TaskForce and block records, instance mappings, globals,
  threat/hit inputs, pending attachments, events, orders and wallet deltas.

The caller supplies the SHA-256 of the exact selected content/XML/Lua closure.
The component also checks its parsed plan/goal/player/function configuration.
Old versions and mismatched closures are refused. Decode, reference validation
and pointer rebinding finish in private staging before publication. Saved IDs
are exact; there is no remapping or fresh initialization after load.

Restore the world/perception provider first, then Engine identities, then funding
and execution consumers. Execution validates reservation ownership against the
retained potential-plan and object-to-goal map. A known pending loss/cancellation
may refer to an absent actor; liveness is resolved by its consumer after identity
fixup. Lua instance/thread validation belongs to the composed checkpoint owner.

Worker pointers, producer caches, perception caches and diagnostic timelines are
outside this component. Perception owns its caches; execution owns derived route
rebuilding. Tactical Engine/grid persistence remains a separate contributor seam.
The installed proof covers a Normal Empire allocation turn and exact continuation
on 1/2/4/8 workers, not the full stock opponent or mixed-force execution.
