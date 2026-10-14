# Credits roll

What the main menu's emblem button (MNU-07 in [menu-screens.md](../ui/menu-screens.md)) shows: the
`Credits` movie with the credits text scrolling up over it, followed by three logos, until
everything has scrolled off the top. Evidence: the effective FoC XML (`movies.xml`,
`GameConstants.xml`), `CreditsText_English.dat` and the debug build, read for the route, the setup of the roll and
its per-frame movement. The engine-free model is `include/eawr/presentation/ui/credits.hpp`; the viewer's
`EawrCreditsScreen` (`src/presentation/godot/ui/credits_view.cpp`) draws it. Contract cases are in
`tests/ui/credits_tests.cpp`; the viewer case is `tests/presentation/renderer/test_credits.py`.

Positions are fractions of the screen: y of the height (0 top, 1 bottom), x of the width.

| Rule | Behaviour | Evidence | Ours |
|---|---|---|---|
| CRD-01 | The route plays the movie declared as `Credits` in `movies.xml`; its file, `Overlay` and `Has_Credits` come from the data. The movie declaration marks the roll: the `Has_Credits` movie is the one the text is drawn over. | debug build; XML | Applied: the name is the route's own and the file resolves through the same movie loader as the HUD movies, so a mod replaces it. |
| CRD-02 | The roll's look is the GameConstants `Credits_*` values: font and size, line spacing, fade margin, scroll rate, body and header gradient colours (top and bottom), and three logos (name, width, height, y offset). | debug build; XML | Applied. A value that is absent or malformed, or a rate that does not scroll up, leaves the screen without text and says why. |
| CRD-03 | The text is every record of the language's `CreditsText` database in file order (no CRC order; repeated keys all stay). A record whose key contains `HEADER` takes the header colours; a value `[TBL]` is an empty line. | debug build; data | Applied. |
| CRD-04 | Lines stack at the data's spacing, snapped down to whole screen pixels, the first one half a pixel below the screen's bottom edge, each centred horizontally. The roll moves up at `Credits_Scroll_Rate` screen heights a second in whole pixels. | debug build | Applied. The debug build moves at most one pixel per frame; ours moves the whole elapsed distance (a difference only below one frame per pixel). |
| CRD-05 | A line's or logo's opacity follows its top edge: zero above the margin, a smooth step up over the next margin, one until two margins above the bottom, then a smooth step down to zero a margin above it. | debug build (logo path read; the text path has the same branches) | Applied to both. |
| CRD-06 | The roll is done when every line's and logo's lower edge is at or above the top of the screen; the credits then end and the main menu returns. | debug build | Applied. Whether the debug build tests the top or the bottom edge is not traced; ours uses the bottom edge. |
| CRD-07 | The three logos are placed under the last text line by their y offsets: the first at the left edge, the second centred, the third at the right edge. A logo is `Data/Art/Textures/<name>`; width is the data's fraction of the screen width and the height keeps the image's proportions scaled by the data's height value. | debug build | Applied. The exact aspect rule is inferred from the width and height reads (unverified); a logo that cannot be read is skipped with a note. |
| CRD-08 | Starting the roll starts the music event `Game_Credits_Music_Event` in the credits music mode and raises the process priority. | debug build | Not applied: the viewer has no menu music route yet (fidelity list). |
| CRD-09 | Esc or a click ends the credits at once and returns to the menu. | owner statement; not traced in the debug build | Applied. |
| CRD-10 | Project choice: the movie loops under the roll (its name says so; whether the debug build loops it is not traced), and a movie that cannot be played yet (not converted, not in the data) leaves the roll over a plain backdrop. With no text to roll, the movie's end closes the credits. | project | Applied. |

## Unknowns

- Which key the debug build uses to end the credits besides the roll finishing (CRD-09 is the owner's statement).
- The exact edge (top or bottom) of the end test (CRD-06) and the logos' height formula (CRD-07).
- Whether the movie loops or plays once under the roll (CRD-10).
