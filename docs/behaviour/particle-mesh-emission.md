# V1 EnhancedMesh CPU emission

This presentation-only implementation follows the MIT-licensed alo-viewer revision
`9bb0053919cc5df8377610d4f91b11d956d6c2f4`. The pinned source remains in
`reference/alo-viewer/`; its notice is retained in `THIRD_PARTY_NOTICES.md`.
The random-sample surface offset below follows the debug build; it corrects
the fixed-distance offset inherited from that reference implementation.
V2 creator parameters remain metadata only.

Legacy mini `0x34` selects disabled (`0`), random vertex (`1`), random surface
(`2`), or every vertex (`3`). Any other nonzero value falls back to random
vertex and is retained in `mesh_mode_raw`. Mini `0x3c` supplies the surface
offset; its absent default is `0.5`. A malformed mode or offset, or a non-finite
offset, fails parsing.

The registry's four-argument `spawn` takes an owned `MeshBinding` containing
ordered submeshes of position-and-normal vertices, optional triangle indices,
and a separate source-Z-up affine mesh-bone frame. Passing a temporary or
destroying or changing the caller's copy is safe. The original three-argument
`spawn` remains available; it fails with `EAWR-PARTICLE-0008` for a system with
an EnhancedMesh emitter before any backend resource is created. The binding
must have at least one vertex in each submesh. Random surface additionally
requires complete triangles in every submesh. All indices, vertex components,
frame components and count bounds are checked. Finite degenerate geometry is
kept as supplied. `set_mesh_frame` validates a live handle and finite frame;
updates affect only particles emitted afterward. `set_frame` changes the
emitter's independent frame.

Each mesh particle samples its velocity property group first, and applies the
emitter basis when `local_velocity` is set. It does not sample the Shape
position group or inherit a parent particle's position or velocity. Random
vertex mode draws a submesh uniformly, then a vertex uniformly inside it.
Random surface draws a submesh and then a triangle without area weighting. Its
two float draws form `w1` in `[0,1)`, `w2` in `[0,1-w1)`, and
`w3 = 1-w1-w2`; both point and normal are interpolated. Every-vertex mode
walks vertices and then submeshes in storage order, wrapping per emitter.
Its count is total vertex count times the Shape count: truncated
`particles_per_interval` for bursts and one for continuous events. Capacity
drops consume neither a vertex cursor step nor a mesh RNG draw.

The sampled point uses the mesh-bone affine frame. The normal uses that
frame's ordinary linear basis, with no inverse transpose. For random vertex
and random surface modes, normalize the transformed normal and displace the
sampled point by the signed surface offset times the sampled particle size
times `0.5`. Particle size already means billboard half-extent (the loader
halves authored full widths); this is an additional half factor. Size includes
its gradient at the birth age and its sampled variation. The displacement
is fixed at birth and does not shrink with the later gradient. A zero normal
or zero extent adds no displacement. Every-vertex mode uses the vertex point
without this offset. These offset and size rules are observed in the debug
build's random mesh sampling and particle initialization paths. Velocity alignment
composes a Z quarter-turn, a Y turn of `pi/2 - tilt(normal)`, then a Z
azimuth turn. The complete composition preserves roll for nonaxial velocity.
The mesh creator registers as a root even when parent metadata names it as a
child; ordinary parsing still rejects duplicate and cyclic parent links.
Existing killer and modifier initialization follows creator initialization.

The project keeps xorshift32, with seed zero replaced by one. Float draws use
the high 24 bits to form a half-open unit interval; bounded integer draws use
one full xorshift32 state and multiply-high to map to `[0, upper)`. This gives
fixed-seed reproducibility within this implementation, including restart
equality, but does not claim bitwise equality with alo-viewer's host `rand()`.
Velocity-group draws precede mesh selection; random vertex draws submesh then
vertex; random surface draws submesh, triangle, `w1`, then `w2`.

EffectMode accepts an explicit host ALO logical path and named proxy. It rejects
an attachment selector at the same time, selects exactly one proxy, and chooses
the first mesh in source model order whose bone equals the proxy bone's
immediate parent. The viewer copies ordered position/normal vertices and
widens triangle indices into an owned binding. It uses no skinned vertices.
Each presentation time samples one pose: the proxy bone's `model_asset` gives
the emitter frame and the owner mesh bone's `model_asset` gives the separate
mesh frame. Both are already source Z-up, so neither is converted through the
render basis. The dry camera fit, fitted-camera replay probe and graphical
registry all receive this binding and both frame updates before advancing.


PS-14 in the [particle walk](walks/particles.md) supersedes storage-order
every-vertex selection: each admitted batch shuffles the complete bound vertex
list across submeshes, cycles that permutation for larger bursts, and selects
its admitted prefix under the emitter's independent reserve. The local seed
repeats the permutation; its random stream is a project choice. Flattened
vertex locations and permutation scratch are allocated at construction, so
steady-state batches allocate nothing. Dynamic energy belongs to PS-17.
