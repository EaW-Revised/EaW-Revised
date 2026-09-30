# TerrainWater technique selection

## Interface contract

- Inputs:
  - **Water family** `F`, an integer taken from TED water mini `0x16`. Values: 0 none,
    1 water, 2 lava, 3 ice.
  - **Requested shader detail** `S_req` (0 = fixed function, 1 = DX8, 2 = DX8ATI,
    3 = DX9) and **requested water detail** `W_req` (0, 1, 2). The traced preset
    path supplies these requests. The options-screen caller remains untraced.
  - **Device pixel-shader version** `PS`, as a major/minor pair (for example 1.4, 2.0).
  - **Device vendor ID** (PCI vendor, for example 0x1002).
  - Whether the device supports a two-channel signed 8:8 bump (du/dv) texture format for
    the current display format (`V8U8` below).
  - Whether the device accepts each technique (`valid(t)`), as Direct3D effect validation
    reports it.
- Outputs:
  - Applied levels `S` and `W`.
  - The selected technique name (or none) for the water effect.
  - The texture resources bound for the water draw, and whether reflection and refraction
    passes are rendered.
- Cadence: requests are queued. They are applied at the start of a later rendered frame
  (R-05). Technique selection runs when effects load, after the device is created or reset,
  and whenever an applied level changes.
- Retained state: the applied levels, the latest unapplied request for each level,
  and each effect's selected technique.

## Ordered behaviour rules

### Settings and device capability

| Rule | Behaviour |
|---|---|
| R-01 | Capability ceiling for shader detail: PS ≥ 2.0 allows 3, PS ≥ 1.4 allows 2, PS ≥ 1.3 allows 1, otherwise 0. All comparisons are inclusive. A shader request above the ceiling is reduced to the ceiling when it is made. |
| R-02 | On the traced preset and device-create/reset paths, the water-detail ceiling is 2 for PS ≥ 2.0, 1 for PS ≥ 1.3, and 0 otherwise. Requested levels of 0–2 are reduced to that ceiling on these paths. Other request paths are G-06. |
| R-03 | A water request of exactly 1 becomes 0 when the device lacks `V8U8` support. That check happens before the R-02 ceiling. Requests of 0 or 2 skip it. This applies to the traced preset and coupling paths; the options-screen caller has not been traced. |
| R-04 | Applying a preset requests shader detail first, then water detail. Each request is made only when the preset value differs from the currently applied value. |
| R-05 | At render-begin, a requested level replaces its applied level if they differ. Each request is then considered handled. If either applied level changed, every loaded effect selects its technique again (R-08). |
| R-06 | Coupling runs in the same render-begin step, after R-05. (a) If applied `S` < 3 and applied `W` > 1, request water 1 (subject to R-03). (b) Then, if applied `S` < 1 and applied `W` ≠ 0, request water 0. These requests take effect at the next render-begin, one frame later. When both apply, (b)'s request replaces (a)'s. |
| R-07 | When the device is created or reset, both applied levels are re-clamped to the R-01/R-02 ceilings and applied, without the R-03 `V8U8` check. Every effect then selects its technique again. |

Steady states after coupling settles without a new request are: `S` = 3 with `W` of 0, 1 or 2; `S` of 1 or 2
with `W` of 0 or 1; `S` = 0 with `W` = 0.

### Technique selection (all effects)

| Rule | Behaviour |
|---|---|
| R-08 | Techniques are examined in the order the effect declares them. The **first** one meeting every condition below is selected. (1) Its name is not the reserved name `max_viewport`. (2) Its `LOD` string annotation has a rank ≤ applied `S`. Ranks: `FIXEDFUNCTION` 0, `DX8` 1, `DX8ATI` 2, `DX9` 3, compared ASCII case-insensitively. A missing or unrecognised `LOD` counts as rank 3. (3) If it has an integer `WaterLOD` annotation, that value is ≤ applied `W`. Without one, water detail does not constrain it. (4) The device vendor ID is not listed in its `VENDOR_FILTER_0`, `VENDOR_FILTER_1`, … integer annotations. Those are read in order, stop at the first missing index and never go past index 9. (5) If its `SWVP` annotation is true, software vertex processing is enabled before validation. (6) The device validates it. If validation succeeds after step 5 enabled software processing, it is switched back off before selection; after a failed validation it stays enabled while later techniques are tried. The `CPUSKIN` annotation does not affect selection. |
| R-09 | This is **not** "the highest technique ≤ the shader level", which is what `alo-viewer` does. Retail uses declaration order and first match. It gates on `WaterLOD`, treats a missing `LOD` as the highest rank, and treats `VENDOR_FILTER_n` as an exclusion list. `alo-viewer` ignores `WaterLOD`, maps unknown `LOD` strings to fixed function, skips techniques that have no `LOD` in EaW mode, and treats `VENDOR_SPECIFIC` as an inclusion. Under `alo-viewer` rules, `t0` would be chosen on any DX9 setting, whatever the water detail. |
| R-10 | If no technique qualifies, the effect reports a load/validation failure and has no selected technique. |
| R-11 | For the selected technique, a trailing pass whose `AlamoCleanup` annotation is true is recorded as a cleanup pass. The effect also records, per texture parameter, whether the selected technique uses it. R-17 depends on that record. |

