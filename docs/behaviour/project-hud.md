# Project HUD controls

**EAWR-HUD-01** is a project rule: the engine supplies a stock-art Gift control, and control definitions merge by name in VFS layer order, with the higher layer replacing that control. An absent file leaves the engine default intact. Candidate placements are tried in authored order and rejected when outside the viewport or intersecting occupied rectangles, including the expanded recipient list. If none fits, the control is hidden and the tactical HUD emits EAWR-UI-0790. This is an EAWR extension, not a claim about original-game behavior.
