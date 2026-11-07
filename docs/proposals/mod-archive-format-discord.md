# Proposal for modders: an optional compressed mod archive

A draft for discussion with the Empire at War modding community. We plan to add this as an
**optional** capability: most mods will never need it. No dates are set, and the details below
are what we would like modders' input on before it is built. The measurements
behind it are in [mod archives and disk space](../research/mod-archives-and-disk-space.md).

## The problem

Big mods are big. On our test machines:

| Mod | On disk today | Packed with zstd (measured) |
|---|---:|---:|
| Republic at War | 14.8 GiB | about 6.4 to 7.1 GiB |
| Remake + Holdouts + FFT | 33.9 GiB | about 10.3 to 11.8 GiB |
| Retail FoC + EaW | 3.5 GiB | about 1.7 GiB |

The space goes to models and textures, not XML. XML and Lua are well under 1 % of a mod.

- **Models (`.alo`)** pack to about a sixth of their size. Most bytes in each vertex are
  unused fields that are always zero.
- **DDS textures** pack to a third or half of their size. Most mod textures are DXT3/DXT5,
  and their alpha half is often one flat value.
- **MP3, Bink video and PNG/JPG** are already compressed and don't shrink.

Load time barely changes either way. Starting a battle is spent on decoding models and
uploading textures, not on reading the disk. We measured about ±6 %.

## What we propose

An **optional** archive that EAWR can read next to a mod's normal files.

- **One file per mod.** The mod's `Data` folder is packed into a single archive (working
  name `.eawrpak`) that sits in the mod folder.
- **Each file is compressed on its own** with zstd, so the game can open any file without
  unpacking the rest. Models, textures, maps, WAV and text are compressed. MP3, Bink and
  PNG/JPG are stored as they are. Identical files are stored once; one mod we measured has
  2 GiB of duplicate sound files.
- **Loose files still win.** Any loose file in the mod folder overrides the same file in the
  archive, exactly as loose files override MEGs today. You can hot-fix or tweak a packed
  mod by dropping a single XML next to it.
- **MEGs keep working.** Mods that ship MEGs or raw folders keep working unchanged. Mod
  order and override rules (`MODPATH`, the leaf mod first) don't change.

## What modders would have to do

- **Nothing**, if you don't want to. Raw folders stay fully supported, and no mod will ever be
  required to ship an archive.
- If you want a smaller download, run **one pack command** on your mod folder before
  uploading. A matching **unpack command** gives back the exact original files, checked by
  hash.

## What stays the same

- The XML, Lua, models and textures inside are byte-for-byte your files. Nothing is
  converted, and no data is changed or "fixed".
- Retail behaviour and how EAWR reads mods (override order, `MegaFiles.xml`, mod chains).
- Mods that ship raw files or MEGs.

## Compatibility, honestly

- **The original game can't read this archive.** A mod uploaded only as an `.eawrpak`
  would run in EAWR but not in retail FoC. A mod that wants to support both would ship its
  normal files, and the archive gains nothing. This is the biggest open question below.
- **It stays open.** zstd is a free, widely used compression library, and we would publish
  the format: an index followed by compressed files. Anyone can write an unpacker, and ours
  would come with EAWR.
- **Windows can already do much of this** without any new format. `compact /exe:lzx` on a
  mod folder roughly halves it (33.9 → about 12.6 GiB for the Remake chain), works for
  retail too, and is invisible to the game. That gets about 90 % of the archive's disk
  saving. The archive would be another 15 to 20 % smaller, plus the duplicate files, and it
  is the only one of the two that also shrinks the download.

## Risks and open questions

- **Two copies on Workshop.** If modders upload both raw and packed versions, users can
  install the wrong one, and the modder has two things to maintain.
- **Tool support.** Existing tools (MEG editors, model viewers, XML editors) can't open the
  archive until they learn to. Unpacking first is always possible.
- **Steam updates.** A small change to a packed mod means re-uploading the whole archive
  unless the archive is split into parts. Steam already sends only changed blocks, but how
  well that works for archives still needs testing.
- **Is it worth it at all** next to `compact /exe:lzx`, which needs no new format?
- **Packing on the player's PC.** Later, the EAWR launcher may also pack Workshop mods the
  player subscribed to, leaving zero-byte placeholder files in the Workshop folder so that Steam
  updates still work. Bug reports from those players would show empty files where your
  assets are. Would that confuse you or your players?

## Questions for you

1. Would you ship a mod as an EAWR-only archive, or does retail compatibility always come
   first for your mod?
2. Is the size of your mod (download or on disk) a problem your players actually report?
3. Which tools would you need to support it first: MEG editors, model viewers or something
   else?
4. One archive per mod, or split into parts (for example models, textures, the rest), so
   that an update re-downloads less?
5. Would you rather we spend this effort elsewhere, for example on making mods load faster,
   which the archive does not do?

---

## Short version for Discord

> **EAWR: optional compressed mod archive? We'd like modders' opinions**
>
> We measured where the space in big mods goes. It's not XML (under 1 %): it's models and
> textures. Models pack to about 1/6 of their size (most vertex bytes are zero) and DXT3/DXT5
> textures to 1/3 to 1/2. In our tests RaW goes from 14.8 to about 7 GiB, and
> Remake + Holdouts + FFT from 33.9 to about 11 GiB. Load time barely changes (about ±6 %).
>
> **Idea:** an *optional* `.eawrpak` per mod: each file zstd-compressed (open format,
> unpacker included), identical files stored once, MP3/Bink/PNG stored as-is.
> **Loose files still override the archive** (like MEGs today), so a single XML hot-fix
> still works. Raw folders and MEGs stay fully supported, and no mod would ever be required
> to pack.
>
> **The catch:** retail FoC can't read it, so a packed-only mod would be EAWR-only. Windows'
> own `compact /exe:lzx` on a mod folder already gets about 90 % of that saving with no
> format at all, and works for retail too (it doesn't shrink downloads, though).
>
> **Questions:**
> 1. Would you ship an EAWR-only archive, or is retail compatibility a must?
> 2. Do your players actually complain about mod size?
> 3. Which tools would need support first?
> 4. One archive, or split parts so updates download less?
> 5. Or should we spend the effort elsewhere (e.g. faster loading)?
>
> We plan to add this as an optional capability (no dates yet); the details are open, and we'd like to hear from you first.
