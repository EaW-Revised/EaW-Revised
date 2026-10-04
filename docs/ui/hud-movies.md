<a id="hud-movies-237-g12"></a>

# HUD movies (G12)

Decision, 2026-09-26: FoC's HUD movies are Bink 1. The player converts them once
with **their own FFmpeg** into a local Theora cache, and the viewer plays that cache
with Godot's built-in `VideoStreamTheora`. The project ships and downloads no FFmpeg,
no RAD SDK, no Bink decoder and no original or converted game data. This PR is the
playback building block; the tactical HUD (UI-09) binds it to the tactical and galactic shell
slots (see [Binding to the shell](#binding-to-the-shell-83)).

## Formats and resolution

The seven read-only effective views were mounted with the repository's
`resolve_manifest_chain` and `Vfs`: retail corruption over GameData; the six
workshop roots from [the survey](mod-hud-survey.md), with 2794270450 and 3229239424
over parent 1770851727. Every movie-bearing XML and MULTIMEDIA parameter-9 XML
was inspected in the effective view, including inherited campaign files that a
mod may never activate. XML comments were excluded. The appendix names every
distinct XML/name reference and its resolution; duplicate events collapse into
one XML/name pair. It also lists alpha-marked movie declarations as a conservative
superset for dynamically requested portraits. It does not infer active campaign
reachability from mere VFS presence.

All resolved candidates have Bink 1 container/video signatures (`BIKi`). No KB2
(Bink 2), Ogg, AVI or other format was found among these candidates. File extension
alone was not used to identify the format. Missing names/files are preserved in
the appendix, including the retail Antilles/Antillies spelling difference. The
corpus test (`ui_data_tests` with `EAWR_EAW_GAME_ROOT` and `EAWR_MOD_HUD_ROOTS`)
resolves every declaration of each view on every run. Bink 1 / without a file:
FoC 57/4, RaW 31/1, Remake 56/5, FotR 109/2, RE5 55/6, RCW 56/5, 3229239424 56/5.

The HUD portrait loops are 200 x 200 Bink 1 with an alpha plane (FFmpeg decodes it
as `yuva420p`), 30 fps, about 15.5 s; they match the 200-unit `Movie_tactical` and
`Movie_galactic` Icon slots. `Default_Incoming_Transmission` is 225 x 195 without
alpha at 29.97 fps and carries `Commandbar_Offset` `0 -235`.

`Movie_tactical` and `Movie_galactic` are slot names, not filenames. Resolve a
requested name through effective `data/xml/movies.xml` (ASCII case-insensitive; a
later declaration of the same name wins, as for command-bar components), then
independently open its `Movie_File` through the same VFS. Keep `Alpha` and
`Commandbar_Offset`. Do not guess a movie from the slot name. Story
`COMMANDBAR_MOVIE` names the movie in parameter 1; `MULTIMEDIA` in parameter 9
(`-1`: none); a faction can name `Tactical_Intro_Command_Bar_Movie_Name`.

## Decoder licensing decision

| Route | Licence and distribution obligations | Decision |
|---|---|---|
| Godot built-in VideoStreamTheora | Godot is MIT; libtheora is BSD-style. Retain Godot's and its third-party copyright and licence notices, as the viewer already must. Plays Theora in Ogg, not Bink. | Plays the converted cache. |
| FFmpeg's open Bink 1 decoder, dynamically linked | The decoder source is LGPL-2.1-or-later. An LGPL-only FFmpeg build lets the application code stay MIT. Distributing it means shipping the licence and notices, the matching library source and build information, and allowing the LGPL parts to be replaced and relinked. Avoid `--enable-gpl` and `--enable-nonfree` builds. | Viable, with packaging and compliance work on every release. |
| FFmpeg statically linked | LGPL permits it only with a way to relink against a modified library (object files or equivalent), plus source and notices. A GPL-enabled build makes the distributed combination GPL; nonfree builds may not be redistributable. | Unnecessary obligations. |
| RAD Bink SDK | Proprietary, licensed per product and platform; the retail DLL grants no right to redistribute or integrate the SDK. | Not downloaded, linked or bundled. |
| User-side FFmpeg conversion | The player supplies and runs an independent FFmpeg; our script starts it as a separate process with an argument list and links nothing. We distribute no FFmpeg, so no library redistribution duty arises for this MIT project. The source and converted files stay on the player's machine. Bundling FFmpeg later needs a new review of that exact build. | **Selected**: the least obligation. |

Sources checked 2026-09-26: [FFmpeg Bink decoder licence header](https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavcodec/bink.c),
[FFmpeg licensing/checklist](https://ffmpeg.org/legal.html),
[LGPL 2.1 section 6](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html#section6),
[RAD licensing](https://www.radgametools.com/sales.htm),
[RAD SDK downloads](https://www.radgametools.com/bnkdown.htm),
[Theora licensing](https://theora.org/faq/),
[Godot runtime video loading](https://docs.godotengine.org/en/stable/tutorials/io/runtime_file_loading_and_saving.html).
This is a software licence and packaging decision, not a right to redistribute
game assets and not a patent clearance for every jurisdiction. Bink 2 was not
found and is refused (EAWR-UI-0704).

## Conversion (player side)

Build the root target `hud_movie_prepare`, then:

```powershell
python tools/ui/convert_hud_movie.py --prepare <hud_movie_prepare> --game-root <FoC install> `
    --cache <dir> --name Underworld_soldier_Loop [--name ...] [--mod-root "leaf;parent"] [--ffmpeg <ffmpeg>]
```

FFmpeg comes from `--ffmpeg`, else `EAWR_FFMPEG`, else `PATH`; it must have the
`binkvideo` decoder and the `libtheora` encoder (EAWR-UI-0706 otherwise; common
Windows and Linux builds have both). `hud_movie_prepare` resolves each name through
the install's VFS (and mod chain) as the viewer does and copies the Bink bytes to
`<cache>/<key>.bik`; the script converts them to `<cache>/<key>.ogv`, decodes the
result once with `-xerror`, and only then replaces the entry. The `.bik` copies are
deleted on every exit path, a failed conversion never replaces an entry
(EAWR-UI-0707), and entries already present are kept. Nothing in the install
changes, and conversion never runs on the game or render thread.

The cache key is `<SHA-256 of the source bytes>-hud-v1-<alpha|rgb>`. A mod that
replaces the bytes selects a new entry; names on one file (`Tyber_Loop` and
`Tyber_loop`, the three `rebel_pilot_loop2` names) share one. `hud-v1` is
`movie_cache_recipe` and changes with the recipe below.

Recipe `hud-v1`: libtheora quality 10, `yuv420p`, source frame rate, no audio (HUD
speech and sound come from their own systems). Theora has no alpha, so an `Alpha`
movie is packed as colour in the left half and opacity as grey in the right half
of the same frame (`format=rgba,split,alphaextract,hstack`): 200 x 200 becomes
400 x 200, and colour and opacity cannot drift apart. An `Alpha` movie without an
alpha plane gets an opaque one.

## Playback

`attach_hud_movie(parent, movie, cache, rect, loop)`
(`src/presentation/godot/ui/movie_player.hpp`) adds a parent-owned, muted,
mouse-transparent `VideoStreamPlayer` that fills `rect`, loops when asked and
starts playing. For an `Alpha` movie a canvas shader takes colour from the left
half and opacity from the luma of the right half (luma is what the grey was
encoded as, so chroma noise at the seam cannot leak into it), keeps UVs half a
texel inside each half, and outputs straight alpha times the item's modulate.
`validate_hud_movie` reports EAWR-UI-0708 when the player has decoded no frame.

The UI gallery shows one movie: `--eawr-ui-gallery --eawr-ui-movie <name>
--eawr-movie-cache <dir>` plus the usual game-root, report and capture flags. It
draws the movie at twice its slot size, centred, with a lighter band behind the
right half so the capture shows the alpha, and reports source, cache key, decoded
texture size, distinct frames, loops and state.

| Diagnostic | Meaning | Raised by |
|---|---|---|
| EAWR-UI-0701 | `movies.xml` missing or malformed | `resolve_hud_movie` |
| EAWR-UI-0702 | Name not declared in `movies.xml` | `resolve_hud_movie` |
| EAWR-UI-0703 | `Movie_File` empty or not in the VFS | `resolve_hud_movie` |
| EAWR-UI-0704 | Not Bink 1 (Bink 2, Ogg or unknown bytes) | `resolve_hud_movie` |
| EAWR-UI-0705 | No cache entry for these bytes: run the conversion | `attach_hud_movie` |
| EAWR-UI-0706 | No usable FFmpeg (absent, no Bink decoder, no libtheora) | `convert_hud_movie.py` |
| EAWR-UI-0707 | Conversion or cache write failed; the old entry is kept | `convert_hud_movie.py`, `hud_movie_prepare` |
| EAWR-UI-0708 | Cache entry is not Ogg, or Godot decoded no frame from it | `attach_hud_movie`, `validate_hud_movie` |

Tests: `ui_data_tests` (synthetic resolution with 0701 to 0704; every declaration of
each corpus with the environment variables), `ui_movie_conversion` (the script with
stand-in tools, 0706 and 0707; a real packing of a generated clip when FFmpeg is on
`PATH`) and the GPU suite `tests/presentation/renderer/test_ui_movies.py` (a
project-authored Theora fixture that plays, loops and composites its alpha; 0702,
0705, 0708).

<a id="binding-to-the-shell-83"></a>

## Binding to the shell

This playback component builds no shell. The tactical HUD binding needs to:

1. Take the movie name from the request: `COMMANDBAR_MOVIE` parameter 1, `MULTIMEDIA`
   parameter 9 unless `-1`, or the faction's `Tactical_Intro_Command_Bar_Movie_Name`
   at tactical start. Resolve it with `resolve_hud_movie`; on an error, log it once
   and leave the slot empty.
2. Take the rect from the shell anchor `Movie_tactical` (tactical) or
   `Movie_galactic` (galactic) (`ShellAnchors`, reference units to pixels by the UI-L3
   scale), moved by the movie's `offset_x`/`offset_y` (`Commandbar_Offset`).
3. Call `attach_hud_movie(slot_parent, movie, cache, rect, true)` and keep the node.
   One movie per slot: a new request frees the previous node.
4. On `STOP_COMMANDBAR_MOVIE`, stop and free the node.
5. A few frames after attaching, call `validate_hud_movie` and log EAWR-UI-0708 once.
6. Choose the production cache directory (the viewer takes `--eawr-movie-cache`) and
   offer the conversion when EAWR-UI-0705 appears, once per name per session.

Fidelity lines F6 to F8 in the [survey's list](mod-hud-survey.md) cover what to
observe on the rig before emulating: the offset's sign and origin, loop or play-once,
and the portrait blend.

## Complete effective-view inventory

All paths below are VFS logical paths, lower-cased; resolution is independently applied to the XML and the movie file. `base` means GameData, `expansion` means corruption, and `mod`/`mod-parent-1` mean leaf/parent. COMMANDBAR_MOVIE Reward_Param1, MULTIMEDIA Reward_Param9 and faction Tactical_Intro_Command_Bar_Movie_Name are scanned after XML comments are removed. XML references are grouped by XML and distinct name (repeated story events with the same name are collapsed). Alpha-marked Movies.xml entries are included as dynamic HUD candidates, even if no static story reference selects them. Full-screen alpha text crawls in that conservative superset are not claimed to be HUD requests. STOP_COMMANDBAR_MOVIE has no file argument; MULTIMEDIA parameter 9 equal to -1 is the no-movie sentinel and is excluded. Movie_tactical and Movie_galactic are 200 by 200 Icon slot declarations in CommandBarComponents.xml, not movie names.


### retail

Movie registry: `data/xml/movies.xml`, supplied by `expansion:Data/Config.meg`.

| Referencing XML (under data/xml/) | XML source | Movie names |
|---|---|---|
| expansion_factions.xml | expansion:Data/Config.meg | Underworld_soldier_Loop |
| factions.xml | expansion:Data/Config.meg | Luke_Loop, Stormtrooper_Loop |
| story_campaign_empire_act_i.xml | expansion:Data/Config.meg | Boba_Fett_Loop, Commander_Moff_Loop, Emperor_Loop, Stormtrooper_Loop, Tarkin_Loop, Vader_Loop |
| story_campaign_empire_act_ii.xml | expansion:Data/Config.meg | Boba_Fett_Loop, Commander_Moff_Loop, Emperor_Loop, Tarkin_Loop, Vader_Loop |
| story_campaign_empire_act_iii.xml | expansion:Data/Config.meg | Commander_Moff_Loop, Emperor_Loop, Stormtrooper_Loop, Tarkin_Loop, Vader_Loop |
| story_campaign_empire_act_iv.xml | expansion:Data/Config.meg | Commander_Moff_Loop, Emperor_Loop, Tarkin_Loop, Vader_Loop |
| story_campaign_rebel_act_i.xml | expansion:Data/Config.meg | Antillies_Loop, C3PO_Loop, Mon_Mothma_Loop, R2D2_Loop, Wedge_Loop |
| story_campaign_rebel_act_ii.xml | expansion:Data/Config.meg | Antillies_Loop, C3PO_Loop, Han_Loop, Mon_Mothma_Loop, R2D2_Loop |
| story_campaign_rebel_act_iii.xml | expansion:Data/Config.meg | Antillies_Loop, C3PO_Loop, Han_Loop, Mon_Mothma_Loop, Rebel_Trooper_Loop |
| story_campaign_rebel_act_iv.xml | expansion:Data/Config.meg | Han_Loop, Mon_Mothma_Loop |
| story_campaign_underworld.xml | expansion:Data/Config.meg | Jabba_Loop, Pirate2_Loop, Saboteur_Loop, Silri_Loop, Thrawn_Loop, Tyber_Loop, Underworld_soldier_Loop, Urai_Loop, Xizor_Loop |
| story_campaign_underworld_demo.xml | expansion:Data/Config.meg | Commander_Moff_Loop, Jabba_Loop, Pirate_Loop, Stormtrooper_Loop, Tyber_Loop, Urai_Loop |
| story_campaign_underworld_focus_demo.xml | expansion:Data/Config.meg | Tyber_Loop |
| story_campaign_underworld_tutorial.xml | expansion:Data/Config.meg | Commander_Moff_Loop, Jabba_Loop, Pirate_Loop, Stormtrooper_Loop, Tyber_Loop, Urai_Loop |
| story_empire_acti_m02_land.xml | expansion:Data/Config.meg | Commander_Moff_Loop, Emperor_Loop, Rebel_Trooper_Loop, Stormtrooper_Loop, Vader_Loop |
| story_empire_acti_m03_space.xml | expansion:Data/Config.meg | Boba_Fett_Loop, Commander_Moff_Loop, Pirate2_Loop, Pirate_Loop |
| story_empire_acti_m04_space.xml | expansion:Data/Config.meg | Vader_Loop |
| story_empire_actii_m04_space.xml | expansion:Data/Config.meg | Commander_Moff_Loop, Tarkin_Loop |
| story_empire_actii_m05_land.xml | expansion:Data/Config.meg | Commander_Moff_Loop, Han_Loop, Stormtrooper_Loop |
| story_empire_actii_m06_land.xml | expansion:Data/Config.meg | Commander_Moff_Loop, Emperor_Loop, Tarkin_Loop, Veers_Loop |
| story_empire_actiii_m08_space.xml | expansion:Data/Config.meg | Emperor_Loop, Tarkin_Loop |
| story_empire_activ_m10_space.xml | expansion:Data/Config.meg | Antillies_Loop, Commander_Moff_Loop, Vader_Loop, Wedge_Loop |
| story_empire_activ_m11_space.xml | expansion:Data/Config.meg | Commander_Moff_Loop, Vader_Loop |
| story_focus_demo_felucia_land_tactical.xml | expansion:Data/Config.meg | Pirate_Loop, Stormtrooper_Loop, Tyber_Loop, Urai_Loop |
| story_focus_demo_felucia_space_tactical.xml | expansion:Data/Config.meg | Tyber_Loop |
| story_rebel_acti_m01_space.xml | expansion:Data/Config.meg | Antillies_Loop, Commander_Moff_Loop, Piett_Loop |
| story_rebel_acti_m02_land.xml | expansion:Data/Config.meg | Antillies_Loop, C3PO_Loop, R2D2_Loop, Rebel_Trooper_Loop, Stormtrooper_Loop |
| story_rebel_acti_m03_land.xml | expansion:Data/Config.meg | Antilles_Loop, Antillies_Loop, Rebel_Trooper_Loop, Wedge_Loop |
| story_rebel_actii_m04_space.xml | expansion:Data/Config.meg | Antillies_Loop, Mon_Mothma_Loop, Wedge_Loop |
| story_rebel_actii_m06_land.xml | expansion:Data/Config.meg | Han_Loop, Mon_Mothma_Loop |
| story_rebel_actiii_m07_space.xml | expansion:Data/Config.meg | Antilles_Loop, Antillies_Loop, Commander_Moff_Loop, Wedge_Loop |
| story_rebel_actiii_m07a_space.xml | expansion:Data/Config.meg | Commander_Moff_Loop, Han_Loop |
| story_rebel_actiii_m08_space.xml | expansion:Data/Config.meg | Boba_Fett_Loop, Commander_Moff_Loop, Han_Loop, Mon_Mothma_Loop |
| story_rebel_actiii_m09_space.xml | expansion:Data/Config.meg | Antillies_Loop, Rebel_Pilot_Loop, Rebel_Trooper_Loop, Wedge_Loop |
| story_rebel_actiii_m10_land.xml | expansion:Data/Config.meg | Han_Loop |
| story_rebel_activ_m11_galactic.xml | expansion:Data/Config.meg | Mon_Mothma_Loop, Wedge_Loop |
| story_rebel_activ_m12_galactic.xml | expansion:Data/Config.meg | Han_Loop, Mon_Mothma_Loop, Rebel_Trooper_Loop, Wedge_Loop |
| story_sandbox_focusdemo_empire.xml | expansion:Data/Config.meg | Boba_Fett_Loop, Emperor_Loop, Vader_Loop |
| story_tutorial_ii.xml | expansion:Data/Config.meg | Mon_Mothma_Loop, Rebel_Trooper2_Loop |
| story_tutorial_iii.xml | expansion:Data/Config.meg | Piett_Loop |
| story_tutorial_iv.xml | expansion:Data/Config.meg | Piett_Loop |
| story_tutorial_m01_land.xml | expansion:Data/Config.meg | Rebel_Trooper2_Loop, Rebel_Trooper3_Loop, Rebel_Trooper_Loop, Stormtrooper_Loop |
| story_tutorial_m03_space.xml | expansion:Data/Config.meg | Commander_Moff_Loop, Veers_Loop |
| story_tutorial_m05_land.xml | expansion:Data/Config.meg | Veers_Loop |
| story_tutorial_v.xml | expansion:Data/Config.meg | Piett_Loop |
| story_underworld_aetenii_piracy.xml | expansion:Data/Config.meg | Bossk_Loop |
| story_underworld_alzociii_intimidation.xml | expansion:Data/Config.meg | Tyber_Loop |
| story_underworld_anaxes_intimidation.xml | expansion:Data/Config.meg | Bossk_Loop |
| story_underworld_bespin_piracy.xml | expansion:Data/Config.meg | Tyber_Loop |
| story_underworld_bespin_piracy_story.xml | expansion:Data/Config.meg | Tyber_Loop |
| story_underworld_bestine_kidnapping.xml | expansion:Data/Config.meg | IG-88_Loop |
| story_underworld_bothawui_intimidation.xml | expansion:Data/Config.meg | Tyber_Loop |
| story_underworld_byss_kidnapping.xml | expansion:Data/Config.meg | Silri_Loop |
| story_underworld_carida_kidnapping.xml | expansion:Data/Config.meg | Bossk_Loop |
| story_underworld_corellia_piracy.xml | expansion:Data/Config.meg | Tyber_Loop |
| story_underworld_corulag_piracy.xml | expansion:Data/Config.meg | IG-88_Loop |
| story_underworld_dantooine_intimidation.xml | expansion:Data/Config.meg | Urai_Loop |
| story_underworld_fondor_piracy.xml | expansion:Data/Config.meg | Bossk_Loop |
| story_underworld_honoghr_intimidation.xml | expansion:Data/Config.meg | Urai_Loop |
| story_underworld_honoghr_intimidation_story.xml | expansion:Data/Config.meg | Urai_Loop |
| story_underworld_kessel_piracy.xml | expansion:Data/Config.meg | IG-88_Loop |
| story_underworld_kuat_kidnapping.xml | expansion:Data/Config.meg | Bossk_Loop |
| story_underworld_m01_land.xml | expansion:Data/Config.meg | Han_Loop |
| story_underworld_m02_space.xml | expansion:Data/Config.meg | Commander_Moff_Loop, Piett_Loop, Pirate_Loop, Rebel_Pilot_Loop, Tyber_Loop, Underworld_soldier_Loop, Wedge_Loop |
| story_underworld_m06_land.xml | expansion:Data/Config.meg | Silri_Loop, Tyber_Loop, Urai_Loop |
| story_underworld_m07_land.xml | expansion:Data/Config.meg | Collector_Droid_Loop, Commander_Moff_Loop, IG-88_Loop, Pirate_Loop, Stormtrooper_Loop, Tyber_Loop, Urai_Loop, Vader_Loop |
| story_underworld_m08_space.xml | expansion:Data/Config.meg | Rebel_Pilot_Loop, Urai_Loop |
| story_underworld_m09_space.xml | expansion:Data/Config.meg | Thrawn_Loop, Tyber_Loop, Underworld_soldier_Loop |
| story_underworld_m12_space.xml | expansion:Data/Config.meg | Han_Loop, Tyber_Loop, Underworld_soldier_Loop |
| story_underworld_mandalore_piracy.xml | expansion:Data/Config.meg | Tyber_Loop |
| story_underworld_mandalore_piracy_prologue.xml | expansion:Data/Config.meg | Tyber_Loop |
| story_underworld_mandalore_piracy_story.xml | expansion:Data/Config.meg | Tyber_Loop |
| story_underworld_moncalamari_piracy.xml | expansion:Data/Config.meg | IG-88_Loop |
| story_underworld_muunilinst_kidnapping.xml | expansion:Data/Config.meg | IG-88_Loop |
| story_underworld_naboo_intimidation.xml | expansion:Data/Config.meg | Silri_Loop |
| story_underworld_nalhutta_kidnapping.xml | expansion:Data/Config.meg | Urai_Loop |
| story_underworld_saleucami_intimidation.xml | expansion:Data/Config.meg | Urai_Loop |
| story_underworld_taris_intimidation.xml | expansion:Data/Config.meg | Silri_Loop |
| story_underworld_thyferra_kidnapping.xml | expansion:Data/Config.meg | Tyber_loop |
| story_underworld_utapau_kidnapping.xml | expansion:Data/Config.meg | Silri_Loop |

| Movies.xml name | Resolved VFS file | Supplying source | Signature / codec |
|---|---|---|---|
| Akbar_Loop | data/art/movies/binked/akbar_loop2.bik | MISSING | MISSING |
| AlphaTest | data/art/movies/binked/text_scroll_intro.bik | MISSING | MISSING |
| Antilles_Loop | UNDECLARED | MISSING | MISSING |
| Antillies_Loop | data/art/movies/binked/antillies_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Biggs_Loop | data/art/movies/binked/rebel_pilot_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Boba_Fett_Loop | data/art/movies/binked/boba_fett_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Bossk_Loop | data/art/movies/binked/bossk_loop.bik | expansion:Data/Movies.meg | BIKi / Bink 1 |
| C3PO_Loop | data/art/movies/binked/c3po_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Chewie_Loop | data/art/movies/binked/chewy_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Collector_Droid_Loop | data/art/movies/binked/collector_droid.bik | expansion:Data/Movies.meg | BIKi / Bink 1 |
| Commander_Moff2_Loop | data/art/movies/binked/moff_loop1.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Commander_Moff_Loop | data/art/movies/binked/moff_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Default_Incoming_Transmission | data/art/movies/binked/incoming_transmission.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Emperor_Loop | data/art/movies/binked/emperor_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Garm_Loop | data/art/movies/binked/garm_loop.bik | expansion:Data/Movies.meg | BIKi / Bink 1 |
| Generic_Sith_Loop | data/art/movies/binked/generic_sith.bik | expansion:Data/Movies.meg | BIKi / Bink 1 |
| Han_Loop | data/art/movies/binked/han_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| IG-88_Loop | data/art/movies/binked/ig-88_loop.bik | expansion:Data/Movies.meg | BIKi / Bink 1 |
| Jabba_Loop | data/art/movies/binked/jabba_loop.bik | expansion:Data/Movies.meg | BIKi / Bink 1 |
| Leia_Loop | data/art/movies/binked/leia_loop2.bik | MISSING | MISSING |
| Light_Loop | data/art/movies/binked/light_loop.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Luke_Loop | data/art/movies/binked/rebel_pilot_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Mon_Mothma_Loop | data/art/movies/binked/mon_mothma_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Obi_Wan_Loop | data/art/movies/binked/obi_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Piett_Loop | data/art/movies/binked/moff_loop1.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Pirate2_Loop | data/art/movies/binked/pirate_loop3.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Pirate_Loop | data/art/movies/binked/pirate_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Porkins_Loop | data/art/movies/binked/rebel_pilot_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| R2D2_Loop | data/art/movies/binked/r2d2_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Rebel_Pilot_Loop | data/art/movies/binked/rebel_pilot_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Rebel_Trooper2_Loop | data/art/movies/binked/rebel_trooper_loop3.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Rebel_Trooper3_Loop | data/art/movies/binked/rebel_trooper_loop4.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Rebel_Trooper_Loop | data/art/movies/binked/rebel_trooper_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Saboteur_Loop | data/art/movies/binked/saboteur_loop.bik | expansion:Data/Movies.meg | BIKi / Bink 1 |
| Silri_Loop | data/art/movies/binked/silri_loop.bik | expansion:Data/Movies.meg | BIKi / Bink 1 |
| Star_Wars_Intro | data/art/movies/binked/text_intro.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Star_Wars_Intro_Rebel | data/art/movies/binked/text_scroll_intro_short.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Star_Wars_Intro_Short | data/art/movies/binked/text_scroll_intro_short.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Star_Wars_Intro_Underworld | data/art/movies/binked/text_scroll_intro_short.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Stormtrooper_Loop | data/art/movies/binked/stormtrooper_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Tarkin_Loop | data/art/movies/binked/tarkin_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Thrawn_Loop | data/art/movies/binked/thrawn_loop.bik | expansion:Data/Movies.meg | BIKi / Bink 1 |
| Tyber_loop | data/art/movies/binked/tyber_zann.bik | expansion:Data/Movies.meg | BIKi / Bink 1 |
| Tyber_Loop | data/art/movies/binked/tyber_zann.bik | expansion:Data/Movies.meg | BIKi / Bink 1 |
| Underworld_Prolog_Text | data/art/movies/binked/text_intro.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Underworld_soldier_Loop | data/art/movies/binked/underworld_soldier_loop.bik | expansion:Data/Movies.meg | BIKi / Bink 1 |
| Urai_Loop | data/art/movies/binked/urai_loop.bik | expansion:Data/Movies.meg | BIKi / Bink 1 |
| Vader_Loop | data/art/movies/binked/vader_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Veers_Loop | data/art/movies/binked/veers_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Wedge_Loop | data/art/movies/binked/rebel_pilot_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Xizor_Loop | data/art/movies/binked/xizor_loop.bik | expansion:Data/Movies.meg | BIKi / Bink 1 |

Stop-only or stop-bearing XMLs (no additional movie-file reference): `story_campaign_empire_act_i.xml`, `story_campaign_empire_act_ii.xml`, `story_campaign_empire_act_iii.xml`, `story_campaign_empire_act_iv.xml`, `story_campaign_rebel_act_i.xml`, `story_campaign_rebel_act_ii.xml`, `story_campaign_rebel_act_iii.xml`, `story_campaign_rebel_act_iv.xml`, `story_campaign_underworld.xml`, `story_campaign_underworld_demo.xml`, `story_campaign_underworld_focus_demo.xml`, `story_campaign_underworld_tutorial.xml`, `story_empire_acti_m02_land.xml`, `story_empire_acti_m03_space.xml`, `story_empire_acti_m04_space.xml`, `story_empire_actii_m04_space.xml`, `story_empire_actii_m05_land.xml`, `story_empire_actii_m06_land.xml`, `story_empire_actiii_m08_space.xml`, `story_empire_activ_m10_space.xml`, `story_focus_demo_felucia_land_tactical.xml`, `story_focus_demo_felucia_space_tactical.xml`, `story_rebel_acti_m01_space.xml`, `story_rebel_acti_m02_land.xml`, `story_rebel_acti_m03_land.xml`, `story_rebel_actii_m04_space.xml`, `story_rebel_actii_m06_land.xml`, `story_rebel_actiii_m07_space.xml`, `story_rebel_actiii_m07a_space.xml`, `story_rebel_actiii_m08_space.xml`, `story_rebel_actiii_m09_space.xml`, `story_rebel_actiii_m10_land.xml`, `story_rebel_activ_m11_galactic.xml`, `story_rebel_activ_m12_galactic.xml`, `story_sandbox_focusdemo_empire.xml`, `story_tutorial_ii.xml`, `story_tutorial_iii.xml`, `story_tutorial_iv.xml`, `story_tutorial_m01_land.xml`, `story_tutorial_m03_space.xml`, `story_tutorial_m05_land.xml`, `story_tutorial_v.xml`, `story_underworld_aetenii_piracy.xml`, `story_underworld_alzociii_intimidation.xml`, `story_underworld_anaxes_intimidation.xml`, `story_underworld_bespin_piracy.xml`, `story_underworld_bespin_piracy_story.xml`, `story_underworld_bestine_kidnapping.xml`, `story_underworld_bothawui_intimidation.xml`, `story_underworld_byss_kidnapping.xml`, `story_underworld_carida_kidnapping.xml`, `story_underworld_corellia_piracy.xml`, `story_underworld_corulag_piracy.xml`, `story_underworld_dantooine_intimidation.xml`, `story_underworld_fondor_piracy.xml`, `story_underworld_honoghr_intimidation.xml`, `story_underworld_honoghr_intimidation_story.xml`, `story_underworld_kessel_piracy.xml`, `story_underworld_kuat_kidnapping.xml`, `story_underworld_m01_land.xml`, `story_underworld_m02_space.xml`, `story_underworld_m06_land.xml`, `story_underworld_m07_land.xml`, `story_underworld_m08_space.xml`, `story_underworld_m09_space.xml`, `story_underworld_m12_space.xml`, `story_underworld_mandalore_piracy.xml`, `story_underworld_mandalore_piracy_prologue.xml`, `story_underworld_mandalore_piracy_story.xml`, `story_underworld_moncalamari_piracy.xml`, `story_underworld_muunilinst_kidnapping.xml`, `story_underworld_naboo_intimidation.xml`, `story_underworld_nalhutta_kidnapping.xml`, `story_underworld_saleucami_intimidation.xml`, `story_underworld_taris_intimidation.xml`, `story_underworld_thyferra_kidnapping.xml`, `story_underworld_utapau_kidnapping.xml`.


### 1129810972

Movie registry: `data/xml/movies.xml`, supplied by `mod:loose:data/XML/Movies.xml`.

| Referencing XML (under data/xml/) | XML source | Movie names |
|---|---|---|
| factions.xml | mod:loose:data/XML/Factions.xml | Clone_Captain_Loop, Count_Dooku_Loop |
| story_republic_kamino_hologram.xml | mod:loose:data/XML/Story_Republic_Kamino_Hologram.xml | Clone_Ph1_Loop_Kamino, Commander_Cody_Loop, Cp_Rex_Loop, General_Grievous_Kamino |
| story_republic_order66_utapau.xml | mod:loose:data/XML/Story_Republic_Order66_Utapau.xml | Obi_Loop |
| story_sandbox_fall_of_the_jedi_empire.xml | mod:loose:data/XML/Story_Sandbox_Fall_of_The_Jedi_Empire.xml | Anakin_Loop, Anakin_Saber_Draw, Anakin_Saber_Loop, Commander_Cody_Loop, Emperor_Loop, Mace_Loop, Obiwan_Loop, Padme_Loop, Palpatine_Loop, Yoda_Loop |
| story_sandbox_mid_rim_conflict_empire.xml | mod:loose:data/XML/Story_Sandbox_Mid_Rim_Conflict_Empire.xml | Padme_Loop |
| story_sandbox_multiplayer_shared_republic.xml | mod:loose:data/XML/Story_Sandbox_Multiplayer_Shared_Republic.xml | Padme_Loop |
| story_sandbox_outer_rim_sieges_empire.xml | mod:loose:data/XML/Story_Sandbox_Outer_Rim_Sieges_Empire.xml | Padme_Loop |
| story_sandbox_raising_conflict_empire.xml | mod:loose:data/XML/Story_Sandbox_Raising_Conflict_Empire.xml | Padme_Loop |
| story_sandbox_the_clone_wars_empire.xml | mod:loose:data/XML/Story_sandbox_The_Clone_Wars_empire.xml | Padme_Loop |
| story_sandbox_triad_of_evil_empire.xml | mod:loose:data/XML/Story_Sandbox_Triad_of_Evil_Empire.xml | Padme_Loop |

These XML reference rows are identical to retail: `story_campaign_empire_act_i.xml`, `story_campaign_empire_act_ii.xml`, `story_campaign_empire_act_iii.xml`, `story_campaign_empire_act_iv.xml`, `story_campaign_rebel_act_i.xml`, `story_campaign_rebel_act_ii.xml`, `story_campaign_rebel_act_iii.xml`, `story_campaign_rebel_act_iv.xml`, `story_campaign_underworld.xml`, `story_campaign_underworld_demo.xml`, `story_campaign_underworld_focus_demo.xml`, `story_campaign_underworld_tutorial.xml`, `story_empire_acti_m02_land.xml`, `story_empire_acti_m03_space.xml`, `story_empire_acti_m04_space.xml`, `story_empire_actii_m04_space.xml`, `story_empire_actii_m05_land.xml`, `story_empire_actii_m06_land.xml`, `story_empire_actiii_m08_space.xml`, `story_empire_activ_m10_space.xml`, `story_empire_activ_m11_space.xml`, `story_focus_demo_felucia_land_tactical.xml`, `story_focus_demo_felucia_space_tactical.xml`, `story_rebel_acti_m01_space.xml`, `story_rebel_acti_m02_land.xml`, `story_rebel_acti_m03_land.xml`, `story_rebel_actii_m04_space.xml`, `story_rebel_actii_m06_land.xml`, `story_rebel_actiii_m07_space.xml`, `story_rebel_actiii_m07a_space.xml`, `story_rebel_actiii_m08_space.xml`, `story_rebel_actiii_m09_space.xml`, `story_rebel_actiii_m10_land.xml`, `story_rebel_activ_m11_galactic.xml`, `story_rebel_activ_m12_galactic.xml`, `story_sandbox_focusdemo_empire.xml`, `story_tutorial_ii.xml`, `story_tutorial_iii.xml`, `story_tutorial_iv.xml`, `story_tutorial_m01_land.xml`, `story_tutorial_m03_space.xml`, `story_tutorial_m05_land.xml`, `story_tutorial_v.xml`, `story_underworld_aetenii_piracy.xml`, `story_underworld_alzociii_intimidation.xml`, `story_underworld_anaxes_intimidation.xml`, `story_underworld_bespin_piracy.xml`, `story_underworld_bespin_piracy_story.xml`, `story_underworld_bestine_kidnapping.xml`, `story_underworld_bothawui_intimidation.xml`, `story_underworld_byss_kidnapping.xml`, `story_underworld_carida_kidnapping.xml`, `story_underworld_corellia_piracy.xml`, `story_underworld_corulag_piracy.xml`, `story_underworld_dantooine_intimidation.xml`, `story_underworld_fondor_piracy.xml`, `story_underworld_honoghr_intimidation.xml`, `story_underworld_honoghr_intimidation_story.xml`, `story_underworld_kessel_piracy.xml`, `story_underworld_kuat_kidnapping.xml`, `story_underworld_m01_land.xml`, `story_underworld_m02_space.xml`, `story_underworld_m06_land.xml`, `story_underworld_m07_land.xml`, `story_underworld_m08_space.xml`, `story_underworld_m09_space.xml`, `story_underworld_m12_space.xml`, `story_underworld_mandalore_piracy.xml`, `story_underworld_mandalore_piracy_prologue.xml`, `story_underworld_mandalore_piracy_story.xml`, `story_underworld_moncalamari_piracy.xml`, `story_underworld_muunilinst_kidnapping.xml`, `story_underworld_naboo_intimidation.xml`, `story_underworld_nalhutta_kidnapping.xml`, `story_underworld_saleucami_intimidation.xml`, `story_underworld_taris_intimidation.xml`, `story_underworld_thyferra_kidnapping.xml`, `story_underworld_utapau_kidnapping.xml`.


| Movies.xml name | Resolved VFS file | Supplying source | Signature / codec |
|---|---|---|---|
| Anakin_Loop | data/art/movies/binked/anakin.bik | mod:loose:data/Art/Movies/Binked/Anakin.bik | BIKi / Bink 1 |
| Anakin_Saber_Draw | data/art/movies/binked/anakindraw.bik | mod:loose:data/Art/Movies/Binked/AnakinDraw.bik | BIKi / Bink 1 |
| Anakin_Saber_Loop | data/art/movies/binked/anakinsword.bik | mod:loose:data/Art/Movies/Binked/AnakinSword.bik | BIKi / Bink 1 |
| Antillies_Loop | UNDECLARED | MISSING | MISSING |
| Boba_Fett_Loop | UNDECLARED | MISSING | MISSING |
| Bossk_Loop | UNDECLARED | MISSING | MISSING |
| C3PO_Loop | UNDECLARED | MISSING | MISSING |
| Clone_Captain_Loop | data/art/movies/binked/capt_holo2.bik | mod:loose:data/Art/Movies/Binked/capt_holo2.bik | BIKi / Bink 1 |
| Clone_Ph1_Loop_Kamino | data/art/movies/binked/capt_holo2.bik | mod:loose:data/Art/Movies/Binked/capt_holo2.bik | BIKi / Bink 1 |
| Collector_Droid_Loop | UNDECLARED | MISSING | MISSING |
| Commander_Cody_Loop | data/art/movies/binked/cody.bik | mod:loose:data/Art/Movies/Binked/cody.bik | BIKi / Bink 1 |
| Commander_Moff_Loop | UNDECLARED | MISSING | MISSING |
| Count_Dooku_Loop | data/art/movies/binked/dooku0.bik | mod:loose:data/Art/Movies/Binked/dooku0.bik | BIKi / Bink 1 |
| Cp_Rex_Loop | data/art/movies/binked/rex.bik | mod:loose:data/Art/Movies/Binked/Rex.bik | BIKi / Bink 1 |
| Emperor_Loop | UNDECLARED | MISSING | MISSING |
| General_Grievous_Kamino | data/art/movies/binked/grievous.bik | mod:loose:data/Art/Movies/Binked/Grievous.bik | BIKi / Bink 1 |
| Han_Loop | UNDECLARED | MISSING | MISSING |
| IG-88_Loop | UNDECLARED | MISSING | MISSING |
| Jabba_Loop | UNDECLARED | MISSING | MISSING |
| Mace_Diplo_Loop | data/art/movies/binked/mace.bik | mod:loose:data/Art/Movies/Binked/Mace.bik | BIKi / Bink 1 |
| Mace_Loop | data/art/movies/binked/mace.bik | mod:loose:data/Art/Movies/Binked/Mace.bik | BIKi / Bink 1 |
| Mon_Mothma_Loop | UNDECLARED | MISSING | MISSING |
| Obi_Loop | UNDECLARED | MISSING | MISSING |
| Obiwan_Diplo_Loop | data/art/movies/binked/obiwan.bik | mod:loose:data/Art/Movies/Binked/Obiwan.bik | BIKi / Bink 1 |
| Obiwan_Loop | data/art/movies/binked/obiwan.bik | mod:loose:data/Art/Movies/Binked/Obiwan.bik | BIKi / Bink 1 |
| Padme_Loop | data/art/movies/binked/padme.bik | mod:loose:data/Art/Movies/Binked/Padme.bik | BIKi / Bink 1 |
| Palpatine_Loop | data/art/movies/binked/palpatine.bik | mod:loose:data/Art/Movies/Binked/Palpatine.bik | BIKi / Bink 1 |
| Piett_Loop | UNDECLARED | MISSING | MISSING |
| Pirate2_Loop | UNDECLARED | MISSING | MISSING |
| Pirate_Loop | UNDECLARED | MISSING | MISSING |
| R2D2_Loop | UNDECLARED | MISSING | MISSING |
| Rebel_Pilot_Loop | UNDECLARED | MISSING | MISSING |
| Rebel_Trooper2_Loop | UNDECLARED | MISSING | MISSING |
| Rebel_Trooper3_Loop | UNDECLARED | MISSING | MISSING |
| Rebel_Trooper_Loop | UNDECLARED | MISSING | MISSING |
| Saboteur_Loop | UNDECLARED | MISSING | MISSING |
| Silri_Loop | UNDECLARED | MISSING | MISSING |
| Stormtrooper_Loop | UNDECLARED | MISSING | MISSING |
| Tarkin_Loop | UNDECLARED | MISSING | MISSING |
| Thrawn_Loop | UNDECLARED | MISSING | MISSING |
| Tyber_loop | UNDECLARED | MISSING | MISSING |
| Tyber_Loop | UNDECLARED | MISSING | MISSING |
| Underworld_soldier_Loop | UNDECLARED | MISSING | MISSING |
| Urai_Loop | UNDECLARED | MISSING | MISSING |
| Vader_Loop | UNDECLARED | MISSING | MISSING |
| Veers_Loop | UNDECLARED | MISSING | MISSING |
| Wedge_Loop | UNDECLARED | MISSING | MISSING |
| Xizor_Loop | UNDECLARED | MISSING | MISSING |
| Yoda_Loop | data/art/movies/binked/yoda.bik | mod:loose:data/Art/Movies/Binked/yoda.bik | BIKi / Bink 1 |

These movie resolution rows are identical to retail: `Antilles_Loop`, `Default_Incoming_Transmission`, `Light_Loop`, `Star_Wars_Intro`.


Stop-only or stop-bearing XMLs (no additional movie-file reference): `story_campaign_empire_act_i.xml`, `story_campaign_empire_act_ii.xml`, `story_campaign_empire_act_iii.xml`, `story_campaign_empire_act_iv.xml`, `story_campaign_rebel_act_i.xml`, `story_campaign_rebel_act_ii.xml`, `story_campaign_rebel_act_iii.xml`, `story_campaign_rebel_act_iv.xml`, `story_campaign_underworld.xml`, `story_campaign_underworld_demo.xml`, `story_campaign_underworld_focus_demo.xml`, `story_campaign_underworld_tutorial.xml`, `story_empire_acti_m02_land.xml`, `story_empire_acti_m03_space.xml`, `story_empire_acti_m04_space.xml`, `story_empire_actii_m04_space.xml`, `story_empire_actii_m05_land.xml`, `story_empire_actii_m06_land.xml`, `story_empire_actiii_m08_space.xml`, `story_empire_activ_m10_space.xml`, `story_focus_demo_felucia_land_tactical.xml`, `story_focus_demo_felucia_space_tactical.xml`, `story_rebel_acti_m01_space.xml`, `story_rebel_acti_m02_land.xml`, `story_rebel_acti_m03_land.xml`, `story_rebel_actii_m04_space.xml`, `story_rebel_actii_m06_land.xml`, `story_rebel_actiii_m07_space.xml`, `story_rebel_actiii_m07a_space.xml`, `story_rebel_actiii_m08_space.xml`, `story_rebel_actiii_m09_space.xml`, `story_rebel_actiii_m10_land.xml`, `story_rebel_activ_m11_galactic.xml`, `story_rebel_activ_m12_galactic.xml`, `story_republic_kamino_hologram.xml`, `story_republic_order66_utapau.xml`, `story_sandbox_fall_of_the_jedi_empire.xml`, `story_sandbox_focusdemo_empire.xml`, `story_sandbox_mid_rim_conflict_empire.xml`, `story_sandbox_multiplayer_shared_republic.xml`, `story_sandbox_outer_rim_sieges_empire.xml`, `story_sandbox_raising_conflict_empire.xml`, `story_sandbox_the_clone_wars_empire.xml`, `story_sandbox_triad_of_evil_empire.xml`, `story_tutorial_ii.xml`, `story_tutorial_iii.xml`, `story_tutorial_iv.xml`, `story_tutorial_m01_land.xml`, `story_tutorial_m03_space.xml`, `story_tutorial_m05_land.xml`, `story_tutorial_v.xml`, `story_underworld_aetenii_piracy.xml`, `story_underworld_alzociii_intimidation.xml`, `story_underworld_anaxes_intimidation.xml`, `story_underworld_bespin_piracy.xml`, `story_underworld_bespin_piracy_story.xml`, `story_underworld_bestine_kidnapping.xml`, `story_underworld_bothawui_intimidation.xml`, `story_underworld_byss_kidnapping.xml`, `story_underworld_carida_kidnapping.xml`, `story_underworld_corellia_piracy.xml`, `story_underworld_corulag_piracy.xml`, `story_underworld_dantooine_intimidation.xml`, `story_underworld_fondor_piracy.xml`, `story_underworld_honoghr_intimidation.xml`, `story_underworld_honoghr_intimidation_story.xml`, `story_underworld_kessel_piracy.xml`, `story_underworld_kuat_kidnapping.xml`, `story_underworld_m01_land.xml`, `story_underworld_m02_space.xml`, `story_underworld_m06_land.xml`, `story_underworld_m07_land.xml`, `story_underworld_m08_space.xml`, `story_underworld_m09_space.xml`, `story_underworld_m12_space.xml`, `story_underworld_mandalore_piracy.xml`, `story_underworld_mandalore_piracy_prologue.xml`, `story_underworld_mandalore_piracy_story.xml`, `story_underworld_moncalamari_piracy.xml`, `story_underworld_muunilinst_kidnapping.xml`, `story_underworld_naboo_intimidation.xml`, `story_underworld_nalhutta_kidnapping.xml`, `story_underworld_saleucami_intimidation.xml`, `story_underworld_taris_intimidation.xml`, `story_underworld_thyferra_kidnapping.xml`, `story_underworld_utapau_kidnapping.xml`.


### 1770851727

Movie registry: `data/xml/movies.xml`, supplied by `mod:loose:data/Xml/MOVIES.XML`.

| Referencing XML (under data/xml/) | XML source | Movie names |
|---|---|---|

These XML reference rows are identical to retail: `expansion_factions.xml`, `story_campaign_empire_act_i.xml`, `story_campaign_empire_act_ii.xml`, `story_campaign_empire_act_iii.xml`, `story_campaign_empire_act_iv.xml`, `story_campaign_rebel_act_i.xml`, `story_campaign_rebel_act_ii.xml`, `story_campaign_rebel_act_iii.xml`, `story_campaign_rebel_act_iv.xml`, `story_campaign_underworld.xml`, `story_campaign_underworld_demo.xml`, `story_campaign_underworld_focus_demo.xml`, `story_campaign_underworld_tutorial.xml`, `story_empire_acti_m02_land.xml`, `story_empire_acti_m03_space.xml`, `story_empire_acti_m04_space.xml`, `story_empire_actii_m04_space.xml`, `story_empire_actii_m05_land.xml`, `story_empire_actii_m06_land.xml`, `story_empire_actiii_m08_space.xml`, `story_empire_activ_m10_space.xml`, `story_empire_activ_m11_space.xml`, `story_focus_demo_felucia_land_tactical.xml`, `story_focus_demo_felucia_space_tactical.xml`, `story_rebel_acti_m01_space.xml`, `story_rebel_acti_m02_land.xml`, `story_rebel_acti_m03_land.xml`, `story_rebel_actii_m04_space.xml`, `story_rebel_actii_m06_land.xml`, `story_rebel_actiii_m07_space.xml`, `story_rebel_actiii_m07a_space.xml`, `story_rebel_actiii_m08_space.xml`, `story_rebel_actiii_m09_space.xml`, `story_rebel_actiii_m10_land.xml`, `story_rebel_activ_m11_galactic.xml`, `story_rebel_activ_m12_galactic.xml`, `story_sandbox_focusdemo_empire.xml`, `story_tutorial_ii.xml`, `story_tutorial_iii.xml`, `story_tutorial_iv.xml`, `story_tutorial_m01_land.xml`, `story_tutorial_m03_space.xml`, `story_tutorial_m05_land.xml`, `story_tutorial_v.xml`, `story_underworld_aetenii_piracy.xml`, `story_underworld_alzociii_intimidation.xml`, `story_underworld_anaxes_intimidation.xml`, `story_underworld_bespin_piracy.xml`, `story_underworld_bespin_piracy_story.xml`, `story_underworld_bestine_kidnapping.xml`, `story_underworld_bothawui_intimidation.xml`, `story_underworld_byss_kidnapping.xml`, `story_underworld_carida_kidnapping.xml`, `story_underworld_corellia_piracy.xml`, `story_underworld_corulag_piracy.xml`, `story_underworld_dantooine_intimidation.xml`, `story_underworld_fondor_piracy.xml`, `story_underworld_honoghr_intimidation.xml`, `story_underworld_honoghr_intimidation_story.xml`, `story_underworld_kessel_piracy.xml`, `story_underworld_kuat_kidnapping.xml`, `story_underworld_m01_land.xml`, `story_underworld_m02_space.xml`, `story_underworld_m06_land.xml`, `story_underworld_m07_land.xml`, `story_underworld_m08_space.xml`, `story_underworld_m09_space.xml`, `story_underworld_m12_space.xml`, `story_underworld_mandalore_piracy.xml`, `story_underworld_mandalore_piracy_prologue.xml`, `story_underworld_mandalore_piracy_story.xml`, `story_underworld_moncalamari_piracy.xml`, `story_underworld_muunilinst_kidnapping.xml`, `story_underworld_naboo_intimidation.xml`, `story_underworld_nalhutta_kidnapping.xml`, `story_underworld_saleucami_intimidation.xml`, `story_underworld_taris_intimidation.xml`, `story_underworld_thyferra_kidnapping.xml`, `story_underworld_utapau_kidnapping.xml`.


| Movies.xml name | Resolved VFS file | Supplying source | Signature / codec |
|---|---|---|---|
| Underworld_soldier_Loop | data/art/movies/binked/boba_fett_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |

These movie resolution rows are identical to retail: `Akbar_Loop`, `AlphaTest`, `Antilles_Loop`, `Antillies_Loop`, `Biggs_Loop`, `Boba_Fett_Loop`, `Bossk_Loop`, `C3PO_Loop`, `Chewie_Loop`, `Collector_Droid_Loop`, `Commander_Moff2_Loop`, `Commander_Moff_Loop`, `Default_Incoming_Transmission`, `Emperor_Loop`, `Garm_Loop`, `Generic_Sith_Loop`, `Han_Loop`, `IG-88_Loop`, `Jabba_Loop`, `Leia_Loop`, `Light_Loop`, `Luke_Loop`, `Mon_Mothma_Loop`, `Obi_Wan_Loop`, `Piett_Loop`, `Pirate2_Loop`, `Pirate_Loop`, `Porkins_Loop`, `R2D2_Loop`, `Rebel_Pilot_Loop`, `Rebel_Trooper2_Loop`, `Rebel_Trooper3_Loop`, `Rebel_Trooper_Loop`, `Saboteur_Loop`, `Silri_Loop`, `Star_Wars_Intro`, `Star_Wars_Intro_Rebel`, `Star_Wars_Intro_Short`, `Star_Wars_Intro_Underworld`, `Stormtrooper_Loop`, `Tarkin_Loop`, `Thrawn_Loop`, `Tyber_loop`, `Tyber_Loop`, `Underworld_Prolog_Text`, `Urai_Loop`, `Vader_Loop`, `Veers_Loop`, `Wedge_Loop`, `Xizor_Loop`.


Stop-only or stop-bearing XMLs (no additional movie-file reference): `story_campaign_empire_act_i.xml`, `story_campaign_empire_act_ii.xml`, `story_campaign_empire_act_iii.xml`, `story_campaign_empire_act_iv.xml`, `story_campaign_rebel_act_i.xml`, `story_campaign_rebel_act_ii.xml`, `story_campaign_rebel_act_iii.xml`, `story_campaign_rebel_act_iv.xml`, `story_campaign_underworld.xml`, `story_campaign_underworld_demo.xml`, `story_campaign_underworld_focus_demo.xml`, `story_campaign_underworld_tutorial.xml`, `story_empire_acti_m02_land.xml`, `story_empire_acti_m03_space.xml`, `story_empire_acti_m04_space.xml`, `story_empire_actii_m04_space.xml`, `story_empire_actii_m05_land.xml`, `story_empire_actii_m06_land.xml`, `story_empire_actiii_m08_space.xml`, `story_empire_activ_m10_space.xml`, `story_focus_demo_felucia_land_tactical.xml`, `story_focus_demo_felucia_space_tactical.xml`, `story_rebel_acti_m01_space.xml`, `story_rebel_acti_m02_land.xml`, `story_rebel_acti_m03_land.xml`, `story_rebel_actii_m04_space.xml`, `story_rebel_actii_m06_land.xml`, `story_rebel_actiii_m07_space.xml`, `story_rebel_actiii_m07a_space.xml`, `story_rebel_actiii_m08_space.xml`, `story_rebel_actiii_m09_space.xml`, `story_rebel_actiii_m10_land.xml`, `story_rebel_activ_m11_galactic.xml`, `story_rebel_activ_m12_galactic.xml`, `story_sandbox_focusdemo_empire.xml`, `story_tutorial_ii.xml`, `story_tutorial_iii.xml`, `story_tutorial_iv.xml`, `story_tutorial_m01_land.xml`, `story_tutorial_m03_space.xml`, `story_tutorial_m05_land.xml`, `story_tutorial_v.xml`, `story_underworld_aetenii_piracy.xml`, `story_underworld_alzociii_intimidation.xml`, `story_underworld_anaxes_intimidation.xml`, `story_underworld_bespin_piracy.xml`, `story_underworld_bespin_piracy_story.xml`, `story_underworld_bestine_kidnapping.xml`, `story_underworld_bothawui_intimidation.xml`, `story_underworld_byss_kidnapping.xml`, `story_underworld_carida_kidnapping.xml`, `story_underworld_corellia_piracy.xml`, `story_underworld_corulag_piracy.xml`, `story_underworld_dantooine_intimidation.xml`, `story_underworld_fondor_piracy.xml`, `story_underworld_honoghr_intimidation.xml`, `story_underworld_honoghr_intimidation_story.xml`, `story_underworld_kessel_piracy.xml`, `story_underworld_kuat_kidnapping.xml`, `story_underworld_m01_land.xml`, `story_underworld_m02_space.xml`, `story_underworld_m06_land.xml`, `story_underworld_m07_land.xml`, `story_underworld_m08_space.xml`, `story_underworld_m09_space.xml`, `story_underworld_m12_space.xml`, `story_underworld_mandalore_piracy.xml`, `story_underworld_mandalore_piracy_prologue.xml`, `story_underworld_mandalore_piracy_story.xml`, `story_underworld_moncalamari_piracy.xml`, `story_underworld_muunilinst_kidnapping.xml`, `story_underworld_naboo_intimidation.xml`, `story_underworld_nalhutta_kidnapping.xml`, `story_underworld_saleucami_intimidation.xml`, `story_underworld_taris_intimidation.xml`, `story_underworld_thyferra_kidnapping.xml`, `story_underworld_utapau_kidnapping.xml`.


### 1976399102

Movie registry: `data/xml/movies.xml`, supplied by `mod:loose:data/XML/Movies.xml`.

| Referencing XML (under data/xml/) | XML source | Movie names |
|---|---|---|
| conquests/historical/19_bby_clonewarsouterrimsieges/story_sandbox_outerrimsieges_cis.xml | mod:loose:data/XML/Conquests/Historical/19_BBY_CloneWarsOuterRimSieges/Story_Sandbox_OuterRimSieges_CIS.xml | Dooku_Loop, Emperor_Loop, Grievous_Loop, Gunray_Loop, SoBilles_Loop |
| conquests/historical/19_bby_clonewarsouterrimsieges/story_sandbox_outerrimsieges_republic.xml | mod:loose:data/XML/Conquests/Historical/19_BBY_CloneWarsOuterRimSieges/Story_Sandbox_OuterRimSieges_Republic.xml | Aayla_Loop, Anakin_Loop, Antillies_Loop, Dyne_Loop, Emperor_Loop, Mace_Loop, ObiWan_Loop, PalpatineDED_Loop, PalpatineFotR_Loop, Tholme_Loop |
| conquests/historical/20_bby_clonewarsfoerost/story_sandbox_foerost_cis.xml | mod:loose:data/XML/Conquests/Historical/20_BBY_CloneWarsFoerost/Story_Sandbox_Foerost_CIS.xml | Dooku_Loop, Sian_Tevv_Loop, Tambor_Loop |
| conquests/historical/20_bby_clonewarsfoerost/story_sandbox_foerost_republic.xml | mod:loose:data/XML/Conquests/Historical/20_BBY_CloneWarsFoerost/Story_Sandbox_Foerost_Republic.xml | Antillies_Loop, PalpatineFotR_Loop, Piett_Loop |
| conquests/historical/21_bby_clonewarsdurgeslance/story_sandbox_durgeslance_cis.xml | mod:loose:data/XML/Conquests/Historical/21_BBY_CloneWarsDurgesLance/Story_Sandbox_DurgesLance_CIS.xml | Dooku_Loop, Grievous_Loop, Piett_Loop, SoBilles_Loop, Viqi_Shesh_Loop |
| conquests/historical/21_bby_clonewarsdurgeslance/story_sandbox_durgeslance_republic.xml | mod:loose:data/XML/Conquests/Historical/21_BBY_CloneWarsDurgesLance/Story_Sandbox_DurgesLance_Republic.xml | Garm_Loop, Grievous_Loop, Navik_Loop, Piett_Loop |
| conquests/historical/21_bby_clonewarsknighthammer/story_sandbox_knighthammer_cis.xml | mod:loose:data/XML/Conquests/Historical/21_BBY_CloneWarsKnightHammer/Story_Sandbox_KnightHammer_CIS.xml | Anakin_Loop, Dooku_Loop, Generic_Sith_Loop, Tonith_Loop |
| conquests/historical/21_bby_clonewarsknighthammer/story_sandbox_knighthammer_hutts.xml | mod:loose:data/XML/Conquests/Historical/21_BBY_CloneWarsKnightHammer/Story_Sandbox_KnightHammer_Hutts.xml | Generic_Sith_Loop, Xizor_Loop |
| conquests/historical/21_bby_clonewarsknighthammer/story_sandbox_knighthammer_republic.xml | mod:loose:data/XML/Conquests/Historical/21_BBY_CloneWarsKnightHammer/Story_Sandbox_KnightHammer_Republic.xml | Anakin_Loop, Mace_Loop, Tarkin_Loop, Viqi_Shesh_Loop, Yoda_Loop |
| conquests/historical/21_bby_clonewarstennuutta/story_sandbox_tennuutta_cis.xml | mod:loose:data/XML/Conquests/Historical/21_BBY_CloneWarsTennuutta/Story_Sandbox_Tennuutta_CIS.xml | Antilles_Loop, Dooku_Loop, Grievous_Loop, Gunray_Loop |
| conquests/historical/21_bby_clonewarstennuutta/story_sandbox_tennuutta_republic.xml | mod:loose:data/XML/Conquests/Historical/21_BBY_CloneWarsTennuutta/Story_Sandbox_Tennuutta_Republic.xml | Anakin_Loop, Commander_Moff_Loop, PalpatineFotR_Loop, Tarkin_Loop |
| conquests/historical/22_bby_clonewarsmalevolence/story_sandbox_malevolence_cis.xml | mod:loose:data/XML/Conquests/Historical/22_BBY_CloneWarsMalevolence/Story_Sandbox_Malevolence_CIS.xml | Dooku_Loop, Grievous_Loop |
| conquests/historical/22_bby_clonewarsmalevolence/story_sandbox_malevolence_republic.xml | mod:loose:data/XML/Conquests/Historical/22_BBY_CloneWarsMalevolence/Story_Sandbox_Malevolence_Republic.xml | Anakin_Loop, Collector_Droid_Loop, PalpatineFotR_Loop |
| conquests/historical/22_bby_clonewarsrimward/story_sandbox_rimward_cis.xml | mod:loose:data/XML/Conquests/Historical/22_BBY_CloneWarsRimward/Story_Sandbox_Rimward_CIS.xml | Dooku_Loop, Droid_Loop, Grievous_Loop, Navik_Loop, Passel_Agente_Loop, Poggle_Loop, Silri_Loop, Sobilles_Loop, Tambor_Loop, Teneniel_Loop, Ventress_Loop |
| conquests/historical/22_bby_clonewarsrimward/story_sandbox_rimward_hutts.xml | mod:loose:data/XML/Conquests/Historical/22_BBY_CloneWarsRimward/Story_Sandbox_Rimward_Hutts.xml | Jabba_Loop, Saboteur_Loop, Zorba_Loop |
| conquests/historical/22_bby_clonewarsrimward/story_sandbox_rimward_republic.xml | mod:loose:data/XML/Conquests/Historical/22_BBY_CloneWarsRimward/Story_Sandbox_Rimward_Republic.xml | Anakin_Loop, C3PO_Loop, Mace_Loop, ObiWan2_Loop, PalpatineFotR_Loop, Piett_Loop, R2_Loop, Teneniel_Loop |
| conquests/progressive/story_sandbox_clonewars_cis.xml | mod:loose:data/XML/Conquests/Progressive/Story_Sandbox_CloneWars_CIS.xml | Durge_Loop, Grievous_Loop |
| conquests/progressive/story_sandbox_clonewars_container.xml | mod:loose:data/XML/Conquests/Progressive/Story_Sandbox_CloneWars_Container.xml | Geonosian_Loop |
| conquests/story_sandbox_government_rep.xml | mod:loose:data/XML/Conquests/Story_Sandbox_Government_Rep.xml | Anakin_Loop, Antillies_Loop, Emperor_Loop, Mace_Loop, Mon_Mothma_Loop, PalpatineDED_Loop, PalpatineFotR_Loop, Pestage_Loop, Viqi_Shesh_Loop, Wedge_Loop |
| storyevents/tactical_crazy_clone.xml | mod:loose:data/XML/StoryEvents/Tactical_Crazy_Clone.xml | Commander_Moff_Loop |
| storyevents/tactical_duro_defence.xml | mod:loose:data/XML/StoryEvents/Tactical_Duro_Defence.xml | Sobilles_Loop |
| storyevents/tactical_duro_drama.xml | mod:loose:data/XML/StoryEvents/Tactical_Duro_Drama.XML | Grievous_Loop, Sobilles_Loop |
| storyevents/tactical_holo_hunt.xml | mod:loose:data/XML/StoryEvents/Tactical_Holo_Hunt.xml | Commander_Moff_Loop, Grievous_Loop |
| storyevents/tactical_jyvus_joyride.xml | mod:loose:data/XML/StoryEvents/Tactical_Jyvus_Joyride.xml | Grievous_Loop, Sobilles_Loop |
| storyevents/tactical_mission_malevolence_i.xml | mod:loose:data/XML/StoryEvents/Tactical_Mission_Malevolence_I.XML | Anakin_Loop, Grievous_Loop, Piett_Loop, Veers_Loop |
| storyevents/tactical_mission_malevolence_ii.xml | mod:loose:data/XML/StoryEvents/Tactical_Mission_Malevolence_II.XML | Anakin_Loop, Grievous_Loop, Piett_Loop, Veers_Loop |
| storyevents/tactical_ruusan_roulette.xml | mod:loose:data/XML/StoryEvents/Tactical_Ruusan_Roulette.XML | Anakin_Loop |
| storyevents/tactical_shipyard_struggle.xml | mod:loose:data/XML/StoryEvents/Tactical_Shipyard_Struggle.xml | Bossk_Loop |

These XML reference rows are identical to retail: `story_campaign_empire_act_i.xml`, `story_campaign_empire_act_ii.xml`, `story_campaign_empire_act_iii.xml`, `story_campaign_empire_act_iv.xml`, `story_campaign_rebel_act_i.xml`, `story_campaign_rebel_act_ii.xml`, `story_campaign_rebel_act_iii.xml`, `story_campaign_rebel_act_iv.xml`, `story_campaign_underworld.xml`, `story_campaign_underworld_demo.xml`, `story_campaign_underworld_focus_demo.xml`, `story_campaign_underworld_tutorial.xml`, `story_empire_acti_m02_land.xml`, `story_empire_acti_m03_space.xml`, `story_empire_acti_m04_space.xml`, `story_empire_actii_m04_space.xml`, `story_empire_actii_m05_land.xml`, `story_empire_actii_m06_land.xml`, `story_empire_actiii_m08_space.xml`, `story_empire_activ_m10_space.xml`, `story_empire_activ_m11_space.xml`, `story_focus_demo_felucia_land_tactical.xml`, `story_focus_demo_felucia_space_tactical.xml`, `story_rebel_acti_m01_space.xml`, `story_rebel_acti_m02_land.xml`, `story_rebel_acti_m03_land.xml`, `story_rebel_actii_m04_space.xml`, `story_rebel_actii_m06_land.xml`, `story_rebel_actiii_m07_space.xml`, `story_rebel_actiii_m07a_space.xml`, `story_rebel_actiii_m08_space.xml`, `story_rebel_actiii_m09_space.xml`, `story_rebel_actiii_m10_land.xml`, `story_rebel_activ_m11_galactic.xml`, `story_rebel_activ_m12_galactic.xml`, `story_sandbox_focusdemo_empire.xml`, `story_tutorial_ii.xml`, `story_tutorial_iii.xml`, `story_tutorial_iv.xml`, `story_tutorial_m01_land.xml`, `story_tutorial_m03_space.xml`, `story_tutorial_m05_land.xml`, `story_tutorial_v.xml`, `story_underworld_aetenii_piracy.xml`, `story_underworld_alzociii_intimidation.xml`, `story_underworld_anaxes_intimidation.xml`, `story_underworld_bespin_piracy.xml`, `story_underworld_bespin_piracy_story.xml`, `story_underworld_bestine_kidnapping.xml`, `story_underworld_bothawui_intimidation.xml`, `story_underworld_byss_kidnapping.xml`, `story_underworld_carida_kidnapping.xml`, `story_underworld_corellia_piracy.xml`, `story_underworld_corulag_piracy.xml`, `story_underworld_dantooine_intimidation.xml`, `story_underworld_fondor_piracy.xml`, `story_underworld_honoghr_intimidation.xml`, `story_underworld_honoghr_intimidation_story.xml`, `story_underworld_kessel_piracy.xml`, `story_underworld_kuat_kidnapping.xml`, `story_underworld_m01_land.xml`, `story_underworld_m02_space.xml`, `story_underworld_m06_land.xml`, `story_underworld_m07_land.xml`, `story_underworld_m08_space.xml`, `story_underworld_m09_space.xml`, `story_underworld_m12_space.xml`, `story_underworld_mandalore_piracy.xml`, `story_underworld_mandalore_piracy_prologue.xml`, `story_underworld_mandalore_piracy_story.xml`, `story_underworld_moncalamari_piracy.xml`, `story_underworld_muunilinst_kidnapping.xml`, `story_underworld_naboo_intimidation.xml`, `story_underworld_nalhutta_kidnapping.xml`, `story_underworld_saleucami_intimidation.xml`, `story_underworld_taris_intimidation.xml`, `story_underworld_thyferra_kidnapping.xml`, `story_underworld_utapau_kidnapping.xml`.


| Movies.xml name | Resolved VFS file | Supplying source | Signature / codec |
|---|---|---|---|
| A_Long_Time_Ago_Campaign_Intro | data/art/movies/binked/text_intro.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Aayla_Loop | UNDECLARED | MISSING | MISSING |
| Ackbar_Loop | data/art/movies/binked/ackbar.bik | mod:loose:data/Art/Movies/Binked/ackbar.bik | BIKi / Bink 1 |
| Anakin_Loop | data/art/movies/binked/anakin-adca.bik | mod:loose:data/Art/Movies/Binked/anakin-adca.bik | BIKi / Bink 1 |
| Booster_Loop | data/art/movies/binked/booster.bik | mod:loose:data/Art/Movies/Binked/booster.bik | BIKi / Bink 1 |
| Borsk_Loop | data/art/movies/binked/borsk.bik | mod:loose:data/Art/Movies/Binked/borsk.bik | BIKi / Bink 1 |
| Carnor_Loop | data/art/movies/binked/jax_loop.bik | mod:loose:data/Art/Movies/Binked/jax_Loop.bik | BIKi / Bink 1 |
| Daala_Loop | data/art/movies/binked/daala.bik | mod:loose:data/Art/Movies/Binked/daala.bik | BIKi / Bink 1 |
| Delvardus_Loop | data/art/movies/binked/delvardus.bik | mod:loose:data/Art/Movies/Binked/delvardus.bik | BIKi / Bink 1 |
| Dooku_Loop | data/art/movies/binked/dooku_loop.bik | mod:loose:data/Art/Movies/Binked/Dooku_Loop.bik | BIKi / Bink 1 |
| Droid_Loop | UNDECLARED | MISSING | MISSING |
| Durge_Loop | UNDECLARED | MISSING | MISSING |
| Durges_Lance_Campaign_Intro | data/art/movies/binked/text_scroll_intro_short.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Dyne_Loop | UNDECLARED | MISSING | MISSING |
| Firwirrung_Loop | data/art/movies/binked/firwirrung_loop.bik | mod:loose:data/Art/Movies/Binked/firwirrung_Loop.bik | BIKi / Bink 1 |
| Foerost_Campaign_Rep_Intro | data/art/movies/binked/text_scroll_intro_short.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Gavrisom_Loop | data/art/movies/binked/gavrisom_loop.bik | mod:loose:data/Art/Movies/Binked/gavrisom_Loop.bik | BIKi / Bink 1 |
| Geonosian_Loop | UNDECLARED | MISSING | MISSING |
| Grievous_Loop | data/art/movies/binked/grievous-1abca.bik | mod:loose:data/Art/Movies/Binked/grievous-1abca.bik | BIKi / Bink 1 |
| Grunger_Loop | data/art/movies/binked/grunger.bik | mod:loose:data/Art/Movies/Binked/grunger.bik | BIKi / Bink 1 |
| Gunray_Loop | UNDECLARED | MISSING | MISSING |
| Han_Loop | UNDECLARED | MISSING | MISSING |
| Han_Solo_Loop | data/art/movies/binked/han_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Han_Solo_Officer_Loop | data/art/movies/binked/han_solo_officer.bik | mod:loose:data/Art/Movies/Binked/han_solo_officer.bik | BIKi / Bink 1 |
| Harrsk_Loop | data/art/movies/binked/harrsk_loop.bik | mod:loose:data/Art/Movies/Binked/harrsk_Loop.bik | BIKi / Bink 1 |
| Hissa_loop | data/art/movies/binked/hissa_loop.bik | mod:loose:data/Art/Movies/Binked/hissa_loop.bik | BIKi / Bink 1 |
| Hunt_for_Malevolence_Campaign_Intro | data/art/movies/binked/text_scroll_intro_short.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Imperial_Naval_Officer_Loop | data/art/movies/binked/imperial-naval-officer.bik | mod:loose:data/Art/Movies/Binked/imperial-naval-officer.bik | BIKi / Bink 1 |
| Isard_Loop | data/art/movies/binked/isard_loop.bik | mod:loose:data/Art/Movies/Binked/Isard_Loop.bik | BIKi / Bink 1 |
| ISB_Officer_Loop | data/art/movies/binked/isb_officer.bik | mod:loose:data/Art/Movies/Binked/isb_officer.bik | BIKi / Bink 1 |
| Jerec_Loop | data/art/movies/binked/jerec.bik | mod:loose:data/Art/Movies/Binked/jerec.bik | BIKi / Bink 1 |
| Kaine_Loop | data/art/movies/binked/kaine_loop.bik | mod:loose:data/Art/Movies/Binked/Kaine_Loop.bik | BIKi / Bink 1 |
| Karrde_Loop | data/art/movies/binked/karrde.bik | mod:loose:data/Art/Movies/Binked/karrde.bik | BIKi / Bink 1 |
| Knight_Hammer_Hutts_Intro | data/art/movies/binked/text_scroll_intro_red_short.bik | mod:loose:data/Art/Movies/Binked/Text_Scroll_Intro_Red_short.bik | BIKi / Bink 1 |
| Knight_Hammer_Intro | data/art/movies/binked/text_scroll_intro_short.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Kosh_Teradoc_Loop | data/art/movies/binked/kosh_teradoc.bik | mod:loose:data/Art/Movies/Binked/kosh_teradoc.bik | BIKi / Bink 1 |
| Krennel_Loop | data/art/movies/binked/krennel_loop.bik | mod:loose:data/Art/Movies/Binked/krennel_Loop.bik | BIKi / Bink 1 |
| Lando_Fancy_Loop | data/art/movies/binked/lando-ir.bik | mod:loose:data/Art/Movies/Binked/lando-ir.bik | BIKi / Bink 1 |
| Lando_Officer_Loop | data/art/movies/binked/lando-heir.bik | mod:loose:data/Art/Movies/Binked/lando-heir.bik | BIKi / Bink 1 |
| Lando_Young_Loop | data/art/movies/binked/lando-dis.bik | mod:loose:data/Art/Movies/Binked/lando-dis.bik | BIKi / Bink 1 |
| Leia_Loop | data/art/movies/binked/leia.bik | mod:loose:data/Art/Movies/Binked/leia.bik | BIKi / Bink 1 |
| Lumiya_Loop | data/art/movies/binked/lumiya.bik | mod:loose:data/Art/Movies/Binked/lumiya.bik | BIKi / Bink 1 |
| Mace_Loop | data/art/movies/binked/mace-bacd.bik | mod:loose:data/Art/Movies/Binked/mace-bacd.bik | BIKi / Bink 1 |
| Makati_Loop | data/art/movies/binked/makati.bik | mod:loose:data/Art/Movies/Binked/makati.bik | BIKi / Bink 1 |
| Melvar_Loop | data/art/movies/binked/melvar.bik | mod:loose:data/Art/Movies/Binked/melvar.bik | BIKi / Bink 1 |
| Navik_Loop | data/art/movies/binked/navik.bik | mod:loose:data/Art/Movies/Binked/navik.bik | BIKi / Bink 1 |
| NilSpaar_Loop | data/art/movies/binked/nilspaar_loop.bik | mod:loose:data/Art/Movies/Binked/NilSpaar_Loop.bik | BIKi / Bink 1 |
| ObiWan2_Loop | data/art/movies/binked/obiwan-cbea.bik | mod:loose:data/Art/Movies/Binked/obiwan-cbea.bik | BIKi / Bink 1 |
| ObiWan_Loop | data/art/movies/binked/obiwan-abeb.bik | mod:loose:data/Art/Movies/Binked/obiwan-abeb.bik | BIKi / Bink 1 |
| Odumin_Loop | data/art/movies/binked/odumin_loop.bik | MISSING | MISSING |
| Order66_Video | data/art/movies/binked/order66-mov.bik | mod:loose:data/Art/Movies/Binked/order66-mov.bik | BIKi / Bink 1 |
| Outer_Rim_Sieges_Campaign_Intro | data/art/movies/binked/text_scroll_intro_short.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Palpatine_Reborn_Loop | data/art/movies/binked/palpatine-reborn.bik | mod:loose:data/Art/Movies/Binked/palpatine-reborn.bik | BIKi / Bink 1 |
| PalpatineDED_Loop | data/art/movies/binked/palpatine_ded.bik | mod:loose:data/Art/Movies/Binked/Palpatine_DED.bik | BIKi / Bink 1 |
| PalpatineFotR_Loop | data/art/movies/binked/palpatinefotr_loop.bik | mod:loose:data/Art/Movies/Binked/PalpatineFotR_Loop.bik | BIKi / Bink 1 |
| Parrck_Loop | data/art/movies/binked/parrck.bik | MISSING | MISSING |
| Passel_Agente_Loop | UNDECLARED | MISSING | MISSING |
| Pellaeon_Loop | data/art/movies/binked/pellaeon_loop.bik | mod:loose:data/Art/Movies/Binked/Pellaeon_Loop.bik | BIKi / Bink 1 |
| Pestage_Loop | data/art/movies/binked/pestage_loop.bik | mod:loose:data/Art/Movies/Binked/pestage_Loop.bik | BIKi / Bink 1 |
| Poggle_Loop | UNDECLARED | MISSING | MISSING |
| R2_Loop | UNDECLARED | MISSING | MISSING |
| Raptor_Trooper_Loop | data/art/movies/binked/raptor-trooper.bik | mod:loose:data/Art/Movies/Binked/raptor-trooper.bik | BIKi / Bink 1 |
| Rimward_Campaign_AU_Intro | data/art/movies/binked/text_scroll_intro_short.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Rimward_Campaign_Hutts_Intro | data/art/movies/binked/text_scroll_intro_short.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Rimward_Campaign_Intro | data/art/movies/binked/text_scroll_intro_short.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Shadowspawn_Loop | data/art/movies/binked/shadowspawn.bik | mod:loose:data/Art/Movies/Binked/shadowspawn.bik | BIKi / Bink 1 |
| Sian_Tevv_Loop | data/art/movies/binked/sian_tevv.bik | mod:loose:data/Art/Movies/Binked/sian_tevv.bik | BIKi / Bink 1 |
| SoBilles_Loop | data/art/movies/binked/sobilles.bik | mod:loose:data/Art/Movies/Binked/sobilles.bik | BIKi / Bink 1 |
| Sobilles_Loop | data/art/movies/binked/sobilles.bik | mod:loose:data/Art/Movies/Binked/sobilles.bik | BIKi / Bink 1 |
| Ssi_Ruuk_Loop | data/art/movies/binked/ssi-ruuk.bik | mod:loose:data/Art/Movies/Binked/ssi-ruuk.bik | BIKi / Bink 1 |
| Stormtrooper_Loop | UNDECLARED | MISSING | MISSING |
| Stormtrooper_Officer_Loop | data/art/movies/binked/stormtrooper-officer.bik | mod:loose:data/Art/Movies/Binked/stormtrooper-officer.bik | BIKi / Bink 1 |
| Syn_Loop | data/art/movies/binked/syn.bik | mod:loose:data/Art/Movies/Binked/syn.bik | BIKi / Bink 1 |
| TaaChume_Loop | data/art/movies/binked/taachume_loop.bik | mod:loose:data/Art/Movies/Binked/taachume_Loop.bik | BIKi / Bink 1 |
| TaaChume_Loop2 | data/art/movies/binked/chume.bik | mod:loose:data/Art/Movies/Binked/chume.bik | BIKi / Bink 1 |
| Tambor_Loop | UNDECLARED | MISSING | MISSING |
| Teneniel_Loop | data/art/movies/binked/teneniel_loop.bik | mod:loose:data/Art/Movies/Binked/teneniel_loop.bik | BIKi / Bink 1 |
| Teneniel_Loop2 | data/art/movies/binked/teneniel_loop2.bik | mod:loose:data/Art/Movies/Binked/teneniel_loop2.bik | BIKi / Bink 1 |
| Tennuutta_Skirmishes_Campaign_Intro | data/art/movies/binked/text_scroll_intro_short.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Tennuutta_Skirmishes_CIS_AU_Campaign_Intro | data/art/movies/binked/text_scroll_intro_short.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Tennuutta_Skirmishes_Rep_AU_Campaign_Intro | data/art/movies/binked/text_scroll_intro_short.bik | base:Data/Movies.meg | BIKi / Bink 1 |
| Tetran_Loop | data/art/movies/binked/tetran.bik | mod:loose:data/Art/Movies/Binked/tetran.bik | BIKi / Bink 1 |
| Tholme_Loop | UNDECLARED | MISSING | MISSING |
| Thrawn_Loop | data/art/movies/binked/thrawn.bik | mod:loose:data/Art/Movies/Binked/thrawn.bik | BIKi / Bink 1 |
| Tonith_Loop | data/art/movies/binked/porstonith_loop.bik | mod:loose:data/Art/Movies/Binked/PorsTonith_Loop.bik | BIKi / Bink 1 |
| Treuten_Teradoc_Loop | data/art/movies/binked/treuten_teradoc.bik | mod:loose:data/Art/Movies/Binked/treuten_teradoc.bik | BIKi / Bink 1 |
| Trioculus_Loop | data/art/movies/binked/trioculus.bik | mod:loose:data/Art/Movies/Binked/trioculus.bik | BIKi / Bink 1 |
| Tyber_Loop | UNDECLARED | MISSING | MISSING |
| Tyber_loop | UNDECLARED | MISSING | MISSING |
| Urai_Loop | UNDECLARED | MISSING | MISSING |
| Ventress_Loop | UNDECLARED | MISSING | MISSING |
| Viqi_Shesh_Loop | data/art/movies/binked/viqi.bik | mod:loose:data/Art/Movies/Binked/viqi.bik | BIKi / Bink 1 |
| Wedge_Loop | data/art/movies/binked/wedge.bik | mod:loose:data/Art/Movies/Binked/wedge.bik | BIKi / Bink 1 |
| Yoda_Loop | UNDECLARED | MISSING | MISSING |
| Zorba_Loop | data/art/movies/binked/zorba.bik | mod:loose:data/Art/Movies/Binked/zorba.bik | BIKi / Bink 1 |
| Zsinj_Loop | data/art/movies/binked/zsinj_loop.bik | mod:loose:data/Art/Movies/Binked/Zsinj_Loop.bik | BIKi / Bink 1 |

These movie resolution rows are identical to retail: `Antilles_Loop`, `Antillies_Loop`, `Boba_Fett_Loop`, `Bossk_Loop`, `C3PO_Loop`, `Chewie_Loop`, `Collector_Droid_Loop`, `Commander_Moff2_Loop`, `Commander_Moff_Loop`, `Emperor_Loop`, `Garm_Loop`, `Generic_Sith_Loop`, `IG-88_Loop`, `Jabba_Loop`, `Mon_Mothma_Loop`, `Piett_Loop`, `Pirate2_Loop`, `Pirate_Loop`, `R2D2_Loop`, `Rebel_Pilot_Loop`, `Rebel_Trooper2_Loop`, `Rebel_Trooper3_Loop`, `Rebel_Trooper_Loop`, `Saboteur_Loop`, `Silri_Loop`, `Tarkin_Loop`, `Underworld_soldier_Loop`, `Vader_Loop`, `Veers_Loop`, `Xizor_Loop`.


Stop-only or stop-bearing XMLs (no additional movie-file reference): `conquests/groundtactical/tactical_groundbattles.xml`, `conquests/player_agnostic_plot.xml`, `conquests/spacetactical/tactical_spacebattles.xml`, `story_campaign_empire_act_i.xml`, `story_campaign_empire_act_ii.xml`, `story_campaign_empire_act_iii.xml`, `story_campaign_empire_act_iv.xml`, `story_campaign_rebel_act_i.xml`, `story_campaign_rebel_act_ii.xml`, `story_campaign_rebel_act_iii.xml`, `story_campaign_rebel_act_iv.xml`, `story_campaign_underworld.xml`, `story_campaign_underworld_demo.xml`, `story_campaign_underworld_focus_demo.xml`, `story_campaign_underworld_tutorial.xml`, `story_empire_acti_m02_land.xml`, `story_empire_acti_m03_space.xml`, `story_empire_acti_m04_space.xml`, `story_empire_actii_m04_space.xml`, `story_empire_actii_m05_land.xml`, `story_empire_actii_m06_land.xml`, `story_empire_actiii_m08_space.xml`, `story_empire_activ_m10_space.xml`, `story_focus_demo_felucia_land_tactical.xml`, `story_focus_demo_felucia_space_tactical.xml`, `story_rebel_acti_m01_space.xml`, `story_rebel_acti_m02_land.xml`, `story_rebel_acti_m03_land.xml`, `story_rebel_actii_m04_space.xml`, `story_rebel_actii_m06_land.xml`, `story_rebel_actiii_m07_space.xml`, `story_rebel_actiii_m07a_space.xml`, `story_rebel_actiii_m08_space.xml`, `story_rebel_actiii_m09_space.xml`, `story_rebel_actiii_m10_land.xml`, `story_rebel_activ_m11_galactic.xml`, `story_rebel_activ_m12_galactic.xml`, `story_sandbox_focusdemo_empire.xml`, `story_tutorial_ii.xml`, `story_tutorial_iii.xml`, `story_tutorial_iv.xml`, `story_tutorial_m01_land.xml`, `story_tutorial_m03_space.xml`, `story_tutorial_m05_land.xml`, `story_tutorial_v.xml`, `story_underworld_aetenii_piracy.xml`, `story_underworld_alzociii_intimidation.xml`, `story_underworld_anaxes_intimidation.xml`, `story_underworld_bespin_piracy.xml`, `story_underworld_bespin_piracy_story.xml`, `story_underworld_bestine_kidnapping.xml`, `story_underworld_bothawui_intimidation.xml`, `story_underworld_byss_kidnapping.xml`, `story_underworld_carida_kidnapping.xml`, `story_underworld_corellia_piracy.xml`, `story_underworld_corulag_piracy.xml`, `story_underworld_dantooine_intimidation.xml`, `story_underworld_fondor_piracy.xml`, `story_underworld_honoghr_intimidation.xml`, `story_underworld_honoghr_intimidation_story.xml`, `story_underworld_kessel_piracy.xml`, `story_underworld_kuat_kidnapping.xml`, `story_underworld_m01_land.xml`, `story_underworld_m02_space.xml`, `story_underworld_m06_land.xml`, `story_underworld_m07_land.xml`, `story_underworld_m08_space.xml`, `story_underworld_m09_space.xml`, `story_underworld_m12_space.xml`, `story_underworld_mandalore_piracy.xml`, `story_underworld_mandalore_piracy_prologue.xml`, `story_underworld_mandalore_piracy_story.xml`, `story_underworld_moncalamari_piracy.xml`, `story_underworld_muunilinst_kidnapping.xml`, `story_underworld_naboo_intimidation.xml`, `story_underworld_nalhutta_kidnapping.xml`, `story_underworld_saleucami_intimidation.xml`, `story_underworld_taris_intimidation.xml`, `story_underworld_thyferra_kidnapping.xml`, `story_underworld_utapau_kidnapping.xml`.


### 2794270450

Movie registry: `data/xml/movies.xml`, supplied by `mod:loose:data/Xml/MOVIES.XML`.

| Referencing XML (under data/xml/) | XML source | Movie names |
|---|---|---|

These XML reference rows are identical to retail: `expansion_factions.xml`, `story_campaign_empire_act_i.xml`, `story_campaign_empire_act_ii.xml`, `story_campaign_empire_act_iii.xml`, `story_campaign_empire_act_iv.xml`, `story_campaign_rebel_act_i.xml`, `story_campaign_rebel_act_ii.xml`, `story_campaign_rebel_act_iii.xml`, `story_campaign_rebel_act_iv.xml`, `story_campaign_underworld.xml`, `story_campaign_underworld_demo.xml`, `story_campaign_underworld_focus_demo.xml`, `story_campaign_underworld_tutorial.xml`, `story_empire_acti_m02_land.xml`, `story_empire_acti_m03_space.xml`, `story_empire_acti_m04_space.xml`, `story_empire_actii_m04_space.xml`, `story_empire_actii_m05_land.xml`, `story_empire_actii_m06_land.xml`, `story_empire_actiii_m08_space.xml`, `story_empire_activ_m10_space.xml`, `story_empire_activ_m11_space.xml`, `story_focus_demo_felucia_land_tactical.xml`, `story_focus_demo_felucia_space_tactical.xml`, `story_rebel_acti_m01_space.xml`, `story_rebel_acti_m02_land.xml`, `story_rebel_acti_m03_land.xml`, `story_rebel_actii_m04_space.xml`, `story_rebel_actii_m06_land.xml`, `story_rebel_actiii_m07_space.xml`, `story_rebel_actiii_m07a_space.xml`, `story_rebel_actiii_m08_space.xml`, `story_rebel_actiii_m09_space.xml`, `story_rebel_actiii_m10_land.xml`, `story_rebel_activ_m11_galactic.xml`, `story_rebel_activ_m12_galactic.xml`, `story_sandbox_focusdemo_empire.xml`, `story_tutorial_ii.xml`, `story_tutorial_iii.xml`, `story_tutorial_iv.xml`, `story_tutorial_m01_land.xml`, `story_tutorial_m03_space.xml`, `story_tutorial_m05_land.xml`, `story_tutorial_v.xml`, `story_underworld_aetenii_piracy.xml`, `story_underworld_alzociii_intimidation.xml`, `story_underworld_anaxes_intimidation.xml`, `story_underworld_bespin_piracy.xml`, `story_underworld_bespin_piracy_story.xml`, `story_underworld_bestine_kidnapping.xml`, `story_underworld_bothawui_intimidation.xml`, `story_underworld_byss_kidnapping.xml`, `story_underworld_carida_kidnapping.xml`, `story_underworld_corellia_piracy.xml`, `story_underworld_corulag_piracy.xml`, `story_underworld_dantooine_intimidation.xml`, `story_underworld_fondor_piracy.xml`, `story_underworld_honoghr_intimidation.xml`, `story_underworld_honoghr_intimidation_story.xml`, `story_underworld_kessel_piracy.xml`, `story_underworld_kuat_kidnapping.xml`, `story_underworld_m01_land.xml`, `story_underworld_m02_space.xml`, `story_underworld_m06_land.xml`, `story_underworld_m07_land.xml`, `story_underworld_m08_space.xml`, `story_underworld_m09_space.xml`, `story_underworld_m12_space.xml`, `story_underworld_mandalore_piracy.xml`, `story_underworld_mandalore_piracy_prologue.xml`, `story_underworld_mandalore_piracy_story.xml`, `story_underworld_moncalamari_piracy.xml`, `story_underworld_muunilinst_kidnapping.xml`, `story_underworld_naboo_intimidation.xml`, `story_underworld_nalhutta_kidnapping.xml`, `story_underworld_saleucami_intimidation.xml`, `story_underworld_taris_intimidation.xml`, `story_underworld_thyferra_kidnapping.xml`, `story_underworld_utapau_kidnapping.xml`.


| Movies.xml name | Resolved VFS file | Supplying source | Signature / codec |
|---|---|---|---|
| Tarkin_Loop | data/art/movies/binked/hologram_tarkin_portrait.bik | mod:loose:data/Art/Movies/Binked/HOLOGRAM_Tarkin_Portrait.bik | BIKi / Bink 1 |
| Underworld_soldier_Loop | data/art/movies/binked/boba_fett_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |

These movie resolution rows are identical to retail: `Akbar_Loop`, `AlphaTest`, `Antilles_Loop`, `Antillies_Loop`, `Biggs_Loop`, `Boba_Fett_Loop`, `Bossk_Loop`, `C3PO_Loop`, `Chewie_Loop`, `Collector_Droid_Loop`, `Commander_Moff2_Loop`, `Commander_Moff_Loop`, `Default_Incoming_Transmission`, `Emperor_Loop`, `Garm_Loop`, `Generic_Sith_Loop`, `Han_Loop`, `IG-88_Loop`, `Jabba_Loop`, `Leia_Loop`, `Light_Loop`, `Luke_Loop`, `Mon_Mothma_Loop`, `Obi_Wan_Loop`, `Piett_Loop`, `Pirate2_Loop`, `Pirate_Loop`, `Porkins_Loop`, `R2D2_Loop`, `Rebel_Pilot_Loop`, `Rebel_Trooper2_Loop`, `Rebel_Trooper3_Loop`, `Rebel_Trooper_Loop`, `Saboteur_Loop`, `Silri_Loop`, `Star_Wars_Intro`, `Star_Wars_Intro_Rebel`, `Star_Wars_Intro_Short`, `Star_Wars_Intro_Underworld`, `Stormtrooper_Loop`, `Thrawn_Loop`, `Tyber_loop`, `Tyber_Loop`, `Underworld_Prolog_Text`, `Urai_Loop`, `Vader_Loop`, `Veers_Loop`, `Wedge_Loop`, `Xizor_Loop`.


Stop-only or stop-bearing XMLs (no additional movie-file reference): `story_campaign_empire_act_i.xml`, `story_campaign_empire_act_ii.xml`, `story_campaign_empire_act_iii.xml`, `story_campaign_empire_act_iv.xml`, `story_campaign_rebel_act_i.xml`, `story_campaign_rebel_act_ii.xml`, `story_campaign_rebel_act_iii.xml`, `story_campaign_rebel_act_iv.xml`, `story_campaign_underworld.xml`, `story_campaign_underworld_demo.xml`, `story_campaign_underworld_focus_demo.xml`, `story_campaign_underworld_tutorial.xml`, `story_empire_acti_m02_land.xml`, `story_empire_acti_m03_space.xml`, `story_empire_acti_m04_space.xml`, `story_empire_actii_m04_space.xml`, `story_empire_actii_m05_land.xml`, `story_empire_actii_m06_land.xml`, `story_empire_actiii_m08_space.xml`, `story_empire_activ_m10_space.xml`, `story_focus_demo_felucia_land_tactical.xml`, `story_focus_demo_felucia_space_tactical.xml`, `story_rebel_acti_m01_space.xml`, `story_rebel_acti_m02_land.xml`, `story_rebel_acti_m03_land.xml`, `story_rebel_actii_m04_space.xml`, `story_rebel_actii_m06_land.xml`, `story_rebel_actiii_m07_space.xml`, `story_rebel_actiii_m07a_space.xml`, `story_rebel_actiii_m08_space.xml`, `story_rebel_actiii_m09_space.xml`, `story_rebel_actiii_m10_land.xml`, `story_rebel_activ_m11_galactic.xml`, `story_rebel_activ_m12_galactic.xml`, `story_sandbox_focusdemo_empire.xml`, `story_tutorial_ii.xml`, `story_tutorial_iii.xml`, `story_tutorial_iv.xml`, `story_tutorial_m01_land.xml`, `story_tutorial_m03_space.xml`, `story_tutorial_m05_land.xml`, `story_tutorial_v.xml`, `story_underworld_aetenii_piracy.xml`, `story_underworld_alzociii_intimidation.xml`, `story_underworld_anaxes_intimidation.xml`, `story_underworld_bespin_piracy.xml`, `story_underworld_bespin_piracy_story.xml`, `story_underworld_bestine_kidnapping.xml`, `story_underworld_bothawui_intimidation.xml`, `story_underworld_byss_kidnapping.xml`, `story_underworld_carida_kidnapping.xml`, `story_underworld_corellia_piracy.xml`, `story_underworld_corulag_piracy.xml`, `story_underworld_dantooine_intimidation.xml`, `story_underworld_fondor_piracy.xml`, `story_underworld_honoghr_intimidation.xml`, `story_underworld_honoghr_intimidation_story.xml`, `story_underworld_kessel_piracy.xml`, `story_underworld_kuat_kidnapping.xml`, `story_underworld_m01_land.xml`, `story_underworld_m02_space.xml`, `story_underworld_m06_land.xml`, `story_underworld_m07_land.xml`, `story_underworld_m08_space.xml`, `story_underworld_m09_space.xml`, `story_underworld_m12_space.xml`, `story_underworld_mandalore_piracy.xml`, `story_underworld_mandalore_piracy_prologue.xml`, `story_underworld_mandalore_piracy_story.xml`, `story_underworld_moncalamari_piracy.xml`, `story_underworld_muunilinst_kidnapping.xml`, `story_underworld_naboo_intimidation.xml`, `story_underworld_nalhutta_kidnapping.xml`, `story_underworld_saleucami_intimidation.xml`, `story_underworld_taris_intimidation.xml`, `story_underworld_thyferra_kidnapping.xml`, `story_underworld_utapau_kidnapping.xml`.


### 3229239424

Movie registry: `data/xml/movies.xml`, supplied by `mod-parent-1:loose:data/Xml/MOVIES.XML`.

| Referencing XML (under data/xml/) | XML source | Movie names |
|---|---|---|

These XML reference rows are identical to retail: `expansion_factions.xml`, `story_campaign_empire_act_i.xml`, `story_campaign_empire_act_ii.xml`, `story_campaign_empire_act_iii.xml`, `story_campaign_empire_act_iv.xml`, `story_campaign_rebel_act_i.xml`, `story_campaign_rebel_act_ii.xml`, `story_campaign_rebel_act_iii.xml`, `story_campaign_rebel_act_iv.xml`, `story_campaign_underworld.xml`, `story_campaign_underworld_demo.xml`, `story_campaign_underworld_focus_demo.xml`, `story_campaign_underworld_tutorial.xml`, `story_empire_acti_m02_land.xml`, `story_empire_acti_m03_space.xml`, `story_empire_acti_m04_space.xml`, `story_empire_actii_m04_space.xml`, `story_empire_actii_m05_land.xml`, `story_empire_actii_m06_land.xml`, `story_empire_actiii_m08_space.xml`, `story_empire_activ_m10_space.xml`, `story_empire_activ_m11_space.xml`, `story_focus_demo_felucia_land_tactical.xml`, `story_focus_demo_felucia_space_tactical.xml`, `story_rebel_acti_m01_space.xml`, `story_rebel_acti_m02_land.xml`, `story_rebel_acti_m03_land.xml`, `story_rebel_actii_m04_space.xml`, `story_rebel_actii_m06_land.xml`, `story_rebel_actiii_m07_space.xml`, `story_rebel_actiii_m07a_space.xml`, `story_rebel_actiii_m08_space.xml`, `story_rebel_actiii_m09_space.xml`, `story_rebel_actiii_m10_land.xml`, `story_rebel_activ_m11_galactic.xml`, `story_rebel_activ_m12_galactic.xml`, `story_sandbox_focusdemo_empire.xml`, `story_tutorial_ii.xml`, `story_tutorial_iii.xml`, `story_tutorial_iv.xml`, `story_tutorial_m01_land.xml`, `story_tutorial_m03_space.xml`, `story_tutorial_m05_land.xml`, `story_tutorial_v.xml`, `story_underworld_aetenii_piracy.xml`, `story_underworld_alzociii_intimidation.xml`, `story_underworld_anaxes_intimidation.xml`, `story_underworld_bespin_piracy.xml`, `story_underworld_bespin_piracy_story.xml`, `story_underworld_bestine_kidnapping.xml`, `story_underworld_bothawui_intimidation.xml`, `story_underworld_byss_kidnapping.xml`, `story_underworld_carida_kidnapping.xml`, `story_underworld_corellia_piracy.xml`, `story_underworld_corulag_piracy.xml`, `story_underworld_dantooine_intimidation.xml`, `story_underworld_fondor_piracy.xml`, `story_underworld_honoghr_intimidation.xml`, `story_underworld_honoghr_intimidation_story.xml`, `story_underworld_kessel_piracy.xml`, `story_underworld_kuat_kidnapping.xml`, `story_underworld_m01_land.xml`, `story_underworld_m02_space.xml`, `story_underworld_m06_land.xml`, `story_underworld_m07_land.xml`, `story_underworld_m08_space.xml`, `story_underworld_m09_space.xml`, `story_underworld_m12_space.xml`, `story_underworld_mandalore_piracy.xml`, `story_underworld_mandalore_piracy_prologue.xml`, `story_underworld_mandalore_piracy_story.xml`, `story_underworld_moncalamari_piracy.xml`, `story_underworld_muunilinst_kidnapping.xml`, `story_underworld_naboo_intimidation.xml`, `story_underworld_nalhutta_kidnapping.xml`, `story_underworld_saleucami_intimidation.xml`, `story_underworld_taris_intimidation.xml`, `story_underworld_thyferra_kidnapping.xml`, `story_underworld_utapau_kidnapping.xml`.


| Movies.xml name | Resolved VFS file | Supplying source | Signature / codec |
|---|---|---|---|
| Underworld_soldier_Loop | data/art/movies/binked/boba_fett_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |

These movie resolution rows are identical to retail: `Akbar_Loop`, `AlphaTest`, `Antilles_Loop`, `Antillies_Loop`, `Biggs_Loop`, `Boba_Fett_Loop`, `Bossk_Loop`, `C3PO_Loop`, `Chewie_Loop`, `Collector_Droid_Loop`, `Commander_Moff2_Loop`, `Commander_Moff_Loop`, `Default_Incoming_Transmission`, `Emperor_Loop`, `Garm_Loop`, `Generic_Sith_Loop`, `Han_Loop`, `IG-88_Loop`, `Jabba_Loop`, `Leia_Loop`, `Light_Loop`, `Luke_Loop`, `Mon_Mothma_Loop`, `Obi_Wan_Loop`, `Piett_Loop`, `Pirate2_Loop`, `Pirate_Loop`, `Porkins_Loop`, `R2D2_Loop`, `Rebel_Pilot_Loop`, `Rebel_Trooper2_Loop`, `Rebel_Trooper3_Loop`, `Rebel_Trooper_Loop`, `Saboteur_Loop`, `Silri_Loop`, `Star_Wars_Intro`, `Star_Wars_Intro_Rebel`, `Star_Wars_Intro_Short`, `Star_Wars_Intro_Underworld`, `Stormtrooper_Loop`, `Tarkin_Loop`, `Thrawn_Loop`, `Tyber_loop`, `Tyber_Loop`, `Underworld_Prolog_Text`, `Urai_Loop`, `Vader_Loop`, `Veers_Loop`, `Wedge_Loop`, `Xizor_Loop`.


Stop-only or stop-bearing XMLs (no additional movie-file reference): `story_campaign_empire_act_i.xml`, `story_campaign_empire_act_ii.xml`, `story_campaign_empire_act_iii.xml`, `story_campaign_empire_act_iv.xml`, `story_campaign_rebel_act_i.xml`, `story_campaign_rebel_act_ii.xml`, `story_campaign_rebel_act_iii.xml`, `story_campaign_rebel_act_iv.xml`, `story_campaign_underworld.xml`, `story_campaign_underworld_demo.xml`, `story_campaign_underworld_focus_demo.xml`, `story_campaign_underworld_tutorial.xml`, `story_empire_acti_m02_land.xml`, `story_empire_acti_m03_space.xml`, `story_empire_acti_m04_space.xml`, `story_empire_actii_m04_space.xml`, `story_empire_actii_m05_land.xml`, `story_empire_actii_m06_land.xml`, `story_empire_actiii_m08_space.xml`, `story_empire_activ_m10_space.xml`, `story_focus_demo_felucia_land_tactical.xml`, `story_focus_demo_felucia_space_tactical.xml`, `story_rebel_acti_m01_space.xml`, `story_rebel_acti_m02_land.xml`, `story_rebel_acti_m03_land.xml`, `story_rebel_actii_m04_space.xml`, `story_rebel_actii_m06_land.xml`, `story_rebel_actiii_m07_space.xml`, `story_rebel_actiii_m07a_space.xml`, `story_rebel_actiii_m08_space.xml`, `story_rebel_actiii_m09_space.xml`, `story_rebel_actiii_m10_land.xml`, `story_rebel_activ_m11_galactic.xml`, `story_rebel_activ_m12_galactic.xml`, `story_sandbox_focusdemo_empire.xml`, `story_tutorial_ii.xml`, `story_tutorial_iii.xml`, `story_tutorial_iv.xml`, `story_tutorial_m01_land.xml`, `story_tutorial_m03_space.xml`, `story_tutorial_m05_land.xml`, `story_tutorial_v.xml`, `story_underworld_aetenii_piracy.xml`, `story_underworld_alzociii_intimidation.xml`, `story_underworld_anaxes_intimidation.xml`, `story_underworld_bespin_piracy.xml`, `story_underworld_bespin_piracy_story.xml`, `story_underworld_bestine_kidnapping.xml`, `story_underworld_bothawui_intimidation.xml`, `story_underworld_byss_kidnapping.xml`, `story_underworld_carida_kidnapping.xml`, `story_underworld_corellia_piracy.xml`, `story_underworld_corulag_piracy.xml`, `story_underworld_dantooine_intimidation.xml`, `story_underworld_fondor_piracy.xml`, `story_underworld_honoghr_intimidation.xml`, `story_underworld_honoghr_intimidation_story.xml`, `story_underworld_kessel_piracy.xml`, `story_underworld_kuat_kidnapping.xml`, `story_underworld_m01_land.xml`, `story_underworld_m02_space.xml`, `story_underworld_m06_land.xml`, `story_underworld_m07_land.xml`, `story_underworld_m08_space.xml`, `story_underworld_m09_space.xml`, `story_underworld_m12_space.xml`, `story_underworld_mandalore_piracy.xml`, `story_underworld_mandalore_piracy_prologue.xml`, `story_underworld_mandalore_piracy_story.xml`, `story_underworld_moncalamari_piracy.xml`, `story_underworld_muunilinst_kidnapping.xml`, `story_underworld_naboo_intimidation.xml`, `story_underworld_nalhutta_kidnapping.xml`, `story_underworld_saleucami_intimidation.xml`, `story_underworld_taris_intimidation.xml`, `story_underworld_thyferra_kidnapping.xml`, `story_underworld_utapau_kidnapping.xml`.


### 3689306867

Movie registry: `data/xml/movies.xml`, supplied by `mod:loose:data/Xml/MOVIES.XML`.

| Referencing XML (under data/xml/) | XML source | Movie names |
|---|---|---|
| gamemodes/tutorial/storyevent__retutorial.xml | mod:loose:data/Xml/GameModes/Tutorial/StoryEvent__ReTutorial.xml | Mon_Mothma_Loop, Rebel_Trooper2_Loop |
| gamemodes/tutorial/storyevent_i_battle.xml | mod:loose:data/Xml/GameModes/Tutorial/StoryEvent_I_Battle.xml | Rebel_Trooper2_Loop, Rebel_Trooper3_Loop, Rebel_Trooper_Loop, Stormtrooper_Loop |
| gamemodes/tutorial/storyevent_ii.xml | mod:loose:data/Xml/GameModes/Tutorial/StoryEvent_II.xml | Mon_Mothma_Loop, Rebel_Trooper2_Loop |
| gamemodes/tutorial/storyevent_iii.xml | mod:loose:data/Xml/GameModes/Tutorial/StoryEvent_III.xml | Piett_Loop |
| gamemodes/tutorial/storyevent_iii_battle.xml | mod:loose:data/Xml/GameModes/Tutorial/StoryEvent_III_Battle.XML | Commander_Moff_Loop, Veers_Loop |
| gamemodes/tutorial/storyevent_iv.xml | mod:loose:data/Xml/GameModes/Tutorial/StoryEvent_IV.xml | Piett_Loop |
| gamemodes/tutorial/storyevent_v.xml | mod:loose:data/Xml/GameModes/Tutorial/StoryEvent_V.xml | Piett_Loop |
| gamemodes/tutorial/storyevent_v_battle.xml | mod:loose:data/Xml/GameModes/Tutorial/StoryEvent_V_Battle.xml | Veers_Loop |

These XML reference rows are identical to retail: `expansion_factions.xml`, `story_campaign_empire_act_i.xml`, `story_campaign_empire_act_ii.xml`, `story_campaign_empire_act_iii.xml`, `story_campaign_empire_act_iv.xml`, `story_campaign_rebel_act_i.xml`, `story_campaign_rebel_act_ii.xml`, `story_campaign_rebel_act_iii.xml`, `story_campaign_rebel_act_iv.xml`, `story_campaign_underworld.xml`, `story_campaign_underworld_demo.xml`, `story_campaign_underworld_focus_demo.xml`, `story_campaign_underworld_tutorial.xml`, `story_empire_acti_m02_land.xml`, `story_empire_acti_m03_space.xml`, `story_empire_acti_m04_space.xml`, `story_empire_actii_m04_space.xml`, `story_empire_actii_m05_land.xml`, `story_empire_actii_m06_land.xml`, `story_empire_actiii_m08_space.xml`, `story_empire_activ_m10_space.xml`, `story_empire_activ_m11_space.xml`, `story_focus_demo_felucia_land_tactical.xml`, `story_focus_demo_felucia_space_tactical.xml`, `story_rebel_acti_m01_space.xml`, `story_rebel_acti_m02_land.xml`, `story_rebel_acti_m03_land.xml`, `story_rebel_actii_m04_space.xml`, `story_rebel_actii_m06_land.xml`, `story_rebel_actiii_m07_space.xml`, `story_rebel_actiii_m07a_space.xml`, `story_rebel_actiii_m08_space.xml`, `story_rebel_actiii_m09_space.xml`, `story_rebel_actiii_m10_land.xml`, `story_rebel_activ_m11_galactic.xml`, `story_rebel_activ_m12_galactic.xml`, `story_sandbox_focusdemo_empire.xml`, `story_tutorial_ii.xml`, `story_tutorial_iii.xml`, `story_tutorial_iv.xml`, `story_tutorial_m01_land.xml`, `story_tutorial_m03_space.xml`, `story_tutorial_m05_land.xml`, `story_tutorial_v.xml`, `story_underworld_aetenii_piracy.xml`, `story_underworld_alzociii_intimidation.xml`, `story_underworld_anaxes_intimidation.xml`, `story_underworld_bespin_piracy.xml`, `story_underworld_bespin_piracy_story.xml`, `story_underworld_bestine_kidnapping.xml`, `story_underworld_bothawui_intimidation.xml`, `story_underworld_byss_kidnapping.xml`, `story_underworld_carida_kidnapping.xml`, `story_underworld_corellia_piracy.xml`, `story_underworld_corulag_piracy.xml`, `story_underworld_dantooine_intimidation.xml`, `story_underworld_fondor_piracy.xml`, `story_underworld_honoghr_intimidation.xml`, `story_underworld_honoghr_intimidation_story.xml`, `story_underworld_kessel_piracy.xml`, `story_underworld_kuat_kidnapping.xml`, `story_underworld_m01_land.xml`, `story_underworld_m02_space.xml`, `story_underworld_m06_land.xml`, `story_underworld_m07_land.xml`, `story_underworld_m08_space.xml`, `story_underworld_m09_space.xml`, `story_underworld_m12_space.xml`, `story_underworld_mandalore_piracy.xml`, `story_underworld_mandalore_piracy_prologue.xml`, `story_underworld_mandalore_piracy_story.xml`, `story_underworld_moncalamari_piracy.xml`, `story_underworld_muunilinst_kidnapping.xml`, `story_underworld_naboo_intimidation.xml`, `story_underworld_nalhutta_kidnapping.xml`, `story_underworld_saleucami_intimidation.xml`, `story_underworld_taris_intimidation.xml`, `story_underworld_thyferra_kidnapping.xml`, `story_underworld_utapau_kidnapping.xml`.


| Movies.xml name | Resolved VFS file | Supplying source | Signature / codec |
|---|---|---|---|
| Underworld_soldier_Loop | data/art/movies/binked/boba_fett_loop2.bik | base:Data/Movies.meg | BIKi / Bink 1 |

These movie resolution rows are identical to retail: `Akbar_Loop`, `AlphaTest`, `Antilles_Loop`, `Antillies_Loop`, `Biggs_Loop`, `Boba_Fett_Loop`, `Bossk_Loop`, `C3PO_Loop`, `Chewie_Loop`, `Collector_Droid_Loop`, `Commander_Moff2_Loop`, `Commander_Moff_Loop`, `Default_Incoming_Transmission`, `Emperor_Loop`, `Garm_Loop`, `Generic_Sith_Loop`, `Han_Loop`, `IG-88_Loop`, `Jabba_Loop`, `Leia_Loop`, `Light_Loop`, `Luke_Loop`, `Mon_Mothma_Loop`, `Obi_Wan_Loop`, `Piett_Loop`, `Pirate2_Loop`, `Pirate_Loop`, `Porkins_Loop`, `R2D2_Loop`, `Rebel_Pilot_Loop`, `Rebel_Trooper2_Loop`, `Rebel_Trooper3_Loop`, `Rebel_Trooper_Loop`, `Saboteur_Loop`, `Silri_Loop`, `Star_Wars_Intro`, `Star_Wars_Intro_Rebel`, `Star_Wars_Intro_Short`, `Star_Wars_Intro_Underworld`, `Stormtrooper_Loop`, `Tarkin_Loop`, `Thrawn_Loop`, `Tyber_loop`, `Tyber_Loop`, `Underworld_Prolog_Text`, `Urai_Loop`, `Vader_Loop`, `Veers_Loop`, `Wedge_Loop`, `Xizor_Loop`.


Stop-only or stop-bearing XMLs (no additional movie-file reference): `gamemodes/tutorial/storyevent__retutorial.xml`, `gamemodes/tutorial/storyevent_i_battle.xml`, `gamemodes/tutorial/storyevent_ii.xml`, `gamemodes/tutorial/storyevent_iii.xml`, `gamemodes/tutorial/storyevent_iii_battle.xml`, `gamemodes/tutorial/storyevent_iv.xml`, `gamemodes/tutorial/storyevent_v.xml`, `gamemodes/tutorial/storyevent_v_battle.xml`, `story_campaign_empire_act_i.xml`, `story_campaign_empire_act_ii.xml`, `story_campaign_empire_act_iii.xml`, `story_campaign_empire_act_iv.xml`, `story_campaign_rebel_act_i.xml`, `story_campaign_rebel_act_ii.xml`, `story_campaign_rebel_act_iii.xml`, `story_campaign_rebel_act_iv.xml`, `story_campaign_underworld.xml`, `story_campaign_underworld_demo.xml`, `story_campaign_underworld_focus_demo.xml`, `story_campaign_underworld_tutorial.xml`, `story_empire_acti_m02_land.xml`, `story_empire_acti_m03_space.xml`, `story_empire_acti_m04_space.xml`, `story_empire_actii_m04_space.xml`, `story_empire_actii_m05_land.xml`, `story_empire_actii_m06_land.xml`, `story_empire_actiii_m08_space.xml`, `story_empire_activ_m10_space.xml`, `story_focus_demo_felucia_land_tactical.xml`, `story_focus_demo_felucia_space_tactical.xml`, `story_rebel_acti_m01_space.xml`, `story_rebel_acti_m02_land.xml`, `story_rebel_acti_m03_land.xml`, `story_rebel_actii_m04_space.xml`, `story_rebel_actii_m06_land.xml`, `story_rebel_actiii_m07_space.xml`, `story_rebel_actiii_m07a_space.xml`, `story_rebel_actiii_m08_space.xml`, `story_rebel_actiii_m09_space.xml`, `story_rebel_actiii_m10_land.xml`, `story_rebel_activ_m11_galactic.xml`, `story_rebel_activ_m12_galactic.xml`, `story_sandbox_focusdemo_empire.xml`, `story_tutorial_ii.xml`, `story_tutorial_iii.xml`, `story_tutorial_iv.xml`, `story_tutorial_m01_land.xml`, `story_tutorial_m03_space.xml`, `story_tutorial_m05_land.xml`, `story_tutorial_v.xml`, `story_underworld_aetenii_piracy.xml`, `story_underworld_alzociii_intimidation.xml`, `story_underworld_anaxes_intimidation.xml`, `story_underworld_bespin_piracy.xml`, `story_underworld_bespin_piracy_story.xml`, `story_underworld_bestine_kidnapping.xml`, `story_underworld_bothawui_intimidation.xml`, `story_underworld_byss_kidnapping.xml`, `story_underworld_carida_kidnapping.xml`, `story_underworld_corellia_piracy.xml`, `story_underworld_corulag_piracy.xml`, `story_underworld_dantooine_intimidation.xml`, `story_underworld_fondor_piracy.xml`, `story_underworld_honoghr_intimidation.xml`, `story_underworld_honoghr_intimidation_story.xml`, `story_underworld_kessel_piracy.xml`, `story_underworld_kuat_kidnapping.xml`, `story_underworld_m01_land.xml`, `story_underworld_m02_space.xml`, `story_underworld_m06_land.xml`, `story_underworld_m07_land.xml`, `story_underworld_m08_space.xml`, `story_underworld_m09_space.xml`, `story_underworld_m12_space.xml`, `story_underworld_mandalore_piracy.xml`, `story_underworld_mandalore_piracy_prologue.xml`, `story_underworld_mandalore_piracy_story.xml`, `story_underworld_moncalamari_piracy.xml`, `story_underworld_muunilinst_kidnapping.xml`, `story_underworld_naboo_intimidation.xml`, `story_underworld_nalhutta_kidnapping.xml`, `story_underworld_saleucami_intimidation.xml`, `story_underworld_taris_intimidation.xml`, `story_underworld_thyferra_kidnapping.xml`, `story_underworld_utapau_kidnapping.xml`.

