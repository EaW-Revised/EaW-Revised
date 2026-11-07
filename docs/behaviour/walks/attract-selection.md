# Main menu attract selection

Scope: FoC debug build `StarWarsI.exe`, its attract manager and effective game data. See [main-menu attract](../main-menu-attract.md) MAT-01 through MAT-04 and [mod loading](mod-loading.md) ML-02 for related rules.

- **From code:** The process-wide manager begins with its current map index before the first entry. An advance increments the index, wraps at the list length and loads the resulting `Demo_Attract_Maps` entry under `Data/Art/Maps/`. The inspected selection path does not draw a random number or use language to select an entry.
- **From code:** Initializing or shutting down that manager does not reset the index, so a subsequent advance in the same process continues the sequence. A fresh process initializes it before entry zero. This does not establish which menu transitions stop or advance the attract; MAT-01 and MAT-13 describe the inspected transitions.
- **From data:** The effective `GameConstants` document supplies `Main_Menu_Demo_Attract_Mode`, `Demo_Attract_Maps`, the indexed `Demo_Attract_Map_Cycle_Delay_Seconds`, and the idle timeout. A selected TED supplies the scene and placements; a map can also contain an object script (MAT-20), which is distinct from selection.
- **From code:** File lookup follows the first matching loose file in the ordered mod roots (ML-02). Therefore the effective constants document replaces a lower-priority document; individual maps also resolve through that lookup. This does not establish all archive precedence cases.
- **From footage:** No new multi-start retail capture was obtained for this selection walk. The owner's Republic at War report observes varying scenes; the exact transitions and whether a fresh process was involved remain unverified. The existing menu capture in MAT-14 establishes visibility, not selection order.

The code observation is not evidence of random first-map selection, cross-process persistence, or language-specific selection. Captures of consecutive advances and fresh launches remain necessary to reconcile the owner's observation with the debug build.
