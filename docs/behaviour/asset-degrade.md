# A refused animation or model never aborts a start (ASD)

| Rule | Behaviour | Evidence |
|---|---|---|
| ASD-01 | A model or animation file the file system holds but the loaders refuse (empty file, no root group, cut short, a clip that does not bind to its model) is a degradation. The object keeps its model and plays no clip from that file; the caller never fails the campaign, battle or environment start. The parsers keep refusing the file. Each refused asset on the start-path sites below yields one class C load note (`EAWR-LOAD-1239`: file, layer that supplied it, asset kind, loader diagnostic code) and the degradation ledger counts it under the mod of that layer. A model that cannot be loaded leaves the object undrawn with the same note. | Debug build: the model-and-animations loader looks each clip up by name, loads the file when it is not cached, and adds the clip to the model only when a clip object exists; a failed load just ends that clip type's search, nothing is reported as an error. Debug build: the model's set-animation step compares the clip's bone count with the skeleton's and, on a mismatch, clears the animation instead of playing it. A renderable that fails to create returns no model object and the caller carries on. |
| ASD-02 | Not walked: whether retail's chunk reader accepts or refuses a 0-byte or rootless animation file. Both outcomes end in "no clip for that name" (ASD-01), so the remake does not depend on it. | Unverified; no capture. |

Sites that apply ASD-01 and publish the note: the galaxy scene's per-object idle clip, its bind to each surface and its
opt-in zoom probe, the battle start's capture-point neutral clip, the environment view's idle clip. Those four share one
helper that keeps a note once per file, kind and stage.

Refusals that already degrade without a note (follow-up, out of scope here): the map-mode placement clips, the
space-populate idle and composed clips, the loading-screen planet clip, and the pause shell's clip. The simulation's
loaders (walk/attack clip frame counts) already ignore a clip that does not load.
