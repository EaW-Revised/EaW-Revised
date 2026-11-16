# Asset-name resolution

How an authored asset name (from XML, Lua or model data) becomes a file in the original game, per asset kind, and
what the remake's one shared resolver must do. It collects what the debug build shows at the name-to-file boundary.
It does not cover decoding (see [material resource selection](material-resource-selection.md), rows RGC-122 and
RGC-123) or which layer's file wins (see [mod loading](walks/mod-loading.md), rows ML-04 to ML-12).

This note exists because the same defect keeps returning: a name's extension in the data differs from the extension
of the shipped file, and one of our many name-to-file helpers does not retry the way the original does. Seen as a sky
texture named `.jpg` in model data while the mod ships `.dds` (legacy EAWR-6760), earlier as the texture-suffix and extensionless
model-name fixes (legacy EAWR-5083, EAWR-5096); audit tracked in (legacy EAWR-6771).

## Applicability

Debug build of Forces of Corruption, read-only walk of the asset-acquisition entry points for models and the audio
sample names, plus the existing texture rows. "Unverified" marks what no walk or capture backs. Mod data in play:
the mods whose data names a source-art extension (`.jpg`, `.tga`) while shipping another (`.dds`).

## Rules

| Rule | Behaviour | Evidence |
|---|---|---|
| ANR-01 | A texture lookup tries the authored name first, then, only after that misses, the dot-token retry: the name cut at its first dot, plus `.dds` (whether the cut covers the whole request or only the leaf is unverified; ML-41 describes leaf-only for the cache key, the RGC-122 case table the whole request). It is not a last-extension replacement: `panel.detail.tga` retries `panel.dds`; an extensionless `panel` retries `panel.dds`, never `panel.tga`; `panel.dds` authored retries only itself again. Retail never tries a `.tga` sibling of an authored `.dds` or of an extensionless name, and never tries `.jpg`/`.png` siblings. Each candidate is looked up as supplied, then under each registered texture directory. | Debug build; RGC-122, ML-40, ML-41 (texture loader and finder). |
| ANR-02 | A model name is normalized for the cache key (lowercase; the leaf cut at its first dot, stopping at a path separator) and looked up case-insensitively in loaded renderables, then in loaded particle systems. A miss, with demand loading on (the default), loads the file: first the name as supplied, then each registered search directory followed by the name, in registration order; if all fail, a stored base string + the normalized name + `.ALO` is tried (suffix and concatenation order read from the instruction listing; the stored string's content is ANR-03). After a successful load the cache is searched again by the normalized name. Consequence: an authored model name with no extension, with `.alo`, or with any other extension resolves to `<stem>.alo` when that file exists under the base directory. | Debug build: renderable-creation function, its cache search, the load-from-file function and the name normalizer; the retry's suffix and order from the instruction listing. Completes ML-39, whose decompile could not show the suffix. |
| ANR-03 | Which directory the stored base directory of ANR-02 holds is not established; the model directory is the likely value. | Unverified. |
| ANR-04 | Sound-effect and speech sample names are opened as authored, with the language token of a localized name (`_ENG`, with `.WAV` or `.MP3`) replaced by the game language's token when the language is not English. Speech samples are opened under `Data/Audio/Speech/<language>/` and music under `Data/Audio/Music/`, both with the authored file name unchanged. No other extension (`.wav` for `.mp3` or the reverse) is tried at this boundary. | Debug build: the sample-name audition routine, which builds the same paths from the same event data. The in-battle playback open was not walked separately, so this row is partial. |
| ANR-05 | Movie files are found through a table built from the movie database XML; the table's file name is expected to be opened as written, with no extension retry, but the open call was not walked, so this row is unverified. | Debug build: movie database load strings and table assertion only. |
| ANR-06 | Effect (shader) names: the identity is the leaf cut at its first dot, `.fx` is tried before `.fxo`, each first as supplied then under each registered effect directory. | RGC-120, RGC-121. |
| ANR-07 | Case and separators: the cache key is lowercase; file opens go through the file service, which treats `\` and `/` alike and compares names without regard to case (ML-10, ML-11). Layer precedence is a file-service matter (ML-05) and is independent of which candidate name is tried. | Debug build; ML-05, ML-10, ML-11. |
| ANR-08 | A miss is not fatal at this boundary. The texture finder logs a warning and may return a shared default texture when the caller asked for one (ML-40); a model miss returns no renderable and the caller decides. Whether the original reports a repeated miss once per name is unverified. | Debug build; ML-12, ML-40. |

## Remake rule (project policy)

Target: one shared resolver per asset kind implements ANR-01 and ANR-02 plus the named deviations below. The texture
resolver exists (`resolve_texture_name`, with the deviations below); the model resolver is the planned addition, today
only `normalized_model_name` (appends `.alo` to an extensionless leaf) exists. A site that needs a name resolved
calls it; a new site that opens an authored name directly is a defect (a scan guard in the CI guards is planned to enforce this). The
resolver returns the path it found, or nothing, and reports a miss once per (kind, authored name) with one clear line
that carries the authored name and the site.

### Named deviations (proposed; numbered when the fixing change lands in the deviations table)

| Proposal | Original | Remake | Reason |
|---|---|---|---|
| Texture sibling tolerance | An authored `.dds` or extensionless name never finds a `.tga` file (ANR-01). | After the authored name misses, an extensionless, `.tga` or `.dds` name tries the stem with `.tga` and then `.dds`. This loads more names than the original, and it also reorders one case: an extensionless `sky` with both `sky.dds` and `sky.tga` present picks `sky.tga` where the original picks `sky.dds` (ANR-C4). | Kept from the shipped texture-suffix fix for installed data that depends on it. For a dotted `.tga` or `.dds` name (`panel.detail.tga`) the full stem is tried first and the first-token retry (`panel.dds`) is not tried at all, so that one shape loads differently from the original (the original is expected to find `panel.dds`; unverified which is right for shipped data). Other extensions use the first-token retry as the original does. |
| Leaf-aware dot-token | Unverified whether the texture retry cuts the whole request at its first dot even when the dot is in a directory part (the RGC-122 case table says the retry of `art.v1/panel.tga` is `art.dds`; ML-41 says leaf-only for the cache key). | Cuts the leaf only. | Dotted directory names are not authored in any checked data; unverified which original helper applies. |
| Model name cache identity | Two spellings of one model share one cache entry (ANR-02). | A scene cache keys the supplied logical path. | Pre-existing (legacy EAWR-1497); a shared model resolver returns the one canonical path, closing most of the gap. |

## Cases

Synthetic names; each case names the files that exist and the file the shared resolver must return.

| Case | Authored name | Files present | Expected |
|---|---|---|---|
| ANR-C1 | `sky.jpg` | `sky.dds` | `sky.dds` (dot-token retry) |
| ANR-C2 | `sky.tga` | `sky.dds` | `sky.dds` |
| ANR-C3 | `sky.dds` | `sky.tga` | `sky.tga` through the sibling tolerance only; the original misses |
| ANR-C4 | `sky` | `sky.dds`, `sky.tga` | `sky.tga` through the tolerance (the original would pick `sky.dds`; the proposal keeps the shipped order) |
| ANR-C5 | `sky.jpg` | `sky.jpg`, `sky.dds` | `sky.jpg` (authored name first) |
| ANR-C6 | `Panel.Detail.Source` | `panel.dds`, `panel.detail.dds` | `panel.dds` (first-token retry) |
| ANR-C11 | `panel.detail.tga` | `panel.detail.dds`, `panel.dds` | `panel.detail.dds` (tolerance keeps the full stem first; the original picks `panel.dds`) |
| ANR-C7 | `ship` (model) | `ship.alo` | `ship.alo` |
| ANR-C8 | `Ship.ALO` (model) | `ship.alo` | `ship.alo` |
| ANR-C9 | `ship.x` (model) | `ship.alo` | `ship.alo` (stem retry) |
| ANR-C10 | any kind | none | nothing, and one log line for the name |

## Unknowns

- The stored base directory of ANR-03, and whether the texture dot-token and the cache-key normalizer are one helper.
- Runtime sound playback, movie open and font file selection were not walked; ANR-04 and ANR-05 are partial.
- A retail capture of a mismatched-extension texture or model was not taken; every row rests on the debug build.
