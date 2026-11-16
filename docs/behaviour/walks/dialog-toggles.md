# Dialog radio buttons and check boxes: glyph size, anchor and caption (legacy EAWR-5345)

This FoC walk records one headless Ghidra pass over the debug build's radio-button and check-box controls of the shared
dialog system: how each builds its graphic from the theme's state arts, and where it places its caption. The decompiles stay
in private research (`out/research/walk-radio`, outside the committed tree). The dialog data it reads is the effective
`GUIDialogs` theme (`Radio_On`, `Radio_Off`, `Radio_Mouse_Over`, `Radio_Disabled`, `Check_On`, `Check_Off`) and the dialog
resource scripts (`BS_AUTORADIOBUTTON`, `BS_PUSHLIKE`). Screen row: MNU-34 in [the menu screens](../../ui/menu-screens.md).

| ID | Rule |
|---|---|
| DTG-1 | A radio button builds one graphic quad. Its size is the size of the theme's **OFF** art divided by the dialog's width and height scales; its position is the control's authored top-left corner plus the dialog's offset. The authored rectangle's width and height never size it. The on, hover and disabled arts are not given quads of their own: each is selected into the same quad by texture coordinates, so a lit art of another size is drawn at the OFF art's size. A check box builds its graphic the same way from the unchecked art (checked art selected into the unchecked art's quad). |
| DTG-2 | An ordinary (not push-style) radio button's caption starts at the quad's right edge plus a constant gap, and is centred vertically on the quad's centre. |
| DTG-3 | A push-style radio button (`BS_PUSHLIKE`) uses the same quad as DTG-1; only the caption differs: it is centred on the quad horizontally (the quad's centre minus half the caption's width) and vertically (DTG-2). The control's push-style flag is read only by the caption placement; the graphic, the event handling and the visibility handling read none. |
| DTG-4 | The authored rectangle of a radio button is the position anchor and the click area only. Rows whose rectangles differ in width (the staging dialog's Easy, Medium and Hard radios: 186, 216 and 206 units, all at the same x, 25 units high, with their captions in separate text controls to the right) therefore draw one glyph size on one x line. |
| DTG-5 | Viewer adaptation for empty resource dimensions: a push-style radio with a zero authored width or height takes that hit dimension from the theme's OFF art. Every positive authored dimension remains unchanged, including dimensions smaller than the art. The glyph's drawing and the level-to-session mapping are unchanged. Evidence: the owner reports visible but unclickable difficulty radios; the loaded mod resource authors three horizontal radios with zero height, while the viewer previously gave push-style controls no minimum size. This fallback is a viewer rule; the debug-build walk above did not establish the original's exact hit extent. |

Not read by this walk (open): the constant gap of DTG-2 (a named constant of the dialog data, not decompiled); the click
area's exact extent; the behaviour of a push-style **check box**, which has no push-style branch in the build's check-box
code read here (the check-box graphic does not read the flag), so the viewer keeps its authored-rectangle fill for it until a
walk or capture decides.

Viewer rule (`ui::toggle_glyph_box`, `ui::toggle_caption_box`, `EawrUiCheck`): the glyph box is the OFF art's size at the
control's top-left, every state draws into it, a push-style radio centres its caption on it, an ordinary one starts the
caption one gap after it, and a push-style check box keeps filling its rectangle. Before this rule the viewer stretched a
push-style radio's art over its whole authored rectangle, so the three Difficulty dots took the three row widths (measured
40, 47 and 44 px at 1080p) and the lit one looked displaced.
