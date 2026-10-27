# Material resource selection

This note answers Q19 and Q20 of the [Remake walk queue](../research/remake-walk-queue.md).
It qualifies cold effect acquisition and the name-based 2D texture loader in the
debug build. It does not establish shader arithmetic, device compatibility,
cache invalidation, or the exact pixels produced by a failed texture decode.
Binary details remain in private walk K evidence.

The input is the final material effect or texture reference. These readers do
not overlay XML definitions or merge resource files: any authoring inheritance
has already supplied the reference. Each candidate uses ordinary file-service
reads, with the loose/archive precedence of
[ML-04 through ML-12](walks/mod-loading.md). Candidate order and mounted-file
precedence are separate decisions.

| Rule | Selection or continuation | Evidence |
|---|---|---|
| RGC-120 | Effect acquisition uppercases its identity and removes every dot suffix in the leaf, stopping at a directory separator and preserving the directory portion. It tries that stem with `.fx` first, then `.fxo` only if the first extension's loader returns no effect. For each extension it first opens the supplied path, then concatenates each registered effect directory, the retained stem and that extension, in registration order. Thus an authored `.fxo` suffix does not force compiled-file priority. | Debug build: effect identity normalization, outer loader and inner path loop; concatenation arguments checked in the instruction listing. |
| RGC-121 | Within one effect extension, the first successful file open ends path search. Failure to load that selected file does not resume later directories for the same extension. A null result from that extension permits the next extension; if acquisition ultimately returns no effect, the material receives the shared missing-effect resource. Its normally restored green rendering is already qualified by [ALDEFAULT-01](missing-material-effect.md). | Debug build: inner loader's first-open break, resource admission and outer missing-resource substitution; existing ALDEFAULT-01 captures. |
| RGC-122 | A cold texture lookup searches the authored name before constructing a DDS retry. The retry is made only after lookup misses, using the existing dot-token operation followed by `.dds`; it is not a last-extension replacement. For ordinary names, `panel.detail.tga` therefore retries `panel.dds`. An extensionless `panel` tries `panel` and then `panel.dds`, without inventing `panel.tga`. Each candidate first uses the supplied path, then registered texture directories in order. The directory retry appends the portion after the last backslash, or the whole candidate when there is none. | Debug build: name-based texture loader and subordinate finder; candidate construction, argument initialization and backslash handling checked in the instruction listing. Extends ML-40/41 at this acquisition boundary. |
| RGC-123 | Once a texture candidate is found, its whole file buffer is passed to 2D texture creation. A decode/device-creation error does not return to the filename finder, try DDS/TGA siblings, or select the optional missing-file default. The native texture pointer starts null; the finalizer reports a failed result, releases buffered load data and returns the result. Both constructor continuation and the deferred finalization callback ignore that result and retain the resource wrapper. The texture accessor returns its native pointer directly. | Debug build: selected-name constructor, file buffering, 2D finalizer, result reporter, callback and native-texture accessor; corroborates the decoder walk (legacy EAWR-1710). |
| RGC-124 | An effect creation error is diagnosed and an embedded missing effect is attempted inside the selected resource. That resource's loader ultimately returns technique-selection success/failure, rather than the original creation result. Consequently a corrupt `.fx` file does not by itself establish that `.fxo` is tried: RGC-120's retry depends on the outer loader receiving no effect. Failure to create the embedded missing effect has an explicit debug break; uninterrupted continuation past that break is not qualified here. | Debug build: effect creation, embedded substitution, technique-result return and inner resource admission. |

## Q19: compiled variants and absent effects

**Deviation (RGC-120).** Retail tries `.fx` first. The remake uses only compiled
`.fxo` files and ignores shipped `.fx` source by owner decision `Q-L2` (legal).
Results can differ when a mod's source and compiled effect disagree.

The loader first normalizes the reference, then checks its existing effect
identity before acquisition. A cold request for `sample.fxo` and one for
`sample.fx` therefore share `SAMPLE` as the identity. A directory component is
retained; dotted directory names are not removed by this normalization.
Registered effect paths are appended in order, without deduplication (ML-13).
The path loop concatenates those supplied strings; it does not insert a new
separator independently.

With both usable forms present, source wins even when the model names `.fxo`.
With only a compiled form, the source search misses and the compiled resource
is selected. With neither form, ALDEFAULT-01 supplies the green placeholder;
mesh omission is not the absent-file rule. A present effect with no validated
technique remains a separate case ([CEM-04](compiled-environment-materials.md)).
No new assertion policy is needed for an ordinary file miss. Effect load errors
and the embedded-resource debug-break edge follow RGC-124.

