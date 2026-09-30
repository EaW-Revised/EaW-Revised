# Source inputs and bounded findings

The engine loads MEG, XML, Lua, ALO/ALA, DDS/TGA, TED and audio from a user's
installation. No game or mod assets are redistributed. Compatibility preserves
XML/Lua names and semantics; mods load without a port.

## Local retail reference tree

A maintainer keeps an extracted FoC/EaW reference outside the repository (the
`<reference>/foc-data/` folder below). `eaw/` and `foc/` retain every archive and
loose file separately; `effective/` contains the FoC-winning files. Its
`MANIFEST.tsv` records hashes and provenance. The reference was generated with
`tools/unpack_megs.py` and the `tools/megx.py` reader at commit
`31298b44325b886d03089e09cb5b7afda5a36fd0`.
Regenerate into an empty destination with one low-priority Python process:

```powershell
$p = Start-Process -FilePath (Get-Command python).Source -ArgumentList 'tools/unpack_megs.py --game-root "<game root>" --output <reference>/foc-data' -WorkingDirectory (Get-Location).Path -PassThru
$p.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::BelowNormal
$p.WaitForExit()
if ($p.ExitCode -ne 0) { throw "unpack failed: $($p.ExitCode)" }
```

Research reads game data from `<reference>/foc-data/effective` instead of re-extracting.

## Shader inputs

The inspected EaW/FoC shader archives contain compiled FXO effects, whose D3DX
effect header is 01 09 FF FE. Parameter names, semantics, annotations,
technique/pass names and per-pass bytecode survive compilation. Material names
are the bindings referenced by ALO files and must be retained.

The published foc_shaders.zip input is pinned by SHA-256
`88b9cb03322aab9be7451968ca514e9d2c810973d83f65459fd8cb52e7f96f8d`:
104 fx and 18 fxh files. The inspected EaW 80 and FoC 82 effects each have a
published source counterpart. Eleven inspected Remake-specific effects lack one:
MESHADDITIVE2X, MESHALPHAGIRDERS, MESHBUMPCOLORIZEDETAIL,
MESHBUMPCOLORIZEVERTEX, MESHBUMPREFLECTCOLORIZE1, MESHBUMPSPECGLOWCOLORIZE,
MESHGLOSSFORSTARFIELDS, MESHSHIELDENGINES, MESHSHIELDFARSEER,
PLANET_DEATHSTAR and SKYDOMEOLD. This is coverage of the pinned inputs, not a
promise about other mod versions. See [translation](../shaders.md).

## Numeric capacity findings

The numeric inventory uses exact decimal text and a tag-name screen, not inferred
units. Maximum candidate TED dimension is 36,000; field order, legal overshoot and
accumulation bounds are unresolved. XML coordinate-like values are not tactical
map bounds. The recorded duration range includes 999999999999999.0 in
Laser_Defense_Ability Hungry_Force_Shield, with Space_Automatic activation and
0.2 recharge. The schema gives no special meaning for that outlier or the negative
sentinels. Keep the lexeme finite; see [fixed point](../fixed-point.md).

The gameplay XML root excludes Data/megafiles.xml and
Data/Text/xml/TranslationManifest.xml; this explains the 1,309 versus 1,311 file
counts. Regenerate plan/inventories/numeric-ranges.json with
tools/analyse_numeric_ranges.py using --xml-root, --ted-archive and --ted-root
inputs, then --output. Labels identify sources without private absolute paths.
