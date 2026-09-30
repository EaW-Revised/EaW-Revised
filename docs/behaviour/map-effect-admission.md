# Map attached-effect admission snapshot

`plan_map_effects` is an engine-independent, CPU-only snapshot planner. Its
caller supplies parsed `assets::Model` proxy records, a matching `scene::Placement`
with already resolved `AttachedEffect` records, explicit particle-system
classification and requested capacity for each proxy ordinal, explicit selected
ALT/LOD values when a proxy names them, and validated bind-pose model frames.
The planner neither opens the VFS nor advances particles, animation, or rendering.
It does not connect MapMode to automatic effect spawning.

The input placement span is in scene source order. Each proxy yields one record
in its original model order, including duplicate names and failures. The stable
identity is map logical path, scene/record ordinal, and proxy ordinal; the output
also keeps the model asset source, XML model provenance, selected effect logical
path, and whether scene resolution removed an `_ALT<digits>` suffix. That suffix
resolution supplies a path only. It does not select or authorize an ALT variant.

Initial admission requires all of these facts: matching model and scene proxy
identity, a fixed placement transform, well-formed proxy ALT/LOD tags with
explicit selections, bind-pose visibility evidence, visible proxy and bone,
resolved particle-system reference, a validated proxy-bone model frame, positive
requested capacity, and room under the aggregate budget. The output's emitter
frame is the checked Q24 composition of placement world matrix and supplied
source Z-up model frame. The per-record seed uses specified byte-order FNV-1a
over caller seed, length-prefixed map path, scene ordinal, record ordinal, and
proxy ordinal; zero is remapped to one for `CpuSystem` compatibility. Capacity
is allocated atomically in source order, so the allocated sum never exceeds the
aggregate budget. No capacity or variant is inferred.

Every failure has one status and cause. Selector mismatch or authored invisible
proxy/bone is `hidden`. Missing scene reference, unresolved effect path, or
unknown particle classification is `unresolved`. Missing selected variant,
unknown visibility, malformed selector, nonparticle reference, absent bone/frame,
zero capacity, budget exhaustion, and fixed-frame overflow are `unsupported`.
The record keeps the selected path even when later admission checks fail.
Unrecognized or malformed `_ALT`/`_LOD` syntax fails closed; only uppercase
decimal tags in the supported suffix forms are accepted.
