# Mod archives and disk space

Research for the owner's question of 2026-10-24: would packing mods into a compressed archive
save disk space, and would load times suffer for it? The measurements come from the retail
FoC/EaW data and the installed Workshop copies of Republic at War (RaW, 1129810972) and the
Remake chain (1770851727 Remake, 2794270450 Holdouts, 3229239424 FFT). They are read-only
measurements on one PC, plus a read-only look at three build hosts. Nothing here changes code.

## The answer

**Yes, mod data compresses well, but mostly not where we expected, and an archive format of our
own is not the cheapest way to get the space back.**

- **The bytes are in models and textures, not in text.** XML, Lua and other text make up 0.2 to
  0.7 % of a mod and 1.6 % of retail FoC. Models (`.alo`) and DDS textures are 65 to 92 % of
  every mod.
- **Models shrink to about a sixth of their size.** FoC's model vertex is a fixed 144-byte
  record, mostly zeros (four UV sets, bone indices and weights, colour). zstd-3 stores models at
  15 to 17 % of their size. **The DDS textures shrink to 33 to 49 %**, even though they are
  already block-compressed. 95 to 99 % of the mod DDS bytes are BC2/BC3, and half of every
  BC2/BC3 block is alpha, mostly a constant fill. True BC1 shrinks only 30 to 40 %. MP3, Bink
  video and PNG/JPG don't shrink. WAV shrinks 6 to 25 %.
- **The owner's RAR test is reproduced.** For RaW the class-weighted estimate is 14.81 →
  6.41 GiB at zstd-19 and 6.89 GiB at zstd-9. The owner's default RAR gave 14.8 → 6.6 GB.
- **Windows can get most of this with zero code.** Windows' built-in transparent compression
  (`compact /exe:lzx`) stores the whole current set (retail + RaW + the Remake chain,
  52.2 GiB) in about 21.6 GiB, saving 30.6 GiB. That is 59 %. A zstd-19 archive of our own would
  store it in 18.4 GiB (65 %). The extra 2.8 GiB doesn't pay for a new format, a packer, tooling
  for mod authors and a second load path.