The decided implementation seams are `src/scene/scene_assets.cpp`
(`missing_material_effect`) and `src/presentation/godot/renderer_materials.cpp`:
absence detection must search both forms, while future effect execution needs
RGC-120's actual selected resource and RGC-121/124's load-result distinction.
An existence query alone cannot establish a successful effect load. This note
does not change the owner's separate compiled-only translation policy or the
supported hand-adapter families (legacy EAWR-3824).

Discriminating synthetic cases for an implementation or retail probe:

| Case | Required observation |
|---|---|
| Authored `sample.fxo`; distinct usable `sample.fx` and `sample.fxo` | Source resource selected. |
| Authored `sample.fx`; only usable `sample.fxo` | Compiled resource selected after all source-path misses. |
| First registered directory has only `.fxo`; second has usable `.fx` | Second directory's source selected: extension loop encloses directory loop. |
| Supplied-path `.fx` and a registered-directory `.fx` both usable | Supplied-path source selected. |
| Neither form exists, with an otherwise visible large mesh | Green missing-effect surface; use ALDEFAULT-01's existing qualified control. |
| First directory's `.fx` fails its resource loader; second has usable `.fx` | No retry in the second directory for `.fx`; next extension is conditional on the resource return. |

These are discriminants derived from the inspected branches, not new capture
claims. Installed compiled-effect examples and the existing missing-effect
capture were recovered from the shader/texture workers' private evidence;
no retail files were copied into the repository.

## Q20: DDS/TGA collisions and failed decode

There is no unconditional DDS preference. With both `panel.tga` and `panel.dds`
available, an authored TGA name selects TGA and an authored DDS name selects
DDS. The first candidate exhausts its directory search before a DDS retry is
constructed. A weaker layer's authored-name match can therefore win before a
stronger layer's different DDS candidate is considered. The effective winner
for each exact path still follows the file service, not a new layer policy.

On final lookup miss, the loader warns and returns the lazily shared default
only when its caller requested fallback; otherwise it returns no texture
(ML-40). RGC-123 is different: a found but undecodable file retains the resource
wrapper and supplies no successful native texture. The inspected reporter logs
the failure without an assertion or scene-refusal branch. There is no automatic
encoding retry or missing-file default selection. This does not prove how
every shader renders an unbound sampler, or guarantee that every later consumer
accepts it. Those pixel/consumer questions remain outside this filename walk.
Constructor assertions concern unexpected pending load data; the finalizer's
assertion concerns the device thread, not an ordinary decoder error.

The decided implementation seams are `src/scene/scene_assets.cpp` (`probe` and
the material texture bindings) and `src/assets/texture.cpp` (`load_texture`).
The unified texture policy (legacy EAWR-1482) should provide authored-name/DDS acquisition without inventing an
extensionless TGA candidate or swallowing decode errors as misses. The exact
path decoder remains responsible for bytes, including format sniffing, under
the decoder work (legacy EAWR-1710). Cache identity and invalidation remain ML-41's separate boundary.

| Discriminating synthetic case | Required observation |
|---|---|
| Authored `panel.tga`, valid TGA and DDS both present | Select TGA only. |
| Authored `panel.dds`, valid TGA and DDS both present | Select DDS only. |
| Authored `panel`, only TGA and DDS present | Try `panel`, then DDS; do not try TGA. |
| Authored `panel`, exact extensionless bytes also present | Exact name wins before DDS. |
| Authored TGA absent, valid DDS present | DDS selected after authored-name search misses. |
| Authored TGA corrupt, valid DDS present | TGA creation fails; no DDS lookup and no optional-default substitution. |
| Authored DDS corrupt, valid TGA present | DDS creation fails; no TGA lookup. |
| Both candidate lookups miss, fallback enabled/disabled | Warning plus shared default/no resource respectively. |
| Authored `panel.detail.tga` absent, both `panel.dds` and `panel.detail.dds` present | Dot-token retry selects `panel.dds`. |
| Authored `art.v1/panel.tga` absent | Dot-token operation acts on the complete request: DDS retry is `art.dds`, not a path-aware leaf-stem rewrite. |

These expected request sequences describe cold loader behavior established by
static evidence. They were not executed as new retail captures in this walk.
