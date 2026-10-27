# EaW Revised: player guide

EaW Revised plays Empire at War: Forces of Corruption with a new engine. You need your
own copy of **Empire at War with the Forces of Corruption expansion**; nothing from the game is
included in this download.

## Start

1. Extract the whole zip into a folder you can write to, for example `Documents\EAWR` (do not run it from
   inside the zip). It does not need an installer, administrator rights or Python, and it does not need to
   sit beside your game.
2. Double-click **`EAWR.exe`**. No console window opens.
3. EAWR looks for Forces of Corruption on its own. If it finds one install it starts; if it finds
   several it asks which one; if it finds none it asks you to browse for the folder that contains
   `GameData` and `corruption`. It remembers your choice.
4. The first start takes a few seconds longer: EAWR reads the four EmpireAtWar interface fonts out of
   your own `corruption\StarWarsG.exe` and keeps them for later starts. They are never copied into
   this folder or shared.
5. Pick a map and factions in the skirmish setup, then play.

The graphics card must support Vulkan or Direct3D 12 (any card from the last several years; update its driver if
EAWR reports it cannot start the renderer).

## What EAWR writes, and where

EAWR is built not to write into this folder or into your game. Everything it creates lives in your Windows
user data, so both folders can be read-only (a Program Files copy, a network share; the last checklist
items below test exactly that):

`%APPDATA%\Godot\app_userdata\EAWR Viewer\` holds your settings and profile, saved games and replays,
the fonts it extracted, reports and logs (`logs\godot.log`).

To reset EAWR, close it and delete that folder. Your game is untouched.

## Options

Reinforcement areas only while placing is enabled by default in **Options > Gameplay**.
The red no-drop grid appears during placement and disappears when placement ends, even with the panel open.
Turn it off to follow the retail/mod panel-or-drag rule. Accept saves the choice; Cancel discards edits; Defaults enables it.
This changes presentation only and takes effect without restarting.

### Render thread update

Separate rendering is now the default. To switch back to Safe, start
**`EAWR (Safe render thread).cmd`** or run `EAWR.exe --safe-render-thread`.
You can also choose **Options > Performance > Render thread (restart)** and apply it; the saved
choice takes effect when you restart EAWR. An explicit launch flag overrides it.

## When something goes wrong

| What you see | What it means | What to do |
|---|---|---|
| "Could not find Forces of Corruption" | No install with `GameData` and `corruption` was found | Browse to your game folder. Base Empire at War alone is not enough. |
| "This Forces of Corruption build is not one EAWR has verified" | Your `corruption\StarWarsG.exe` is not the Steam edition EAWR checks | Use the Steam edition. If you want to try another edition, start `EAWR.exe` with `--eawr-allow-unknown-build`; fonts are still validated, and if they cannot be extracted EAWR shows a message and stops. |
| "Cannot read ..." | The account cannot read a game file | Check the folder's permissions and repair or verify the game files in Steam. |
| Windows says "Windows protected your PC" | The download is not code-signed, so SmartScreen warns about it | If you downloaded it from the official release page, choose **More info**, then **Run anyway**. You can compare the zip with the `SHA256SUMS.txt` on the release page (`Get-FileHash .\<zip>`). |
| Windows or your organisation blocks EAWR with no "Run anyway" | Smart App Control, WDAC / AppLocker or a managed-PC policy refuses unsigned programs | EAWR cannot get around that; ask whoever manages the PC, or use another PC. Never turn off security software, Smart App Control or add exclusions for EAWR. |
| Window opens and closes at once | The graphics driver failed | Update the graphics driver; the log named above says why. |
| Flicker, a frozen picture, a crash on start, or errors about the graphics device, and your drivers are up to date | EAWR draws on a separate render thread by default (measured faster on the tested NVIDIA GPUs); some driver and GPU combinations dislike it | Close EAWR and start **`EAWR (Safe render thread).cmd`** instead (same folder). It uses the older single-threaded drawing path. If that fixes it, tell us your GPU and driver in the bug report. |

## Firewall and multiplayer

Offline skirmish needs no network access. LAN and online play open network ports; Windows may then ask
whether to allow `Godot_v4.7.2-stable_win64.exe` (the engine in the `godot` folder) on private networks.
EAWR itself never adds firewall rules, never asks for administrator rights and never changes
router settings. All players of a match must run the same EAWR version.

## Updating

Close EAWR, delete the old folder, extract the new zip, and run `EAWR.exe`. Your settings, saves, replays and fonts
stay in user data, so nothing is lost.

## Report a problem

Open an issue on the project's GitHub page and attach `logs\godot.log` from the user data folder above.

---

## Acceptance checklist for a downloadable candidate

Run this on a clean Windows machine (no Python, no Godot, nothing from EAWR installed; a freshly installed
PC or VM, with a retail Steam copy of Forces of Corruption) and then again as a normal, non-administrator user. Record pass or fail per line.

- [ ] Extract the zip to a writable folder whose path contains a space and a non-ASCII character.
- [ ] `EAWR.exe` starts by double-click with no console window and no UAC prompt.
- [ ] The game is found without typing a path (Steam install); with the game moved aside, the folder picker appears and a wrong folder gets a readable message.
- [ ] The skirmish setup opens with the EmpireAtWar fonts (not a plain fallback font).
- [ ] A skirmish against the AI starts, runs for a few minutes, a unit can be bought, and the window closes cleanly.
- [ ] After the run, this folder and the game folder are byte-for-byte unchanged (compare a directory listing taken before the first start); `%APPDATA%\Godot\app_userdata\EAWR Viewer\` holds fonts, settings and `logs`.
- [ ] Mark the extracted folder and the game folder read-only (deny write for the user) and repeat the start: it still works.
- [ ] Start a second time: no font extraction pause, same settings.
- [ ] An unverified `StarWarsG.exe` (use a renamed copy of another build) gives the readable "not one EAWR has verified" message and does not crash.
- [ ] No firewall rule was added by EAWR (`Get-NetFirewallRule -DisplayName *EAWR*` and `*Godot*` show nothing new until a LAN or online host is started).
- [ ] Extract a newer zip over a fresh folder: settings, saves and fonts carry over.
- [ ] Windows reputation or policy refusals, if any, are recorded as such (SmartScreen warning, Smart App Control or policy block), not as an EAWR failure.
