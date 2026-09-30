# Clean-room rule for the remake

The remake must be a reimplementation, not a translation of Petroglyph's code. Three inputs
are available and all three are knowledge sources, not code sources:

1. The FoC debug build that ships with the game, studied in Ghidra: what reads a data tag,
   in which order things update, which constants are hard-coded.
2. `reference/GlyphX-Reference`: Petroglyph's reference release of engine source. Headers
   say confidential, do not distribute; no license file; README calls it documentation.
   It is studied as documentation only.
3. `shaders/petroglyph-foc`: published HLSL source, same confidential header on 103 of 122
   files, published for mod tooling. Fetch at install time from Petroglyph's URL; never
   redistribute. Translating it to another shader language on the user's own machine
   (`tools/shaders`; the tooling is committed, no translated output is) is the same use the LSP makes of it.

## How it works in practice

- Research and implementation run as separate tasks. A research task studies the inputs
  above and writes a behaviour note under `docs/behaviour/`: the observable rule in plain
  language, in the project's own words and structure, with a rule ID (for example `BP-02`)
  and test cases. Rig recordings and the owner's play knowledge are evidence too.
- An implementation task works from the note. It may consult the debug build to check a
  fact the note cites, but writes code in the project's own structure and names.
- Committed files (code, comments, docs, tests, commit messages) cite rule IDs and "the
  debug build". They never carry the original engine's class, method or field names,
  binary addresses, decompiler output, reference-source excerpts or shader source.
  `tools/cleanroom_check.py` (CTest `ci_cleanroom_check`) scans the whole tracked tree
  for engine-style names; Lua API and XML tag names that mods use are public interface
  and may stay.
- Research scripts that must match engine symbols take those names at run time from an
  ignored file (for example `tools/ghidra/FocLuaRegistrationDump.java`).
- Shader adapters are written from a behaviour note in the project's own words and
  structure. A few older adapters were adapted closely from the published `.fx` files
  (the bump-colorize, DX8 mesh, gloss-colorize, additive-offset and solid-colour legacy
  families and the viewer's shield shell); they are published as adapted translations (owner
  decision EAWR-864) and are candidates for re-derivation from behaviour notes.
- If an implementation cannot be written from the note alone, the note is incomplete.
  That is the check.

## What the behaviour note keeps

Every decision that changes an observable outcome:

- Distance measured from what to what (ship centre, hardpoint, nearest bounding point).
- Evaluation cadence (every tick or every N ticks) and processing order of units.
- Tie-breaks and hysteresis (how much better a new target must be before switching).
- Filters and priorities, and whether priority beats distance or only breaks ties.
- Occlusion checks and their semantics, if the original raycasts for blocking rather than
  for distance estimation.
- Global facts spec'd once: fixed tick rate, subsystem update order, float behaviour.

## What it discards

Everything that only changes speed or memory: loop structure, data layout, helper calls,
caching, the raycast when it is only a distance estimate. When unsure which bucket
something falls in, that is a test case, not a guess.

MIT code (`alo-viewer`, `pg-starwarsgame-lsp`) may be reused directly with attribution.
