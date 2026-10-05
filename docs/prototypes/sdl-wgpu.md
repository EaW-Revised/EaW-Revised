# SDL3 and wgpu-native prototype A

[Godot presentation decision](../architecture-decisions.md#adr-011-godot-presentation) selected Godot RenderingServer. The SDL/wgpu source was removed from the current tree; reconstruct this historical prototype from commit `6728da1bcc69f5e18c77cc3bd49759a4dc379263`. Its manifest is `plan/inventories/prototype-a.json`; pinned dependencies and licenses are in that revision's `prototypes/sdl_wgpu/dependencies.json`.

## Format and reproduction

The frozen input is `prototypes/common/scene.json`: Hangar ALO geometry, `MeshGloss.fx` technique `sph_t0` pass `sph_t0_p0`, a BC3 sRGB texture, 1280x720, 120 warm-up frames, and 600 measured frames. The source-derived vertex/fragment shader is adapted for WebGPU position and separate texture/sampler bindings; the original `modern-smoke.wgsl` exercises WGSL vertex and fragment stages independently.

At the historical revision, adapt the shaders with `python tools/shaders/adapt_wgpu.py --vertex <vert.hlsl> --fragment <frag.hlsl> --out <shader-dir> --glslang <glslang> --spirv-val <spirv-val>`. Build with `cmake -S prototypes/sdl_wgpu -B <build-dir> -DEAWR_WGPU_ROOT=<wgpu-root> -DSDL3_DIR=<sdl-cmake-dir>`, then `cmake --build <build-dir> --config Release`. Run `eawr_sdl_wgpu --backend dx12 --game-root <game-root> --mod-root <mod-root> --scene prototypes/common/scene.json --shader-root <shader-dir> --metrics <csv> --capture <ppm>`; Linux uses `--backend vulkan`. `--exercise` performs orbit/resize and shutdown; `--closeup` uses supplemental framing.

The historical acceptance marker is `orbit/resize and shutdown`: the lifecycle exercise and captures completed on Windows D3D12 hardware and Linux Vulkan/lavapipe software. The five-tick core replay remained unchanged. The Linux package verification command is `cmake --build <build-dir> --target eawr_sdl_wgpu_package_check`; ARM64 was statically packaged but not run.
