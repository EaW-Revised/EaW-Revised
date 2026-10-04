# Godot RenderingServer prototype B

[Godot presentation decision](../architecture-decisions.md#adr-011-godot-presentation) selected this route. The prototype uses a Godot 4.7.2 GDExtension and direct `RenderingServer` RIDs for the frozen Hangar scene. `prototypes/godot/dependencies.json` pins Godot `4.7.2-stable` and godot-cpp `10.0.0-stable`. The scene has one bootstrap `Node3D`; simulation entities are represented by immutable Q24 snapshots, not scene-tree nodes.

## Build and run

```powershell
pwsh prototypes/godot/install_dependencies.ps1
cmake -S prototypes/godot -B out/godot/build/windows
cmake --build out/godot/build/windows --config Release
ctest --test-dir out/godot/build/windows -C Release --output-on-failure
```

Run `Godot_v4.7.2-stable_win64_console.exe --path prototypes/godot/project --` with `--eawr-game-root <game-root> --eawr-mod-root <mod-root> --eawr-report <private-json> --eawr-capture <private-png> --eawr-closeup-capture <private-png> --eawr-resize-probe --eawr-control-probe --eawr-benchmark`. On Linux, export with `godot-4.7.2 --headless --path prototypes/godot/project --export-release "Linux/X11" <binary>` and pass the same arguments to the exported executable.

The selected material is `MeshGloss.fx` / `sph_t0` / `sph_t0_p0`, expressed in Godot shader language. It keeps the source's quadratic irradiance and exponent-16 specular path; a separate spatial shader exercises modern vertex/fragment bindings. The frozen scene SHA-256 is `de673739583babf0a4541feafc6bd1f0c40da36788a962e7d49b284be7611a18`.

The historical acceptance marker is `resize/control/two captures/shutdown pass`: both Windows project and Linux exported runs produced one legacy draw, a modern shader draw, and the two captures. Linux used Mesa llvmpipe software. `--headless -- --eawr-headless-probe --eawr-benchmark` checks extension startup and core replay without claiming a rendered frame. ARM64 build/package feasibility was checked; native ARM64 rendering was not.

For an ARM64 rebuild, run `bash prototypes/godot/tools/build_arm64_package.sh`.
It explicitly selects the Arm GNU Toolchain 14.2.Rel1 compiler/binutils and
sysroot for both template and extension, exports with the pinned x64 editor,
and checks ELF/dependencies, PCK contents and the three-file package allowlist.
Its default cache is
`out/prototypes/sdl_wgpu/deps/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu`;
the historical directory name does not imply a tracked SDL dependency.

No reliable Godot implementation stopwatch was recorded. The effort field in
the prototype report must not be interpreted as measured person-hours.
