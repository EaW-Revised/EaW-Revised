# Galactic native provider isolation

Native provider retirement belongs to the retained lifecycle owner. A retired player remains in the selected player domain, with its last committed policy/deadlines intact, but its later native services and due predicates are excluded. Retirement is applied only at a healthy joined boundary after a failed speculative phase is rolled back.

## Compatible lifecycle retirement tail

The lifecycle and native envelope versions remain unchanged. When the retirement map is empty, the existing lifecycle v2 payload is byte-identical, with no marker or count appended. A nonempty retirement map appends the following bytes after the existing player records, using the existing unsigned 64-bit little-endian word encoding:

| Field | Encoding | Validation |
| --- | --- | --- |
| Tag length | 8-byte word | Length of the following tag |
| Tag | UTF-8 bytes | Exactly `native-provider-retirement-v1` |
| Count | 8-byte word | At least one, no more than selected player count |
| Player, repeated Count times | 8-byte word | Nonzero uint32 selected owner, strictly ascending and unique |
| Reason, after each Player | 8-byte word | 1 = Result failure; 2 = deterministic exception |

Unknown tags, invalid owners/reasons/counts, truncation and trailing bytes produce a normal load refusal. No raw exception message is persisted. The canonical native SHA and record hash both include retirement. Runtime LoadNotes are derived from the validated identity and reason after restore.

## Published output limit

A process-local phase latch records story_notice entry (including its retained notice and plot notification without an opaque handler), opaque story/control/technology/battle callbacks and direct live technology, production-lock and due-hero reward writes. A subsequent fault must preserve existing refusal behaviour, count one diagnostic fault for that phase, and avoid rollback/retry. The latch and diagnostic counter are neither saved nor hashed. Transactional staging of these outputs is follow-up work; successful callback and reward order remains unchanged.

The later owned plot/reward service has its own O(1) publication latch. It marks before the opaque StoryRewardApplier and before outbound triggered/dialog reports. The validated native plot reducer has no mod callback; reward refusals remain per triggered record. Plot/reward staging is deferred to the story output follow-up. In-process custom native contributors without a rollback participant, including providers that do not opt into tracked execution writers, also preserve the existing refusal on faults. Their healthy service remains supported; installed stock usability has a participant.

## Service transaction and work bounds

The Runtime enlists the native action barrier, scheduler, lifecycle, perception/markup, goals, funding, production/freestore, live execution, private evaluator VMs, journal and stock usability before preparation. Successful order stays native preparation, Lua, native tail, ordered world publication, then owner/report commit. A deterministic attributed fault before any publication restores the complete phase, authoritatively retires that owner and its attached Lua chains, and recomputes the complete phase without that owner. Each failed attempt must retire a previously live selected provider; the selected-provider count bounds retries. World stepping remains outside this operation.

No transaction captures a full native archive. Begin retains bounded player/mod coordination headers and immutable publication pointers; entity/record undo is collected only at existing changed-row writes. World action layers use their existing copy-on-write roots. Private evaluator scheduler retention is lazy on its first actual VM call. The scheduler shares immutable graph encodings and retains host metadata; first-dirty graph encoding and per-VM metadata work are counted separately from native owner archive work. Native field/digest reconstruction and derived-cache rehydration happen on failure only. Usability rehydration belongs to the newly enlisted retry, so it cannot rebind a previously rolled-back world outside a transaction.

Executor failures, allocator/system exceptions, immediate-host failures and malformed load/save failures preserve refusal. No retry occurs after the output latch is set or once final ordered publication begins.
