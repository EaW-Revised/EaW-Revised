# Shader translation policy

This is a project policy, not an original-game format claim.

- **STO-01:** The optional, case-sensitive `modinfo.json` member `"shader_translation"` accepts a JSON boolean. Missing or `true` allows translation when the contributor's metadata is valid. `false` opts out. Other types and ambiguous duplicate members block translation for that contributor and produce a malformed-policy diagnostic naming `shader_translation`.
- **STO-02:** Query the `ModRecord.metadata.modinfo` of the contributor of the selected shader bytes. An inherited parent shader uses the parent's policy; a child replacement uses the child's policy. Retail has no contributor and remains allowed. The active leaf cannot disable an unrelated contributor.
- **STO-03:** The admission owner evaluates this policy before either source or compiled translation, including before a warm cache lookup or store. Opt-out is a cause; the owner independently selects a hand adapter, omission or another supported route. The helper performs no VFS resolution, cache I/O or route selection.
- **STO-04:** The decision borrows the contributor key for the record's lifetime and identifies `shader_translation` when malformed. The activation owner retains owned provenance and full diagnostics. Malformed metadata fails closed for this contributor. A preserved `malformed_modinfo` reader warning also blocks translation: rejection of an unrelated field must not silently discard an author's opt-out. This conservative refusal includes ambiguous sibling metadata; unrelated contributors and retail remain unaffected. Report `selection/malformed_policy`, independently choose the fallback route, and show the field name in the diagnostic/note. Re-evaluate policy for each activation; a previous cache hit is not permission.
- **STO-05:** Policy and viewing diagnostic Details never grant consent to export source or generated text. Any such attachment requires an explicit separate consent action.

The CPU contracts use invented metadata and selected-contributor records. Source/compiled admission and cache wiring belong to the activation owner; these helpers do not introduce a production caller.
