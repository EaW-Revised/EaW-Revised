# EaW Revised viewer project

This is the bare game project for testers who supply their own engine and Python.
To play with the bundled engine and double-click launcher, use the player download.

You need a standard Godot 4.7.2 executable for your platform, Python 3.8 or newer,
and your own Empire at War installation with the Forces of Corruption expansion.
No engine, Python or game data is bundled in this archive.

Extract the whole archive into a writable folder. On Windows, run `play-demo.cmd`;
on Linux, run `sh ./play-demo.sh`. The script finds or asks for your game installation
and Godot executable, extracts fonts from your installation into `out/fonts/`, and
opens space skirmish setup. Use `--help` for options, or `--m2` for the fixed demo battle.

For example, to provide the paths explicitly:

```text
play-demo.cmd --game-root "path to game" --godot "path to Godot executable"
sh ./play-demo.sh --game-root "path to game" --godot "path to Godot executable"
```

This archive uses the **Safe render thread only**. The start scripts select it
automatically (`--render-thread safe`); the standard Godot engine lacks the patch
needed for the player download's default Separate render thread. Start through the
scripts to preserve this setting and prepare the fonts.

`LICENSE` and `THIRD_PARTY_NOTICES.md` are included in this folder.
