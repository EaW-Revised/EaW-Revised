# Missing material effects

| Rule | Behaviour | Inputs | Evidence |
| --- | --- | --- | --- |
| ALDEFAULT-01 | A model material whose effect file is absent uses an opaque green built-in placeholder. It does not inherit an ordinary textured material, disappear, or special-case the effect name. Authored mesh and bone visibility still apply. | The material's effect reference, effective mounted source/compiled effect files, geometry and its ordinary visibility, transform and shadow paths. The placeholder samples no material textures and does not use authored material colour or alpha. | Debug build: effect lookup, embedded placeholder, initial device-resource restoration and ordinary material submission. Stock close-ups and an independent large-hull missing-file control, described below. |

Effect lookup removes the filename extension while retaining directory
components, then checks source (`.fx`) and compiled (`.fxo`) forms at the supplied
path and in the effect search directory. A present effect outside our supported
adapters remains unsupported; file presence does not qualify its rendering.

The debug build creates an embedded placeholder when lookup fails. Initial
device setup then restores resources and selects its usable technique. Looking
only at placeholder construction incorrectly suggests zero draw passes: that
is an intermediate state, not normal rendering. The placeholder supplies green
material values, disables blending and higher texture stages, and selects
diffuse colour without sampling a texture. Ordinary geometry and shadow
coverage remain visible; the captured hull has bright green lit areas and
darker green shaded areas.

The renderer-owned `eawr_missing_effect` selector implements the green material
after VFS lookup establishes absence. It is not an alias for any authored
shader name. Scene evidence keeps the authored effect reference and marks
`effect_missing`. No unused texture is resolved and no authored material binding
is forwarded to the placeholder. A mod that supplies either effect file leaves
this route and requires its own supported adapter. Low-level legacy admission
continues to reject an unknown authored selector because it has no mounted-file
access with which to establish absence.

The placeholder does not sample the fog-of-war texture. Consumers retain their
ordinary whole-object visibility gate; the strict fog-stub composition does not
declare this texture-free surface as an attenuating material consumer. The
existing scene shadow receiver supplies shadow modulation; this rule does not
re-derive the project's shadow approximation.

The stock Vengeance model is a weak pixel witness: its `engines` missing-effect
mesh is hidden, and its visible `engine2` mesh has nearly zero area. The stock
mining facility's `downhandle` has a visible bind bone chain, but a close-up of
the complete model alone cannot distinguish occluded helper art from a
missing-effect rule. Neither stock close-up shows a large green surface.

An independent private control changes exactly one equal-length effect-name
reference on the stock Vengeance model's large hull, preserving all other ALO
bytes. The invented effect is absent, and the hull visibly becomes green while
the other materials retain their ordinary appearance. The control uses a
private XML alias, leaving stock definitions and installed files untouched.

Evidence, captured with the debug build, Highest graphics, fog revealed and the
Coruscant environment pinned to `Environment 1`:

- `out/sk-3825/retail-final/vengeance_frigate_close.png` and its sidecar.
- `out/sk-3825/retail-final/empire_mineral_extractor_close.png` and its sidecar.
- `out/sk-3825/retail-control-qualified/large_missing_effect.png` and its sidecar.
- `out/sk-3825/retail-control-qualified/private-fixture.json`: original model
  hash, equal-length replacement and private fixture hashes. No asset bytes or
  captures are committed.

The covering GPU case checks stock surface routing, a large-hull green control,
and the modded Coruscant view. It also preserves the existing omission control
by mounting an invented present-but-unsupported effect file. These checks do
not claim pixel equivalence for the project's complete lighting or shadow system.
