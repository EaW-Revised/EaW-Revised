# Retail XML registry clarification

**XREG-A1** supplements the [XML registry rules](xml-registry-edge-cases.md) for the identified EaW retail build.

## Applicability

These additional static observations concern the installed EaW retail executable,
11,511,808 bytes, SHA-256
`635d119a3ea17b2c0393fb612c627caa25e5e742d66d898ad2e14245dab03332`.
They are not a claim about all releases, FoC equivalence, or observed gameplay.
No original game or editor was run for this addendum.

## Additional supported behavior

1. On the previously inspected GameObject and hardpoint paths, a negative initial
   file check omits that include and continues to the next registry entry. The
   diagnostic operation on that branch returns without emitting a diagnostic or
   terminating the process. This closes the earlier uncertainty about that
   diagnostic operation returning. It does not establish silence or recovery in
   the underlying file service, or guarantee whole-startup success.
2. An admitted hardpoint file that subsequently fails reading or parsing remains
   a different case: the previously inspected path reports failure. Initial
   absence must not be generalized to malformed XML or a disappearing admitted
   file. Registry-file failure is also separate.
3. Base lookup consults the currently registered binding for the derived lookup
   identity, or reports no match. The inspected lookup does not search a history
   of displaced definitions. Together with the earlier registration observation,
   an existing matching binding is retained when another definition is registered.
   Exact name normalization and collision behavior are not established here;
   this is not a new case-folding or duplicate-resolution specification.
4. Construction explicitly initializes inheritance as incomplete. The constructor
   subsequently processes supplied content and performs further finalization;
   those operations were not fully characterized in this pass. This initialization
   observation is not a complete specification of the object's effective fields
   or all constructor-exit state.
5. When inheritance is attempted with a missing base, or with a selected base
   still incomplete, the inspected admission operation returns without performing
   inheritance or marking the derived object complete. Finding an incomplete
   object by its own name therefore does not itself make it a completed base.
   This does not prove every later loader or gameplay operation leaves it unchanged.

## Limits for the actual Remake freighter

The accepted Remake 4.0 candidate set contains one `Freighter_Acclamator_E`,
authored with itself as base; the prior checked set has no earlier same-name
candidate. There is no source-supported basis here to substitute `Freighter_Acclamator`,
borrow a sibling version, or assign inherited freighter values to the self-base.
The supplied reference release does not expose this registry implementation,
and the permitted LSP's cycle behavior does not supply an original-runtime oracle.

The freighter's effective field values, runtime binding, availability, and ability
to instantiate remain unknown. EaW static observations alone cannot establish
the behavior of this FoC mod. A successful startup or a successful lookup would
not by itself resolve those unknowns. No compatibility waiver follows.
