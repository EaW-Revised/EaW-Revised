# Owner kit runbook

An owner kit is a local player-layout artifact, not a release. Prepare an exact source commit, a label, a changes file, the pinned engine directory containing both executables, the pinned launcher zip, the reviewed Revised layer and an accepted player folder for layout comparison.
Use Python 3.10 or newer for these maintainer tools. Linux and Windows pool hosts require PowerShell 7.6.0 or newer; verify `pwsh --version` during host setup. The Windows provisioning script checks the machine-wide installation and fails with an update instruction if it is too old.

1. Cut on the Windows pool: `python tools/release/cut_kit.py --sha <commit> --suffix <suffix> --changes <file> --engine-dir <dir> --launcher-artifact <zip> --work <new-dir> --host any`. The kit label is the release version (`config/release-version.json`) plus `--suffix` (for example `c`); `--label <full label>` is accepted when it starts with the release version, and is the only choice for a game head older than the version file. An optional `--pin` selects the committed launcher pin; repeat `--extra <file>=<repo-path>` for additional files. A game head older than the host scripts needs `--extra` for `tools/release/host_package.py` too (see below). The cut creates an isolated kit commit without modifying the checkout, queues the relay build, stages inputs after a slot is acquired and verifies the fetched archive against the package receipt. Repeat `--host` to narrow the eligible pool when a proof requires it.
2. Finish: `python tools/release/finish_kit.py --work <dir> --out-root <new-output> --layer <reviewed-layer> --reference <accepted-player-folder>`. It verifies the patched engine, embedded identities, PDB links, launcher pin, checksums and layout; adds the Revised layer and kit notes; and refuses delivery if a check fails. Review `finish-report.txt` and the final archive hash.
3. Run the fresh-user and GPU verification on the exact finished bytes through the rig queue before delivery. That verification requires Windows and a GPU. Owner kits can be compiled and packaged on the Windows pool or on the Linux cross host (see "Linux and Windows kits" for what the two routes share and where they differ).

## The kit manifest

