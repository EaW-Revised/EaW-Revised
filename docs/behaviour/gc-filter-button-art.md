# Galactic filter button art

| Rule | Behaviour | Evidence |
| --- | --- | --- |
| GCF-01 | The two minimap-side filters retain their icon during hover and add the RGB of `Mouse_Over_Texture_Name` without weighting it by that texture's alpha. The shared hover frame does not replace either icon. `Swap_Texture` selects replacement versus retained layers; `Selected_Alpha` independently controls the selected overlay. | Debug-build captures of both stock filters in Galactic Conquest, 2026-10-20; effective command-bar XML and atlas pixels; existing selected-layer rule HCB-12g. |

Both stock components, `b_filters0` and `b_filters1`, author `Swap_Texture=False`,
`Selected_Alpha=True` and `Click_Shift=True`. Their normal icons, selected icons and
disabled icons are distinct per filter, while their blank and hover textures are
shared. All names and flags come from the mounted component catalog.

The renderer uses the existing blank/icon/state layer order. For additive hover,
it makes an RGB-only copy of the resolved hover texture once during setup; the
shared authored texture remains unchanged. Normal, selected and disabled states
keep their own resolved art and blend settings. Swapping components retain the
ordinary state-texture replacement path.
