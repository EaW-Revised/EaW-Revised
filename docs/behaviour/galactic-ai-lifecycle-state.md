# Galactic AI lifecycle state

WGA-56/139: the lifecycle manager implements the shared `StateContributor` interface.
Its native component is `galactic-ai-lifecycle`, version 1. The shared native-state
envelope and digest functions frame this component alongside the other owners.

The component stores both clocks, admission mode/processing/auto-resolve, selected
player identities and difficulty, initialization flags, six deadlines, active
template flags, activation order, the poll cursor, and every final policy value
with its individual priority. Trace diagnostics and query/service capabilities
are derived and excluded. Funding ledgers and retained Engine goals, RNG and plans
belong to their respective contributors.
An incomplete service with a latched failure refuses capture and hashing; it
cannot be saved as a completed boundary. Explicit validated restore clears that
failure and installs the saved boundary.

The payload begins with the length-framed domain
`eawr-galactic-ai-lifecycle-native-v1` and a configuration digest. The remaining
bytes use the existing `eawr-galactic-ai-lifecycle-v1` lifecycle hash encoding:
unsigned eight-byte little-endian words, signed priorities sign-extended to those
words, exact `Number.repr` words, length-framed byte strings, vector counts and
sorted maps. Flags are exactly zero or one. The configuration digest covers XML
bytes and closure order, registered category IDs and normalized plan definitions;
the full checkpoint asset identity additionally validates provider-owned data and
custom converter implementations.

Restore validates the shared envelope budget, component domain/configuration,
bounded counts, ordering, flags, priority range and exact end of payload before
the typed lifecycle reference checks. It installs state atomically without ledger
initialization or template reapplication. Other aggregate components are left to
their owners. `state_hash()` is the shared canonical native-envelope digest;
`lifecycle_hash()` retains its previous mutable-lifecycle digest.

`galactic_ai_lifecycle_state_contracts` exercises the actual contributor through
the shared wire codec, first-consumer continuation, owned snapshots, raw signed
zero, malformed/version/configuration rejection and aggregate composition. This
supporting fixture does not close the full installed live-opponent checkpoint
proof.