Every cut writes `<work>/kit-manifest.json` before it queues anything (`tools/release/kit_manifest.py`). Its `plan` pins the
kit commit and tree, the overlay files, the git blob id and SHA-256 of every file the packaging and finish steps run or read
(including `finish_kit.py` and `kit_manifest.py` themselves), the engine, launcher pin and Revised layer manifest, the changes
file hash, the route, and the orchestrating checkout (its commit, whether its scripts were clean, their hashes). A cut refuses
a checkout whose orchestrating scripts have uncommitted changes (`--allow-dirty-tooling` records that instead). When the host's
archive arrives the cut adds a `host` section (name, archive name, size, SHA-256, and the host's own receipt, below).

Both routes run one host-side script, `tools/release/host_package.py`, taken from the tree the host built (the kit commit).
The cut writes `plan.json` next to the manifest and stages it with the other inputs; the streamed host script carries the plan's
SHA-256 and the plan's git blob id of `host_package.py` as literals. The host compares the blob id of that file in its tree with
the literal before it runs it, and then `host_package.py verify` checks that the tree is the kit commit with no changed tracked
file (content compared through git's own line-ending rule, so a CRLF checkout of the same blob is not a difference), that the
staged `plan.json` hashes to the literal, that every staged input matches its transfer digest and, for the engine and the
launcher artifact, the hash the plan pinned, and that the Revised layer archive is safe; it stages the pinned engine. No script
of the operator's checkout is copied over the host tree: a tracked file that differs from the kit commit refuses the job. The packager itself is started by the same code on both routes: `host_package.py package` (the pool) and `package_cross.py` (Linux)
both build the one `package_release.py` command line with `host_package.packager_command` and print the `EAWR-RELEASE FILE` lines
with `host_package.release_file_lines`. What stays route-specific, because it depends on the host, not on the kit: where the built
binaries are (the pool stages a robocopy copy of the slot tree and takes the relay viewer from another slot; the Linux host packages
in place and looks each binary up by unique name), the Wine runner environment on Linux, and how the output returns (log lines and
scp on the pool, `package.tar.gz` on Linux). After
packaging, `host_package.py receipt` checks the tree once more, hashes every output and records the toolchain (Python, clang,
lld, Wine, PowerShell, git, Windows SDK; a tool that is not there is recorded as null). The pool prints it as one
`EAWR-HOST-RECEIPT` line in the package log and the Linux route returns it as `host-receipt.json` in `package.tar.gz`; the cut
refuses a receipt for another plan or commit, a dirty tree, inputs that are not the engine files and launcher artifact the plan pinned, no compiler or linker, a toolchain that does not fit the route (Linux host and Wine for the cross route, Windows host and PowerShell for the pool), or one that does not list the player archive that arrived, and
stores it in the manifest's `host` section, where `kit_manifest.py verify` checks it again. The receipt is written by the
host and is host-attested, like the rest of the section. If the transfer of inputs fails, the client leaves a `failed` marker on
the host, so the job fails within seconds and not after the 30-minute wait for its inputs.

`finish_kit.py` must be run from the kit commit's own bytes: it refuses unless its own file and `kit_manifest.py` equal the
plan's blobs (for a kit commit that predates them, add both with `--extra` at the cut). It then verifies the manifest and the
player archive before it extracts anything, takes the engine pins, README template, launcher pin and packager modules from
`git archive` of the kit commit, compares the unpacked `launcher/` with the pinned artifact, compares `--layer` with the kit
commit's `revised-layer-manifest.json` (`--allow-unreviewed-layer` ships a different layer and records that), and judges the
layout against each `--reference`: names must match except for the built-in allowances and `--allow-diff` patterns, and the
engine, launcher and library files must be byte-equal to the reference. `--allow-diff` refuses a pattern that could hide an exe,
dll, engine or launcher file (`--allow-binary-diff` names those explicitly); every pattern used, every hidden name and every file
whose bytes differ is recorded in the `finish` section. Name-specific patterns are refused too: an `--allow-diff` pattern that matches any exe, dll, `.pck`/`.so`, engine, launcher, `project/bin` or
`Revised` file in the kit or a reference is rejected (`--allow-binary-diff '*'` as well). Three rebuilt files cannot carry the build identity
(`EAWR.exe`, `EAWR Mods.exe`, the Bink helper); finish records their SHA-256 and whatever identities they embed under
`identityless_binaries`, so the exemption is visible, but their bytes are not compared with anything beyond the name. Pass
`--expect-base <commit or ref>` to refuse a kit cut from anything else.

The delivered kit gets a sidecar `<kit>.zip.manifest.json` beside `.zip` and `.zip.sha256`; the kit folder itself is unchanged.
Check a delivered kit with
`python tools/release/kit_manifest.py verify <kit>.zip.manifest.json --zip <kit>.zip --folder <kit folder> --repo <checkout> --expect-base <commit> --require-finish`.

**What this does not prove.** The manifest is hash-pinned, not signed (signing is out of scope, as for the rest of the unsigned
release policy). It catches accidental or partial edits, and `--repo` checks it against git objects: the kit commit, its
tree, its parent, the files it changed and every payload blob. Someone who can rewrite the manifest, the zip and the sums together
can still produce a set that verifies for a player they built themselves, so keep delivered kits where only you can write, and pin
the commit you expect with `--expect-base`. The `host` section is host-attested: the client compares the download with the
host's own log or bundle, so it shows what arrived, not that the host built it honestly.

## The release package

A release uses the same cut, host and finish steps with `mode: "release"` in the manifest. It differs from a kit in what it
must prove, and `kit_manifest.py verify --release` refuses it unless all of it holds:

- **The tag.** `cut_kit.py --release-tag v<version> --layer <reviewed Revised layer> ...` runs in the checkout that holds the
  tag and refuses unless the tag is an annotated tag object whose signature `git verify-tag` accepts (`git tag -s`), points at
  `--sha` (default: the tag's commit), and equals `v` plus `config/release-version.json` at that commit. The plan records the tag,
  the tag object, the commit and the verified signature (`plan.release`). Git tag signing is separate from the code signing of the
  binaries, which stays outside this pipeline.
- **Nothing added to the tag.** The label is the release version, with no suffix. `--label`, `--suffix`, `--pin`, `--extra` and
  `--allow-dirty-tooling` are refused: the kit commit differs from the tagged commit only by the label file and the tagged
  commit's own launcher pin, so the scripts that cut and package it are the tagged commit's, and the orchestrating checkout
  must be clean.
- **Release names.** The host packager gets the version `v<release version>`, so the archives are `eaw-revised-v0.3.3-...`, the update manifest's tag is `v0.3.3` and its channel `stable`; only a kit carries its label and kit commit in the names.
- **A public package.** The reviewed layer is compared with `revised-layer-manifest.json` before anything is queued, zipped
  reproducibly and staged to the host, which runs the packager with `--public` (the exact reviewed layer, release symbols
  retained). The host receipt records `public: true`. The cut downloads every file the host wrote (player, tools and viewer
  archives, the packager's `SHA256SUMS.txt` and `update-manifest.json`), each checked against the host's hashes, and lists all of
  them in the manifest's `host` section.
- **Finish does not change the package.** `finish_kit.py --work <dir> --out-root <dir> --reference <accepted player folder>` for a
  release accepts no `--layer`, `--allow-unreviewed-layer`, `--allow-diff` or `--allow-binary-diff`. It runs the same checks (engine,
  identity, PDB links, launcher pin and `launcher/`, the layout against the reference, sums), compares the packaged `Revised/`
  with the reviewed manifest, adds no README section and no layer, does not rebuild the archive, and delivers the host's files
  byte for byte, so `update-manifest.json` and `SHA256SUMS.txt` still describe what is delivered.
- **Evidence.** The Debug archive is not an output of this manifest; it comes from a separate Debug build and package, so its fresh-user receipt is bound by the archive hash the receipt names, and the Debug archive uploaded with the release must be that archive. After the fresh-user runs on the delivered player and the Debug archive and the full replay76, add them:
  `python tools/release/release_evidence.py add <name>.zip.manifest.json --replay76 <run>/receipt.json --fresh-user-player
  <run>/fresh-user-verdict.json --fresh-user-debug <run>/fresh-user-verdict.json`. The tool checks that the replay76 receipt credits the full
  19-fixture x 1/2/4/8 run, that each verdict passed with a receipt that matches its archive, and that the player verdict
  names the delivered archive; the manifest then holds only their SHA-256 (the files stay under `out/`), bound to the plan and the
  finish section. The sidecar and its line in `.zip.sha256` are rewritten. `kit_manifest.py verify <sidecar> --zip <zip> --folder
  <folder> --repo . --release` is the last gate before the draft release; it publishes nothing.
  A release that ships no Debug archive (0.3.3) passes `--no-debug-archive` instead of `--fresh-user-debug`; the manifest then records that no Debug archive ships, and `verify --release` accepts exactly that record (a missing Debug entry is still refused).

The delivered kit and release archives are reproducible: finish writes the kit archive with entry times from the kit commit's
committer time, sorted names and fixed modes, so the same kit, layer and commit give the same zip bytes.

## Linux and Windows kits

A kit cut on the Linux cross host is meant to match one cut on the Windows pool. Compared with a pool kit of the same commit:

- **Same on purpose:** the file list, the pinned engine, the unpacked launcher project and the Revised layer (finish checks all four). The packaged text files (the viewer project, `LICENSE`, `THIRD_PARTY_NOTICES.md`) carry the line endings a Windows checkout gives them: the Linux route re-materialises exactly those tracked files with git's own `core.autocrlf` rule before packaging, so `.gitattributes` decides which stay LF. `launcher-pin.json` is written with CRLF on every host. The cross build passes the label in `tools/release/kit-label.txt` to CMake like the Windows scripts do, so the build identity, `runtime.json` and the packager's label check use the kit label.
- **Different by nature:** the rebuilt executables and the viewer DLL (a different compiler and SDK stack on each route), and the files that carry their hashes or the kit's version: `runtime.json`, `SHA256SUMS` and `README.md`. Against the frozen 0.3.3c player a Linux kit differs in exactly these nine files and nothing else; finish lists them under `layout[].changed_content`.
- **Not produced by the packager: the hand-made shortcuts.** Shortcuts are test-kit conveniences that the operator makes by hand on Windows after finish; no tool generates, pins or checks them. They embed an absolute path, so they are location-bound. Compare against a reference that has them with an explicit `--allow-diff "*.lnk"`; the manifest records the pattern and the hidden names. A long-term toggle belongs in the launcher's options, not in shortcuts. Every launch variant of a kit gets a Particles 0.4 twin (owner, 11-10): **`EAWR (Particles 0.4).lnk`** (target `EAWR.exe`, arguments `-- --eawr-live-particle-detail 0.4`) and **`EAWR Mods (Particles 0.4).lnk`** (target `EAWR.exe`, arguments `--mods -- --eawr-live-particle-detail 0.4`); a kit with another launch variant gets a twin for it too. Other shortcuts are made only when the kit's change list asks for them. The two **Instanced-hulls shortcuts** (`EAWR (Instanced hulls).lnk`: target `EAWR.exe`, arguments `-- --eawr-instanced-hulls`; `EAWR Mods (Instanced hulls).lnk`: arguments `--mods -- --eawr-instanced-hulls`) belong to kits built before rest-pose hull batching became the default. Once a kit's commit has that default, `--eawr-instanced-hulls` changes nothing: drop both shortcuts, and make an opt-out shortcut only if the viewer has gained an opt-out flag. The shortcut rules are for test and preview kits only: a release package is exempt from all hand-made shortcuts, the Particles twins included, because they embed absolute paths and a release is delivered byte for byte as the host made it (a release finish judges the layout without `--allow-diff`, so a reference that carries `.lnk` files is not a valid release reference). Instanced hull batching is on by default (the old `--eawr-instanced-hulls` argument is ignored); to bisect a suspect case a tester adds `-- --eawr-no-instanced-hulls` to such a shortcut, and the startup log then says `EAWR instanced hulls: off`.

A cut no longer pauses or holds a specific host. The pool's own FIFO slot lock is held for the job, so a second cut of the same commit cannot hold up or be mistaken for this cut. No process or ticket belonging to another job is changed.

The Linux cross route produces the same player folder as the pool apart from the rebuilt binaries and the files that carry the build hash or version (`README.md`, `SHA256SUMS`, `runtime.json`); the hand-made shortcuts are not produced by either route. Both routes still need the fresh-user and GPU verification on the finished bytes.

For a Linux cross-host, replace `--host any` with `--cross-host <host> --wine <base-relative-wine-executable>`; add
`--wine-lib-dir <base-relative-directory>` when that Wine installation has private shared libraries. The client stages
the inputs only after the VM slot is acquired, builds and packages under that slot, and fetches a hash-verified bundle
containing the archives and private symbols. The host must have a working Wine runtime, its cross compiler, and its
normal queue and toolchain setup. See `docs/releasing.md` for packaging an already completed cross-build tree.
