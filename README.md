# RumbleSlop

Super Monkey Ball Banana Rumble as a native Windows game, built on your PC from your own Switch copy.

**What it adds to the Switch game**

- Any resolution: windowed, borderless or full screen, wide screens (16:10, 21:9, 32:9), Steam Deck detection
- Uncapped frame rate with smooth motion above 60 fps, VSync, render scale and the game's other graphics options
- Keyboard and mouse with rebinding; Xbox, DualSense, DualShock 4 and Switch Pro controllers with rumble and
  matching button pictures
- Up to four players in split screen; keyboard + controller counts as two players
- Online play between PCs by room code, over Steam or directly
- Motion controls with a DualSense, DualShock 4 or Switch Pro Controller
- Your add-on content (DLC) installed from your own files
- The Dole collaboration look can be switched back on
- Skip the intro tutorial on a new save
- Nothing is sent to the publisher: no statistics, no telemetry

It is called RumbleSlop because it is vibecoded slop: the whole port was written by an AI coding agent. It works
surprisingly well. Expect rough edges anyway.

> **No game files are in this repository, and none may be added.** You need your own copy of the game, dumped from
> your own console: the base game and the 2.2.0 update. How to dump is not explained here. The setup builds the
> game on your machine from your files, so the result contains the game. It is for your own use. Do not share it.
>
> Not affiliated with or endorsed by SEGA, Nintendo, Unity or CRI Middleware.

## How it works

A real PC version, not an emulator: the game's own code is recompiled to x64 and runs inside Unity's Windows player
of the same engine version. Display and graphics settings are in the game's own Options menu; above 60 frames a
second the game still simulates at 60 Hz and the port blends every frame in between. The Dole collaboration look is
under Options > General > Dole Collaboration, key rebinding under Options > Key Configuration.

## What you need

- Windows 10 or 11, 64-bit, a CPU with AVX2 (anything from the last ten years), a GPU with Vulkan
- [Python](https://www.python.org/downloads/windows/) 3.10 or newer (tick "Add python.exe to PATH" in its installer)
- About 15 GB free besides your game files (the build uses about 12 GB; 6 GB stay after `setup.cmd --clean`)
- An internet connection for the tools the setup downloads (about 300 MB: the llvm-mingw compiler, Ninja, Unity's
  Windows player, SPIRV-Tools)
- Your own files:
  - `prod.keys` of your console
  - the base game as `.xci` or `.nsp`
  - the 2.2.0 update as `.nsp`
  - optional: your add-on content (DLC) as `.nsp` files

Only version 2.2.0 of the game is supported. The base game without that update, or with another update, stops the
setup with a message.

## Setup (once)

1. Download or clone this repository.
2. Put your files into the `input` folder: `prod.keys`, the base game, the 2.2.0 update and any add-on content
   `.nsp` files. The file names do not matter.
3. Double-click `setup.cmd`.

The setup takes 30 to 60 minutes, most of it converting shaders. It can be closed and started again; finished steps
are not repeated. When it says it is finished:

4. Start `game\RumbleSlop.exe`. Windows Firewall asks once whether the game may use the network (needed only for
   online play).

Afterwards the `build` folder and the files in `input` are not needed any more: `setup.cmd --clean` deletes
`build`. The `game` folder can be moved anywhere.

Add-on content bought later: put its `.nsp` files and `prod.keys` into `input` and run `setup.cmd --only dlc`
(the base game and update are not needed for this). With the game files still in `input`, a plain `setup.cmd`
does the same: it notices the new packs and installs only them.

Saves and the settings the port adds (`port_display.txt`, `port_keys.txt`, `port_name.txt`, `port_net.txt`) are in
`Saved Games\RumbleSlop` in your user folder. To keep them inside the game folder instead (a portable game), put an
empty file `portable.txt` next to `RumbleSlop.exe` before the first start. A game folder that already has a `save`
folder keeps using it.

F12 saves a picture of the game into a `screenshots` folder in the same place.

## Playing

Keyboard defaults are the game's own: W A S D tilt, arrow keys camera, Space spin dash, Enter confirm, Esc cancel
and pause, Q / E tabs. With the mouse: hold the left button and drag to tilt. Everything can be rebound in Options > Key Configuration.

Online: **Online Play > Battle (or Adventure) > Host** creates a room and shows a room code (R shows it
and copies it to the clipboard); send it to the other players, who choose **Join** and paste it (Ctrl+V) or type it. The host must be reachable on UDP port 23711: the port asks the router for that through
UPnP; without UPnP it works on the same network or VPN, or with the port forwarded by hand (`port_net.txt`, see
[docs/TECHNICAL.md](docs/TECHNICAL.md)). Random matchmaking, rankings and play with Switch consoles do not exist.

When Steam is running, rooms go over Steam's relay network instead: nothing to open on the router. The code still
has to be passed on by hand; it tells the joining game which Steam account hosts the room. The port does not look
at your friends list and does not show your room to anyone. The game borrows Valve's test app
Spacewar for this, so Steam shows you as "playing Spacewar" while online play is in use; it needs a
`steam_api64.dll`, which the game copies from any of your installed Steam games. `steam=0` in `port_net.txt`
turns this off.

## Logs

The game writes no log files when playing. If it does not start or misbehaves, run `RumbleSlop.exe --log`
(from a command prompt in the `game` folder, or add `--log` to a shortcut's launch options): it then writes
`game\port.log` and `game\player.log`. If the game stops with an error or freezes, it always leaves `port.log`
with a report.
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