### TerrainWater specifics

| Rule | Behaviour |
|---|---|
| R-12 | Water family `F` from mini `0x16` picks the effect: 1 → `TerrainWater`, 2 → `TerrainLava`, 3 → `TerrainIce`, 0 → no water effect is drawn. No other TED mini feeds technique selection. |
| R-13 | Retail `TerrainWater` declares, in order: `t0` (`LOD` `DX9`, `WaterLOD` 2, one pass), `t1` (`DX8`, 1, a draw pass plus a cleanup pass), `t2` (`FIXEDFUNCTION`, 0, a draw pass plus a cleanup pass). The base and patch copies of the compiled effect agree on all of this, and so does the published FoC source. |
| R-14 | From R-08 and R-13: `t0` qualifies only when `S` = 3 and `W` = 2. `t1` qualifies when `S` ≥ 1 and `W` ≥ 1. `t2` qualifies at any levels. Each also needs device validation. The fallback order is therefore t0 → t1 → t2. A technique that fails validation passes selection to the next one that qualifies. It never raises or lowers the applied levels. |
| R-15 | The water draw decides which resources to bind from the selected technique's **name**. Exactly `t2` gets the fixed-function bindings, exactly `t1` gets the DX8 bindings, and any other name gets the `t0` bindings. |
| R-16 | Resources by technique. **t0**: `Bump0Texture` and `Bump1Texture` are both the water bump texture (mini `0x1d`); it also samples reflection, refraction, the scene sky cube and fog-of-war. **t1**: `Bump0Texture` uses a signed du/dv (`V8U8`) texture derived from the water bump texture. That texture is created or released according to bump availability only while applied `W` is exactly 1; if present, it is retained after `W` leaves 1. `BaseTexture` is bound to the water base texture (mini `0x1e`), but the selected program does not sample it. The reflection target is sampled, fog-of-war is on stage 2, and a per-frame 2×2 bump-environment matrix is set on stage 1. **t2**: `BaseTexture` is the water base texture (mini `0x1e`) with a base-texture transform; fog-of-war is on stage 1, and the water colour is the texture factor. t2 uses no render target. |
| R-17 | Reflection and refraction passes. For family 1, the reflection pass is rendered when the selected technique uses the reflection-semantic texture (t0 and t1). The refraction pass is rendered when it uses the refraction-semantic texture (t0 only). Family 3 (ice) also gets a reflection pass when its technique uses reflection. Both targets are square, size `Water_Render_Target_Resolution` clamped to 256…1024 (EaW `config.meg` and `patch2.meg` GameConstants copies set 512), 32-bit ARGB, with a shared depth surface. If a target cannot be begun, its pass is skipped. |
| R-18 | When a map loads, an empty name in mini `0x1d`, `0x1e` or `0x1f` loads the engine's generic white texture instead. |

### Authored water minis (not selection inputs)

These minis never change which technique is selected. They are listed so EAWR-27 does not
need to reopen them. `0x15` is stored next to the family field, but none of the selection
or draw paths above read it (G-07). `0x16` is the family (R-12). `0x1d`, `0x1e` and `0x1f`
are the bump, base and third texture names (R-16, R-18). `0x1a` is the water RGB colour
and `0x20` its alpha. `0x17` splits that colour between the t0 reflect and refract
colours. `0x18` and `0x19` scale reflection and refraction distortion (each × 0.02).
`0x21` yields a stored value and a derived inverse value; `0x22` is stored.

## Truth table: `F` = 1 (TerrainWater)

Assumes every technique validates unless a column says otherwise.

