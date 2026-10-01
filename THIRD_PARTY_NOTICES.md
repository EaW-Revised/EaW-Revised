# Third-party notices

This file lists the third-party code the project vendors, links, derives from or fetches,
with its licence. Each section below names the pinned version and where it appears:

- **Vendored and linked:** Lua 5.0.2, EnTT, pugixml (under `third_party/`).
- **Code derived or ported (MIT):** alo-viewer; semantics cross-checked against
  pg-starwarsgame-lsp, PetroglyphTools and eaw-schema.
- **Fetched, not in this repository:** Godot and godot-cpp (the viewer), and the tools
  listed in the next section. The viewer package bundles the Godot, godot-cpp and
  project licence texts under `licenses/` (`apps/viewer/tools/export_package.py`).

The project is built and tested with separately installed CMake, Python, Microsoft
Visual C++, GCC, Clang and Ninja; their licences accompany their own distributions.
No original game files, shader source or game assets are part of this repository: the
player supplies them from their own installation.

Adding or reusing library code means adding its name, pinned version or revision,
licence, source URL, and the files or binaries it appears in, here.

## Build and test tooling (not shipped)

These are used by the build, CI or developer tooling only; none is linked into or
shipped with project binaries.

- GitHub Actions `actions/checkout`, `actions/upload-artifact` and
  `actions/download-artifact` (CI only), MIT License.
