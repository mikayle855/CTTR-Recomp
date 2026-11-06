# CTTR-Recomp — Crash Tag Team Racing, natively on PC

A statically recompiled, native Windows port of *Crash Tag Team Racing* (PS2, Europe/Australia, **SLES-53439**),
built on [PS2Recomp](https://github.com/ran-j/PS2Recomp). The game's MIPS code is translated to C++ **at build time,
from your own disc image**: this repository contains no game code, assets or media.

It is not an emulator in the usual sense: the game's program is compiled ahead of time into a native executable,
running on a runtime that provides the PS2 hardware it expects (GS via paraLLEl-GS/Vulkan, VU1/VU0 with a static
recompiler, the IOP sound driver in a small VM, EE kernel services).

> **Status / hand-off.** The port is playable from boot to races, the park, menus, saves, sound and music, at the PS2's
> PAL speed (25 fps races) with PC options on top. It was developed with heavy AI assistance (Anthropic's
> **Claude Opus 5.5**), directed and play-tested by the project's owner, who is **not maintaining it further**: it is
> handed to the community to continue. Everything learned is in [`docs/DEVELOPER_GUIDE.md`](docs/DEVELOPER_GUIDE.md)
> (how it all fits, game addresses, tools, open ends, where to start on mods) and
> [`docs/DEVLOG.md`](docs/DEVLOG.md) (the full working log).

## What you get

- The game running natively, PS2-exact timing (PAL 50 Hz, 25 fps races and park).
- In-game settings window (**F1**, Shift+Tab, or Select+Start on a pad): internal resolution (up to 16x
  super-sampling), anisotropic filtering, aspect ratio (4:3 / 16:9 / fit / 10:7), movie shape, window and full screen,
  language (English, Français, Español, Deutsch, Italiano, Nederlands), volume, controller bindings, the game's cheat
  codes as switches, progress unlocks, skip intro movies.
- **Mods** page (off by default): true 16:9 widescreen (a wider 3D view instead of a stretched picture).
- Pause and mute while the window is in the background, F12 screenshots (Pictures\Crash Tag Team Racing), window size
  and position memory, crash reports with a launcher notice, automatic save backups, a portable folder build.

## Building (Windows 10/11 x64)

You need your own copy of the game: the **PAL** disc (SLES-53439) as an `.iso`. Requirements: Visual Studio 2022+ with
the C++ Clang tools (clang-cl), CMake 3.27+, Ninja, Python 3.10+, Git, a Vulkan-capable GPU.
`install-deps-windows.ps1 -Install` sets up the toolchain.

```
git clone --recursive https://github.com/mikayle855/CTTR-Recomp C:\CTTR-Recomp
cd C:\CTTR-Recomp
powershell -ExecutionPolicy Bypass -File games\cttr\build.ps1 "<path to your CTTR PAL .iso>"
games\cttr\launcher\build_launcher.bat
powershell -ExecutionPolicy Bypass -File games\cttr\make_portable.ps1
```

The first build recompiles the whole game and takes a long time (an hour or more; on 16 GB RAM keep the default low
job count). `dist\CTTR\CTTR.exe` then asks once for your disc image and starts the game. Details, faster rebuilds and
the debugging tools: [`docs/DEVELOPER_GUIDE.md`](docs/DEVELOPER_GUIDE.md).

## Credits

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j — the static recompiler this is built on (GPL-3.0).
- BT3_PC (*Dragon Ball Z: Budokai Tenkaichi 3* port: z3xox, RexxColder and contributors) — the runtime this tree grew
  from (GPL-3.0). `PS2Recomp-README.md` is the upstream readme.
- [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) (Arntzen Software, LGPL-3.0-or-later) and
  [Granite](https://github.com/Themaister/Granite); raylib, Dear ImGui, SDL2, ffmpeg; Russo One font (OFL).
- The PCSX2 project's patch database (pcsx2_patches) for the widescreen research.
- CTTR port: developed with Anthropic's Claude Opus 5.5.

## Legal

GPL-3.0 (see `LICENSE`); paraLLEl-GS keeps its LGPL-3.0-or-later licence (`ps2xRuntime/third_party/parallel-gs`).
*Crash Tag Team Racing* is (c) its rights holders (developed by Radical Entertainment). This project is not affiliated
with or endorsed by them, and it distributes no game content: build it only from a disc you own.
