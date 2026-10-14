# Player profile

Status: built for the main menu wave (legacy EAWR-2935), epic (legacy EAWR-2923); MNU-46 in [menu screens](menu-screens.md).
The model and the store exist; the skirmish setup (legacy EAWR-2958) reads and writes them (space map, faction, colour,
team, game type, local name); the LAN lobby (legacy EAWR-2965) does not read or
write it yet (section 5 lists the calls).

The facts come from the FoC debug build (the profile object's constructor, which gives each
field and its default) and the effective data. Nothing here quotes original code, names or
addresses; evidence cells say "debug build".

## 1. What the profile is

FoC keeps one local player profile in the registry, with the volumes, video and keyboard map
([settings](ui-settings.md)) and the fields below in the same object. Our build keeps two
records beside each other in the user data folder:

- `eawr-settings.cfg`: `PlayerSettings`, [ui-settings.md](ui-settings.md);
- `eawr-profile.cfg`: `PlayerProfile` (`eawr/presentation/ui/player_profile.hpp`, engine-free),
  stored by `ProfileStore` (`apps/viewer/src/profile_store.*`).

Both use the same storage style: UTF-8 text, the version on the first line
(`eawr-profile <version>`), `key=value` lines, written to a sibling file and renamed over the
target (`eawr/platform/ui_files.hpp`). No compatibility is kept before 1.0: change the
format, bump the version.

| Rule | Behaviour | Source |
|---|---|---|
| PRF-S1 | Loading never fails: an empty, unreadable, foreign or other-version file is the defaults; a bad line is dropped and a bad value keeps its default, each with a note; an out-of-range value is clamped with a note. | project |
| PRF-S2 | The profile is UI state (UI-C2): it never enters the simulation, a replay or a hash. The skirmish setup and the LAN lobby read their defaults from it and write back on Start or Accept; the values that reach a match go through the setup seams. | project; UI-C2 |
| PRF-S3 | Text is UTF-8. A name may be non-ASCII; invalid UTF-8, control characters and outer spaces are dropped, and a name is cut on a character boundary. The file path is a native path built from UTF-8, never through an ANSI step. | project |

## 2. Fields and defaults

| Rule | Field | Stored as | Default | Source |
|---|---|---|---|---|
| PRF-01 | Player name | text, at most 32 characters | empty: follows the platform user name, else "Player" (`effective_player_name`) | debug build |
| PRF-02 | Lobby colour (the LAN lobby's colour combo) | row index 0..15 | 0 | debug build |
| PRF-03 | Last space and land map, and the local faction, colour and team for each | map name and faction name (text), colour and team index 0..15 | no map, no faction, 0, 0 | debug build |
| PRF-04 | Last game type, credits, timer and game name | integers and text | game type 1, credits -1 (the authored value), timer 0 (none), no name | debug build |
| PRF-05 | Tutorials played | list of tutorial names, at most 64 | none (`tutorial_seen()` false) | debug build |
| PRF-06 | Network name (the name shown in lobbies) | text, at most 32 characters | empty: follows the player name (`effective_network_name`) | project |
| PRF-07 | Last scenario map | map name | none | debug build |

Where the project differs from the retail layout:

- Maps, factions and tutorials are stored by the data's own **name**, not by list index or a fixed
  name set, so a mod that changes the lists does not make the profile point at another entry. A
  name the loaded data no longer has is the screen's to ignore (use its own default).
- Retail stores the campaign's last internet map too and a password, e-mail address and
  "remember password" flag for the platform account. Those are not kept: the campaign hand-off is
  GC's, and the account is the platform's (MNU-30).
- Retail has no separate network name in its own profile object beyond the player name; PRF-06 is
  a project field so the lobby name can differ. It is empty by default, so nothing changes.
- The volumes, subtitle mode, video, scroll and mouse options, and the keyboard map stay in the
  settings record ([ui-settings.md](ui-settings.md)); MNU-11, MNU-14 and MNU-16 are served by it.

## 3. The text form

```
eawr-profile 1
player.name=...
player.lobby_colour=0
player.network_name=...
skirmish.space.map=...      (also .faction, .colour, .team; and skirmish.land.*)
skirmish.scenario_map=...
skirmish.game_type=1
skirmish.credits=-1
skirmish.timer_minutes=0
skirmish.game_name=...
tutorial.played=<name>      (one line per tutorial)
```

An unknown key is dropped with a note, so a newer file loses only what this build does not know.

## 4. API

`eawr/presentation/ui/player_profile.hpp` (engine-free) and `apps/viewer/src/profile_store.hpp`:

| Call | Does |
|---|---|
| `ProfileStore::load()` / `save(profile, error)` | The profile file in the user folder (or a path a test names). |
| `default_profile()` | The defaults above. |
| `effective_player_name(profile, platform_name)` | The name to show. |
| `effective_network_name(profile, platform_name)` | The lobby name. |
| `set_player_name`, `set_network_name`, `set_lobby_colour` | Write one field, cleaned. |
| `record_skirmish_choices(profile, choices)` | Write back what Start used (map, faction, colour, team per kind; game type, credits, timer, name). |
| `mark_tutorial_played(profile, name)` | Remember a started tutorial. |

## 5. Call sites for the screens

- **Skirmish setup** (legacy EAWR-2958), MNU-21/22: on open, `ProfileStore::load()`; take the remembered map
  per mode (WSS-02) with the first eligible map as the fallback, the local faction, colour and team,
  and the credits and timer (WSS-23: clamp the credits to the authored range; -1 means the authored
  value). On Start: `record_skirmish_choices`, then `save`. Do not save on Cancel.
- **LAN lobby** (legacy EAWR-2965), MNU-26: on open, `effective_network_name` into the name edit and
  `lobby_colour` into the colour combo. When the name edit loses focus or the player hosts or joins:
  `set_network_name` (or `set_player_name`), `set_lobby_colour`, `save`.
- **Tutorial** (MNU-43): `tutorial_seen()` decides the first-run suggestion (`TEXT_SUGGEST_TUTORIAL`,
  MNU-42); `mark_tutorial_played` on Start.
- A screen that writes the profile keeps the record it loaded and saves it whole: a screen must not
  rebuild a profile from its own fields, or it drops the others.

## 6. Open points

- The LAN lobby does not read or write the profile yet (legacy EAWR-2965). The skirmish setup keeps credits, timer and
  game name as loaded (it has no controls for them yet) and does not store a land choice (land is disabled).
- The tutorial name list and the retail "suggest the tutorial" rule are unverified; the suggestion
  condition is a screen decision until MNU-43 is traced.
- The colour index bound (15) is a project bound; the lists on the staging screens have eight or
  nine rows.