| Applied `S` | Applied `W` | Selected | If it fails validation | Reflection / refraction passes |
|---|---|---|---|---|
| 3 | 2 | t0 | t1, then t2 | yes / yes |
| 3 | 1 | t1 | t2 | yes / no |
| 3 | 0 | t2 | none (G-01) | no / no |
| 2 | 1 | t1 | t2 | yes / no |
| 2 | 0 | t2 | none | no / no |
| 1 | 1 | t1 | t2 | yes / no |
| 1 | 0 | t2 | none | no / no |
| 0 | 0 | t2 | none | no / no |
| 2 or 1 | 2 (for one frame, before coupling) | t1 | t2 | yes / no |

The request path also has this device-format edge before the applied-level rows:

| Request and device | Applied levels | Selected | Resource gate |
|---|---|---|---|
| Traced preset (3, 2) on PS 1.3/1.4 without `V8U8` | (1, 1) / (2, 1) | t1 | No supported du/dv format; t1's image is G-12. |

Retail presets on a device with PS ≥ 2.0 and `V8U8` support (assuming each technique validates):
- Explicit levels: `Default_3` (3, 2) → t0; `Default_2` (2, 1), `LowDx9_3`
  (2, 1) and `Default_1` (1, 1) → t1.
- Explicit levels: `LowDx9_1` (1, 0), `LowDx9_0` (0, 0), `Default_0`
  (0, 0) and `FF_0`…`FF_3` (0, 0) → t2.
- `Based_On` outcomes are conditional on G-11: `FF_LowTex_0`…`FF_LowTex_2`
  inherit (0, 0) → t2; `FF_LowTex_3` inherits `Default_3` (3, 2) → t0;
  `LowDx9_2` inherits `Default_2` (2, 1) → t1. These three groups omit
  explicit shader and water levels.

For the traced preset path, device limits change the result:
- PS 1.4: `Default_3` clamps to (2, 1) → t1.
- PS 1.1 or 1.2: every preset clamps to (0, 0) → t2. The DX8 level needs PS 1.3, even
  though `t1`'s own program is ps.1.1.
- No `V8U8` support: `Default_1` or `Default_2` gives water 0 → t2.
- On PS 1.3 or 1.4 without `V8U8`, a water-2 request skips the format check,
  then reaches water 1 on this path. With shader detail 1 or 2, t1 is selected
  even though no supported du/dv format is available (G-12). On other request
  paths, the water-2 outcome is G-06.

For the pinned map (`F` = 1, bump `W_WaterBump00.tga`, base `w_water_fallback_blue.tga`):
`Default_3` on a PS ≥ 2.0 device → t0, `Default_2` → t1, `Default_0` → t2. These are the
selections R-01…R-14 predict. They were **not** observed at runtime (G-08).

## Failure and diagnostic behaviour

- No technique qualifies: the effect load reports failure (R-10). Whether water is then
  skipped or drawn in some other way is unresolved (G-01). Do not invent a fallback.
- A reflection or refraction target cannot be begun: that pass is skipped (R-17). What
  the water then samples is G-04.
- An unrecognised `LOD` string is treated as rank 3, not rejected (R-08).

## Original expected-outcome cases

### C-01: DX9 with high water
- Given: PS 3.0, `V8U8` supported, all techniques validate, `F` = 1, request (3, 2).
- When: effects are selected after the request has been applied.
- Then: `t0`. Both bump slots receive the bump texture. Reflection and refraction
  passes are rendered.
- Covers: R-01, R-02, R-08, R-14, R-16, R-17.

### C-02: DX9 shader, medium water
- Given: as C-01, request (3, 1).
- Then: `t1`, because `t0`'s `WaterLOD` 2 > 1. Bump0 is the derived du/dv texture.
  Reflection only.
- Covers: R-08(3), R-14, R-16, R-17.

### C-03: coupling lag
- Given: `V8U8` supported, applied (3, 2), then a request for shader 2 only.
- When: render-begin 1 runs, then render-begin 2.
- Then: after render-begin 1, applied (2, 2). Techniques are reselected and `t1` is
  chosen, because `t0` needs rank 3. A water-1 request is now pending. After
  render-begin 2, applied (2, 1) and techniques are reselected again: still `t1`.
- Covers: R-05, R-06(a), R-14.

### C-04: fixed-function forces water 0
- Given: applied (1, 1), then a request for shader 0.
- When: render-begin 1, then render-begin 2.
- Then: after render-begin 1, applied (0, 1) and `t2` (rank). After render-begin 2,
  applied (0, 0): still `t2`.
- Covers: R-06(b), R-14.

### C-05: no du/dv format
- Given: PS 2.0, `V8U8` unsupported, request water 1 through the traced preset path.
- Then: the pending water request is 0, and the selection is `t2` (if `S` ≥ 1, `t1`
  would otherwise qualify).
