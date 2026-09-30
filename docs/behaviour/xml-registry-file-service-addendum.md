# XML registry initial file-service boundary

**XREG-A2** applies to the identified EaW retail build in [XML registry edge cases](xml-registry-edge-cases.md) and supplements [XREG-A1](xml-registry-retail-addendum.md).

On the examined hardpoint path, a failed initial candidate can lead to further candidates and a mounted-file lookup. If those attempts return failure normally and cleanup completes, the outer file-service layers return failure to the registry caller; the registry's omission and continuation rule then applies. No diagnostic or termination tied solely to an ordinary unavailable-file result was found in the completed outer layers.

The deeper mounted lookup and cleanup paths remain unresolved. The file service also changes working state and calls shared operations; neither harmless process-wide effects nor unconditional safe return is established. The GameObject file-service context was not independently completed. Thus an initial miss cannot be called silent, harmless or universally recoverable.

An initial admission miss is distinct from registry-file failure, malformed included XML, and a later read/parse failure of an admitted file. An omitted include contributes no definitions. The twelve unavailable EaW include outcomes and their diagnostics remain; no CIN-name exception or loader change follows from this note.
