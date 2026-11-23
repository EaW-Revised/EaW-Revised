## Linux x64 player

This is the first Linux player. It needs 64-bit Linux with glibc 2.38 or newer (Ubuntu 24.04, Debian 13, Fedora 39 or newer),
OpenSSL 3 (`libssl.so.3` and `libcrypto.so.3`, present by default on Ubuntu 24.04), a Vulkan graphics driver (or the Mesa
software renderer, package `mesa-vulkan-drivers`) and your own copy of Empire at War:
Forces of Corruption (Steam is found automatically; otherwise pass `--game-root`).

1. Extract the zip with a tool that keeps file permissions (`unzip` does), or run
   `chmod +x play.sh godot/godot.linuxbsd.editor.x86_64`.
2. Run `./play.sh`. Use `./play.sh --game-root /path/to/"Star Wars Empire at War"` when it cannot find the game, and
   `./play.sh --check` to test your system without starting the game.

It was tested with software rendering on Ubuntu 24.04, not yet on a range of graphics cards: reports from real
hardware are welcome. There is no launcher or mod manager on Linux yet. The player never writes into its own folder.
