# Mod archive: implementation plan and update lifecycle

Follow-up to [mod archives and disk space](mod-archives-and-disk-space.md). On 2026-10-25 the
owner decided to build the optional archive format (legacy EAWR-4927): "we should do it, assuming
most mods won't use it, but enable its capabilities". The owner added that it must save space
**on players' machines**, not only on our test hosts. This note covers the build plan, how a
mod launcher (legacy EAWR-3009) keeps Steam Workshop mods packed across updates, and what Steam
does to a Workshop folder we have changed.

## Summary

- **Steady state on a player's PC** (retail + RaW + the Remake chain, 52.2 GiB today):
  - (C) Windows LZX compression in place: 21.6 GiB, with no new format and Steam unaware.
  - (B) archive plus zero-byte stubs in Steam's folder: about 17.8 GiB, on any operating system.
  - (E1) mods shipped packed by their author: about 15.6 GiB for those mods.
- **Recommendation: (D), a hybrid in stages.**
  1. Now, on Windows: (C) on Steam-owned folders. The launcher re-compresses after updates.
  2. The archive goes where we or the author control the install: our hosts, standalone packs,
     and authors who opt in (E1).
  3. (B) with stubs as the launcher's cross-platform "reclaim more space", once one Steam
     measurement confirms that an update re-downloads only the files the author changed.
- **Stubs instead of deletions.** In (B) every packed file is replaced by a zero-byte file
  instead of being deleted. Steam keeps unchanged stubs, rebuilds changed files and removes
  files the author deleted. So after any update the folder itself says which archive entries
  are still valid, and the game stays correct even if the launcher is not running.
