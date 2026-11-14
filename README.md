# RumbleSlop

Super Monkey Ball Banana Rumble as a native Windows game, built on your PC from your own Switch copy.

It is called RumbleSlop because it is vibecoded slop: the whole port was written by an AI coding agent. It works
surprisingly well. Expect rough edges anyway.

> **No game files are in this repository, and none may be added.** You need your own copy of the game, dumped from
> your own console: the base game and the 2.2.0 update. How to dump is not explained here. The setup builds the
> game on your machine from your files, so the result contains the game. It is for your own use. Do not share it.
>
> Not affiliated with or endorsed by SEGA, Nintendo, Unity or CRI Middleware.

## What you get

A real PC version, not an emulator: the game's own code is recompiled to x64 and runs inside Unity's Windows player
of the same engine version.

- Windowed, borderless and full screen at any resolution, frame rate limit and VSync, in the game's own Options menu
- The game's graphics rows work: render scale, shadow quality, post-processing, anti-aliasing, texture filtering,
  HUD size
- Real motion above 60 frames a second: the game simulates at 60 Hz and the port blends every frame in between
  (Options > Display > Smooth Motion)
- Wide screens (21:9, 32:9) and 16:10, Steam Deck detection
- Keyboard and mouse with the game's own PC key layout, rebinding in Options > Key Configuration, mouse on every menu
- Xbox, DualSense and DualShock 4 controllers, up to four for split screen, with rumble; button pictures follow the
  device in use
- Online play between PCs running this port: friend rooms joined by a room code, no server
- Add-on content you own
- The intro tutorial can be skipped on a new save

## What you need

- Windows 10 or 11, 64-bit, a CPU with AVX2 (anything from the last ten years), a GPU with Vulkan
- [Python](https://www.python.org/downloads/windows/) 3.10 or newer (tick "Add python.exe to PATH" in its installer)
- About 25 GB free while the setup runs (6 GB afterwards) and an internet connection for the tools it downloads
  (about 300 MB: the llvm-mingw compiler, Ninja, Unity's Windows player, SPIRV-Tools)
- Your own files:
  - `prod.keys` of your console
  - the base game as `.xci` or `.nsp`
  - the 2.2.0 update as `.nsp`
  - optional: add-on content `.nsp` files

Only version 2.2.0 of the game is supported. The base game without that update, or with another update, stops the
setup with a message.

## Setup (once)

1. Download or clone this repository.
2. Put your files into the `input` folder. The file names do not matter.
3. Double-click `setup.cmd`.

The setup takes 30 to 60 minutes, most of it converting shaders. It can be closed and started again; finished steps
are not repeated. When it says it is finished:

4. Start `game\RumbleSlop.exe`. Windows Firewall asks once whether the game may use the network (needed only for
   online play).

Afterwards the `build` folder and the files in `input` are not needed any more: `setup.cmd --clean` deletes
`build`. The `game` folder is self-contained and can be moved anywhere.

Saves are in `game\save`. Settings the port adds are small text files next to the exe (`port_display.txt`,
`port_keys.txt`, `port_name.txt`, `port_net.txt`).

## Playing

Keyboard defaults are the game's own: W A S D tilt, arrow keys camera, Space spin dash, Enter confirm, Esc cancel
and pause, Q / E tabs. Everything can be rebound in Options > Key Configuration.

Online: **Online Play > Battle (or Adventure) > Host** creates a room and shows a room code; the other players
choose **Join** and type it. The host must be reachable on UDP port 23711: the port asks the router for that through
UPnP; without UPnP it works on the same network or VPN, or with the port forwarded by hand (`port_net.txt`, see
[docs/TECHNICAL.md](docs/TECHNICAL.md)). Random matchmaking, rankings and play with Switch consoles do not exist.

## Known problems

- Tested: the setup from scratch on one PC; title, menus, options, early Adventure stages, online Battle and Adventure between two copies on one PC,
  split-screen Adventure. Later stages and other modes have had little or no testing.
- Online play has not been tested over the internet.
- The opening movie cannot be skipped.

If the game does not start, look at `game\port.log` and `game\player.log`.

## For developers

[docs/TECHNICAL.md](docs/TECHNICAL.md) explains how the port works, how to rebuild parts of it, the diagnostics, and
everything that was learned on the way. `setup.cmd --list`, `--redo <step>` and `--only <step>` run single steps.

## Credits and licences

RumbleSlop's own code is under the [MIT licence](LICENSE). It builds on other people's work, listed with licences
in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md): above all [XC2 Recomp](https://github.com/AksharDP/xc2-recomp)
and the projects it descends from (TotK Recomp, LAS Recomp, MacNativeGames), whose ARM64-to-C lifter and shader
translator do the heavy lifting, [Ryujinx](https://github.com/Ryujinx/Ryujinx) for the shader translator's core,
[UnityPy](https://github.com/K0lb3/UnityPy), [Kenney](https://kenney.nl) for the button icons (CC0), and
[Switchbrew](https://switchbrew.org) for the file format documentation. That the characters which can no longer be bought or earned only need
their unlock rule changed was found by iswimfly ([Delisted Character Fix](https://gamebanana.com/mods/690229));
the port gives them to every save in its own way and includes nothing of that mod.
