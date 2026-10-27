# EaW Revised command-line tools

This archive is for modders and testers. To play, use the player download.
The eleven programs are in `bin/` (with `.exe` on Windows):

- `sim_headless`: runs simulations and replays without graphics.
- `sim_bench`: measures a synthetic space battle across worker counts and checks state hashes.
- `path_bench`: measures tactical path searches using your game installation, with space and land modes.
- `asset_scan`: inventories mounted archives and loose assets, reporting counts and loading errors.
- `asset_validate`: validates models, animations, textures and maps, or inspects an individual asset.
- `xml_scan`: loads XML data tables and reports references and diagnostics, with optional galactic reports.
- `scene_scan`: builds static scenes from effective maps and reports asset resolution without rendering.
- `sky_scan`: inventories the sky models, surfaces and shader families referenced by maps without rendering.
- `unit_scan`: loads unit tables and reports selected units, unresolved fields and references.
- `script_smoke`: loads a Lua script from your installation and runs a named entry function as a check.
- `hud_movie_prepare`: extracts requested HUD movies from your installation into a conversion cache.

Most scanners need your own
Empire at War / Forces of Corruption installation; no game data is bundled.
Keep generated reports and caches outside your installation.

On Windows, the tools may need the Microsoft Visual C++ Redistributable
(x64). If Windows reports a missing Visual C++ runtime DLL, install that runtime
from Microsoft. The Linux tools use the Linux runtime libraries instead.

`LICENSE` and `THIRD_PARTY_NOTICES.md` are included in this folder.
