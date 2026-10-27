# Optional shader compiler package

The Windows x64 distribution is pinned in
[`third_party/shader_runtime/lock.json`](../third_party/shader_runtime/lock.json).
The package uses separate unmodified vkd3d 2.1 and SPIRV-Cross
`vulkan-sdk-1.3.290.0` DLLs through their public C exports. The latter exposes C
ABI 0.61.0. No import library, private compiler representation or CLI output is
an application interface. The package has no player-time downloader.

`Runtime` starts disabled. Nothing currently calls it from renderer admission or
mod activation. An explicit future activation job can call `open` with the
package directory. Missing, replaced, incomplete, unsupported-platform,
missing-export and ABI-mismatched packages return a diagnostic and leave hand
adapters available. Developer builds compile and run without the optional
package or upstream development headers. The normal build never fetches it.

## Offline build and verification

Maintainers fetch only the exact source URLs in the lock and verify their
SHA-256 values before extracting/building. No floating source fetch belongs in
CI. The vkd3d entry records the retained build's configure flags, header-source
pins and MinGW 9.3 win32/SEH compiler. Build it out of tree with those header
include directories and `make -j2 libvkd3d-shader.la`.

[`build_cross.sh`](../tools/shader_runtime_package/build_cross.sh) builds the
unmodified SPIRV-Cross shared C API on the supervised Linux CPU host using its
existing FIFO slot helper and queue helper. It accepts a previously verified
archive, output directory, slot helper and queue helper. It uses MinGW-w64
13.2 win32/SEH, CMake 3.28.3 and Ninja 1.11.1, two compiler jobs, Release,
`SOURCE_DATE_EPOCH=0`, GLSL only, exceptions enabled, shared C API enabled,
and static GCC/C++ runtimes within this separate DLL. Static SPIRV-Cross
libraries, the CLI and unused target backends are disabled. The exact compiler
and linker executable hashes and distribution versions are recorded in the
lock. No upstream source file is edited. Build outputs and logs stay external.

Arrange the DLLs, notices, original source archives and matching public headers
at the paths listed in the lock in an offline input directory. Assemble into a
fresh directory and verify it:

```text
python -m tools.shader_runtime_package.package --assemble <inputs> --stage <package>
python -m tools.shader_runtime_package.package --verify <package>
python -m tools.shader_runtime_package.abi_probe <package>
```

Verification compares every file, size and hash with the trusted source-tree
lock, refuses extras and symlinks, checks PE32+ x64 import tables against the
full declared closure and checks the publish manifest pin. Delay-loaded imports
are refused. The only external imports are Windows system DLLs; Windows system
files are never copied into the package. `abi_probe` uses invented shader input
and exercises version checks, compilation, SPIRV-Cross context/child ownership,
error storage, release and destruction without emitting shader text.

Set `EAWR_SHADER_RUNTIME_TEST_PACKAGE` to the offline package directory to add
the real ABI and relocated-install control to the Python package tests and the
registered `presentation_shader_runtime_package` native CTest. The native test
returns skip code 77 when that optional offline package is absent. On a
qualification lane, stage the verified offline package and pass its path in
this environment variable to that job; both real native and Python C ABI tests
must run. Use the staging directory's resolved direct local path if the host's
usual build base is a junction. The
ordinary C++ tests use invented DLL doubles to exercise disabled/missing,
integrity and ABI-mismatch refusals. Every successful or failed open owns its
module handles; vkd3d allocations go to vkd3d free functions, and all
SPIRV-Cross child objects/text belong to their context. Copy output needed after
free/release/destruction first. Never retain a function pointer past `close`.

The native loader hashes the trusted file list before any code is loaded. It
opens and pins every directory from the local drive root through the package
and its member directories, denying write/delete while checking that no
component is a reparse point. It derives the final DOS package path from the
guarded directory handle. Logical member identities retain forward slashes;
only the paths passed to Win32 use native separators. After loading each module,
it opens the loaded module's reported file and compares its volume serial and
file ID with the still-guarded, verified file; a mismatch unloads and refuses it.
File and ancestor guards cover the hash/load interval and are closed immediately
after all libraries load, and on every error path; module references remain
owned until close/destruction. The tests attempt an ancestor-to-junction swap
at the hash/load barrier and require it to be blocked before code can load.
It loads dependencies first from absolute paths, restricts imported-library
search to the DLL's package directory and System32, and refuses a different
already-loaded module with the same name. Reuse requires a module reference
already owned by this loader and the
same verified artifact hash: pathname equality cannot prove the contents of an
older mapped image after a filename replacement. Independent Runtime owners
retain separate module references. It does not search PATH, the working
directory or mod directories.

This path check covers only the optional compiler package and its ancestors.
It does not inspect the game root or mod roots. A legitimate junction/symlink
above the compiler install is refused with a diagnostic naming the affected
directory: `shader translation unavailable: package path contains a
junction/symlink at <dir>`. That refusal keeps the game usable through the
hand-adapter fallback ladder; it must not block startup or crash the game.
Placing the compiler package on a direct local path permits translation without
changing a player's linked game/mod installation.

## Distribution and replacement

`tools/release/package_release.py --shader-runtime <package>` explicitly adds
the verified package to Windows viewer/player/Debug archives. Omitting that
argument preserves dependency-free developer/release packaging during this
integration phase. The player manifest records the package-manifest hash. The
source offer also sits beside the top-level third-party notices. The existing
project binary build-identity checks and private-symbol/temporary-file refusal
remain in force; no blanket third-party filename exception bypasses them.

See the [source and replacement notice](../third_party/shader_runtime/SOURCE-OFFER.md).
For a rebuilt library, review its imports and licences, update the artifact
hashes/toolchain in a separate replacement lock and generate a matching identity
table using `package.py --lock <replacement-lock> --header <identities.hpp>`.
The shipping table is `third_party/shader_runtime/identities.hpp`; its exact lock
digest also appears in `tools/publish_scan/config.json`. Review all three
together for a shipping update. An application rebuilding for a local replacement
may instead supply its own trusted `RuntimeFile` table to `Runtime::open`.
Retain the public ABI version checks and LGPL replacement/debugging freedoms.
No signing key is required. An untrusted package cannot override its own hashes.

Only Windows x64 has a qualified artifact in this lock. Other platforms report
unavailable and retain hand adapters until their own artifact, closure and ABI
receipts are reviewed. This package does not qualify translated appearance,
source-effect parsing or battle-time activation.
