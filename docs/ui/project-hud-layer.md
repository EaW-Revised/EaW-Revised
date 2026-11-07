# Project HUD layer format

The engine supplies a default Gift control using stock FoC art. `Data/XML/EawrHud.xml` in the Revised layer or a mod replaces each matching `Control` by its case-insensitive `Name`; omitted controls remain from lower layers or the engine default. `Enabled=false` disables a named control. An absent file leaves the default in place.

`Size` and `Recipient_Size` are width and height in UI reference units. `Caption` and `Tooltip` are language text keys. `Normal_Texture`, `Hover_Texture`, `Pressed_Texture`, and `Disabled_Texture` name complete state textures in the mounted command atlas or standalone textures. Each ordered `Candidate` has an `Anchor`, `Side`, and two-coordinate `Offset`. An anchor is a named shell component or `viewport`; sides are `above`, `below`, `left`, `right`, `inside_top_left`, `inside_top_right`, `inside_bottom_left`, and `inside_bottom_right`. Offsets are reference units with positive y down. Candidates outside the viewport or overlapping occupied controls or the recipient list are rejected. If no candidate fits, the control is hidden with EAWR-UI-0790.

An authored Gift with an unavailable normal texture is also hidden with EAWR-UI-0790, rather than leaving an invisible input target. A malformed layer is reported as EAWR-UI-0791 and clears lower definitions; a later, higher layer may supply a replacement.

For example, a mod can supply its own art and two ordered positions. The texture names below are illustrative and must be supplied by that mod:

```xml
<Eawr_Hud>
  <Control Name="gift">
    <Enabled>true</Enabled>
    <Size>96 30</Size>
    <Recipient_Size>180 30</Recipient_Size>
    <Caption>TEXT_EAWR_GIFT_UNITS</Caption>
    <Tooltip>TEXT_EAWR_GIFT_UNITS</Tooltip>
    <Normal_Texture>my_gift_normal.tga</Normal_Texture>
    <Hover_Texture>my_gift_hover.tga</Hover_Texture>
    <Pressed_Texture>my_gift_pressed.tga</Pressed_Texture>
    <Disabled_Texture>my_gift_disabled.tga</Disabled_Texture>
    <Candidate><Anchor>attack</Anchor><Side>above</Side><Offset>0 -4</Offset></Candidate>
    <Candidate><Anchor>viewport</Anchor><Side>inside_top_left</Side><Offset>40 120</Offset></Candidate>
  </Control>
</Eawr_Hud>
```

The replacement is the whole named `Control`, not a field-by-field patch: include the size, art and candidates needed by the replacement. A layer can disable Gift with `<Control Name="gift"><Enabled>false</Enabled></Control>`. The recipient panel extends upward from the button; the entire panel and button must fit without intersecting an occupied rectangle before a candidate is selected. The built-in Gift keeps the stock 2 reference-unit gap between recipient rows and between the panel and the button; authored controls have no gap. The panel is never clamped to a shorter height: a panel that does not fit above the button rejects the candidate instead of scrolling.

The Gift control is re-docked when the hero tray, the open production pane or the order row changes position, not only when the battle starts.

The test fixture `tests/ui/fixtures/eawr-hud.xml` illustrates the structure with synthetic texture names; packaged layer data is supplied separately.
