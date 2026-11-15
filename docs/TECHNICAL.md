# RumbleSlop: technical guide

A native Windows build of Super Monkey Ball Banana Rumble (Switch, Unity 2022.3.30f1, IL2CPP, version 2.2.0) made
on the user's PC from the user's own dump. This file is for whoever works on the port. It records how the port is
put together, how to build and run it, and the traps already found. The player-facing instructions are in
`README.md`.

Nothing in the repository contains game code or data. Everything game-derived is made by `setup.cmd` under `build\`
and `game\`, both ignored by git. The address tables in `port\recomp\tables\` and the offsets in the runtime's
sources are facts about the 2.2.0 executable (where functions start and end), not its code.

## Design in one paragraph

The game's managed code is hosted inside the real Windows Unity player of the same engine version. Only the
managed side of the Switch executable `main` is recompiled: the IL2CPP-generated code, libil2cpp, the Boehm
collector, the CRIWARE library and a little Nintendo plug-in glue are lifted ARM64 -> C -> x64 into a replacement
`GameAssembly.dll`. The engine itself (`UnityPlayer.dll`) is Unity's own. Everything the lifted code imports from
the Nintendo SDK is implemented on Win32 by a small host runtime. Assets are converted from the Switch build to
what the Windows Vulkan player accepts.

## Layout

| Path | What it is |
|---|---|
| `setup.cmd`, `setup\` | The one-time setup. `setup.py` runs the steps; `nx.py` reads XCI / NSP / NCA with the user's keys (pure Python); `fetch.py` downloads and unpacks the tools; `nsis.py` takes Unity's player out of Unity's installer without running it; `assemble.py` builds `game\`. |
| `input\` | The user's `prod.keys` and dumps. Never committed. |
| `port\paths.py` | Every folder and tool location, in one place. Scripts never name a machine path. |
| `port\recomp\` | The lifter front end. `gen_il2cpp.py` generates C, `ehtab.py` reads exception tables, `hooks.txt` lists entry hooks, `tables\` holds the address tables for game version 2.2.0 (lift set, boundary targets, il2cpp API map). |
| `port\build.py` | Compiles the generated C to objects (ninja, clang). |
| `port\runtime\` | Host runtime: loader, dispatcher, threads, exception unwinding, GC support, icall/API glue, and the `imports_*.c` Nintendo SDK implementations. `build_runtime.py` builds it and links `GameAssembly.dll`. |
| `port\runtime\include\` | Contract shared by generated code and runtime: `cpu.h` (CPU state), `eh.h` (exception unwinding). |
| `port\launcher\` | The game's `.exe`: loads `UnityPlayer.dll` and calls `UnityMain` with `-force-vulkan`. |
| `port\assets\` | Asset conversion: retargeting, catalog patch, texture conversion, verification tools. |
| `port\shaders\` | Shader conversion (Switch NVN binaries -> SPIR-V -> Unity Vulkan programs). |
| `port\tools\` | Debug helpers: `sym_unity.py`, `callees.py`, `shot_window.ps1`, `vklayer\`. |
| `tools\` | Analysis scripts for the Switch binary and metadata (`cs.py`, `fn.py`, `a64xref.py`, `il2cpp_*.py`, `closure_*.py`, ...). |
| `third_party\` | `xc2-recomp\`: the lifter core (MIT), unmodified. `xlate\`: the prebuilt shader translator (MIT). |
| `build\` | Made by the setup: `downloads\`, `toolchain\` (llvm-mingw, ninja, Unity player, SPIRV-Tools), `venv\`, `work\game` (extracted game), `work\re\main.bin`, `work\unity` (converted asset trees, caches), `generated\` (about 890 MB of C, 292 chunks), `obj\`, `logs\`. |
| `game\` | The finished game. Self-contained: `build\` and `input\` can be deleted afterwards. |

## Build

`setup.cmd` does everything (see `setup\setup.py` for the steps; `setup.cmd --list`, `--redo STEP`, `--only STEP`).
It creates `build\venv` and runs every script with that Python. By hand, with `build\venv\Scripts\python.exe`:

```
python setup\fetch.py                           # tools into build\toolchain
python setup\nx.py extract input\<base> input\<update> build\work\game
python tools\nso.py dump build\work\game\exefs\main build\work\re\main.bin
python port\recomp\gen_il2cpp.py                # ~32 s, regenerates build\generated
python port\build.py                            # compiles changed chunks only; a full rebuild is ~100-150 s
python port\runtime\build_runtime.py            # runtime + link; copies GameAssembly.dll to game\ when it exists
python port\runtime\build_runtime.py --test     # stand-alone runtime tests
python setup\assemble.py                        # game\ from the build outputs
```

Changing the lift set shifts chunk boundaries and forces a full recompile. Adding an entry hook only recompiles
the chunks that contain the hooked functions.

`python port\recomp\gen_il2cpp.py --dump 0x<NSO offset>` prints the generated C of one function, which is
the quickest way to read what a managed method does.

Tools already on the PC can be used instead of the downloads: `LLVM_BIN`, `NINJA`, `UNITY_PLAYER`,
`SPIRV_TOOLS_BIN` (see `port\paths.py`). Without SPIRV-Tools the shader conversion still works; the converted
modules are then neither validated nor optimised.

Method names for `tools\cs.py`, `port\tools\callees.py` and named backtraces in `port.log`:
`python tools\il2cpp_link.py build\work\game\exefs\main build\work\game\romfs\Data\Managed\Metadata\global-metadata.dat build\work\unity`
writes `methods.tsv` and `generic_methods.tsv`; `setup\assemble.py` copies them into the game folder when they exist.
The names come from the user's own game files and are never committed.

### How the version-specific tables were made

`port\recomp\tables\` is for the 2.2.0 executable (build id in `port\paths.py`):

* `lift_functions.tsv.gz`: start and end of every function that is lifted, from `tools\closure_build.py`
  (reachability from the managed method table over direct branches and address-taken functions, within the
  libil2cpp / generated-code / Boehm regions; function ends from the unwind tables).
* `extra_lift.tsv`: the CRIWARE library and the Nintendo plug-in glue, taken whole (`port\recomp\extra_lift.py`).
* `boundary_direct.tsv`, `boundary_runtime_to_engine.tsv`: the functions outside the lift set that lifted code
  calls, with what they are (P/Invoke natives, engine entry points).
* `il2cpp_api_map.tsv`: where each `il2cpp_*` API function the Windows player binds lives in the Switch executable
  (the Switch build has no export table; each was located by its code shape, column `evidence`).
* `il2cpp_api.tsv`: the `il2cpp_*` functions the Windows player of this engine version binds, with their C
  signatures.

Another game version needs these tables, `hooks.txt` and the `0x...ull` offsets in `port\runtime\*.c` redone.
That was done once, from 2.0.0 to 2.2.0, like this:

1. The tables are regenerated from the new executable, not carried over: the region constants at the top of
   `tools\closure_build.py` and `port\recomp\extra_lift.py` are set for the new image, then `closure_build.py`,
   `tools\make_tables.py` and `extra_lift.py` are run.
2. An address map old -> new is built for every function (matched by method name from the metadata where there is
   one, by code shape after masking addresses elsewhere, and confirmed by who calls whom) and for the data the
   port names (class and method pointers). Native code (engine, CRIWARE, libil2cpp) moved as a block; the
   generated managed code did not: generic instances were reordered, so `old + constant` is wrong there.
3. `hooks.txt`, `il2cpp_api_map.tsv`, every `0x...ull` constant, `rt_hook_<address>` name and
   `CONTEXT_HOOK(<address>` argument are rewritten through the map. A constant that is not an address (a mask, a
   size) must be left alone: read the diff.
4. Every hooked or called function whose body differs between the versions is read again (in 2.2.0: the button
   glyph functions, which now go through `UIStandingSpriteManager`; the hooks kept working unchanged), and the
   field offsets the port reads are compared between the two versions' type layouts (in 2.2.0 `MgPlayer` fields
   from +0x1b0 moved by 8: `m_IsLocal` is at +0x1c0).
5. As a cross-check the regenerated lift set must equal the old one carried through the map plus the functions the
   new version added.

`gen_il2cpp.py` guards against one weakness of the function-start search: an address that an unrelated
ADRP + ADD pair seems to build is not taken for a start when it lies between a call and that call's landing pad
(2.2.0 has one such address; splitting there would cut the function off from its exception handler).

## Run

`game\<Name>.exe` starts the game. The launcher adds `-force-vulkan` (the converted shaders only carry Vulkan
programs) unless the command line names another graphics API.

For bounded test runs use `sh port/runtime/run_player.sh <seconds> [VAR=value ...]`. It kills any running
instance first, runs headless (`-batchmode -nographics`) unless `BR_PLAYER_ARGS` is given, and kills the player at
the time limit. Logs are `game\port.log` (runtime) and `game\player.log` (engine).

Navigation for scripted runs (timings drift by a few seconds between runs): title appears at about 22 s, A at
30 s reaches the play-style screen by about 55 s, A enters Single Player, Options is the seventh tag (RIGHT six
times, then A), R twice reaches the Display tab. A at 30, 58, 75, 85, 95, 105, 115 and 125 s goes through the
main menu (70 s) and the Adventure stage select (92 s) into the first stage, with the HUD up by about 135 s.
A new save first asks whether to skip the intro tutorial (see below), which shifts these by one press.

### Environment variables

| Variable | Effect |
|---|---|
| `BR_PLAYER_ARGS` | Player arguments for `run_player.sh`, e.g. `-force-vulkan`. |
| `BR_INPUT_SCRIPT=<file>` | Scripted pad input, lines `<ms> <BUTTON> [hold ms]` (A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN). Buttons only, no sticks. |
| `BR_STATUS=<s>` | Periodic counters (audio, pad reads, collections). |
| `BR_FPS_LOG=1` | Every 2 s (`BR_FPS_LOG=<seconds>`: that often): measured frame rate, engine clock rate, physics step rate, engine target/vsync values, in how many frames a camera moved, and how many camera steps were less than half or more than twice the step before ("uneven"); the same for the first player's ball, monkey, the monkey's offset from camera 0 and the second player's ball
(split screen, online). Steps under 1 mm are not judged (a ball pressed against a wall jitters by fractions of a
millimetre). `BR_CAM_TRACE=1` adds a line per frame and camera (step length, wall time since the frame before in ms) and per frame for the ball (step, `Time.time`, `Time.fixedTime`). |
| `BR_INTERP=0\|1`, `BR_INTERP_LOG=1` | Smooth motion (see that section) forced off / on whatever the settings say; every 2 s the number of tracked transforms and blended parts, and the transforms written in both phases of a frame. |
| `BR_CHARA_DUMP=<part>`, `BR_CHARA_DUMP_EXIT=1` | Renderer/material dump of spawned characters (see below). |
| `BR_DLC_LOG=1` | Logs the game's own verdict for each add-on content pack (index, name, installed or not), once per pack. |
| `BR_AUDIO_DEBUG=1` | Audio pacing report, stall report, Start/Stop calls. `BR_AUDIO=off` skips the device. |
| `BR_HANGDUMP=<s>,<s>...` | Dumps every guest thread's host function and stacks at those times. `BR_HANGDUMP_ALL=1` adds engine threads. Resolve `UnityPlayer.dll+0x...` frames with `python port\tools\sym_unity.py`. |
| `BR_FNSAMPLE=<start>,<dur>[,<lines>]` | Counts lifted function entries in a time window. |
| `BR_ICALL_FILTER=<substr>` | Logs matching engine icalls. |
| `BR_DISPLAY_MODE=0\|1\|2` | Test override of the screen mode (windowed, borderless, full screen). |
| `BR_DISPLAY_SIZE=<w>x<h>` | Test override of the size, any aspect ratio (e.g. `2560x720` with `BR_DISPLAY_MODE=0` is a 32:9 window). Never written to `port_display.txt`. |
| `BR_DESKTOP_SIZE=<w>x<h>` | Test: pretend the desktop has this size. With `SteamDeck=1` (the variable Steam itself sets on a Deck) and `1280x800` the port behaves as on a Steam Deck. |
| `BR_SCREEN_DUMP=<s>,<s>...` | At those times: every camera (rect, target) and every UI canvas (scaler settings, pixel rect); also logs each UI element the wide-screen code widens. `BR_SCREEN_TREE=<depth>` adds the active UI objects under each canvas with their pixel boxes. |
| `BR_SCREEN_RESIZE=<s>:<w>x<h>,...` | Switches to a window of that size at those times (tests a resolution change without the menu). |
| `BR_UNLOAD_EVERY=<frames>` | Test: calls `Resources.UnloadUnusedAssets` every that many frames (stress for the engine's unused-asset sweep, which otherwise runs only between scenes). |
| `BR_NO_REAL_INPUT=1` | The real keyboard, mouse and controllers are ignored (a scripted test is not disturbed by someone using the PC; dragging the game's window still stalls it). |
| `BR_INPUT_LIVE=<file>` | Lines `<BUTTON> [hold ms]` appended to the file while the game runs are pressed one after the other (drives a running game from a test). Also `TYPE <text>` (into the open text box; `<` Backspace, `!` Esc in the name box), `KEY <name> [ms]` (a keyboard key, named as in `UnityEngine.InputSystem.Key`), `CLICK <x> <y>` / `MOVE <x> <y>` (left click at / cursor moved to a window pixel, from the top left), `WHEEL <n>` (mouse wheel notches, negative = down), `HOLD <x> <y>` / `RELEASE` (left button held at a window pixel while `MOVE` lines drag it, then let go), `DUMP` (the `BR_SCREEN_DUMP` output now) and `GOAL` (every local player's ball enters the stage's first goal: the game's own goal handler is called, so results, progress and what follows a stage can be checked without playing it; `test_aids.c`). Both input files also take `LSU LSD LSL LSR` (left stick fully up / down / left / right). |
| `BR_START_STAGE=<n>` | A world picked in split screen or online starts at its stage n (from 0) instead of its first stage, so a test reaches a late stage at once (`test_aids.c`; online, set it for every instance; n must be a stage of the world). `BR_PADS=2` gives the second pad split screen needs; its input lines are prefixed `2:`. |
| `BR_PROFILE=<suffix>` | A second copy of the game on the same PC uses `save<suffix>\` and `temp<suffix>\` (give it `BR_LOG` and `-logFile` of its own too). |
| `BR_NICKNAME=<name>` | Player name (default `Player`) when no name was set in the game (`port_name.txt`, see the online display name). |
| `BR_STATE_LOG=1`, `BR_NAME_LOG=1` | UI diagnostics: every UI state request; every button a screen registers. |
| `BR_NET_LOG=1` | Online play: every 5 s, the number of messages sent and received per event code. |
| `BR_HEAPDBG=1` | With the player argument `-systemallocator`: debug heap under the engine (`heapdbg.c`), reports overruns, writes after free and double frees inside UnityPlayer.dll with the allocating call stack. |
| `BR_PAD_LAYOUT=positional` | Face buttons by Switch position instead of by label. `BR_SONY_PAD=off` disables direct DualSense reading. |
| `BR_KB_PAD=1`, `BR_KIND=<n>`, `BR_INPUT_DUMP=1`, `BR_GLYPH_LOG=1` | Keyboard, mouse and glyph tests (see that section). |
| `BR_LANG` | The language a new save starts in, as the console names it (`ja`, `en-US`, `fr`, `de`, `it`, `es`, `es-419`, `zh-Hans`, `zh-Hant`, `ko`, `pt-BR`); default: the Windows display language. |
| `BR_TRACE`, `BR_TRACE_AFTER`, `BR_LOG_MAX`, `BR_PINVOKE=trap`, `BR_LIST_IMPORTS` | Low-level tracing. Never use `BR_TRACE=host` for a whole run (the log explodes). |

Screenshots of a run: `port\tools\shot_window.ps1` captures the game window's own content (not the screen). Where
the execution policy blocks script files, run it as
`& ([scriptblock]::Create((Get-Content -Raw port\tools\shot_window.ps1))) -Out <prefix> -At 30,60`.

### Scripted tests: how the live commands behave (`imports_hid.c`, `test_aids.c`)

What the `BR_INPUT_LIVE` table row does not say, read from the code:

* The file is read every 50 ms from where the last read stopped; a line without its newline yet waits. Button
  lines queue: each starts at least 150 ms after the one before ends. The number after a button is its hold time
  (default 100 ms), so `B 1500` holds B for 1.5 s. A `1:`..`4:` prefix picks the pad (`2:A`, `2:LSU 1500`); a second
  pad exists only with `BR_PADS=<n>` (or a real one). The other commands (`TYPE`, `KEY`, `CLICK`, `MOVE`, `HOLD`,
  `RELEASE`, `WHEEL`, `GOAL`, `DUMP`) act as soon as they are read and do not wait for queued buttons.
* `CLICK`, `MOVE`, `HOLD` take pixels of the window's client area, top left = 0 0, in physical pixels: with
  Windows display scaling the client area is larger than the window size the game was set to.
* `GOAL` calls the game's goal handler (`MgGoalColi.OnTriggerEnterPlayer`) for the stage's first goal with every
  local player's ball; it logs "test: GOAL: ..." either way. It skips the run to the goal, so whatever is computed
  from the run is not real: online the result screen shows a nonsense clear time ("-31:-23.-648"); a real roll into
  the goal shows a correct one. Use it to reach results, progress and the next stage, not to check times.
* `BR_NO_REAL_INPUT=1` makes the port treat the window as never focused: real keys, mouse buttons, wheel and pad
  buttons are ignored, rumble is not sent, and the scripted cursor stays where the last `CLICK` / `MOVE` / `HOLD`
  put it. XInput pads are not looked for, but HID pads (DualSense, DualShock 4, Switch Pro Controller) are still found and
  listed: a plugged-in one counts as a connected controller (which changes what Local Multiplayer does with the keyboard)
  while its buttons do nothing. Dragging the game's window still stalls the game.
* A test that changes a row of Options writes the same `port_*.txt` files the player uses (one file for all
  profiles): put the player's values back afterwards.

## Runtime

* **Address space.** The Switch image is mapped at `0x7000000000` and the code is generated for that base, so a
  guest address is a host address. Lifted functions are `void f_<address>(Cpu *c)`; indirect branches go through
  `rt_dispatch`.
* **Exceptions.** C++ exceptions use return-based unwinding: a flag in the CPU state checked after every call,
  with LSDA-driven landing pads. The contract is in `include\eh.h`.
* **Boundaries.** Engine icalls, the il2cpp API the player binds, PLT imports and P/Invoke natives are bound in
  `dispatch.c`, `icall.c`, `api_hand.c` and the `imports_*.c` files. A P/Invoke native without an implementation
  raises `EntryPointNotFoundException` unless `pinvoke_rules.inc` answers it.
* **Collector.** Stop-the-world suspends only threads registered with the lifted Boehm. Any host lock the
  collector thread can take while the world is stopped must be covered in `gc_stop_world` (heap gate, process
  heap, object table, big-block table and log are).
* **No null checks.** The game is built without IL2CPP null checks, so a managed null dereference is a hard
  access violation logged as FATAL in `port.log`, not a `NullReferenceException`. There are no array bounds
  checks either: an index past the end silently writes into the next heap object, and the fault comes later,
  typically in the engine's unused-asset sweep between scenes (`il2cpp::vm::Liveness`, on the main thread and
  the engine's `AGCThread` workers at once, which is by design), when it follows the damaged object.
* **Crash report.** A fault in lifted code logs all guest registers, the eight words behind each register that
  points at memory (with the class name where the first word is a class pointer, and one level further for
  x19..x28), the host stack, and every other guest thread: suspended or not, registered with the collector or
  not, and where it is. A class pointer that differs from its neighbours' in one byte is an out-of-bounds write.
* **Audio** (`imports_audio.c`). The game appends 5 ms buffers and keeps three queued. Buffers are released by a
  wall clock at the exact real-time rate and the PCM is copied into a ring that a feeder thread plays through
  waveOut in 10 ms blocks. Do not tie releases to waveOut completions (they arrive in 10 ms steps and halve the
  audio rate). The release event is re-signalled every tick while a played buffer is uncollected, because the
  game can clear a signal it has not acted on.
* **Input** (`imports_hid.c`, `pad_hid.c`). Up to four full-key pads: npad No.1..4 are the connected controllers
  in this order: XInput users 0..3, then pads read directly over HID (USB or Bluetooth, up to four, each in a slot
  it keeps while connected). `pad_hid.c` finds them and runs one reader thread per pad; each kind has its own
  protocol file behind the `PadDriver` interface of `pad_hid_dev.h`: `pad_ps.c` (DualSense, DualShock 4) and
  `pad_switch.c` (Switch Pro Controller: USB handshake, subcommands for motion, vibration, player LED, full
  reports, factory stick calibration from SPI flash; written after dekuNukem's documentation and SDL). Buttons
  come out by position (`PAD_SOUTH` ...); a Pro Controller is mapped by position (its labels are the game's own)
  and keeps the game's own button pictures. `BR_HID_PADS=off` turns HID pads off. No.1
  exists even without a controller. The Switch controller applet (press L+R to join) is answered at once with
  every connected pad. Keyboard and mouse are not
  mapped onto a pad any more: they are devices of the game's own action system (see "Keyboard, mouse and button
  glyphs"); `BR_KB_PAD=1` brings the old keyboard-as-controller mapping back.
* **The keyboard seat.** Normally the keyboard and the first controller are both player 1. When the controller
  applet asks for one player more than there are controllers (Local Multiplayer with a single pad), player 1
  becomes the keyboard alone and the controllers are players 2 and up (`rt_hid_keyboard_seat`), until the
  play-style screen is built again. Seen with one DualSense: Local Multiplayer opens with P1 and P2, in a
  split-screen stage the W key moves player 1 and (scripted) pad input moves player 2. Not run: the way back to
  single player afterwards, three players (two pads and the keyboard is not offered: the seat is only taken
  when the pads are one short of the minimum).
* **Rumble** (`imports_hid.c`, `pad_hid.c`). The game's effects are `.bnvib` files that it hands to a native
  plug-in of the Switch build (`nnUser.Util+Vibration::Play(npad, slot, bytes)`, called from
  `AppInput.StateNpad.vibrationSpecific`; the plug-in is not lifted code). The port answers those P/Invoke natives
  itself (`rt_vib_pinvoke`, asked first by `rt_pinvoke_policy`): up to four files play on a pad at once, a thread
  steps through them at the file's rate (200 Hz), the strongest low-band amplitude drives the heavy motor and the
  strongest high-band amplitude the light one. `nn::hid::SendVibrationValue` (which this game does not use) feeds
  the same motors. Output: `XInputSetState`; for a Sony pad its output report with only the motor fields
  marked valid; for a Pro Controller report 0x10, HD rumble at 160 Hz (heavy) and 320 Hz (light) on both sides. `BR_VIB_LOG=1` logs every Play and Stop. Seen: a stage plays 0.5 s files on bumps; a DualSense on
  USB accepted the report. A Pro Controller over Bluetooth rumbled. Not tested: an XInput pad, a DualShock 4, a Sony pad
  over Bluetooth (its report layout and CRC are written from the documentation of other projects).
* **Online.** Friend matches joined by a room code work between PCs running this port (next section). Everything
  else of Nintendo's online service is absent: the platform layer still says there is no network, and the
  remaining `nn.npln` natives raise `EntryPointNotFoundException` if something reaches them.
* **Software keyboard.** Room codes are typed in the game's own screen (see online play). `nn::swkbd::ShowKeyboard`
  itself, which nothing in the supported flows reaches any more, falls back to a small Windows text box
  (`swkbd_win.c`).

### Online play between PCs (`online.c`, `netplay.c`, `online_ui.c`, `upnp.c`, `steam_net.c`)

Friend matches work between PCs that run this port: **Online Play > Battle (or Adventure) > ... Friends > Host**
creates a room and shows a room code (the game calls it the password; R shows it and copies it to the clipboard),
**Join** asks for the code. Random matchmaking, the public room list, rankings and local wireless do not exist on
PC: they fail at once or show an empty list. There is no play with Switch consoles.

How it is put together:

* The game uses three facade objects (`GCS1.Network.NPLN.NplnAPIFacade`, `SessionAPI`, `NplnMatchmakingAPI`) on
  top of Nintendo's NPLN stack, which the port does not have. `online.c` hooks their methods. While a room is being
  entered or is entered the hooks answer from the transport; while idle they let the originals run, which then take
  their own "not online" paths, so offline play is unchanged. Received data goes to the game's own static
  `SessionAPI.OnEventReceived`; observers are notified through the game's own list helpers.
* `netplay.c` is the transport: UDP, a star through the room's creator (always the game's "master"; when it leaves
  the room ends). One reliable ordered channel and unreliable datagrams per connection, clock synchronisation to
  the creator's clock (the game's NetworkTime), relaying between guests. It is pure host code with a stand-alone
  test (`python port\runtime\build_runtime.py --test-net`: three peers over loopback with 20% packet loss).
* Peer ids are small numbers (creator 1, guests 2, 3, ...): the game multiplies a peer id by 1000 into 32-bit
  "view ids" for the position streams, so large ids break position sync.
* The room code is 12 characters (`XXXX-XXXX-XXXX`): the creator's IPv4 address, the port as an offset from the
  base port, and a 24-bit secret. No server is involved. A guest must prove it knows the secret before the creator
  answers with more than a 21-byte cookie; wrong attempts are rate-limited. Traffic is not encrypted.
* The creator must be reachable on UDP port 23711 (or one of the next nine when it is taken). The port asks the
  router for a mapping through Windows' UPnP helper and puts the router's external address in the code; without
  UPnP the code carries the PC's own address, which works on the same network or over a VPN, and otherwise the
  port has to be forwarded by hand and `public_ip=` set. Windows Firewall asks once to allow the game.
  Settings file `port_net.txt` (optional; next to the saves, see "Where the player's files are"): `port=23711`, `upnp=1`, `public_ip=1.2.3.4`, `steam=1`. Guests need the same
  `port=` as the creator.
* Position updates: the game sends each player's position stream (`NetworkStreamDispatcher`: 10 a second in 3
  groups of peers) about 9 times a second to each peer, which suits a Battle with 16 players. In an Adventure room
  (at most 4 players) the port writes the dispatcher's two static values when the room is entered, so every send
  goes to all peers 30 to 40 times a second depending on the frame rate (`set_stream_rate` in `online.c`; measured
  with `BR_NET_LOG=1`, two instances: 35 a second received against 9.2). Battle rooms keep the game's values.
  `adventure_rate=` in `port_net.txt` sets it (10 to 60, default 30; 0 = the game's values). Each player sets only
  its own sending, and the receiver extrapolates from position, velocity and send time, so players with different
  settings can share a room.
* UI (`online_ui.c`): the code is typed straight into the game's own screen, with no pop-up. The game contains a PC
  layout for that screen (`createChoicesModule_Windows`: Search button and a paste button, no "open keyboard"
  button), which the hooks select; the console input module's empty per-frame `MonitorCharacterInput` reads the
  PC keyboard (letters and digits, Backspace, Ctrl+V), and while it runs those keys do not act as controller
  buttons (`rt_text_entry_active`). The six one-character boxes are merged into one text box, the English
  explanation says "12-character", the code counts as complete (Search enabled and pressed by the game) at 12
  characters instead of 6.
* **Rooms over Steam** (`steam_net.c`; `steam=1` in `port_net.txt`, the default). When Steam runs, a created
  room goes over Steam instead of UDP: every datagram of the same protocol (handshake with the secret, reliable
  channel, relaying) is sent to a Steam account with `ISteamNetworkingMessages`, which goes through Valve's relay
  network, so no port has to be open, any router works, and players do not learn each other's IP addresses. The
  code keeps its 12 characters: port index 15 (a UDP host only uses 0..9) marks a Steam room and the address bits
  hold the host's 32-bit account number. Joining follows the code, so a Steam code needs Steam on the guest too.
  Without Steam, or with `steam=0` / `BR_NET_STEAM=0`, rooms are UDP as before.
  * App: Spacewar (480), Valve's shared test app: Steam shows the player as "playing Spacewar", nothing goes to
    any publisher. Nothing else of Steam is used (no lobbies, stats, achievements; the overlay gets loaded into the
    process by Steam but is not used).
  * `steam_api64.dll` is Valve's and not in the repo: looked for next to the exe, else in the player's own Steam
    libraries (registry `SteamPath` + `libraryfolders.vdf`, up to five folders deep in `steamapps\common`), the first
    one with every function used is copied next to the exe. All calls go through the flat C API, loaded at runtime;
    SDK generations before and after 1.58 are both accepted (`SteamAPI_Init` / `SteamAPI_InitFlat`, interface
    versions `SteamUser_v021/v023`, `SteamFriends_v017/v018`, `SteamNetworkingUtils_v003/v004`).
  * One worker thread makes every Steam call (start, manual callback dispatch, send, receive, presence, friend
    list, shut down); the rest only touches queues. Reason: Steam's client reports internal assertions through
    `OutputDebugString`, which stalled for minutes here; when that happened inside the room lock the game froze.
    Steam is started when online play needs it (code entry screen, creating or joining a Steam room) and shut down
    a minute after the last room ended, and on exit from `il2cpp_shutdown` (`sn_shutdown`): a process that exits
    with Steam up hangs in Steam's unload.
  * Friends: the host sets rich presence `brn` = `BRN<protocol>:<code>` (protocol includes the content tag, so
    RumbleSlop and RumbleManiaSlop rooms do not mix). On the code entry screen the port looks for a Steam friend in
    Spacewar with a matching `brn` for 6 s; when found and the box is still empty, the code is filled in and the
    explanation line names the friend. Unlike a typed code, a filled-in code is not searched for at once (the hook
    on `ForceFocusOnDecide` holds back): the player presses Search, or types another code. Only Steam friends can see
    the presence; the code's secret still decides who may join.
  * Test aids: `BR_STEAM_FAKE_FRIEND=name|code` answers the friend lookup as if that friend hosted;
    `BR_NET_STEAM=0` for two instances on one PC (one Steam account cannot reach itself: Steam reports "a message
    session failed"). Seen on this PC: Steam started under app 480, a Steam room created and the lobby reached, a
    datagram to the own account and back (stand-alone probe), the friend line and filled code, Search starting the
    join, clean exit with Steam up, direct rooms unchanged. **Not tested: a join between two Steam accounts**
    (needs a second PC or account), the relay's latency, a friend's presence actually seen.
* Lobby password panel (Battle and Adventure, `restyle_password_panel`): widened from 610 to 800 units with a 440
  wide code field, so the 12-character code has room. The code starts hidden as in the original (R shows it). The
  game's own "copy room name" button, which the Switch build hides, is switched on and moved into the panel between
  the code field and the show/hide (eye) control. Its pill background is hidden and it shows Google's Material
  "content copy" icon (`icons\`, Apache 2.0) tinted dark like the eye. It copies the code on a mouse click or Ctrl+C
  and plays a short shrink-and-spring-back motion (`rt_ui_press_anim`), as does the show/hide (eye) control when
  clicked. Clicks reach both like any other button of the game (the mouse part of `pc_input.c`). `BR_MOUSE_LOG=1`
  logs the mouse device state and dumps the panel's objects.
* In the code entry screen a held Backspace or Delete keeps deleting (after 400 ms, then every 45 ms).
* The chat log's translucent strip (system canvas, top left, 600 units wide) reaches under the left end of the
  wider panel. The panel has a Canvas of its own that sorts one step above the chat log's canvas, switched on only
  while the lobby itself takes input (`panel_layer_tick`): the lobby's overlays (chat wheel editor, How to Play)
  and dialogs sit between the lobby and the chat log and must cover the panel.
* Sprites made by the port carry `HideFlags.DontUnloadUnusedAsset` and are re-made when found dead: the engine
  unloads unreferenced assets when a lobby closes, which left the copy icon a plain dark square in the next lobby.
* Online Play > Adventure and > Battle skip the "friends or random people" screens (random matchmaking does not
  exist here): `RequestStateTransition` is redirected in `playstyle_ui.c` (main menu 9, Adventure choice 36 ->
  friends 40, Battle choice 45 -> friends 47, and Back from the friends screens -> 9). `BR_STATE_LOG=1` logs every
  state request with the class that made it, which is how to find such ids.
* Leaving an online lobby (Leave Room, Disband Room, a lost connection) ends on the main menu, not on the Create
  Room / Join Room screen: the lobby is a scene of its own and returns with `RequestSceneChange` and an argument
  object whose `get_stateAtSceneStart` names the state the menu scene opens in (40 Adventure, 47 Battle). Both
  getters are hooked to answer 9 (`playstyle_ui.c`).

### Online display name (`name_ui.c`)

The name other players see is set in the game: a square button with a name-badge icon in the bottom-left button
stack of the play-style screen and of both lobbies opens the text box "Set your online display name!". L opens it
on the play-style screen and in the Adventure lobby; in the Battle lobby it is mouse-only, because every button the
game can draw a glyph for there (A B X Y L R -/+) is taken. The name is stored in `game\port_name<BR_PROFILE>.txt`
(it wins over `BR_NICKNAME`; default `Player`), at most 16 characters and 32 UTF-8 bytes, without `<` and `>`.

* Button: every screen polls a `UIUserInputDetector`. The hook on `SetIndependentButtonAction` sees each button a
  screen registers; for a known one it copies a square button of that screen (`Object.Instantiate`), swaps the icon
  (or the whole box where the icon is painted into it, as on the Adventure kick button, which also needs a copied
  guide part for the glyph) and registers the copy with the same detector under `eUserInput.CommCustomize`, which
  the game draws and reads as L. The hook on `MonitorInputOnUpdate` takes that input (or a click on the button)
  away from the screen and opens the box. Because the game only polls a detector while its screen takes input,
  dialogs, overlays and fades block the button for free. `BR_NAME_LOG=1` logs every registration (button name,
  input id): use it to find the anchor for another screen.
* Glyphs: the input ids are `Wonder.UI.Choices.eUserInput` in declaration order. Only these glyphs exist for them:
  L (7 8 11 13 32 53), R (12 14), X (19 22 27 28 29 33 55), Y (16 21 23 24 26 35 38 48 54 56), + (15 17 37), A (39),
  B (40). There is no ZR.
* Text box: `GameStateManager.OpenSystemDialog(string)` (static, usable from any screen; returns a handle whose
  `isEnd` flips when it closes). The dialog's text is rewritten as the player types (`UIViewDialog.SetDialogText`;
  the view is caught by a hook while the dialog opens), characters come from the game window's `WM_CHAR` messages
  (window procedure subclassed on first use), Enter or A confirms, Esc cancels by pressing A for the player
  (`rt_input_inject_a`) with the change dropped. That dialog hides the standing Back button and nothing brings it
  back, so the port shows it again 250 ms after the box (`UIViewStandingReturnButton.PlayInAnimAndActivate`).
* In a room the new name goes out through the game's own path: `MatchingPeersInfo.UpdateLocalPeerInfo()` re-reads
  the account name (answered by `rt_player_name`) and marks the peer info for sending. Checked with two instances
  in an Adventure lobby, both directions.

### Editing the game's UI from the port (what was learned doing the online screens)

Finding things
* `python tools\cs.py type <name>` (fields with offsets, method signatures and addresses, enum members with their values), `dis <addr|Type::Method>`
  (annotated disassembly: call targets, classes, strings), `find <regex>`, `callers`, `got <regex>` (the metadata
  pointer slot of a class or method, needed to make objects or pass a MethodInfo from host code).
* Look for a PC path before building anything. The game was written for several platforms and the Switch build
  keeps much of the PC code, merely unused: `CharacterInputModule_Windows` and `createChoicesModule_Windows` (typing
  into the code screen), `eUserInput.ClipboardCopy` / `ClipboardPaste`, a hidden copy button in both lobbies, a
  `WindowsDefault` render pipeline asset, `AppMouse` / `UIMouseInputModule`. A method with no callers is usually
  such a path; a `setupXxxBtn` that only calls `SetActive(false)` is a console override of one.
* To see what a screen is made of, dump its objects: `dump_tree` in `online_ui.c` (`BR_MOUSE_LOG=1`) prints names,
  rectangles, anchors and positions. Children are found by name with `child_named`. UI sizes are in 1920x1080
  units; `Transform.position` of UI objects is in window pixels from the bottom left (overlay canvases), so a host
  hit test against the Windows cursor works.

* A screen is more than one canvas. System views that persist across screens (the chat log `ui_View_ChatLog`,
  dialogs, the busy indicator) hang under a different root from the screen's own view, so walking up from a
  view's transform never reaches them: use `GameObject.Find(name)`. `BR_SCREEN_DUMP=<seconds>` with
  `BR_SCREEN_TREE=<depth>` (ultrawide.c) lists every active UI object with its pixel box, which is how to find out
  what an unexplained shape on screen is (the "broken panel" behind the password was the chat log's header strip).
  The log reaches `port.log` with a delay; read it a little after the dump time.

Changing things
* Before moving or widening a part, check what else occupies that area of the screen in every screen that uses
  the part; widening the password panel put it over the chat log strip and, at first, under the player list.
* Views are plain objects with fields pointing at their parts (`UIPartView...`); a part's text is a TextMeshPro
  object. Moving and resizing is `RectTransform` `anchoredPosition` / `sizeDelta` on the right object: a part's
  root often has a fixed-size child (`ImageBackGround`) that does not follow the root, so resizing the root alone
  changes nothing visible.
* Hide a part with `GameObject.SetActive(false)` rather than shrinking it: sliced backgrounds squeezed below
  their design size draw as broken shapes.
* Text that must fit: `enableWordWrapping = false`, `characterSpacing = 0`, `fontSizeMin` low and
  `enableAutoSizing = true` (`fit_one_line`). Fixed English captions can be replaced after the game sets them
  (hook the setter, run the original, then edit the text).
* New pictures: `Texture2D` + `ImageConversion.LoadImage(png bytes)` + `Sprite.Create`, shown by a new
  `GameObject` with `AddComponent<Image>` parented into the part. Tint with `Graphic.color` (a white sprite takes
  any colour). Keep the sprite alive with a GC handle; never keep a managed pointer in a host static.
* Buttons: an "independent button" is registered with
  `UIChoicesEventCaller_External.SetIndependentButtonEvent(input, button, RegisteredEvent, null, true)`; the
  `RegisteredEvent` is a delegate made from a target object and a `MethodInfo` (`find_method` walks
  `il2cpp_class_get_methods`). The engine's pointer events do not reach the game's buttons in this build (the
  `AppMouse` object is never created); `pc_input.c` calls the buttons' mouse handlers itself.
* Calling managed code with float or struct arguments: floats go in `v0..`, a `Vector2` / `Rect` / `Color` is
  its floats in consecutive `v` registers, set with `rt_setf` and called with `rt_call_regs`; results come back
  the same way. A struct result larger than 16 bytes is written through `x8`.
* Completion rules live in several places. The code screen counted six characters in two observers and pressed
  Search by itself; changing the length meant gating `SetEnableSearchBtn` and `ForceFocusOnDecide`, not just the
  input.
* While a text box reads the keyboard, the keyboard-to-controller mapping in `imports_hid.c` must stand down
  (`rt_text_entry_active`), or typed letters also press buttons.

Testing
* Two instances on one PC: `BR_PROFILE`, `BR_LOG`, `-logFile`, `BR_NICKNAME`; drive them with `BR_INPUT_LIVE`
  (`A`, `2:LSU 1500`, `TYPE ABC`). The first-time tutorial pop-ups stop appearing once the save has seen them, so
  scripted navigation must look at the screen instead of assuming them.

### Quit, pointer, icon, error message (`pc_shell.c`, `window.c`, `port\launcher`)

* **Quit.** The game has no way out. Backing out of the play-style screen (B, Esc, the Back button:
  `UIStateTitle.onCancel_SelectPlayStyle`, which used to return to the "press any button" logo) opens the game's
  Yes/No dialog with "Quit the game?", focus on No; Yes calls the engine's `Application.Quit`. The game's code
  never names that function, so it is taken from the engine's internal-call table (`rt_icall_host(name)` gives the
  engine's Win64 function of any internal call, also `Cursor.set_visible` below). `BR_NO_QUIT_DIALOG=1` keeps
  the old behaviour. Seen: No stays, Yes ends the process.
* **Pointer.** Hidden while the pad or the keyboard gave the last input, shown when the mouse moves
  (`pc_mouse_mode`). Seen in the log only ("cursor: hidden"); not judged by eye.
* **Pads and focus.** A controller is not read while another program is in front (`poll` in `imports_hid.c`;
  the keyboard always behaved so). Scripted input is not affected.
* **Icon and version.** `port\launcher\build_launcher.py` builds the exe with `launcher.rc`: icon resource 103
  (`build\obj\icon.ico`, made by `make_icon.py` at build time from the title-screen texture `paths.ICON_TEXTURE`
  of the user's dump, `ui_view_titleloop_title.prefab.bundle`; Aiai here, Gongon in the fork; nothing of it is in
  the repository) and a version block with the game version.
  `window.c` gives the window the same icon. `build_launcher.py --copy` replaces the exe in the game folder.
* **Errors.** A FATAL shows a message box that names `port.log`, unless the run is scripted
  (`BR_NO_REAL_INPUT`) or `BR_FATAL_BOX=0`. Not triggered since the change (no fault at hand).
* **Screenshot.** F12 saves the picture on screen as `screenshots\<date> <time>.png` in the player's files
  (`rt_user_dir()`; `screenshot_tick` in `pc_shell.c`). A console has a Capture button for this and the game
  saves nothing itself: its photo mode only moves the camera and hides the HUD
  (`MgPhotoModeCamera.writeScreenshot` reads the screen into a byte array and drops it; nothing requests it).
  The file is written by the engine (`ScreenCapture.CaptureScreenshot` from the internal-call table, at the
  end of the frame, in the render size). The key is fixed and is not taken away from Key Configuration: an
  action bound to F12 fires too. Seen with a scripted key (`KEY F12 100`): the main menu and a stage, 1920 x
  1080 PNGs, two in a row. Not tried: a real key press, photo mode itself, full screen, split screen.

### The port's version (`PORT_LABEL` in `port\paths.py`, shown by `pc_shell.c`)

The title's version label reads the game's version, a space and the port's own version: "Ver. 2.20 RS 0.1". The
game writes "Ver. <game version>" (a string literal in `UIStateTitle`'s start-up, through a StringBuilder into
`UIViewTitle.m_VersionText`); the hook on `TMPro.TMP_Text::set_text` (`3e31d50`) adds the port's part to a text
that is exactly "Ver. " and digits and dots. RumbleSlop is "RS", RumbleManiaSlop "RMS".

**Every major release needs a version number agreed for it, and `PORT_LABEL` in `port\paths.py` changed to it
before the release is built** (both repos keep their own prefix; `build_runtime.py` passes it in as a define and
recompiles the runtime when `paths.py` changes). The number started at 0.1 on 2026-11-15 and has not been raised
since.

### Language (`imports_plat.c`, `port_text.c`)

The game has its own Language page in Options (eleven languages; changing returns to the title) and it works in
the port as it is. A new save starts in the language `nn::oe::GetDesiredLanguage` reports: now the Windows display
language (`GetUserDefaultUILanguage`, mapped to the console's codes), or `BR_LANG`. The port's own texts (the added
option rows and their values, "Quit the game?", the tutorial question, the name box) go through `rt_tr_str` /
`rt_tr_utf16`: a table of the English text and nine translations, chosen by `LocalizationManager.language`.
A text without an entry stays English, so a new text needs a table row. The translations were written with the
port, not by native speakers. Seen in Japanese: the quit question and the Display rows, every character drawn
(the game's fonts had the kanji). Not looked at: the other languages on screen (Chinese and Korean could miss
characters in the game's fonts), a Windows in another language, the row help texts (still none), the game's own
untranslated PC words ("Windowed", "MIDDLE" in Japanese).

### Where the player's files are (`rt_user_dir`, `log.c`)

Save data (`save\`, `temp\`) and the port's settings files (`port_display.txt`, `port_keys.txt`, `port_name.txt`,
`port_net.txt`) are in `rt_user_dir()`: `%USERPROFILE%\Saved Games\<Name>\` for a game folder that has no save yet.
The game folder itself is used ("portable") when a `save\` folder or a file `portable.txt` is next to the exe, when
`BR_PROFILE` names a test profile, or with `BR_PORTABLE=1`; `BR_PORTABLE=0` forces the profile folder. So a game
folder made before this existed keeps its saves where they are, and an empty file `portable.txt` put next to the
exe before the first start makes a new one portable. The log line "user files: ..." tells which. Logs, add-on
content and everything the setup made stay in the game folder. Seen: `BR_PORTABLE=0` starts a new save under
Saved Games; an existing game folder logs "portable". Not tried: a game folder that really has no save (only
the forced mode), a profile path with characters outside the system code page.

### Play-style screen (`playstyle_ui.c`)

"Select Your Playstyle!" has no Nintendo eShop button and no Local Communication card on PC. The cards come from
three parallel lists (parent items, child item lists, card views) handed to
`UIChoicesGroupModule_TwoDimension_Param..ctor`, which only the title state calls: the hook removes the Local
Communication entry from all three before the module is built, hides its card and moves the other three right by
240 units so the middle card sits on the panel's centre. `SetPlayStyleSelectMain` is followed by switching the
eShop button off, and `onCallEShop` (the -/+ press) does nothing. The X sub-menu (tutorial, notices) is unchanged.
The name button (next section but one, `name_ui.c`) sits above the X button.
A hook that makes guest calls and then returns 0 must restore the argument registers and `x30` first.

### First start: skip the tutorial (`tutorial_skip.c`)

On a save that has not played the tutorial, the title (after "Press any button", the auto-save notice and "Creating
save data") used to go straight into the opening movie and the tutorial. It now asks first, in the game's own Yes/No
dialog: "Would you like to skip the intro tutorial?" (focus starts on Yes; B answers No).

* The title's last start-up state (`UIStateTitle`, `Title_ClearCache`) checks `SaveUserParam.AdventureParam.m_playedTutorial`
  and calls `Util.Movie.RequestMgMovieToTutorial` every frame while it is unset. The hook on that function holds the
  request back, opens the dialog (`UIStateManager.RequestInterruptState(0x3b, ..., UIStateArgYesNoDialog)`) and reads
  the answer from the returned `UIStateHandle` (`status` 3 = finished, `result` 2 = Yes, 3 = No): no callbacks needed.
  That is the pattern for any Yes/No question from host code.
* Yes sets the flag, which is all the tutorial's own ending does (`MgTutorial.returnToMainMenu`), and calls
  `SaveDataContainer.UserParam.Save()`: the game does not write the save again before the main menu, so without it
  quitting on the play-style screen brought the question back. The title then takes its normal branch to the
  play-style screen. No lets the request through (movie, then tutorial).
* The tutorial stays available on the play-style screen (X sub-menu), where the flag is already set.
* Test: a new `BR_PROFILE` suffix is a first start. Checked: Yes (play-style screen, not asked again after a
  restart), No (opening movie starts). Not checked: playing the tutorial to its end after No.

### New save: no update notices (`tutorial_skip.c`)

Update 2.20 brings announcements for the main menu ("You can now play new Stages during Race in Battle Mode!", the
end of a collaboration, ...): rows of `DB.UpdateInfo.Kind`, each with a per-save flag
(`SaveUserParam.UpdateInfoParam`, `UpdateInfoParamItem.m_IsPlayed`). `NoticeManager.Start` calls
`Util.Unlock.CreateUpdateInfo` once the save is loaded (before the title), which queues a notice for every row not
yet flagged. A save made on 2.20 has no flag set, so a new player was told about "new" content first thing.

The hook on `CreateUpdateInfo` looks at `AdventureParam.m_playedTutorial`: unset means a new save, and then every
row is flagged and nothing is queued (log line "new save: 7 of 7 update notices marked as shown"). The flags reach
the file with the next save (the Yes of the tutorial question, or the tutorial's end). A save that has played the
tutorial goes through unchanged, so one from before the update still gets its notices once.

Checked in both games on a new `BR_PROFILE`: no notice on the main menu (the unhooked game shows the Race stages
one there), none after a restart. Not checked: answering No and playing the tutorial, a save from 2.00 (unchanged
path), the notices list in the X sub-menu.

### Keyboard, mouse and button glyphs (`pc_input.c`, `pc_glyphs.c`)

The game is written for several platforms around "actions" (`Wonder.AppInput.State.Action*`,
`ActionAnalogValue`; ids in `Wonder.DB.Input.Action.eID`): an action is bound to a pad button and, in the PC code,
to keys and mouse buttons. The Switch build keeps the data and compiles the reading out. The port puts it back, so
the keyboard and mouse behave as in a PC release, with the game's own default layout.

* **Default bindings are the game's.** `AppInput`'s class constructor still calls
  `ActionAssignKeyboard.KeyDef / KeyArrayDef / AnalogKeyDef` and `ActionAssignMouse.ButtonDef` with the PC keys
  (they return a shared "unused" object on Switch); entry hooks collect the arguments and the following
  `ActionAssignDatum` constructor tells the action. `BR_INPUT_DUMP=1` logs the table (W/A/S/D tilt, arrows camera,
  Space spin dash, Enter confirm, Esc cancel and pause, Q/E tabs, right mouse button cancel, ...).
* **Reading.** `State.checkAction` (behind Action, ActionDown, ActionUp, ActionRepeat and their FixedUpdate
  twins: the variant is told by which of the state's eight delegates is passed) and `State.ActionAnalogValue` ask
  the pad first and then the keys and mouse buttons of the action, behind the same gates as the pad (valid state,
  active input layer, device mask, the chat wheel's blocked actions). Keys are sampled in `AppInput.Update` and
  `FixedUpdate`; a key is a `UnityEngine.InputSystem.Key` (physical position, read through its scan code, so WASD
  stays put on any layout); analog keys ramp over the spans the game gives (128 ms for tilt). "Press any button"
  (Title_Deside, Movie_Skip_Active) takes any key. The keyboard is player 1's. While a text box has the keyboard
  only Enter, Esc and the arrows act. Esc is both Cancel and Pause in the game's layout and the UI detector lets
  Pause win, so a Pause that came from the Cancel key is reported as Cancel.
* **Rebinding** is the game's Options > Key Configuration. Its edit state (`UIStateOptionKeyConfigEdit.exec`) waits
  for a pad button; a pressed key is handed down the same road (`onPushButton`), and the row's item
  (`UIOptionChoicesItemData_KeyConfig`, `+0x44` its action) binds the key instead of reading a pad button. Esc
  closes the dialog unchanged; a key another action of the same group had is swapped; Options > Reset resets the
  keys too (`ResetValue`). Changed bindings are in `port_keys<BR_PROFILE>.txt` (`<action id> <key name>...`).
* **Mouse on the UI.** Every button already has a mouse handler (`UIButtonMouseAction`: its screen's
  `UIUserInputDetector`, the input it stands for, click and hover callbacks). `SetMouseAction` is hooked to keep a
  list of buttons; when a screen polls its detector (`MonitorInputOnUpdate`), a pending left click or a moved
  cursor is hit-tested against that detector's buttons and given to the handler (click: `OnClick`; hover:
  `OnPointerEnter`, which moves the focus), and a click on the standing Back button is reported as Cancel (to every detector of that button polled in the
  same frame, as Esc is: the Style screen polls two and leaves on the second one's Cancel). A
  screen that is not polling (dialog on top, fade) is therefore not clickable. This covers menu entries, cards,
  list rows, option arrows, dialog buttons and the corner buttons (eye, copy, name, Change Character, ...).
* **Mouse drag as a stick.** The game's default table also binds actions to the mouse as a "virtual pad"
  (`ActionAssignMouse.VirtualPadDef(button, direction, sensitivity)`; the component that would read them,
  `AppActionMouse`, does not run in this build). The port captures the definitions like the keys and does what
  `ActionParamVirtualPad.Process` does: while the binding's button is held, the place where it went down is the
  centre; after 10 pixels the drag counts, and the action's value is the part of the drag in its direction, full
  at `sensitivity` pixels (of a 1080-line window). The game's layout: left button drag tilts the stage (actions
  46 to 49, 200 pixels) and moves the cameras of the viewing modes, right button drag moves in menus (32 to 35).
  The main-game camera has no mouse binding in the game's table (arrow keys). `BR_INPUT_DUMP=1` lists them.
  Seen: a scripted drag (`HOLD 960 540`, `MOVE 960 300`) rolls the ball forward into the goal of the first
  stage. Not done: the two `GestureDef` bindings (actions 61, 62), a real mouse in the hand.
* **Mouse Sensitivity.** The game has the option (`OptionParam.dataMouseSensitivity`, 5 in every save), its text
  in eleven languages and the arithmetic (`AppMouse.SetSensitivityReviseParamByInt`,
  `ActionAssignMouse.CalcRevisedSensitivity`), but this build shows no row for it and calls neither. The port
  adds the row as the last one of Options > Controls (`ROLE_MOUSE` in `display_opt.c`, a copy of the Screen
  Mode row like the other added rows; the page is told by its dead zone rows), 1 to 10, kept as `mouse=` in
  `port_display.txt`, and uses the game's arithmetic on the drag length (`pc_mouse_sensitivity` in
  `pc_input.c`): 5 = the binding's length, 1 = twice as long in four steps, 10 = a tenth in five steps. The
  label is the game's own text. `BR_MOUSE_SENS=<n>` forces a value. Measured with the same scripted drag in a
  stage: 40 pixels do nothing at 1 and 5 and roll the ball into the goal at 10; 100 pixels give 0 mph at 1 and
  5 mph at 5 after the same time. Seen: the row in both games, changing it, the value in the file.
  Not tried: a real mouse.
* **Reset and the port's rows.** The Reset button of an Options page asks "Return all options to their default
  settings?"; on Yes, `UIStateOption.activate` (which runs again when the dialog closes) calls `ResetValue` on
  every row of the open page's own list (`m_OptionCategoryDict`). The port's rows are copies that exist only in
  the list on screen, so the game never reached them: Frame Rate Limit, VSync and the others stayed as
  they were. `rt_hook_4564c70` (`display_opt.c`) sees the confirmed dialog (`m_StateHandleInit.result` 2) at the
  start of `activate`, puts the settings of the open page's added rows back (page = `m_TabSM`'s state: 0
  General, 2 Controls, 3 Audio, 4 Display) and gives the rows on screen their picker positions (their GC
  handles are kept per page build). Seen: Display (Frame Rate Limit 120 and the then Smooth Motion row Off return to
  60 and On, and both step correctly afterwards), Controls (Mouse Sensitivity), and in the fork General (Dole
  Collaboration, Spin Dash) and Audio (Imported Stage Music). Not tried: Reset from the pause menu's Options,
  Resolution changed before a Reset, a Steam Deck.
* **Lists: wheel, scroll bar, hover.** Every list is a `UnityEngine.UI.ScrollRect` whose content the game moves
  towards the focused item, in three functions (`UIPartViewBaseVirtualScrollRect.Update`,
  `UICalcContentPosModule_Base.OnUpdate`, `UIPartView_ScrollRect_ChoicesOp_Duplicatoin_Vertical.Update`). The
  game's PC code leaves them out while `AppInput.latestInputDeviceMask` is Mouse (4), which nothing sets in this
  build; the port decides itself (`mouse_mode`: the cursor moved, a click or the wheel came after the last key
  and pad input) and skips the three functions, keeping their target equal to where the list is. So hovering a
  row at the edge of a list focuses it without scrolling, and the list follows the focus again with the first key
  or pad press. `ScrollRect.LateUpdate` is hooked to know the lists on screen (GC handles). The wheel scrolls the
  list under the cursor, or the only one, through `verticalNormalizedPosition` (`WHEEL_PART` of the visible
  height a notch, eased; horizontal lists sideways); a press on the scroll bar or within 14 px beside it drags
  the handle one to one, a press beside the handle brings it under the cursor. After a scroll the hover test runs
  again, so the row under the cursor has the focus. The part of a row cut off by the list's edge is not
  clickable (`list_hides`). On a screen without a scrolling list a notch is still a tap of the Up / Down arrow
  key. A list whose rows are buttons of screens that are not polling input takes neither the bar nor the wheel:
  Select World's list stays alive at the same place under Select Stage and used to take the press on Select
  Stage's bar (`list_dead`, the detectors seen in `MonitorInputOnUpdate` during the last two frames; a list
  without known buttons counts as alive). While a stage is played (`get_mainGameState` 4) only lists whose rows
  belong to a screen taking input are used: the pause menu's Options pages take the bar and the wheel, the game
  itself never does, and a wheel notch is not turned into an arrow-key tap there (it would tilt the stage).
  `BR_DRAG_LOG=1` logs each wheel use and bar press (with every known list's state), `BR_CLICK_LOG=1` a
  click on a Back button. Not checked since that rule: a list behind a dialog (it used to take the wheel).
* **Glyphs.** The game draws glyphs per controller kind (`Wonder.DB.Input.ControllerKind`: Switch kinds, PS4 8,
  XboxOne 9, Steam 10, PS5 11) and redraws them when a player's kind changes
  (`UIInputDeviceSwitchingDetector`); the build only has Switch pictures (64 px sprites). The port sets the kind
  of each pad state just before it is copied to the states the game reads (`State.CopyToActiveLayer`): XboxOne for
  an XInput pad, PS5 / PS4 for a DualSense / DualShock 4, Steam standing for keyboard and mouse; player 1 gets the
  kind of whatever gave input last. `UIIconSpriteManager_ControllerBtn.GetSprite(ButtonIcon, kind)`, the one
  source of pad glyph sprites, returns a cell of `icons\glyphs.png` for those kinds. For the keyboard the glyph
  is the action's key, which GetSprite cannot know: the callers that know the action or the guide's fixed keyboard
  icon (`UIBtnGuideDisplayModule_ForInputAction / _ForDB`, the fixed guides, the Key Configuration rows) are
  hooked to leave it in a context; without one the button's most common action is used. Face buttons follow the
  pad mapping of `imports_hid.c` (by name, or by position with `BR_PAD_LAYOUT=positional`).
* **Glyphs in text** are TextMeshPro sprites named by the DB per kind; the names of the other kinds exist
  (`ps5_button_...`) but not their pictures, which printed as raw `<sprite ...>` text. The port clones the game's
  sprite asset (material and metrics), replaces its tables by one sprite per atlas cell (`br_<cell>`), adds the
  clone as a fallback of the sprite assets texts use, and `UITextEmojiDisplayModule.getbuttonIconSpriteTag` /
  `getEmojiTag` answer with those names. A text inside a button (the kick button's "-/+") names pad buttons; for
  the keyboard `SetupButtonGuide` records which action the button stands for and `updateText` writes that
  action's key.
* **The atlas** is made by `python port\tools\make_glyphs.py <Kenney "Input Prompts" folder>` (CC0): the pack's
  filled icons rebuilt as black shape, white outline, white lettering; 16 x 14 cells of 96 px; it is linked into
  the DLL by `glyph_data.S`. Cells: `family * 32 + ButtonIcon.eID` (Xbox, PS5, PS4), `96 + Key`, mouse and the
  WASD / arrows groups from 208. The two groups are drawn by the script (`key_cluster`): four keycaps apart
  from each other with the pack's lettering made large (shrunk pack keys side by side overlapped).
* **Tests without devices**: `BR_INPUT_LIVE` lines `KEY <Key name> [ms]`, `CLICK x y`, `MOVE x y`; `BR_KIND=<n>`
  forces a controller kind for the glyphs; `BR_GLYPH_LOG=1` logs the size of the game's own glyph sprites.
  Checked this way: menus and a stage by keys only, hover and click, Back button, rebinding and reset, the three
  pad glyph sets and live switching between a (scripted) pad and the keyboard with a DualSense connected. Not
  checked: real key presses, real mouse, real pad buttons.

### Add-on content (DLC)

The DLC packs are unlock keys: the content is already in the game, and the game unlocks a pack when
`nn::aoc::ListAddOnContent` reports its index. The runtime reports every numbered folder in `game\dlc\` (decimal
add-on content index = pack title id minus `0x010031F019295000`) and `nn::fs::MountAddOnContent` mounts that folder.
The setup's `dlc` step builds `game\dlc\` from every add-on content `.nsp` in `input\` (the title id of the
pack's data archive gives the index, its RomFS is copied; only three packs have a file, `dlc_msg.txt`).
To add a pack later: put its `.nsp` into `input\` and run `setup.cmd --only dlc`.

The game knows 34 packs (indices 1-30 and 34-37). On the first start after packs appear the game shows one "now available"
dialog per pack.

### Characters that can no longer be bought or earned (`delisted.c`, `port\assets\delisted.py`)

Six characters are in every copy of the game's data but out of a player's reach today: Dr. Bad-Boon
(`Def.Chara.eID` 12, `Madhihi`; unlocked by one online battle on Nintendo's service) and the five collaboration
characters (19..23, `Collab01..05`; their add-on content was taken off sale). The port gives them to every save,
from the start, without unlock notices or "new" marks. Two halves:

* **Runtime** (`delisted.c`). Whether a save has a character is one value: `SaveUserParam.ShopItemParam.m_Character`,
  a dictionary `Def.Chara.eID -> ShopItemParamCharacterItem`, whose condition (`Shop.Item.Condition.eID`) is set to
  Have (5). Hooks on `ShopItemParam.OnCreate` / `OnAfterLoad` (`43fa930`, `43faae0`) set it for the six when the
  save's shop data is made or loaded, and hooks on both `ShopItemParamCharacter.GetItem` overloads (`43fbd10`,
  `4400890`) set it again on every look-up, so nothing in between can leave them locked. A character counts only
  when its model folder is in the game data (`chara_model_assets_chara\model\<id>*`); otherwise a save that says
  "owned" is set back to not owned (it would be an empty slot). On a new save `OnCreate` runs before the dictionary
  exists, so the work is done by the `OnAfterLoad` that follows. Log line at the first look-up:
  `delisted: 6 of 6 characters have model data and are owned in every save` (2.20 has all six; 2.00 had three).
* **Data** (`delisted.py`, run by the setup's game-folder step, `setup\assemble.py`). Sets `_empty` = 1 on the six
  characters' rows of the table `Unlock_Subject_Chara` (bundle `db_assets_db\unlock_subject_chara.asset.bundle`),
  so the game's own unlock rules never fire a notice for a character the player already has. The bundle file is
  replaced, not written through (it may be a hard link into the build tree).

The paid add-on characters (`Def.Chara.eID` 13..18) are not touched: they come from the player's own add-on
content files (see above). That these six only need their unlock rule changed was found by iswimfly's mod
"Delisted Character Fix"; the port takes a different route and uses nothing of it.

### Style menu for the add-on characters (`chara_style.c`)

Whether a character has a Style entry is the `IsCustomize` flag of its row in the character table `Def_Chara`,
read only through `Wonder.DB.Def.Chara.Record::IsCustomize` (`47cdba0`). The save makes a
`CharaCustomizeCharacter` per character with the flag (`SaveUserParam.CharaCustomizeParam.GetCharacterParam`,
on demand, so old saves get it too), and the character select screen shows the Style button only when that entry
exists. The table has the flag off for the SEGA characters and the five collaboration characters
(`Def.Chara.eID` 13..23), so they had no Style button. The hook answers yes for them. `IsCustomizeFull` stays as
the table has it (off for them, as for Dr. Bad-Boon): they get his reduced Style menu, ball and ball effect, with the
outfit tabs greyed out. `BR_STYLE_LOG=1` logs each character's two table flags once (the five collaboration
characters: both 0).

### Entry hooks

`port\recomp\hooks.txt` lists NSO offsets. The generator makes each listed function start with
`if (rt_hook_<offset>(c)) return;`. The hook is a host function in `port\runtime`; it returns 1 when it handled
the call (result in `c->x[0]`) and 0 to let the lifted body run, and it may change argument registers before
returning 0. To run the original from inside a hook, set a thread-local bypass flag and `rt_dispatch` to the
function's address. Managed methods are called from host code with `rt_call(c, address, n, args...)`.

### Display options (`display_opt.c`)

The game's own Options > Display page is extended with Screen Mode (Windowed / Borderless / Full Screen),
Resolution, Frame Rate Limit and VSync. The added rows are copies of the game's Screen Mode row object,
inserted after it when the page's row list is built; each copy carries a role number in a padding word and the
hooks answer by role. Settings are stored in `port_display.txt`. The game pins `targetFrameRate` to 60 and
resets `vSyncCount` with every quality change, so both setters are hooked to keep the port's values. Game time
and the 60 Hz physics step were measured to be independent of frame rate.

Frame Rate Limit with VSync On (`apply_rate`): Unity ignores `targetFrameRate` while `vSyncCount` > 0, so until
2026-11-15 the limit did nothing with VSync on (measured: limit 144 on a 240 Hz display ran at 239 fps). Now VSync
is used only when the limit is Unlimited or at least the refresh rate; a lower limit runs the engine's frame
limiter with VSync off (a display without variable refresh may tear then). `vSyncCount` 2 (every second refresh)
is not usable: the game reads it back and switches its logic to 30 Hz (measured: 120 on 240 Hz gave 30 physics
steps a second). Log line `display: frame rate limit ..., display ... Hz -> vSyncCount ..., targetFrameRate ...`.
Seen: the limit changed in Adventure's pause menu (Help Menu > Options > Display) applies at once (60 -> 144
measured in play after resuming, ball moving in every frame); the same from Battle's pause menu (Options).
The refresh rate is the one of the monitor the window is on (`refresh_hz`, read again every 2 s), and `rate_tick`
chooses again once a frame when it changes (window dragged to another monitor). VSync does not always follow the
window's monitor: on a 60 Hz monitor next to the 240 Hz main display a window ran at 240 frames a second with
VSync. So when VSync stands for a limit and the game runs more than 15 % faster than the limit for 2 s, the frame
limiter takes over ("display: ... the frame limiter takes over") until the window is on a display with another
rate. Measured: limit 60, window dragged 240 Hz -> 60 Hz -> 240 Hz: 60 frames a second on all three after 2 s.

The Resolution row lists the 16:9 sizes that fit the desktop, on a 16:10 desktop the 16:10 sizes too, and the
desktop size itself.

Steam Deck. The port recognises a Deck at start-up (`is_steam_deck`): Steam's own `SteamDeck=1` variable, which
Proton hands to the game, or, with Windows installed on the Deck, the firmware's machine name (Valve, Jupiter or
Galileo). On a Deck the game starts borderless at the display's own size (1280x800 on the built-in screen) whatever
the game's save says about windowed or full screen, and the settings are kept in `port_display_deck.txt`, so a
folder shared with a desktop PC keeps both machines' choices. Changes made in Options are kept as usual; Reset does
not change the screen mode there. When the Display page is built the game's saved windowed / full screen value is
made to agree with the port's mode, so the Screen Mode row's own text is right.

Dole Collaboration (Options > General, after Show Tips; Off by default, `dole=` in `port_display.txt`). Game 2.20 ended
the collaboration: its look (the Dole logo under the start of a stage, bananas with the Dole sticker, the Dole
deck chairs and signs of the main menu) is shown only while a selected character wears a Dole item. The game keeps
that in one flag, `RuntimeParam.mainGame.isDoleCollabActive`, set from
`Util.CharaCustomize.IsExistDoleItemSelectedChara` and `IsExistDoleItemCoordinate` whenever characters or outfits
change. Both are hooked and answer with the row: On shows the look whatever is worn, Off never. A change also
writes the flag, so the main menu and the next stage follow at once. Changed during a stage (pause menu >
Options), the stage follows on the next frame (`dole_tick`): the logo through the game's own
`MainGameStage.SetupDole` (shows or hides `m_DoleGo`, loads and places it under the start if the stage has none
yet), the bananas by `MgBananaManager.Load` with the new model and then, per banana, its one visual child
(`m_RotTransform`) replaced by a copy of the new prefab's, with `m_Renderers` / `m_Materials` rebuilt as
`MgBanana.Awake` does; the old prefabs are released afterwards. The game's own way (the character change mid-stage)
destroys and creates the bananas, which would put back the ones already taken; this keeps each banana's state.
Log lines `bananas: ...`. The row is one more copy of the Screen Mode
row object, appended to the page that has the Show Tips row. `BR_OPT_LOG=1` logs the row classes of every Options
page as it is built.

Bananas (Options > General, under Dole Collaboration; Auto by default, `bananas=` in `port_display.txt` = the
model id, 0 Auto). Which model a stage's collectibles use. The game decides in
`MgBananaManager.getCurrentBananaModelId`: each player's character has a banana model (`Def.Chara` BananaModel;
`Banana.Model.eID` 1 banana, 3 rings for the Sonic characters, 5 spray cans for Beat, 6 coins for Axel, 7 notes for
Miku, 8 trains for Godzilla, 9 pizza for the Crewmate, 11 feathers for Red, 12 Pac-Dots for PAC-MAN); when all players
agree that model, when they differ the plain banana; the plain banana becomes the Dole one (4) while the Dole look is
on. `MgBananaManager.load` loads that model's three prefabs (single, bunch, golden), pick-up sound and effects. The
hook on `getCurrentBananaModelId` keeps the game's answer for Auto, gives the plain (or Dole) banana for Bananas and
the chosen model for the others; the Battle trick item (model 2, Jewel, while `MainGameParam` +0x40 is set) is left
alone. A hook on `load` notes the model last loaded; a change in a stage reloads and swaps the bananas in place as
for Dole Collaboration above (`dole_tick` compares the wanted model with the loaded one). All models are in the
game's data, add-on packs or not.

When adding more rows or options, read the file's header comment first: it lists the object layouts the hooks
rely on.

**Rule for every option the port adds** (a row in Options, the pause menu's Options or a multiplayer settings
screen):

* **Reset puts it back.** The page's Reset button ("Return all options to their default settings?", Yes) must
  set the option to its default, save that, and show the default in the row. The port's rows take part through
  the `ResetValue` hook and the Reset pass in `UIStateOption.activate` in `display_opt.c`: a new row is added
  there, not left out.
* **A change shows at once, also in a stage.** Changed from the pause menu during play, the running game must
  follow on return, without a restart or the next stage: whatever the option changes on screen or in play is
  updated in place (example: Dole Collaboration's `dole_tick` above). Writing a flag that only the next load
  reads is not enough.
* **It has a caption.** The line under the Options list explains the row in focus. The game looks the row's
  option up in `Option_OptionCaptionMessage`; the port's rows are copies of the Screen Mode row, whose caption is
  blank (so are the captions of the game's own PC rows: Screen Mode, VSync, Mouse Sensitivity and the five graphics
  rows). The hook on `UIOptionScrollModule.onUpdateFocusIndex` (`4560720`, `display_opt.c`, "captions") sets the
  port's text after the game's: by role for a Screen Mode row (`ROLE_CAPTION`), by class name for the game's rows
  (`CLASS_CAPTION`). A new row gets a line there and its translations in `port_text.c`, in the game's style ("Set
  ...!", "Toggle ... ON or OFF!", at most two lines of about 60 characters: the box is full at that width).

Test both for each new option: Reset on its page, and a change from the pause menu in a stage; and look at its
caption.

Quality levels. The game names its quality level `<platform><pattern>` and still says "Switch", so it selects the
console's levels: 1 `SwitchDefault` in single-player stages, 7 / 8 in the menus, and cut-down ones elsewhere:
2 (online Adventure), 3 (two-player split screen), 4 (three or four players), 5 (Battle), 6 (split-screen Battle).
Those have render scale 0.85 or 1.0 instead of 1.2, no MSAA, a 512 shadow map, half the shadow distance or no
shadows, a simpler renderer, and levels 4 to 6 carry vSyncCount 2, which `GraphicsManager.setQualityCore` turns
into 30 fps with the 30 Hz logic mode. The hook on `QualitySettings.SetQualityLevel` replaces levels 2 to 6 by
level 1 and puts the port's VSync value back, so every mode looks and runs like the main game (the levels'
renderer lists have the same order). `BR_KEEP_30=1` switches the hook off for comparison.

The game moves everything on its 60 Hz physics step: with `BR_FPS_LOG=1` the log shows how many rendered frames a
camera moved in. Measured at about 240 rendered frames per second: 60 camera moves per second in a single-player
stage, an online Battle with 16 players and split screen alike. Without the next section a frame rate above 60
therefore draws repeated pictures.

### The game's graphics rows (`gfx_opt.c`)

Version 2.20 lists Graphics Settings, Shadow Quality, Post-Processing Effects (Low / Medium / High), Anti-Aliasing,
Texture Smoothness (On / Off) and HUD Size (Standard / Large) on the Display page and saves them, but nothing in
this build acts on them: `OptionElementSaveData_Quality.changeValue`, `_OnOff.changeValue` and
`_HudScale.changeValue` are a bare `ret`, and the getters of the saved values have no caller. The port applies
them. The saved values are read every frame (`SaveUserParam.m_OptionParam` +0x30; elements at +0x40, +0xc8, +0xd0,
+0xd8, +0xe0, +0xe8, value at +0x10) and applied when one changes or the game has switched its quality level:

| Row | Low / Off | Medium / On | High |
|---|---|---|---|
| Graphics Settings | render scale 0.85, LOD bias x0.7 | the level's own (1.2 in a stage, 1.1 in the menus) | 1.5, LOD bias x2 |
| Shadow Quality | 512 shadow map | the level's own (1024 in a stage, 2048 in the menus) | 4096 |
| Post-Processing Effects | off | the game's | the game's plus SMAA |
| Anti-Aliasing | MSAA off | MSAA 4x (the console level has 2x) | |
| Texture Smoothness | anisotropic filtering off | forced on | |
| HUD Size | | Standard | Large: 1.2 times, by the HUD canvases' scaler reference 1600x900 (`ultrawide.c`) |

Render scale, shadow map and MSAA are set on the level's `UniversalRenderPipelineAsset` (what each asset had is
kept by asset); LOD bias and filtering through `QualitySettings`, whose setters change the active level's stored
values, so a level's own LOD bias is read the first time the level is seen. Post-processing is per camera: Low
turns it off for cameras that have it on and remembers them. `BR_GFX_LOG=1` logs what is applied,
`BR_GFX_FORCE=<graphics>,<shadow>,<post>,<aa>,<aniso>[,<hud>]` overrides the save for a test, `BR_GFX_OFF=1`
leaves the rows dead as before. What the publisher's own PC version does with these rows is not known; the
meanings above are the port's.

Seen in the first Adventure stage with forced values: all Low (flat picture without bloom and haze), all High
(no missing-shader colours with SMAA), HUD Large; 240 frames a second in each. Not done: changing the rows by
hand in the menu and watching the preview, split screen with HUD Large, Battle, a frame rate comparison on a
slow machine, a close look at edges and shadows (the pictures were 960 wide).

### Smooth motion above 60 frames a second (`interp.c`)

Every rendered frame shows the moment it belongs to: a transform the game wrote during a fixed step is drawn
between its value before that step and its value after it, by how far the frame's time lies between the two
steps (what Unity's Rigidbody interpolation does, here for everything the game moves by hand: ball, camera,
stage parts, other players, items). The picture is up to one step (16.7 ms) behind the simulation; the simulation
itself is untouched, so physics, replays and online play are as before.

* **Where.** The engine's Transform internal calls are wrapped when the engine registers them
  (`rt_interp_wrap`, called from `il2cpp_add_internal_call`). Unity 2022.3 `_Injected` calls take the managed
  object and struct pointers; the wrappers are plain Win64 functions with the same arguments.
* **Phases** are told by `Time.inFixedTimeStep`. A setter during a fixed step notes the transform and the part
  written (position, rotation, scale) with the value before the step. The first Transform call after the fixed
  steps of a frame reads the value after the step and writes the blend, so Update / LateUpdate scripts and the
  renderer see one consistent pose. The first Transform call of the next fixed step writes the true values back
  before game logic reads anything. Local values are blended (a moved child of a moved parent comes out right).
* **Ownership.** A part that a script writes outside the fixed step belongs to that script until a fixed step
  writes it again: it is neither blended nor written back (`BR_INTERP_LOG=1` names such transforms).
* **Jumps.** More than 6 units in one step (restart, wormhole, camera cut) is shown at once.
* **Lifetime.** Tracked Transform objects are held by strong GC handles (host memory is not scanned); an entry
  goes when the object is destroyed or was not written for two seconds. A stage tracks 10 to 50 transforms.
* **When.** Whenever the picture can be drawn more than 60 times a second: VSync on a display above 60 Hz, or a
  limit above 60 (`rt_display_above_60`). At 60 and below a blend would only add its delay. There is no row for it
  any more (the Smooth Motion row was removed 2026-11-15; a `smooth=` line in `port_display.txt` is ignored).
* **Render clock.** The blend is not taken at `Time.time` (when the frame began) but at a smoothed clock that
  advances by the average frame time and is pulled towards `Time.time` minus a quarter step (`render_time`). With
  VSync the frame that runs a fixed step takes longer and the queued frames behind it start early (at 240 Hz:
  3.3, 8.1, 2.2, 2.2 ms apart, shown 4.2 ms apart), which put a 60 Hz rhythm back into the motion. Behind the start
  of the last step the clock blends the step before (`prev2` -> `prev`). Measured at 240 Hz with VSync, first
  stage, rolling: ball 0 uneven steps of 120 (about 50 before), camera 0 of 238 (about 40 % before). Cost: a
  quarter step (4 ms) more delay.

Measured on a 240 Hz display (`BR_FPS_LOG`): first Adventure stage, steering: a camera moved in 30 of 120 frames
with it off (every step uneven) and in 120 of 120 with it on (0 to 2 uneven steps of 240). An imported stage with
sliding bars: 41 parts blended a frame. Online Adventure with two instances: both blend, the guest with 0 uneven
steps of 960. A five-stage run with `BR_UNLOAD_EVERY=20`: no fault. Two-player split screen: both cameras move in
every frame, but a third of the steps are uneven; `BR_CAM_TRACE` shows each step proportional to that frame's
real duration (the frame that runs a two-player fixed step takes about 7.5 ms, more than a 240 Hz frame), so
that is frame pacing, not the blend. Not looked at: Battle modes, replays, three and four players, a display
between 60 and 240 Hz, how it feels in the hand (only numbers and stills). What is not covered: motion the
engine itself makes in the fixed step without a script setter (Rigidbody-driven objects, Animators in physics
mode) and values that are not transforms (a texture scrolled in FixedUpdate).

### Wide screens (`ultrawide.c`)

The engine and the 3D cameras fill any window; only the UI is authored for 16:9. Every UI canvas (reference
1920x1080, Expand) has one child, `SafeMargin`, that parents all its UI. In menus the game keeps `SafeMargin` at
1920x1080 in the middle and covers the rest with four black images (`UIPartViewOutOfRangeBlack`, toggled by
`UISystemViewManager::SetActiveOutOfRangeBlackView`); in a stage it leaves `SafeMargin` screen-wide, so the HUD
goes to the screen edges. On a screen that is not 16:9, wider (21:9, 32:9) or taller (16:10, the Steam Deck's
1280x800), the port:

* switches the four black images off (they are off-screen at 16:9 anyway), so the scene behind a menu fills the
  screen;
* resizes full-screen childless UI elements that are backdrops by name (flat backgrounds, dimming layers, the
  blurred menu backdrop) from 1920x1080 to the canvas size (wider on a wide screen, higher on a tall one), and
  creates that blurred backdrop's render texture at the screen's aspect ratio;
* on a wide screen, lengthens the title screen's drifting pattern rows (`P01` / `P02` under
  `ui_View_TitleLoop_TitleBackground`, tiled images 11000 wide) on their right end by the extra canvas width; they
  start each title loop just past the 16:9 edge, so the right side used to stay empty until they drifted in;
* on a wide screen, gives the in-game HUD canvases (those `UIManager_MainGame` sets up without a margin) a centred
  1920x1080 `SafeMargin`, when there is one full-screen player view. With split screen the game's layout is kept.
  On a tall screen the HUD keeps the game's layout, which reaches the top and bottom edges;
* on a wide screen, switches cameras that use physical properties with a horizontal gate fit (the main menu's 3D
  scene) to a vertical fit (`gate_fit_tick`, twice a second over `Camera.allCameras`, only cameras that draw to the
  screen). With a horizontal fit the view's width is fixed, so a wider screen cuts off top and bottom and zooms in
  on the characters; with a vertical fit the view keeps its 16:9 height and gains width at the sides. Log line
  "screen: wide screen: camera <name> fits its view vertically" (once); back at 16:9 the changed cameras return to
  the horizontal fit. Other cameras are left alone (a camera without physical properties keeps its vertical field of view and widens
  by itself).

Nothing is changed at 16:9, and a resolution change is followed at once (checked 32:9 -> 16:9 -> 21:9 -> 32:9 in
a stage and on the stage select; not checked between wide and tall).

Checked at 32:9 and 21:9: title, play-style select, main menu, Adventure stage select, Options, Style, first
Adventure stage. Checked at 16:10 (a 1280x800 window): title, play-style select, main menu, Adventure world
select, Options, first Adventure stage. A menu whose flat background is an element with children still ends at
16:9 with the scene behind it showing at the sides, or above and below on a tall screen (none seen so far); add
its name handling in `widen_walk` if one turns up.

Two traps found here:

* Host statics must not keep a pointer to a managed object across frames: host memory is not scanned by the
  collector, so the object can be freed and its memory reused. Do the work inside a hook on the object's own
  per-frame method, or mark the object in its padding bytes.
* Calling `Component.GetComponent<Graphic>` on a few objects every frame made the engine's render thread crash in
  its allocator (`tlsf_free` under `GfxVersionManager::GarbageCollect`) as soon as the title scene was unloaded,
  every time. The cause was not found; the name test replaced it.

## Asset pipeline

The setup's `assets`, `shaders` and `textures` steps, in this order; each reads the previous output tree under
`build\work\unity\`.

1. `tools\decrypt_bundle.py --all`, `port\assets\retarget_all.py`, then `patch_catalog.py` -> `aa_win`. Sets every serialized file's build target
   from Switch (38) to StandaloneWindows64 (19), writes the 145 encrypted bundles decrypted, and replaces the
   game's encrypted bundle provider with the stock one in the Addressables catalog.
2. `port\shaders\batch.py all --jobs 8` -> `aa_vk`, `data_vk`. Translates the Switch NVN shader binaries to
   SPIR-V and repacks them as Unity Vulkan programs (about 16 minutes). `batch.py verify` re-validates.
3. `port\assets\tex_convert.py tree <aa_vk> <aa_pc> --cache <tex_cache>`, and `tex_convert.py file` for
   `data.unity3d` -> `aa_pc`, `data_pc`. This is the tree `game\` gets.

Texture facts found the hard way:

* Non-crunched Switch textures with an `m_PlatformBlob` are stored in the Tegra block-linear layout, one padded
  block per mip at the offset listed in the blob. `tex_convert.py` rewrites them as tight linear mip chains.
  This includes textures whose blob says block height 1; UnityPy treats those as linear and decodes them wrong,
  so do not use it as the reference for them (`tex_verify.py` reports them as differences).
* The Windows player cannot decode ASTC. ASTC 4x4 becomes BC7 (same block geometry). ASTC textures whose size is
  not a power of two become RGBA32 instead, because the Windows Vulkan player showed non-power-of-two BC7 as
  garbage even though the data decodes correctly.
* Crunched DXT textures need no change.
* Check tools: `tex_scan.py` (format inventory), `tex_convert.py check` (layout validation), `tex_verify.py`
  (against UnityPy's decode of the originals), `tex_mipcheck.py` (each mip against a downscale of the top level).

## Environment traps

* Git Bash heredocs mangle backslashes and can break on quotes. Write patch scripts with a file-writing tool and
  run them, rather than inline heredocs.
* Do not edit `run_player.sh` while a run is using it.
* A frame rate stuck at exactly 60 with VSync off is the NVIDIA driver's global Max Frame Rate, not the port.
  `port\tools\vklayer\` is a small diagnostic Vulkan layer that logs (and can override) the present mode the
  engine requests; enable it for one run with `VK_LAYER_PATH`, `VK_INSTANCE_LAYERS=VK_LAYER_BR_swapchain` and
  `BR_VK_LOG`.
* Graphics jobs are off in `game\<Name>_Data\boot.config` (`setup\assemble.py` writes it so). The Switch build's
  `gfx-enable-gfx-jobs=1` / `gfx-enable-native-gfx-jobs=1` made the Windows Vulkan player crash now and then at
  scene changes: access violation in `tlsf_free` under `GfxVersionManager::GarbageCollect` on the
  `vk::TaskExecutor` thread (a block of the non-main-thread heap with a destroyed header). Measured with two
  instances navigating title > Online Play > Battle > Battle Friends: 3 crashes in 10 starts with jobs on, 0 in 16
  with jobs off. What destroys the header was not found: the debug heap (`BR_HEAPDBG=1` with `-systemallocator`)
  saw no overrun, write after free or double free in 14 starts, but those runs stopped at the title.
* `game\` holds the non-development Unity player (no debug console overlay).

## Known gaps

* The setup was run from scratch on one PC (Windows 11, NVIDIA GPU) with the downloaded tools: every step passed,
  the extracted game matched a hactool extraction file for file, and the built game was started and played
  through the title, menus and the first Adventure stage. Not tried: a PC without the tools' run-time
  libraries, an add-on content `.nsp` through the `dlc` step, a base game given as `.nsp`.
* Exercised: title, main menu, options, the first Adventure stage; online Battle (2 players and 2 + 14 CPUs: lobby,
  race, results, back to the lobby, host disbanding), online Adventure (first stage, ball collision, restart after
  time over), split-screen Adventure with 2 and 4 players. Other modes and later stages are untested.
* Online rooms carry a content tag (`np_set_content_tag`, netplay.c): a build with other game tables (a fork
  that adds courses) cannot join a plain one. The plain game's tag is 0.
* Online was tested with two instances on one PC only (same address, no packet loss beyond the transport test's
  simulation). Not tested: over the internet, UPnP on a router that offers it, more than two real players, a
  guest leaving in the middle of a match.
* Controllers: XInput pads, Sony pads and the Switch Pro Controller (no Joy-Cons or other pads unless something
  presents them as XInput). Motion controls (the game's Accelerometer option) come from the HID pads:
  `GetSixAxisSensorState` fills just the acceleration, the only part the game reads
  (`Wonder.AppInput+StateNpad::getButtonState`), from `HidPad.accel` at 8192 per g, Switch axis = pad axis
  `-x, +z, -y` (`BR_MOTION_MAP` overrides; `BR_MOTION_LOG=1` logs raw and converted values). XInput pads and an
  unfocused window report lying flat (0, 0, -1). Felt right with a USB DualSense and a Bluetooth Pro Controller; DualShock 4 and a Pro Controller on USB not tried.
* The game refuses Battle with 3 or 4 local players and online or local-wireless play with 2 local players (its
  own rule, unchanged).
* Exclusive Full Screen has not been tried from the menu; the Reset button on the Display page ran without a
  crash but its result was not checked.
* The added option rows have no help text.
* Wide and tall screens: battle modes, results screens, the pause menu, split screen and movies have not been
  looked at. With the top and bottom black images off, a 16:9 movie on a 16:10 screen shows whatever is behind it
  above and below.
* Steam Deck: only simulated on a desktop PC (`SteamDeck=1`, `BR_DESKTOP_SIZE=1280x800`), never run on a Deck.
  Checked that way: borderless 1280x800 on a fresh save, the Resolution row showing 1280 x 800. Not checked: the
  Screen Mode row's text following the port's mode when the game's save disagrees, the firmware-name detection,
  the 16:10 entries of the Resolution list beyond 1280x800.
* Start-up, measured on one PC (`_slop_scratch\start_time.py`, pictures every 1.5 s): the window opens at once and
  is white for about 4.5 s (the engine loading its first scene and presenting an empty picture; painting the
  window black from the port changed nothing on the screen), then the health warning (3 s), the publisher's logo
  (3 s), and the title at about 14 s. The title bar has the port's name and icon from the first moment
  (`window_early_start`). Nothing was done about the 4.5 s.
* The opening scene (a real-time cutscene, about 25 s, then the tutorial) plays through on a new save whatever is
  pressed (tried: +, A, Enter, Space, X, Y, -). By the game's code (`MgMovieSequeuce`), the skip prompt is only
  offered for a scene the save has already seen, or in the viewer of the Memories menu; that a second viewing
  can be skipped was not tried. Answering Yes to the port's tutorial question skips scene and tutorial.
* Only game version 2.2.0 is supported (base game plus the 2.2.0 update). The base game alone, or another
  update, has a different executable: the setup stops with a message.
* Linux and Android builds are not started. The fixed load address is the main risk for Android.
