# Shader bridge gate

Opt-in research harness for the shader conversion bridge. The default project build does not discover,
compile, load or require vkd3d or SPIRV-Cross. This is not runtime integration.
Only invented bytecode and compiler/harness code live here. Generated installed
effect source, bytecode and GPU captures must remain in ignored local output.

Build on Windows with the public headers of an external, unmodified official
vkd3d release. No import library is used:

```powershell
cmake -S tools/shader_bridge_gate -B out/shader-bridge-build `
  -DEAWR_ENABLE_SHADER_BRIDGE_GATE=ON `
  -DEAWR_VKD3D_INCLUDE_DIR=<external-release>/include
cmake --build out/shader-bridge-build --config Release
out/shader-bridge-build/Release/eawr_shader_bridge_gate.exe `
  <external-dll> synthetic <private-output>
```

The root build exposes the same option, OFF by default. The standalone build
also defaults OFF. The executable resolves compile/free/version functions with
`LoadLibraryExW` and `GetProcAddress`, logs compiler status and writes SPIR-V.
Pass a standalone shader bytecode path instead of `synthetic` for private tests.
Effect-container parsing and technique selection remain outside this executable.

Use the external SPIRV-Cross CLI to generate GLSL 330 and its JSON reflection.
Then run `bridge.py --vs <GLSL> --ps <GLSL> --vs-reflection <JSON>
--ps-reflection <JSON> --policy <JSON> --out <private.gdshader>`.
The policy maps `vs.inputs` by reflected name to test vertex expressions and
`ps.samplers` by combined-sampler name to material bindings. It is an explicit
experiment input, not inferred engine semantics.

The bridge tokenizes and parses declarations, expressions and straight-line
assignments into a syntax tree. A separate emitter lowers stage interfaces by
reflected location, uniform blocks into individually bindable static registers,
vertex position into `POSITION`, and the opaque fragment output into `ALBEDO`.
The production material upload owns stored-value output conversion. Unknown
syntax, dynamic constant indexing, missing resources and incompatible varyings
are rejected. Control flow, derivative operations, arbitrary resource types,
alpha/blend state, preshaders and engine bindings need later work. Acceptance of
these small cases does not qualify arbitrary mod shaders.

Run the offline feature contracts with `python -m unittest discover -s
tools/shader_bridge_gate -p test_bridge.py`. A GPU receipt is required before
claiming this path works through the production material route.

The separate `gpu/` project builds `eawr_shader_bridge_probe` against the pinned
external godot-cpp cache, using `cmake -S tools/shader_bridge_gate/gpu -B
<build-dir> -DEAWR_ENABLE_SHADER_BRIDGE_GATE=ON
-DEAWR_GODOT_CPP_ROOT=<accepted-cache>` and the viewer's normal compiler/cache
options. It compiles the production renderer sources unchanged. The rig stages
the resulting DLL with its normal probe artifact receipt. `test_gpu.py` consumes
`EAWR_SHADER_BRIDGE_GPU_TEST=1`, `EAWR_SHADER_BRIDGE_PROBE_LIBRARY`,
`EAWR_SHADER_BRIDGE_OUTPUT`, and a private manifest artifact named by
`EAWR_JOB_BRIDGE_MANIFEST`. Each manifest case provides a name, generated shader
text, vector/texture bindings, and optionally a production legacy selector.
The test checks engine errors, captures every panel, compares the two invented
shader profiles to an independent reference, and checks their stored texels
after a non-unit transform. Installed-effect captures are for inspection;
they do not by themselves establish numerical equivalence to a hand adapter.

Controlled manifests may provide a per-sampler `sampler_states` policy of
`nearest_clamp` (the original default) or `linear_repeat`. The probe accepts
separate RGBA8 texture descriptions (dimensions plus bytes or a private raw
file), explicit lighting, and UV bounds. `@hand-control` exports the production
specular/glow shader into the same modern upload route, substitutes the
orthogonal test mesh's `BINORMAL` for authored-binormal coefficients, and
applies only the selected sampler hints. Its optional `flat_normal` control
substitutes an exact `(0.5,0.5,1)` sample; an unchanged legacy-selector control
can test the export seam separately. These are opt-in differential controls,
not production changes or general tangent-frame qualification.

Every GPU launch requests Separate rendering and requires an observed callback
on a different engine thread. The suite records the engine log, driver versions,
capture hashes, and per-pair mean/max RGBA deltas and changed RGB pixel counts
from the manifest's `comparisons`. Differential residuals are evidence, not a
new relaxed acceptance threshold. Opaque ALBEDO still does not preserve effect
output alpha; a captured opaque alpha channel cannot qualify that behaviour.

External dependency distribution is not implemented here. A future separately
loaded vkd3d distribution needs its LGPL text, notices, exact version and source
availability; any modified implementation belongs in a separate LGPL fork.
Adding SPIRV-Cross as a project dependency remains an owner decision.
