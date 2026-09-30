# XML registry edge cases

These rules distinguish an absent include, a malformed or unreadable admitted file, registry-file failure, and inheritance failure. Retail observations apply only to the identified builds: EaW `StarWarsG.exe` SHA-256 `635d119a3ea17b2c0393fb612c627caa25e5e742d66d898ad2e14245dab03332` and FoC `StarWarsG.exe` SHA-256 `07daeeaec9d1e751383b1db3f5847b6c66f68785440a6c52c87b85f723d018a4`. The separate FoC debug build is scoped to R-07–R-09. [Retail admission](xml-registry-retail-addendum.md) and [file-service limits](xml-registry-file-service-addendum.md) narrow the EaW result.

## Interface and observable rules

The input is an identified product/build and ordered active registry. Observable results are load status, diagnostics, later definitions, name bindings and effective values. Each comparison needs a fresh catalog so prior loads cannot supply definitions.

- **R-01.** Eleven CIN GameObject includes and one CIN hardpoint include are absent from the installed EaW base archives and present in FoC. Their FoC presence does not establish EaW fallback.
- **R-02.** EaW missing-CIN recovery is conditional under R-13; no CIN-name or cinematic-mode exception is established. FoC's separate result is R-10.
- **R-03.** The accepted Remake corpus and three siblings author `Freighter_Acclamator_E` with itself as base. Repetition does not establish usable inherited values or an earlier matching binding.
- **R-04.** In the pinned LSP, a winning self-base, including an ASCII case variant, is cyclic. A weaker definition does not become its base. The project API reports the repeated ID as a cycle error; LSP tag data alongside a cycle flag does not show successful original resolution.
- **R-05.** No runtime observation establishes the freighter's effective fields or gameplay usability. Successful startup or lookup alone does not establish them.
- **R-06.** Project policy retains missing-include and cycle diagnostics. Do not repair EaW with FoC or sibling files, drop the self-base object, or guess a base.
- **R-07.** In the identified FoC debug build, a missing GameObject include emits an error and enters assertion handling. If handling returns, the include is omitted and later entries are considered; handling may instead abort or break. Registry-file read failure returns failure. This does not describe retail or the hardpoint loader.
- **R-08.** In that debug build, inheritance needs an already completed base and is attempted before new-name registration. An earlier completed same-name binding may be selected, while duplicate registration retains the earlier public binding and triggers an assertion. This is not last-definition-wins behavior.
- **R-09.** An isolated self-base starts incomplete and cannot complete inheritance merely by resolving its own name. Bounded retries and a successful loader return do not prove completion or usability. The bound is ten parse passes; after the first, a pass re-parses only the files that own types still incomplete ([audit](debug-build-audit.md#xml-registry)).
- **R-10.** In the identified FoC retail build, initial-open failure for an individual GameObject or hardpoint include omits it and continues without the debug assertion path or a CIN-specific rule. Lower file-service diagnostics remain unknown. Registry-file failure and later read/parse failure of an admitted hardpoint return failure. This is a loader-local observation, not whole-startup success.
- **R-11.** FoC retail attempts inheritance before new-name registration and requires a completed base. An earlier completed same-name object can supply one, but duplicate registration retains the earlier binding. An isolated self-base remains incomplete; retries and a successful loader return do not prove effective values.
- **R-12.** In the checked Remake candidate set, all 687 GameObject include sources were read. Exactly one contains and defines `Freighter_Acclamator_E`, at zero-based include index 494, line 389; it names itself as base. No earlier same-name candidate appears in that ordered set or in the searched installed base/FoC XML members. Retail VFS order, hidden additions and effective values remain unknown.
- **R-13.** In the inspected EaW retail branches, an initial failed file check omits the GameObject or hardpoint include and considers later entries **if** its diagnostic helper returns. No CIN-specific branch was found. Registry-file failure and later read/parse failure of an admitted hardpoint return failure. The helper and lower file service prevent an unconditional recovery or silence claim.
- **R-14.** In the inspected EaW portions, inheritance precedes new-name registration and needs a completed selected base; duplicate registration preserves an existing binding. Full base lookup, constructor completion and the admission tail were not established, so FoC's self-base result cannot be imported into EaW.

## Original synthetic cases

- **C-01, absent middle include.** With present includes before and after an absent one, the project retains a missing-include diagnostic and loads both present files. Original continuation and later availability need an identified runtime observation.
- **C-02, name and product control.** Repeat C-01 with a CIN-style name and with a valid empty file, separately on EaW and FoC. The result must not be generalized across products.
- **C-03, isolated self-base.** The pinned LSP and project report a cycle for `Probe_Cedar` based on itself or an ASCII case variant. Original effective values remain unknown.
- **C-04, displaced definition.** A weaker completed `Probe_Cedar` followed by a stronger self-base remains cyclic in the LSP and project. In retail, inherited values and the final public binding are separate observations; an earlier value cannot be inferred from a later successful lookup.
- **C-05, genuine cycle.** `Probe_Cedar` and `Probe_Elm` based on each other produce a project cycle chain Cedar → Elm → Cedar; original behavior is unknown.
- **C-06, missing versus malformed.** A missing include, malformed present XML, and a valid definition with a missing base are distinct outcomes. R-10 and R-13 do not make admitted parse failure recoverable.

## Unknowns

- **XREG G-01:** Unconditional EaW continuation, lower file-service diagnostics and whole-startup result for the twelve missing includes.
- **XREG G-02:** EaW self-base resolution and the actual freighter's effective values and usability. The checked candidate set has no earlier same-name base.
- **XREG G-03:** Any adoption of an original-engine exception would require an explicit project-contract decision; none follows from these bounded findings.
