# Patched Godot engine (EAWR fork)

We pin Godot 4.7.2-stable (`apps/viewer/dependencies.json`). To ship the Separate render thread
(`rendering/driver/threads/thread_model` = 2) we carry upstream fixes that are not in a stable release yet.
Separate is the game's default (`apps/viewer/project/project.godot`); `--render-thread safe` on the engine command line (launcher
`--safe-render-thread`, the player zip's `EAWR (Safe render thread).cmd`, preview `-RenderThread safe`) opts out per run. Anything that runs the
official (unpatched) 4.7.2 exe, such as the rig by default and `play-demo`, passes `--render-thread safe`.
The source lives in a public fork, not in this repository; nothing here is built by CMake.

- Fork: https://github.com/Thom-Ernst/godot (public, Issues off, Actions disabled: keep it so).
- `master` is a pure mirror of `godotengine/godot`, no commits of ours.
- `eawr/4.7.2-stable` is cut from tag `4.7.2-stable` (`ed1daf0bf001b61586d9930840f2f1394092c079`) with one commit per
  upstream fix. Each commit message names the upstream PR/issue and says "drop when upstream merges".
- We post nothing on `godotengine/godot` (no PRs, comments or issues). MIT notices stay in the fork; the game bundle
  ships the attribution.

## Carried patches

| Commit | Upstream | What it fixes |
|---|---|---|
| `5fc215c1e8abe37f98dbf92efc283f3befb05ea0` | [Godot PR 123391](https://github.com/godotengine/godot/pull/123391) (open) | Quit with the Separate render thread: `RenderingDevice::finalize()` ran on the main thread after the render worker stopped ("This function (finalize) can only be called from the render thread", 2 leaked ObjectDB instances, a driver crash in [Godot issue 124799](https://github.com/godotengine/godot/issues/124799)). Issues [Godot issue 119000](https://github.com/godotengine/godot/issues/119000), Godot issue 124799. |
| `fad06b57944fbf17833fe3c65dc9ff1d7446c639` | none (EAWR-authored) for [Godot issue 122206](https://github.com/godotengine/godot/issues/122206) | `RenderingServerDefault::texture_2d_update` queues an `Image::duplicate()` snapshot instead of the caller's Image, so the render thread never reads an Image the text server is still writing (`_texture_2d_update`: `p_image.is_null() \|\| p_image->is_empty()`). Not an upstream fix; drop when upstream fixes Godot issue 122206. |

Godot issue 122206 has no upstream fix PR as of 2026-10-24.

## Shipped runtime

The player and debug archives bundle the patched exe, not the official one (owner-delegated switch, 2026-10-24). It is the editor
build of fork commit `fad06b579` (`--version`: `4.7.2.stable.eawr.fad06b579`), built with MSVC and `lto=full`
(`python tools/offload/godot_build.py --sha fad06b57944fbf17833fe3c65dc9ff1d7446c639 --label patched-ltofull --scons-extra lto=full`).
The official exe is MinGW with LTO; the MSVC `lto=full` build matched it in Safe mode on two GPU hosts (PF engine-4 parity),
the MSVC build without LTO did not. Assets live on the fork release `eawr-4.7.2-fad06b579`
(https://github.com/Thom-Ernst/godot/releases/tag/eawr-4.7.2-fad06b579): `Godot_v4.7.2-stable-eawr-fad06b579_win64.exe.zip`
(the GUI exe, named `Godot_v4.7.2-stable_win64.exe` inside), `..._win64_console.exe.zip`, `SHA512-SUMS.txt`, `SHA256-SUMS.txt`.
Three pins change together with any new build: SHA-512 of the zip in `tools/release/fetch_pinned_godot.ps1`, SHA-256 of the exe in
`tools/release/package_release.py`, the identity in `apps/viewer/tools/export_package.py` (which still accepts the stock official
identity so the viewer export and probes keep working with the official exe). The remote-run harness (`tools/remote_godot`) keeps
its stock pin. Attribution: `THIRD_PARTY_NOTICES.md` and the bundled Godot `LICENSE.txt`/`COPYRIGHT.txt`.

## Building

`python tools/offload/godot_build.py --sha <fork commit> --label <name>` builds the Windows x86_64 editor with scons and MSVC
on a pool CPU host (it waits as a queue ticket and holds one build slot) and stores the exe, console exe, PDB, `build.log` and
`manifest.json` (source URL, branch, commit, flags, compiler, SHA-256) under
`<reference-dir>/godot-patched/<label>/<sha12>/` (the maintainer's reference directory). Binaries are never committed. On the build host the tree
`<base>\godot-build\src` is kept for incremental builds; delete it by hand when the disk is short.

Official release options come from `godotengine/godot-build-scripts` (`build-windows/build.sh`): `production=yes
debug_symbols=yes separate_debug_symbols=yes debug_paths_relative=yes use_mingw=yes d3d12=yes` plus the ANGLE, Mesa and
AccessKit SDK paths. We use the same options with MSVC instead of MinGW; the manifest records the difference.

## Moving to a new upstream version

```
git remote add upstream https://github.com/godotengine/godot
git fetch upstream --tags
git checkout -b eawr/<new-version> <new-tag>
git cherry-pick <our commits that upstream has not shipped yet>
```

Drop a commit when the pinned release contains its upstream fix (check `git merge-base --is-ancestor <upstream sha> <tag>`
or read the release notes); the table above and `apps/viewer/dependencies.json` change in the same PR.
