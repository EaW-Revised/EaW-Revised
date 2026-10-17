# EaW Revised: debug build

The debug archive (`eaw-revised-<version>-windows-x64-debug.zip`) is the same game as the player zip, built
in the compiler's Debug configuration. Use it to reproduce a crash or a wrong result so that a bug report
can name the exact function; use the normal player zip to play.

## What is different

- **Checks are on.** Internal consistency checks (`assert`) that the normal build compiles out stay active.
  A failed check stops the program with its file and line instead of carrying on with bad data.
- **Symbols are included.** Every program comes with its `.pdb` file in the same folder. Keep each `.pdb`
  next to its `.dll` or `.exe`; a debugger (Visual Studio, WinDbg) then shows function names and source lines
  in a crash dump.
- **The viewer shows its debug menu entries.** The front end exposes the options that the normal build hides.
- **It is much slower and larger.** There is no optimisation, so big battles can run well below the normal
  speed. Do not judge performance with this build.
- **It carries the headless tools.** `tools\` holds the same programs as the tools zip (`sim_headless.exe`,
  the scanners and benchmarks), also in the Debug configuration, each with its `.pdb`.

The folder is laid out like the player zip: double-click `EAWR.exe`, which starts the bundled Godot with the
debug viewer extension in `project\bin`. Everything in [the player guide](README.md) applies, including where
settings and logs are written and that the download is not code-signed.

## Reporting a crash

1. Reproduce it with this build if you can; say that you used the debug build, and which version.
2. Attach `logs\godot.log` from the user data folder named in the player guide.
3. If a debugger caught a failed check or a crash, add the call stack (the top ten frames are enough).
4. A crash dump (`.dmp`) is welcome too, but only attach one after checking that you are happy to share it:
   a dump can contain parts of your computer's memory, including file paths.

## Playing with others

Match the build kind of the other players. The simulation is meant to give the same result in both
configurations, but the debug build is slower and may fall behind in a busy online or LAN match.
