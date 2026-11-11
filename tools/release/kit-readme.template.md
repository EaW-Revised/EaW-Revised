# EaW Revised @LABEL@: local test kit (not a release)

This is a local test kit cut for the owner. It is **not** a release: it is unsigned, not published and not meant to be
passed on. It is laid out like a release player zip.

- Built from commit `@BASESHA@`; build identity @SHA8@ and kit label `@LABEL@`. @EXTRAS_TEXT@
- Launcher @LAUNCHER@. Godot engine: the pinned EAWR build (patched; checked by the cut).
- Nothing was started or played from this build before it was handed over: no smoke run, no tests.
- Packaged from the build outputs (symbols required); checked after the fact: pinned engine, build
  identities, required PDB links, and the file list against the previous kit layout.

## Changes since the previous kit

@CHANGES@

## Start

1. Extract the whole zip into a folder you can write to (not from inside the zip).
2. Double-click **`EAWR.exe`**.
3. **`EAWR Mods.exe`** starts the mod manager.
4. You need your own copy of Empire at War with the Forces of Corruption expansion; nothing from the game is
   included.

The player guide follows.

---