- **Load time hardly moves either way.** Start-up is dominated by decoding models and
  textures on the CPU and by GPU upload, not by disk reads. The estimate is within about ±6 % of a
  Remake start on both the GTX 970 rig and a modern laptop (see the
  [load time cost model](#load-time-cost-model)). Compression is a disk-space measure, not a
  load-time measure.
- **On the build hosts, mods aren't what fills the disks.** On the host whose system drive filled
  today, the two warm build trees take 128 GiB (57 + 71 GiB: 42 GiB `.obj`, 25 GiB `.lib`). Its
  retail mirror takes 3.5 GiB, and it has no mod chain. The fix for that is the build-host retention rule (legacy EAWR-4673).
  Compressing game data is a side gain there.

Recommendation, in short:

| Where | Now | Gain | Cost |
|---|---|---|---|
| (i) Pool and rig hosts | Apply `compact /exe:lzx` to the read-only retail and mod mirrors (never to build trees), via a script that re-applies it after a mirror refresh. Land the build-tree retention rule (legacy EAWR-4673). | about 1.7 GiB per retail-only host; about 21 GiB per Remake chain copy; about 7.6 GiB per RaW copy | one small tools PR |
| (ii) Owner kits | Nothing to change. Kits are engine-only (about 220 MB zipped) and read game and mod data from the player's own install. Offer the owner the same `compact` command for their own Workshop folders, as their choice. | up to about 29 GiB on a PC with RaW + Remake | none |
| (iii) Long term | A **cooked model cache** (compact vertex layout, derived and rebuildable, optional) for load time and memory. The space saving comes on top. An **optional** compressed archive: the owner decided on 2026-10-25 to build it ([plan and lifecycle](mod-archive-lifecycle.md)). | model decode is 40 % of a Remake start on the GTX rig | see the tickets |

## What the mods weigh

Bytes per file class. Entries inside MEG archives are counted by their own class (RaW keeps
2.78 GiB in 10 MEGs, the Remake base 0.30 GiB in 7). Text = XML + Lua + `.dat` + other text.
Allocation slack at 4 KiB clusters is under 0.3 % everywhere.

| Set | Total | Files | Models | DDS | WAV | MP3 | Bink | Maps | Other | Text |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Retail FoC | 1.46 GiB | 9,792 | 24 % | 16 % | 19 % | 9 % | 4 % | 25 % | 2 % | 1.6 % |
| Retail EaW | 2.06 GiB | 14,911 | 27 % | 17 % | 23 % | 18 % | 5 % | 8 % | 2 % | 0.7 % |
| RaW | 14.81 GiB | 16,254 | 22 % | 43 % | 20 % | 5 % | 4.5 % | 4 % | 1.3 % | 0.24 % |
| Remake | 20.66 GiB | 26,160 | 36 % | 40 % | 4 % | 5 % | 0 % | 1.5 % | 13 % (PNG/JPG 8 %, PSD 3 %, TGA 2 %) | 0.24 % |
| Holdouts | 6.19 GiB | 7,207 | 60 % | 26 % | 5 % | 6 % | 0.2 % | 0.4 % | 2 % | 0.7 % |
| FFT | 7.06 GiB | 4,966 | 78 % | 15 % | 1 % | 3 % | 0 % | 0 % | 3 % | 0.7 % |

The Remake chain is 33.9 GiB, and 17.0 GiB of that is models. The Remake base also ships about
0.65 GiB of Photoshop sources and 1.6 GiB of PNG/JPG, which the game doesn't read as such.

## How compressible each class is

Random samples of real files per class (48 MiB per class and set; 512 MiB for DDS, models and WAV).
The ratio is compressed/original, so lower is better. zstd is per file except "blocks" (1 MiB
blocks of the concatenated sample, as a random-access archive would store them) and "solid"
(one long-window stream over the sample, the RAR-solid analogue). Decompression speed is
single-threaded on a 12th-generation desktop CPU (i7-12700K) from memory. Ranges span the six sets.

| Class | zstd-3 | zstd-9 | zstd-19 | 1 MiB blocks (z9) | solid (z19) | deflate-6 | zstd-3 decompress |
|---|---:|---:|---:|---:|---:|---:|---:|
| Models `.alo` | 0.15–0.17 | 0.14–0.16 | 0.11–0.16 | 0.15–0.20 | 0.11–0.15 | 0.16–0.20 | 1.2–1.5 GB/s |
| DDS (all formats) | 0.34–0.49 | 0.31–0.47 | 0.21–0.37 | 0.25–0.40 | 0.21–0.37 | 0.26–0.41 | 0.7–0.9 GB/s |
| — of which BC1 | 0.58–0.69 | | | | | | |
| — of which BC2/BC3 | 0.19–0.56 | | | | | | |
| — of which uncompressed | 0.02–0.36 | | | | | | |
| TGA | 0.10–0.27 | 0.10–0.25 | 0.09–0.22 | | 0.08–0.21 | 0.11–0.26 | 0.9–1.8 GB/s |
| Maps `.ted` | 0.05–0.15 | 0.05–0.15 | 0.04–0.13 | 0.05–0.15 | 0.04–0.12 | 0.06–0.15 | 1.5–3.0 GB/s |
| Animations `.ala` | 0.09–0.58 | 0.09–0.57 | 0.08–0.55 | 0.08–0.53 | 0.02–0.35 | 0.10–0.58 | 0.8–4 GB/s |
| XML | 0.07–0.10 | 0.06–0.09 | 0.05–0.08 | 0.05–0.07 | 0.03–0.04 | 0.08–0.11 | 2.1–2.3 GB/s |
| Lua | 0.24–0.31 | 0.23–0.29 | 0.22–0.28 | 0.07–0.15 | 0.04–0.11 | 0.23–0.29 | 0.6–0.9 GB/s |
| WAV | 0.82–0.94 | 0.81–0.94 | 0.69–0.93 | 0.74–0.94 | 0.68–0.93 | 0.74–0.93 | 0.8–1.2 GB/s |
| MP3 | 0.97–1.00 | | | | 0.96–0.99 | | |
| Bink video | 0.76–0.99 | | 0.71–0.96 | | | | |
| PNG/JPG | 0.96–1.00 | | | | | | |

Per-file zstd is within a few percent of solid compression for the classes that matter (models,
DDS, WAV). Solid compression wins only on many small similar text files, which are too
few bytes to matter. A random-access archive therefore gives up almost nothing against RAR.

Deflate decompresses at about 250 to 900 MB/s, zstd at 0.7 to 4 GB/s, for the same ratio. If we ever
build an archive, it should use zstd.

## Size per set and option

Class bytes × measured ratio. Unsampled classes count as incompressible, so these are
conservative. The NTFS columns come from Windows' own codecs (`RtlCompressBuffer`) with NTFS's
unit and cluster rounding. The emulation agrees with a real `compact` run on the same 139 MB of
files to within 0.01 for models, DDS and WAV. LZX has no API outside the filesystem, so its column
is XPRESS8K × the LZX/XPRESS8K ratio of that real run per class.

| Set | Raw | zstd-3 | zstd-19 | `compact /c` (LZNT1) | `/exe:xpress8k` | `/exe:lzx` |
|---|---:|---:|---:|---:|---:|---:|
| Retail FoC + EaW | 3.52 GiB | 1.75 | 1.67 | 2.24 | 1.95 | 1.81 |
| RaW | 14.81 GiB | 7.10 | 6.41 | 9.12 | 7.92 | 7.23 |
| Remake chain | 33.91 GiB | 11.82 | 10.29 | 17.51 | 14.40 | 12.58 |
| **All** | **52.24 GiB** | **20.67** | **18.37** | **28.87** | **24.27** | **21.62** |
| Saved | | 31.6 (60 %) | 33.9 (65 %) | 23.4 (45 %) | 28.0 (54 %) | 30.6 (59 %) |

### Duplicate content

Hashing every payload of 4 KiB or more across the six sets (27.8 GiB had a size twin) finds
6.0 GiB of byte-identical copies:

- inside RaW: 2.25 GiB, of which 2.02 GiB is duplicate WAV;
- inside the Remake base: 1.73 GiB (DDS 0.75, models 0.66, PNG 0.24). One example is the
  same 149 MiB model stored under two names;
- inside FFT: 0.55 GiB (models);
- across the Remake chain: 0.71 GiB; RaW ↔ Remake base: 0.56 GiB;
- mods ↔ retail: 0.05 GiB. Mods override retail; they don't copy it.

After compression, deduplication adds about 3 GiB more, mostly RaW's WAVs. It needs a
content-addressed store (option e), which only a format of our own provides. Hard links
inside Steam-managed folders would break on the next Workshop update.

## How the loader reads files today

- **Mount.** `Vfs::mount` indexes every layer (mods leaf first, then FoC, then EaW). Loose
  files are found by a directory walk, and active MEGs (from `MegaFiles.xml` plus the SFX and
  patch slots) by their index. Within a layer, loose files win over archives
  ([VFS contract](../vfs.md)). This is how mods stay loosely editable, and an optional archive
  would have to keep that rule.
- **Read.** `Vfs::open` opens the source file (loose file or MEG), seeks to the entry and reads
  the whole entry into one buffer. There is no memory mapping and no streaming. A compressed
  entry would therefore be a local change: read the compressed span, decompress it into the same
  buffer. Peak transient memory grows by the compressed size of the largest file in flight (up to
  about 150 MiB for the largest model).
- **Caches.** `VfsAssetCache` shares decoded models across scenes (legacy EAWR-4662) with a byte budget
  (legacy EAWR-4857). Textures go through upload recipes that can be reloaded (legacy EAWR-4835). There is no on-disk
  cache of decoded data.
- **When things load.** The XML catalog, unit tables, Lua and map are read at start. Models and
  textures are read on first use while the battle scene is prepared, so they count towards
  start-up too, because the scene prepares every unit it may show.

### Start-up phases (existing receipts)

From the viewer's load profile (`--eawr-report` startup phases). `model_decode` and
`texture_decode` **include** the file read (`load_model`/`load_texture` call `Vfs::open` inside
the timed scope). Times are seconds.

| Run | Total | Mount index | XML catalog | Model read + decode (loads) | Texture read + decode (loads) | Texture GPU upload |
|---|---:|---:|---:|---:|---:|---:|
| GTX 970 rig, Remake chain skirmish, test kit of 2026-10-24 | 212.1 | 1.2 | 2.3 | 85.8 (1,612) | 38.7 (1,161) | 6.3 |
| GTX 970 rig, RaW skirmish, same kit | 33.8 | 0.6 | 0.9 | 4.9 (209) | 6.1 (666) | 1.5 |
| GTX 970 rig, vanilla skirmish, same kit | 19.9 | 0.1 | 0.4 | 5.4 (219) | 3.2 (382) | 0.2 |
| Modern laptop (RTX 4070 laptop), Remake Dev4 kit, 2026-10-23 | 90.9 | 2.0 | 3.3 | 22.3 (1,616) | 7.2 (749) | 5.4 |
| Owner PC, Remake Dev4 kit | 57.0 | 0.6 | 1.5 | 3.6 (1,616) | 2.1 (749) | 5.7 |
| GTX 970 rig, Remake, build of 2026-10-22, first run | 455.6 | 14.4 | 19.1 | 241.8 (4,127) | 54.8 (729) | 6.4 |
| Same, immediate repeat (warm file cache) | 404.8 | 17.8 | 18.7 | 200.1 (4,127) | 52.5 (729) | 9.0 |

What this shows:

- **Text parsing is small.** The XML catalog is 1 to 4 % of a start. A pre-parsed XML/Lua cache
  (option d for text) could save at most about 2 s on the GTX rig.
- **Model decode is the big block on the weak rig.** It takes 40 % of the Remake start there, at
  53 ms per model, against 2.2 ms on the owner PC. That gap is far larger than the CPU
  difference, so it is not disk speed. The warm-cache repeat on 2026-10-22 was only 42 s faster
  in model decode for 4,127 models, about 10 ms per model, or roughly 8 % of a start. That
  bounds the cold-read share.
- **The GC initialization of 810 s on the Remake chain has no phase breakdown in any receipt.**
  I could not attribute it, and this doc makes no claim about it.

## Load time cost model

The receipts record no byte counts, so the bytes read per start are estimated as unique loads ×
mean file size of the chain: 1,612 models × 2.2 MiB + 1,161 textures × 1.85 MiB ≈ 5.6 GiB
for a Remake start. That figure may be off by a factor of 2.

Read throughput, measured on 139 MB of real mod files with uncached reads
(`FILE_FLAG_NO_BUFFERING`, best of 3 to 5 passes):

| Drive | Raw | `/exe:xpress8k` | `/exe:lzx` |
|---|---:|---:|---:|
| SATA SSD | 557 MB/s | 475 MB/s | 408 MB/s |
| NVMe SSD | 1,189 MB/s | 811 MB/s | 590 MB/s |

On a fast drive, transparent compression makes **cold** reads slower, because the kernel
decompresses on the read path and that becomes the limit. **Warm** reads cost nothing extra:
the file cache keeps the decompressed pages. LZNT1 (`compact /c`) files can't be read uncached
at all, so they always go through the cache.

For a user-space zstd archive it is the other way round. The file cache would hold the
compressed bytes, so every start pays decompression, warm or cold. In return, cold reads move
about a quarter of the bytes.

Estimates for a Remake start. The GTX rig's CPU (i7-4790K) is taken at 0.6× the measured
decompression speed. That factor is an assumption; the rig was not measured.

| Option | GTX 970 rig (212 s start) | Modern laptop (91 s start) |
|---|---|---|
| (a) `compact /exe:lzx` | cold: about −9 to +6 s (−4 to +3 %), depending on the rig's disk, which I could not measure; warm: 0 | cold: about +5 s (+6 %); warm: 0 |
| (a) `compact /exe:xpress8k` | cold: about −10 to +2 s (−5 to +1 %); warm: 0 | cold: about +2.5 s (+3 %); warm: 0 |
| (c) own zstd-3 archive, single-threaded decode | cold: about −3 to +1 s (−1.5 to +0.5 %); warm: about +9 s (+4 %) | cold: about +2 s (+2 %); warm: about +5.5 s (+6 %) |
| (c) same, decompression on the loader's worker threads | cold: −5 s or better; warm: +2 to +3 s | ±1 s |
| (b) MEG of the same bytes, uncompressed | part of the 1.2 s mount walk; no read change | part of the 2.0 s mount walk |
| (d) cooked model cache (no parse, about 3 to 4× fewer bytes) | could cut a large part of the 86 s model decode: not measured, needs a prototype | could cut a large part of the 22 s model decode |

Memory: options (a), (b) and (e) change nothing in the process. Option (c) adds one transient
compressed buffer per read. Option (d) could reduce retained geometry, which matters for the
8 GiB memory target (legacy EAWR-4645): the model vertex is 144 bytes, most of them unused.

## The options compared

| | (a) Transparent compression on hosts | (b) MEG of the same bytes | (c) Own compressed archive | (d) Pre-built caches | (e) Dedupe across the chain |
|---|---|---|---|---|---|
| Space saved, full set | 23 to 31 GiB (LZNT1 to LZX) | under 0.1 GiB (cluster slack only) | 33 GiB (zstd-19) | none on its own; it adds a cache unless it replaces the source | about 3 GiB after compression |
| Load time | cold −5 to +6 %, warm 0 | slightly faster mount | cold −1.5 to +2 %, warm +4 to +6 % unless decoding runs in parallel | the only option that can cut start time much (models) | 0 |
| Memory | 0 | 0 | small transient buffer | can cut retained geometry | 0 |
| Code | none; one script | MEG writer + mount entry | format, reader, packer, verifier, docs | cache format, invalidation, validation per asset kind | needs (c) |
| Mod authors | invisible | familiar (MEG tools exist) | new format; must stay optional | invisible (derived) | invisible |
| Modding compatibility risk | none; bytes are unchanged | none | low if loose files keep winning and the archive is optional | stale-cache bugs if invalidation is wrong | none |
| Caveat | a file rewritten by Steam or a mirror refresh is stored uncompressed until compacted again; never use it on build trees (write-heavy) | saves almost nothing | warm starts pay decompression | must never be required to run a mod | only inside our own archive |

### What others do

- **FoC itself** packs data into MEG v1: an index plus concatenated, **uncompressed** files.
  Petroglyph's later MEG versions added encryption, not compression.
- **Engines that compress use random-access units and fast codecs**: per-file or per-block
  (64 to 128 KiB) LZ4, zstd or Oodle with an index (Unreal pak/IoStore, Unity LZ4 bundles,
  Bethesda BSA/BA2 per-file). Whole-stream compression (Unity LZMA, RAR/7z solid) is used only for
  downloads, because a single file can't be read without decompressing what comes before it.
- **Steam** compresses Workshop content in transit and stores it raw on disk. A Workshop item is
  a folder; mods may ship archives inside it (RaW ships MEGs). An update replaces changed files
  whole, which drops transparent compression on those files.
- **Players already compress Steam libraries** with `compact /exe` (for example with the
  CompactGUI tool). Windows' WOF compression (`/exe:`) is meant for files that are written once and
  read often. Volume deduplication exists only on Windows Server; our hosts run client Windows.

## Recommendation

1. **Pool and rig hosts, now.** Add a tools script that applies `compact /exe:lzx` to the
   read-only retail mirror and mod folders on a host and reports the bytes saved. It never
   touches build trees, caches or run folders, and is safe to re-run after a mirror or Workshop
   refresh. Expected gain: about 1.7 GiB per retail-only build host, and about 21 GiB + 7.6 GiB
   on a host that keeps the Remake chain and RaW. The cost to a cold start is between −5 and
   +6 % (LZX on NVMe is the worst case), and a warm start costs nothing. Use XPRESS8K instead
   of LZX on a host where cold-start timing is being measured. The disk pressure itself comes
   from warm build trees: land the retention rule (legacy EAWR-4673). Its proposed 45 GiB per-tree
   cap is exceeded on the host measured today, at 57 and 71 GiB.
2. **Owner kits.** No change. Kits contain no game data. The owner may run the same command on
   their own Workshop folders. That is their choice and not a default, since it changes how their
   PC stores files.
3. **Long term.**
   - The lever for load time and memory is a **cooked model cache**: a compact, versioned,
     rebuildable vertex layout derived from the `.alo` and stored beside it, never required,
     with the loose `.alo` still authoritative. It belongs with the performance work (legacy EAWR-4645) and
     needs a prototype measurement on the GTX rig before any claim.
   - An **optional zstd archive** (per-file zstd, an index, content-addressed payloads, loose
     files and MEGs still winning in the same layer). It would save about 3 GiB more than LZX
     plus about 3 GiB of dedupe. The owner decided on 2026-10-25 to build it as an optional
     capability; see [plan and lifecycle](mod-archive-lifecycle.md).
   - Follow-ups: host mirror compression (legacy EAWR-4925), cooked model cache (legacy EAWR-4926),
     optional archive format (legacy EAWR-4927).
   - Not recommended: compressing XML/Lua on its own (under 0.2 GiB in total), a MEG repack of
     the same bytes (under 0.1 GiB), and solid archives (no random access).

## What I could not measure

- The GTX rig's drive type and its cold-read speed. Its load-time rows are estimates from its
  receipts and a scaled CPU factor.
- The exact bytes a start reads. The profile has no byte counters, so the per-start volume is a
  mean-size estimate.
- A start with compressed data. No engine run was made. The deltas come from the cost model,
  not from a receipt.
- Where the 810 s Remake GC initialization goes.
- The owner's exact RAR settings. The class-weighted zstd estimate reproduces the result between
  zstd-9 and zstd-19, so they didn't matter.
- Mods other than RaW and the Remake chain, and the EaW-only data beyond DDS, models and WAV
  (the FoC ratios were reused for its smaller classes).

## Method and receipts

Scripts and raw JSON stay in the worktree's ignored `out/research/mod-archives/`:
`inventory.py` (bytes per class, MEG v1 entries included), `sample_compress.py` (48 MiB
per class: zstd 3/9/19, 1 MiB blocks, solid, deflate, DDS format census), `big_sample.py`
(512 MiB of DDS, models and WAV), `ntfs_codecs.py` (LZNT1/XPRESS emulation with NTFS rounding),
`unbuffered_read.py` (uncached read timing), `dedupe.py` (size then BLAKE2b hashing) and
`aggregate.py`. zstd is python-zstandard 0.25 (libzstd 1.5.7). The start-up rows are copied from
existing kit-proof and performance reports (2026-10-22 to 2026-10-24). The build-host sizes come
from a read-only listing over the build account.
