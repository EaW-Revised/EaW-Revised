# Planet search layout

The Revised layer installer includes `tools/revised_planet_search/RevisedPlanetSearch.xml`
as `Data/XML/RevisedPlanetSearch.xml`. A mod may replace that file. Disabling Revised
also removes this dialog unless another layer supplies it.

The XML uses the command-bar conventions `Control Name`, `Type`, `Offset`, `Size`
and `Caption`. `Text_Field` and `List` map to the existing kit's edit and list
controls; `TextButton` maps to its normal button. Coordinates use the normal
dialog reference units and scaling. The root `Offset` is an inset from the right
and top of the viewport. `Planet_Picture` and `Owner_Emblem` enable their columns.
Neither column accepts a pixel size: both follow the measured `List_Box` font. The optional `Row_Scale` (default 1.0, accepted from 0.5 to 3; the shipped layout uses 1.2) scales the rows' `List_Box` font size; both icons follow the resulting text height. A value outside that range or not a number rejects the layer like other malformed geometry.

Required controls are `IDC_PLANET_SEARCH_TEXT`, `IDC_PLANET_SEARCH_RESULTS` and
`IDC_PLANET_SEARCH_CLOSE`. The dialog name is `IDD_EAWR_PLANET_SEARCH`.
`GUIDialogs.xml` font and texture overrides may use those names, exactly as for
other dialogs. Malformed geometry, missing required controls or invalid kinds
reject the whole layer, preserving a valid lower layer.

Behaviour is defined by [PS-01 through PS-06](../behaviour/planet-search.md).