- Covers: R-03.

### C-06: validation fallback does not change levels
- Given: applied (3, 2), and the device rejects `t0`.
- Then: `t1` is selected and applied `W` stays 2. Refraction is not rendered because
  `t1` does not use it. `t1`'s Bump0 slot is empty if no earlier du/dv texture
  exists, or receives a stale derived texture retained from an earlier `W` = 1
  period (R-16). The resulting image is G-10.
- Covers: R-14, R-16, R-17.

### C-07: boundary PS versions
- Given a traced preset requesting (3, 2):
  - PS 1.3 → applied (1, 1) → `t1`.
  - PS 1.4 → (2, 1) → `t1`.
  - PS 2.0 → (3, 2) → `t0`.
  - PS 1.2 → (0, 0) → `t2`.
- Covers: R-01, R-02, equality boundaries.

### C-08: synthetic effect ordering (generic selector)
- Given an effect declaring, in order:
  - `A` (`LOD` "dx9", no `WaterLOD`);
  - `B` (`LOD` "DX8");
  - `C` (`LOD` "banana");
  - `D` (no `LOD`).
- With applied (1, 0):
  - `A` fails (rank 3).
  - `B` is selected: case-insensitive match, rank 1, no water gate.
- With applied (3, 0): `A` is selected, because it comes first.
- With only `C` and `D` present and `S` = 2: neither qualifies (both rank 3), so the
  result is none.
- Covers: R-08(2), R-08(3), R-10.

### C-09: vendor exclusion
- Given: a technique with `VENDOR_FILTER_0` = 0x10DE and `VENDOR_FILTER_2` = 0x1002 (no
  index 1), device vendor 0x1002.
- Then: the technique is **not** excluded, because the filter scan stops at the missing
  index 1. With device vendor 0x10DE it is excluded, and the next technique is examined.
- Covers: R-08(4).

### C-10: name-keyed binding
- Given: an alternate effect whose selected technique is named `t3`.
- Then: the draw uses the t0-style bindings (R-15).
- Covers: R-15.

### C-11: empty texture mini
- Given: `F` = 1, applied (0, 0), mini `0x1e` present but empty.
- Then: `t2` draws with the generic white base texture, tinted by the water colour.
- Covers: R-16, R-18.

### C-12: water-2 request without du/dv support at the PS 1.4 ceiling
- Given: PS 1.4, `V8U8` unsupported, all techniques validate, `F` = 1,
  request (3, 2) through a traced preset.
- Then: applied (2, 1), and `t1` is selected. The water-2 request bypasses the
  `V8U8` check before the ceiling lowers it to 1. The derived du/dv texture is
  unavailable (G-12).
- Covers: R-01, R-02, R-03, R-14, G-12.

## Uncertainties and gates

| Gate | Unknown |
| --- | --- |
| G-01 | What the water draw does when no technique qualified (R-10). |
| G-02 | Draw behaviour for family 0 and out-of-range family values. |
| G-03 | The precise texture operations and visual result of each retail program remain unverified. Retail compiled sampler tables and texture-state references already identify the sampled resource sets in R-16/R-17; the FoC source agrees. A positional comparison of the unequal-length base and patch effect files finds 2,508 differences in their shared byte range, with 12 additional patch bytes. |
| G-04 | What the reflection/refraction slot samples when a target could not be begun. |
| G-05 | In land mode, if the 128-pixel scene sky-cube target exists but its surface is unavailable for a frame, a water-detail request of at least 1 can result even when the applied level was 0. This could change the selected technique after a device reset; trigger frequency and purpose are unresolved. |
| G-06 | Outside the traced preset and device-create/reset paths, a water-2 request on PS 1.3/1.4 may remain at 2; requests above water detail 2 also have no established outcome. |
| G-07 | What mini `0x15` means. |
| G-08 | No runtime draw has confirmed any row. |
| G-09 | When water detail is exactly 2 and the family is water, the reflection pass uses an additional setup variant. What that changes is unresolved. |
| G-10 | `t1` selected while applied `W` = 2 (a validation fallback, or the one-frame coupling lag). Its Bump0 slot is empty if no derived du/dv texture exists, or holds a stale one retained from an earlier `W` = 1 period. |
| G-11 | The XML uses `Based_On` for several presets, but its inheritance behaviour in the retail loader has not been traced. |
| G-12 | On the traced preset path, a water-2 request on PS 1.3/1.4 without `V8U8` settles at water 1, yet no supported du/dv format is available. The t1 image and any late fallback are unobserved. |