- glslang 16.5.0 (BSD-3-Clause, BSD-2-Clause, MIT, Apache-2.0 and other permissive
  terms, <https://github.com/KhronosGroup/glslang>) and SPIRV-Tools v2026.1
  (Apache-2.0, <https://github.com/KhronosGroup/SPIRV-Tools>): the local shader
  translator (`tools/shaders/install_tools.ps1` downloads them into ignored `out/`).
- Python packages in `requirements-dev.txt`: Pillow (HPND), numpy (BSD-3-Clause),
  jsonschema and referencing (MIT), PyYAML (MIT), mpmath 1.3.0 (BSD-3-Clause,
  <https://mpmath.org/>; the offline fixed-point constant and vector generator, whose
  integer outputs are checked in with their reproduction method) and psutil
  (BSD-3-Clause).

## Godot and godot-cpp

The production viewer (`apps/viewer`) links godot-cpp 10.0.0-stable
(commit `507ed9d840c01a3c5b2a39af8bb4000bfac30bf5`) and runs with Godot
4.7.2-stable (commit `ed1daf0bf001b61586d9930840f2f1394092c079`). Both are
MIT-licensed and fetched into ignored `out/godot/deps/`; neither source tree nor
engine binary is redistributed in this repository. Exact archive hashes, source
URLs and the licence files a viewer package bundles are pinned in
`apps/viewer/dependencies.json`. Godot's own `COPYRIGHT.txt` lists the third-party
code the engine includes (FreeType, mbedTLS and others) and ships in each package.

The optional Linux ARM64 package reproduction uses Arm GNU Toolchain 14.2.Rel1,
GPL-3.0-or-later with the GCC Runtime Library Exception. The helper reuses a verified
ignored toolchain cache and does not vendor or ship that toolchain. SDL3 and
wgpu-native were used only by an earlier, removed prototype and are not part of
the runtime.

## alo-viewer

The VFS MEG-v1 reader and archive lookup semantics in `src/vfs/vfs.cpp` are
derived from alo-viewer revision `9bb0053919cc5df8377610d4f91b11d956d6c2f4`,
<https://github.com/AlamoEngine-Tools/alo-viewer>, specifically its MIT-licensed
`src/Assets/MegaFile.*` and `src/Assets/Assets.*` implementations.

The portable P0-06 ALO/ALA CPU readers in `src/assets/asset_internal.hpp`,
`src/assets/model.cpp`, and `src/assets/animation.cpp` use the same pinned
revision's MIT-licensed `src/Assets/ChunkFile.*`, `Models.*`, `Animations.*`, and
the required public record declarations in `src/General/GameTypes.h` as format
reference evidence. The DDS/TGA readers are original implementations of their
documented byte layouts and do not reuse alo-viewer graphics or image-decoder code.

The P1-08 portable particle ALO reader, legacy-emitter conversion, plugin catalog,
and CPU behavior in `src/presentation/particles/` and
`tools/particle_inventory.py` are derived from the same pinned revision's
MIT-licensed `src/RenderEngine/Particles/*` and CPU update order in
`src/RenderEngine/DirectX9/ParticleEmitterInstance.cpp`. The P1-08 particle quad
builder in `src/presentation/particles/render.cpp` follows the same revision's
MIT-licensed `src/RenderEngine/DirectX9/ParticleRenderers.cpp` for quad corner order,
index order, texture-cell assignment, kite tail geometry and the legacy
renderer/selector table in `src/RenderEngine/Particles/ParticleSystem.cpp`; it is a
portable reimplementation, and no DirectX code, shader, or asset byte is copied or
linked.

The P1-04 spherical-harmonics lighting in `src/presentation/lighting/lighting.cpp`
ports the same pinned revision's MIT-licensed `src/RenderEngine/SphericalHarmonics.cpp`
(direction evaluation with the direction's Z negated, the Ramamoorthi-Hanrahan
irradiance-matrix packing and the ambient term), uses the default environment of
`src/config.cpp` (`Config::GetDefaultEnvironment`) and the angle convention of
`src/General/3DTypes.cpp` (`Vector3(zAngle, tilt)`). The D3DX SH basis is written
from its published constants; no D3DX or Wine code is used.

MIT License

Copyright (c) 2017 GlyphX Tools

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## EnTT

The deterministic simulation vendors the unmodified EnTT v3.15.0 single-include
header and license from commit `d4014c74dc3793aba95ae354d6e23a026c2796db`,
<https://github.com/skypjack/entt>. The vendored files are
`third_party/entt/single_include/entt/entt.hpp` and `third_party/entt/LICENSE`;
their verified SHA-256 values and public-handle boundary are documented in
`third_party/entt/README.md`.

The MIT License (MIT)

Copyright (c) 2017-2025 Michele Caini, author of EnTT

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## pg-starwarsgame-lsp

The Lua wire cross-check cases in `tests/ci/test_lua_debugger.py` are adapted
from the MIT-licensed bitstream, handshake, datagram, reliable-channel, and
Lua-message codec tests at v0.4.0 revision
`19b37b8777ea8e1058b29bfbff70babccc1287d2`. The signed variable-type boundary
in `tools/rig/lua_debugger/luadbg_harness.py` was cross-checked against that
revision and the debug build (LD-X02). No implementation library is vendored
or linked; the separately pinned Python client remains the wire transport.

The active patch-slot ordering and leaf-first multi-MODPATH evidence documented
for the VFS were cross-checked against pg-starwarsgame-lsp revision
`4461416d401b0f665bc1fe82aad60b90bd707fa9`,
<https://github.com/AlamoEngine-Tools/pg-starwarsgame-lsp>. No library from that
repository is linked into the runtime. The P0-05 variant-chain order, effective
value/provenance model, token append operation, and missing/cycle behavior in
`src/data/xml.cpp` are derived from its MIT-licensed
`PG.StarWarsGame.LSP.Core/Symbols/EffectiveObjectResolver.cs` and pinned tests.
The P1-10 MTD layout and top-left coordinate convention in
`src/assets/mega_texture.cpp`, `tools/inventory/mtd_inventory.py`, and their
synthetic tests are derived from the same pinned revision's MIT-licensed
`PG.StarWarsGame.LSP.Assets.Tests/Icons/MegaTextureFixture.cs` and
`PG.StarWarsGame.LSP.Assets/Icons/MegaTextureIconExtractor.cs`. No game asset
bytes or third-party fixture bytes are redistributed.

MIT License

Copyright (c) 2026 Alamo Engine Tools

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## PetroglyphTools

The P1-10 MTD field meanings and 81-byte record size were cross-checked against
PetroglyphTools revision `3be4a58549897baa4c64aba8ffefc5eea574c495`,
<https://github.com/AlamoEngine-Tools/PetroglyphTools>, specifically its
MIT-licensed `PG.StarWarsGame.Files.MTD` reader and metadata declarations. No
PetroglyphTools library is linked or redistributed.

MIT License

Copyright (c) 2026 Alamo Engine Tools

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## eaw-schema

The generated P0-05 XML classification table is derived at build time from the
accepted P0-09 inventory made with eaw-schema revision
`3e1b825a124fbc13b2293665f34a36dd4d4be80f`,
<https://github.com/AlamoEngine-Tools/eaw-schema>. The checkout and its 130
hashed schema files are not vendored into runtime code; the pinned revision,
source references, status, and applicability are retained in the inventory
contract and generated lookup.

MIT License

Copyright (c) 2026 Alamo Engine Tools

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Lua 5.0.2

The bounded script host statically links Lua 5.0.2 from the official upstream
archive <https://www.lua.org/ftp/lua-5.0.2.tar.gz>. The archive is vendored under
`third_party/lua/`; its verified SHA-256 is
`a6c85d85f912e1c321723084389d63dee7660b81b8292452b190ea7190dd73bc`.
The upstream C sources are unmodified except for one portability correction in
`third_party/lua/src/llimits.h`: Lua 5.0.2 declared a bytecode `Instruction` as
`unsigned long`, which is 32-bit on Windows LLP64 but 64-bit on Linux LP64. EAWR
uses `unsigned int` on its supported targets to retain Lua's specified 32-bit
instruction word and the converter/loader's four-byte ABI; the loader's existing
header-size validation continues to fail closed on an incompatible target.
`third_party/lua/CMakeLists.txt` is an independently written integration file that builds only the VM, auxiliary
library, base/coroutine, string, and table libraries.

Copyright (C) 2003-2004 Tecgraf, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## pugixml

The VFS and XML data loader link pugixml 1.15 to parse `MegaFiles.xml` and game XML.
The unmodified runtime source,
headers, configuration header, and license are vendored under `third_party/pugixml/`
from the upstream release archive:
<https://github.com/zeux/pugixml/releases/download/v1.15/pugixml-1.15.tar.gz>.
The verified release archive SHA-256 is
`655ade57fa703fb421c2eb9a0113b5064bddb145d415dd1f88c79353d90d511a`.
The vendored file SHA-256 values are:

- `LICENSE.md`: `0d0b3772af2fa45628a548a3b34583707ebcc68bb6f83ec48ca273aab4a510f1`
- `src/pugiconfig.hpp`: `981cd9ad3313878817d7b548c9929cc09d3bc42be2c0c1dcb0d77290fc98b92d`
- `src/pugixml.cpp`: `67c3892efba51d4e4eb6ce5609286fd2703aede6ba49a6d2130068cad09e0744`
- `src/pugixml.hpp`: `2555f950fd080e02ff16f36698ddf8d4a46d3a484b9337b4cbfc6300043be734`

MIT License

Copyright (c) 2006-2025 Arseny Kapoulkine

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
