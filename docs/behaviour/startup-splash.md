# Startup splash

The FoC debug build supplies the following startup rules. The recording needed
to qualify duration, fade, aspect handling and removal is still outstanding.

| Rule | Behaviour | Evidence |
|---|---|---|
| SPL-01 | After game constants and graphics initialization, startup selects the language's `Localized_Splash_Screen`; the UK locale instead selects `Localized_UK_English_Splash_Screen`. An empty selected name falls back to `Splash.TGA`. | debug build |
| SPL-02 | Startup loads the selected name as a texture, places it in a full-screen rectangle and draws it to both buffers before continuing initialization. | debug build |

EAWR resolves the name through its texture-name rules, probing each mounted
layer in priority order. A missing or unreadable texture falls through to the
next candidate and ultimately to a black background. The project's early engine
splash is black; the selected texture is shown after the content mount and
before the remaining menu preparation. Timing and the transition are pending
the retail and viewer recordings.