- **Not measured yet.** Anonymous SteamCMD cannot download FoC Workshop items, so the
  deciding test needs a Steam login on a non-owner machine. It is set out in
  [Experiment](#experiment).
- **Build the format in four steps:** the reader in the VFS first, then the pack/unpack/verify
  tool, then byte-for-byte determinism, then the modding guide. Loose files keep winning, and
  MEGs and mod order are unchanged. About 7 tasks, 21 to 47 lane-h, plus 4 to 5 launcher tasks
  later.

## 1. Implementation plan

The format is ours, written from this note and the [VFS contract](../vfs.md): no code is taken
from other archive tools. The one dependency is the zstd library (BSD licence), vendored
unmodified at a pinned release with recorded hashes, the way pugixml is (see
`docs/reports/P0-02-dependency.md`). If "no third-party code" is meant to exclude libraries
too, the alternative is our own codec. That costs much more and would decompress more slowly.
I recommend vendoring zstd.

### Format, in short

- **Header:** magic, format version, zstd version used to pack, entry count, offset of the index.
- **Index:** one record per logical file, sorted by canonical path (the VFS's ASCII-folded
  key). Each record holds the path, original size, stored size, codec (zstd or stored), payload
  offset and the BLAKE2b or SHA-256 of the original bytes. Identical payloads share one
  offset (dedupe).
- **Payloads:** one independent zstd frame per unique file, so any file can be read alone.
  MP3, Bink, PNG and JPG are stored without compression (measured gain under 4 %).
- **Optional source record:** where the archive came from (a Workshop item id and Steam
  manifest id, or "author build"), used only by the launcher lifecycle below.
- **Bounds and diagnostics:** the same rules as MEG. Counts, offsets and sizes are checked
  before use, and malformed input gets new stable `EAWR-VFS-00xx` codes.

### Mount rule

An archive is a source inside its mod layer, like a MEG. Within a layer: loose files, then
active MEGs, then the archive. A loose file always overrides the same path in the archive, so a
one-file hot-fix next to a packed mod works. Layer order (mods leaf first, then FoC, then EaW)
is unchanged, and so is `MegaFiles.xml` handling. An archive is never required: a mod with
only raw files or MEGs runs exactly as today.

### Work packages

Lane-hour ranges come from comparable merged PRs (first commit to merge). The asset and loader
class median is 3.23 h and its p80 is 6.83 h, over 8 PRs (legacy EAWR-4662, EAWR-4715, EAWR-4811, EAWR-4857, EAWR-4835,
EAWR-2649, EAWR-4130, EAWR-2665). The tools class median is 1.3 h and its p80 is 6.2 h, over 4 PRs
(legacy EAWR-1831, EAWR-4300, EAWR-3572, EAWR-39). GPU-lane waits and owner reviews are not included.

| # | Work package | Tasks | Lane-h |
|---|---|---:|---:|
| 1 | Vendor zstd (pinned, provenance and hashes, builds on all presets, relay-OFF compile unaffected) | 1 (tools) | 1.3–6.2 |
| 2 | Format note (`docs/` format fact) and reader: index parse with bounds, `Vfs::open` decompresses into the existing buffer, mount rule, provenance in `stat`/`asset_scan`, precedence tests (loose > MEG > archive, layer order) | 2 | 6.5–13.7 |
| 3 | Pack / unpack / verify tool (`tools/` CLI): pack a mod's `Data` tree, unpack to exact bytes, verify all hashes; refuses to pack over unknown files | 2 | 6.5–13.7 |
| 4 | Determinism: same input gives the same archive bytes. Sorted entries, fixed zstd parameters, one frame per file (independent of thread count), no timestamps, pinned zstd version in the header. Golden test on Windows and Linux. | 1 | 3.2–6.8 |
| 5 | Modding guide: how to pack, how overrides work, how to unpack; the Discord proposal becomes the guide | 1 | 3.2–6.8 |
| | **Total** | **7** | **20.7–47.2 (midpoint about 1.4 agent-days)** |

Order: 1 → 2 → 3 → 4 (3 and 4 can share one PR) → 5. The per-PR checks follow the usual rules:
own tests plus one identity check. No replay or sim change is expected, since this is
presentation-side I/O only.

## 2. Steam Workshop mods: keeping them small across updates

When the mod launcher (legacy EAWR-3009) exists, it keeps subscribed mods small without the player doing
anything. This section covers what it can rely on in Steam and which design survives Steam's updates.

### Update detection (evidence)

Steam keeps per-item state in `steamapps/workshop/appworkshop_32470.acf`. Read-only on a
real install, it has, per subscribed item:

```text
"WorkshopItemsInstalled" { "<item id>" { "size" ... "timeupdated" ... "manifest" ... } }
"WorkshopItemDetails"    { "<item id>" { "manifest" ... "timeupdated" ... "timetouched" ...
                                         "latest_timeupdated" ... "latest_manifest" ... } }
```

There is also an app-level `"NeedsUpdate"` flag. The launcher records the installed `manifest`
and `timeupdated` when it processes a mod. "Updated since processed" means the current
`manifest` differs from the recorded one. "An update is pending" means `latest_manifest` differs
from `manifest`, or `NeedsUpdate` is 1. The public Steamworks API exposes the same item state
(`ISteamUGC::GetItemState` with an installed and needs-update flag, and `GetItemInstallInfo`
with a timestamp). The launcher only needs to read the file. The `.acf` is Steam's internal
format, so the launcher must treat a parse failure as "unknown, change nothing".

### What Steam does to a modified Workshop folder (evidence)

- **Updates are built from local chunks.** Valve's SteamPipe documentation: content is split
  into about 1 MB chunks. On update the client downloads only new chunks, "builds the new
  version alongside the old version" and commits by "deleting old files and moving the new
  files in". A changed file is rebuilt whole (a 25 GB pack file is rebuilt as a new 25 GB
  file). If its old local copy is missing, its chunks are simply downloaded.
  [Steamworks: Uploading to Steam](https://partner.steamgames.com/doc/sdk/uploading)
- **Unchanged files are not checked on update.** Players report that Workshop files deleted
  locally are not restored automatically, and that Steam "thinks the files are still there"
  until the game's files are verified or the item is re-subscribed. Verify also checks
  Workshop content.
  ([Steam forum](https://steamcommunity.com/discussions/forum/1/558746089050408723/),
  [Wallpaper Engine developer](https://steamcommunity.com/app/431960/discussions/2/1621726179568679369),
  [Paradox forum](https://forum.paradoxplaza.com/forum/threads/missing-workshop-files-not-downloading-fix-here.1389790/))
- **Extra files are left alone.** Verify "will not remove any files you added", but reverts
  installed files to their installed state.
  ([Steam forum](https://steamcommunity.com/discussions/forum/1/1643170903482169513))
- **Not proven by experiment.** These points come from Valve's upload documentation and from
  player reports, not from a controlled test. The conclusive test needs a Workshop item whose
  updates we control. See "Experiment" below.

### What Steam is expected to do with a packed folder

This is the expected behaviour from the evidence above, not yet measured. The same table for
deleted files and for zero-byte stubs:

| Event | Raw file deleted | Raw file replaced by a zero-byte stub |
|---|---|---|
| Author changes file X | X comes back as a full file | X comes back as a full file (no local chunks to reuse, so it is downloaded whole) |
| Author adds file Y | Y appears | Y appears |
| Author deletes file Z | nothing visible: Z was already gone, so **the archive still serves Z (wrong)** | Steam removes the stub, so Z is no longer served |
| Author leaves file W unchanged | stays deleted | the stub stays, so W is served from the archive |
| Player or Steam verifies | every deleted file is downloaded again | every stub is replaced by the full file again |

With stubs, the folder itself records what happened. The mount rule for a packed mod reads the
pack manifest stored beside the archive, which lists path, size and hash per packed file:

- a zero-byte file whose recorded size is not zero → served from the archive;
- a non-empty file → the loose file wins (an author update, a verify, or the player's own edit);
- a recorded path with no file → deleted by the author, so it is not served;
- a file that isn't in the manifest → normal loose file.

That makes every state after a Steam update correct without the launcher. The launcher is only
needed to win the space back.

**Not measured.** It is not known whether Steam, on an update, re-checks **unchanged** files
and treats the stubs as damaged. (B) is designed to stay correct either way:

| | Outcome 1: an update restores only the files the author changed | Outcome 2: an update restores every stubbed file |
|---|---|---|
| Correctness | stub rule as above | same rule. Restored files are non-empty, so they win as loose files, and the game stays correct. |
| After the update | the launcher packs only the changed or added files into a new archive and stubs them | the launcher hashes the restored files. Files equal to the pack manifest are stubbed again without recompressing; only the changed ones are packed. |
| Download per update | only what the author changed (Steam's normal update) | the whole mod, every update |
| Disk spike per update | changed bytes + one new archive | the mod's full raw size + one new archive (Remake base about +28 GiB) |
| Steady state | about 17.8 GiB (whole set) | the same, about 17.8 GiB, between updates |
| When (B) is worth it | always on non-Windows; on Windows +3.8 GiB over (C) | only for mods that rarely update; the launcher offers (C) for the others |

Update frequency matters for outcome 2. From the public Workshop API, the Remake base (20.7 GiB,
the largest) was last updated on 2023-08-19, RaW on 2026-04-02, FFT on 2026-07-20 and Holdouts
on 2026-10-10. The bytes mostly sit in mods that change rarely.

### Options compared

The numbers are for the set measured in [mod archives and disk space](mod-archives-and-disk-space.md):
retail FoC + EaW 3.52 GiB, RaW 14.81 GiB, Remake chain 33.91 GiB, 52.24 GiB in total. Retail is
a Steam app, not a Workshop item, and the original game must keep running from it. It only ever
gets (C), 3.52 → 1.81 GiB. Packing on a player's PC uses zstd-3, which compresses at 250 to
650 MB/s per core, so all mods take a few minutes. zstd-19 runs at only 3 to 11 MB/s and is for
authors. Deduplication inside and across mods saves about 2.9 GiB after compression.

| | (B) archive + stubs in Steam's folder | (C) LZX in place on Steam's folder | (D) hybrid: (C) now, archive where we or the author own the install, (B) later | (E1) the author ships the archive on the Workshop | (E2) pack, then unsubscribe |
|---|---|---|---|---|---|
| **Steady state, whole set** | about 17.8 GiB (mods 18.9 at zstd-3 minus 2.9 dedupe, plus retail 1.8). Saves 34.4 GiB. | 21.6 GiB. Saves 30.6 GiB. | Windows PC: 21.6 GiB now, about 17.8 GiB once (B) is added. Our hosts with copies: about 15.6 GiB. | about 15.6 GiB for mods whose authors do this (mods 16.7 at zstd-19 minus 2.9, plus retail 1.8); other mods as (C) | as (B), about 17.8 GiB |
| **Spike when an update drops** | Changed files arrive full-size, then the repack writes a new archive next to the old one. Worst case (verify, or Steam re-downloads everything): the mod's full raw size plus two archives. For the Remake base that is 20.7 + 2 × 7.7 GiB, about **+28 GiB** over its steady 7.7 GiB. For the whole set, done mod by mod, the peak is the largest mod. | Changed files land uncompressed until they are compressed again. Worst case, the whole mod: Remake base 20.7 vs 9.0 GiB, **+11.7 GiB**. Whole set: +30.6 GiB. | as (C) for Steam folders, as (B) once enabled | Steam rebuilds each changed archive file whole next to the old one: **+ the size of the changed part**. Parts of at most 1 to 2 GB keep that small. | re-subscribing downloads the whole mod: Remake base +20.7 GiB plus the new archive |
| **Needs the launcher running?** | For packing and repacking, yes. For correctness, no: the stub rule is in the engine. | No for the first run (one command). Yes, or a manual re-run, to compress files after updates, otherwise the saving slowly erodes. | as (C)/(B) | No | Yes, and the player must unsubscribe and re-subscribe |
| **Files the player edited** | Never stubbed: a file is stubbed only if its hash matches what was just packed. An edited file is non-empty, so it wins as a loose file. A later verify overwrites player edits, which is what Steam already does today. | Not affected. Edited files are simply stored uncompressed. | as (C)/(B) | Overrides go next to the archive as loose files. Steam leaves extra files alone. | kept in the launcher's library as loose overrides |
| **Rollback** | Archive written as `.partial`, verified, then renamed. Stubbing is journaled and done only after that. `unpack` restores exact bytes (hash checked). Last resort: Steam verify. | Nothing to roll back: the content never changes. `compact /u` undoes it, and an interrupted run leaves harmless partly compressed files. | per part | Steam's own | re-subscribe |
| **Platforms** | all | Windows NTFS only. Linux on btrfs has transparent zstd; ext4 has none. macOS APFS compression exists but only through tools. | all | all | all |
| **Risk** | Steam behaviour unmeasured; Steam may count the item as damaged; a stub is a zero-byte file that tools other than ours will see as empty | lowest | low | the original game can't read the archive, so a packed-only mod is EAWR-only; authors maintain two uploads if they want both | subscriber counts drop, which modders care about; required-item chains (Remake submods) break the flow |

Other Steam-compatible ideas, rejected:
- hard links between duplicate files: Steam replaces them on update;
- moving Workshop folders to another drive: no saving;
- a separate "packed edition" Workshop item: this is (E1) with twice the uploads.

### Free space and compressing again around a Steam update (requirement)

Requirement (chief, 2026-10-25): before a Steam update touches a compressed Workshop folder, the
launcher checks free space. After the update, it compresses the folder again.

**Free space before an update.** Steam builds updated files at full size before it moves them
in and deletes the old ones. The worst case is that every file changed, so the need is the new
version's full raw size. The public Workshop API reports it per item (`file_size`, no key
needed). The launcher checks when it starts and whenever it sees an update pending for an item
(`latest_manifest` differs from `manifest`, or `NeedsUpdate` is 1 in the `.acf`). It requires
`file_size` plus a margin of free space on the library's drive. If there isn't enough, it warns
before the game (and with it Steam's Workshop update) is launched, and names the item and the
GiB missing. It cannot pause Steam itself. Whether Steam's own space check allows for compressed
folders is not measured.

| Worst case (every file changed) | Free space needed during the update | Extra on disk after it, until compressed again |
|---|---:|---:|
| Remake base (20.66 GiB raw, 9.00 GiB with LZX) | 20.7 GiB | +11.7 GiB |
| Whole Remake chain (33.91 → 12.58 GiB) | 33.9 GiB | +21.3 GiB |
| RaW (14.81 → 7.23 GiB) | 14.8 GiB | +7.6 GiB |

**Compressing again after an update.** Measured on scratch copies, on NTFS:

| Case | Result |
|---|---|
| File created inside a folder flagged with `compact /c` (LZNT1) | compressed automatically (a model file: 0.50 of its size; the space is released when the data is flushed to disk, after a few seconds) |
| File created elsewhere and **moved into** the flagged folder | **not** compressed; it keeps its own state until `compact /c /s` runs again (then 0.30) |
| LZX (`/exe:lzx`) file rewritten in place | loses its compression (1.00) |
| `compact /exe:lzx` on a file that already has LZNT1 from the folder flag | no effect; it stays LZNT1 |

Valve describes Steam as building new files alongside and **moving** them in. So the folder flag
probably does not compress Steam's updated files, and the launcher must run compression after
every update in either case. Because it has to run anyway, use LZX: on the whole set it saves
30.6 GiB against 23.4 GiB for LZNT1. Don't set the LZNT1 folder flag on the same folders,
because LZX skips files that already carry LZNT1. Which of these Steam really does (create in
place, or move in) is part of the Steam measurement below.

After it sees a new `manifest`, the launcher compresses only the files that are not compressed
yet: `compact /c /exe:lzx /s:<item folder>` skips files that are already compressed. If the
launcher is not running when the update lands, the folder stays larger until its next start
(worst case in the table above). Nothing breaks.

### Recommendation: (D)

1. **Now (Windows):** the launcher, and until it exists a documented command, applies (C) to
   Steam-owned mod folders and retail. Saving: 30.6 GiB on this set. After each detected
   update (new `manifest` in the `.acf`) it compresses only the files that are uncompressed.
   On our own hosts this is the host-mirror ticket (legacy EAWR-4925).
2. **Archive where we or the author own the install:** our test hosts and rigs keep packed
   copies of the mods they test (about 15.6 GiB for this set). Standalone mod packs and authors
   who opt in (E1) ship it; for those, recommend parts of at most 1 to 2 GB for small Steam
   updates.
3. **(B) with stubs after the measurement:** if Steam re-downloads only the changed files, the
   launcher offers "reclaim more space" for Steam mods on every platform: 21.6 → about
   17.8 GiB on Windows, and the main saving on Linux/macOS, where (C) is missing. If Steam
   re-downloads everything, (B) is dropped and (C) stays the Steam answer.

On a Windows player's PC, (C) gets about 89 % of what the best client-side option (B) can reach
(30.6 vs 34.4 GiB saved). (B) earns its extra code mainly on non-Windows systems.

### Work packages, later

After the launcher exists. Same asset/loader class as above.

| # | Work package | Tasks | Lane-h |
|---|---|---:|---:|
| L1 | Read Steam Workshop state (`.acf`) robustly; record per-mod state; update detection; "unknown means hands off" | 1 | 3.2–6.8 |
| L2 | (C) Compress Steam folders with LZX, then only the files that are uncompressed after each update; free-space check against the new version's `file_size` before a pending update; progress and cancel in the launcher | 1 | 3.2–6.8 |
| L3 | (B) Pack manifest and the engine's stub mount rule (with tests for every row of the table above) | 1 | 3.2–6.8 |
| L4 | (B) Atomic pack commit, journaled stubbing, hash-guarded "never stub an edited file", repack after update, unpack/restore | 2 | 6.5–13.7 |
| | **Total** | **5** | **16.1–34.1** |

## Experiment

Tried: SteamCMD with an anonymous login, in a throwaway folder on a build host (not the owner's
machines), downloading a 678 KB FoC Workshop item. Result: `Download item ... failed
(Failure)`. FoC Workshop content needs a login that owns the game. Nothing else was run.

The deciding test needs one of these:
- a SteamCMD login with an account that owns FoC on a non-owner machine. The Steam Guard code
  is entered once, and the credentials stay in that throwaway folder;
- an existing Steam client on a test machine, subscribed to the tiny item.

Ready to run: [`tools/research/steam_stub_probe.ps1`](../../tools/research/steam_stub_probe.ps1).
Once the owner has logged SteamCMD in once in the throwaway folder (password and Steam Guard
code, entered only by the owner), it does the following:
1. Downloads a 678 KB FoC Workshop item and records every file's SHA-256.
2. Stubs every other file to zero bytes, deletes two, edits one and adds an extra file.
3. Runs the download again, plain and then with `validate`. After each run it records, per
   file, whether it is original, a stub, missing or changed, plus Steam's `.acf`.
4. Writes `report.json` and `report.txt`. Nothing outside the throwaway folder is touched.

Steps 1 to 3 show what Steam does to stubs, missing, edited and extra files on a plain
re-download and on verify. Outcome 1 vs 2 also needs an **update**. That needs an item whose
updates we control: a tiny unlisted item published by the owner, with one update that changes
one file, adds one and deletes one, while an unchanged file is stubbed. The script is then
run again after the update.
