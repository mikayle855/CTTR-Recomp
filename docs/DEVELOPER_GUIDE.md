# CTTR-Recomp developer guide

Everything learned while porting *Crash Tag Team Racing* (PS2, Europe/Australia, SLES-53439) to a native PC program,
organised for whoever picks the project up next: how the pieces fit, where the game keeps its data, every CTTR-specific
hook with its addresses, the debugging and test tools, the known open ends, and where to start on mods or features.
The chronological working log (every bug, how it was found, A/B measurements) is `DEVLOG.md`.

All addresses are EE (main CPU) virtual addresses in the PAL ELF `SLES_534.39`
(sha256 `8c0816196d16c76470f4443a49f24148873cfb68becb27ba25f461af8280eec2`) unless marked "file offset".

---

## 1. What this is, in one page

- **Static recompilation, not emulation.** `ps2xRecomp` translates every MIPS R5900 function of the game's ELF into
  C++ (`games/cttr/work/output`, ~23,000 functions, generated from *your* disc and never committed). The runtime
  (`ps2xRuntime`) provides what the code expects of a PS2: 32 MB RAM, scratchpad, DMA controller, VIF0/VIF1, VU0/VU1,
  GS, IOP services (CD, memory card, pad, sound), the EE kernel (threads, semaphores, interrupts, timers).
- **Graphics**: the GS is emulated by **paraLLEl-GS** (Vulkan compute, `ps2xRuntime/third_party/parallel-gs`, with
  CTTR fixes), presented through raylib/OpenGL with an ImGui settings window (F1).
- **VU1/VU0**: interpreted (`ps2_vu1.cpp`) and, for the game's known microprograms, statically recompiled to C++ at
  build time (`vu1_jit_gen.inc`, from `ps2xRuntime/tools/gen_vu1.py`). The JIT is what makes races run at full speed.
- **Sound**: the game's own IOP sound driver `AUDIO.IRX` runs as real R3000 code in a small IOP VM
  (`Kernel/Iop/IopModuleVm.*`) on an SPU2 voice model (`Kernel/Iop/Spu2.*`); the game's other IOP module (streamed
  file reads + movie/PCM channels) is high-level emulated in `Kernel/Syscalls/CttrIop.cpp`. Output via SDL2.
- **Game-specific code** lives in `ps2xRuntime/src/lib/game_overrides.cpp` (functions replaced or wrapped by guest
  address; CTTR's are keyed on `SLES_534.39`) and in CTTR branches gated by `g_ps2xIsCttr` across the runtime.
- The tree grew out of the *Budokai Tenkaichi 3* port (BT3_PC), which grew out of PS2Recomp. BT3-only code is still in
  the runtime, gated off for CTTR (`g_ps2xIsBt3` / keyed on `SLUS_216.78`). It does nothing for CTTR.

## 2. Repository layout

| Path | What |
| --- | --- |
| `ps2xRecomp/` | The recompiler (ELF + function map -> C++). Translators per instruction class (`src/lib/*_translator.cpp`, `mmi_translation_helpers.cpp`, `control_flow_emitter.cpp`). Ghidra scripts in `tools/ghidra/`. |
| `ps2xAnalyzer/` | Finds SDK library functions in a stripped ELF (`ps2_analyzer`), used to build the function map. |
| `ps2xRuntime/` | The runtime. `src/lib/` (EE kernel, memory/DMA, VIF, VU, GS backends, IOP pieces, settings window, game overrides), `src/main.cpp`, `src/native_gs/`, `include/`, `tools/gen_vu1.py`, `patches/` (raylib/Granite patches applied at configure). |
| `ps2xRuntime/third_party/parallel-gs/` | paraLLEl-GS sources with this project's changes (LGPL-3.0-or-later, `COPYING.LGPLv3`). `Granite/` is a git submodule. |
| `ps2xTest/` | Runtime unit tests (`ps2x_tests`). |
| `games/cttr/` | Everything CTTR: build scripts, function map, config, VU image builder, launcher, test helpers. |
| `games/common/` | Shared build helpers (`build_common.py`, `split_functions.py`). |
| `mods/` | Staged next to the program (runtime mod loader, unused by CTTR so far). |
| `docs/` | This guide and the development log. |

Generated (git-ignored, never commit -- it is game code): `games/cttr/work/` (the ELF, Ghidra project, generated C++),
`ps2xRuntime/src/runner/`, `ps2xRuntime/src/runner_overlay/`, `ps2xRuntime/include/ps2_recompiled_*.h`,
`ps2xRuntime/src/lib/vu1_jit_gen.inc`, `build/`, `dist/`.

## 3. Building

Windows 10/11 x64 is the tested platform (the runtime still has Linux/macOS code paths from BT3; untested for CTTR).

Requirements: Visual Studio 2022 or newer with the **C++ Clang tools (clang-cl)**, CMake 3.27+, Ninja, Python 3.10+,
Git, a Vulkan GPU (paraLLEl-GS). `install-deps-windows.ps1` checks for (and with `-Install` installs via winget) the
toolchain: Visual Studio Build Tools with clang-cl, CMake, Ninja, Python. SDL2, raylib and ffmpeg are downloaded by
CMake at configure time. The ISO is extracted with Windows' own `tar.exe` (or 7-Zip).

```
git clone --recursive https://github.com/mikayle855/CTTR-Recomp C:\CTTR-Recomp
cd C:\CTTR-Recomp
powershell -ExecutionPolicy Bypass -File games\cttr\build.ps1 "<path to your CTTR PAL .iso>"
```

`build.ps1` loads the Visual Studio environment and runs `games/cttr/setup.py`, which:
1. extracts `SLES_534.39` from the ISO into `games/cttr/work/` and checks its sha256 (other releases are refused;
   `PS2X_SETUP_FORCE=1` overrides, but the function map and every address in this guide are PAL-only);
2. builds the recompiler, splits oversized map rows (`split_functions.py`), generates the C++ (`work/output`);
3. installs it into `ps2xRuntime/src/runner` and writes an empty overlay table (CTTR has no code overlays);
4. builds the VU images from the ELF (`make_vu_images.py`) and the VU JIT (`gen_vu1.py` -> `vu1_jit_gen.inc`);
5. builds `ps2EntryRunner` (a full build is long: on 16 GB RAM keep `--jobs 2`-`3`; the generated units are huge).

Then: `games\cttr\launcher\build_launcher.bat` (the no-console launcher) and `games\cttr\make_portable.ps1` (assembles
`dist\CTTR`: launcher, `runtime\`, `assets\`, README). On first start the portable launcher asks for the disc image and
copies the ELF out of it; saves, settings and controller bindings stay in that folder.

Faster loops while developing:
- `build.ps1 --skip-setup --jobs 4`: rebuild the runtime only.
- `games\cttr\regen.ps1`: after a **recompiler** change, regenerate and rewrite only the changed sources.
- A change to `ps2xRuntime/include/ps2_runtime.h` (or anything the generated code includes) rebuilds everything.
- The game is configured once; `ninja -C build ps2EntryRunner` works for runtime-only changes.

Regenerating the function map (`games/cttr/functions.csv`, `config.toml.in`) from scratch (Ghidra + analyzer) is
described in `games/cttr/README.md`.

## 4. Running and the files the game uses

The launcher sets these; for direct runs:

```
set PS2X_EXEDIR=<game folder>          (holds savedata\, crashes\, play.log)
set PS2X_NODEFAULTS=1                  (BT3's renderer defaults off)
set PS2X_GPU=1
set PS2X_ASYNC_KICK=1                  (VIF1/VU1 on a worker thread: required for full speed)
set PS2X_FPSUNLOCK=0                   (PS2-exact 25 fps frame pacing)
set PS2X_CD_IMAGE=<your .iso>
build\ps2xRuntime\ps2EntryRunner.exe <game folder>\data\SLES_534.39
```

- The **memory card** is `<ELF dir>\..\savedata\` (`BESLES-53439-C1\` = the save). `savedata\settings.toml` = all F1
  settings; `savedata\pad_p1.conf` = bindings (written to `<working dir>\savedata`).
- The game reads its data straight from the ISO at run time: `ENGLISH.RCF` (and the other 5 language RCFs),
  `MOVIES.RCF` (Bink `.bik`), the level folders (`ADVENT`, `DINO`, `EGYPT`, `FAIRY`, `ONFOOT`, `SOLAR`, ...), `IRX\`.
- **Disc layout**: `SLES_534.39` (4.9 MB), `IRX\` (AUDIO, CORE, DEV9, NET*, MCMAN, PADMAN, SIO2MAN, LIBSD, SDRDRV, USB*,
  IOPRP280.IMG), one RCF per language (~200-270 MB each, compressed text: plain-text search finds nothing),
  `MOVIES.RCF` (~690 MB), level folders.
- Logs: stderr = `play.log` (launcher). `PS2X_PRINTF=1` shows the game's own debug prints.

## 5. The game, from the inside

- **Engine**: Radical Entertainment's **Pure3D**; gameplay in C++ with Lua scripting (`ScriptTable` bindings: strings
  like `void f( const char* pMovieName )` are binding signatures). The ELF is stripped, but C++ RTTI type names survive
  in old g++ 2.9x mangling (`Q26pure3d14ParticleSystem` = `pure3d::ParticleSystem`, `15MovieRenderLoop`, ...).
  - Find a class: its name string -> the code that builds its type_info (`__tf<name>`, loads the string with
    lui/addiu) -> the vtable, which holds a pointer to that function (g++ 2.9x vtables are `{delta, fn}` pairs).
  - Strings/keys: the game hashes names with `h = h*31 + c` over the lowercase string (`cttrKey()` in
    `game_overrides.cpp`). E.g. `"default"` = 0x5c13d641.
- **Sections**: `.text` 0x100000-0x506380, `.vutext` 0x506380 (VU microcode), data up to ~0x62392c, heap after it
  (crt0 asks for ~26 MB; the runtime gives the hardware layout, heap end 0x1FF0000).
- **Frame loop / limiter**: `0x343af0` is called once per game frame. Its object (0x7B3A40) has the vblanks-per-frame
  mode at +0x9c (2 in races and the park = 25 fps, 1 in some menus = 50 fps). It counts vblanks at 0x51c744 (bumped
  by the sceGsSyncV callback 0x344b90, which also stamps COP0 Count at 0x51c740). The runtime hooks this function for
  the cheats/progress tools, the frame meter and pacing (`cttrCheatFrame`).
- **Settings object** (`OptionManager`, 0x5c8e98): SkipIntroMovies at +0x638 (0x5c94d0); cheat state at +0x20c
  (Japanese Crash), +0x640 (free camera), +0x654 (toy cars), +0x658 (high speed: read by the car code via 0x2328d8).
- **Profile / progress** (`*(0x51c220)`): coins +8, crystals +0xc, collected totals +0x10/+0x14; option values
  (invert Y +0x40 via getter 0x240258, invert X +0x44 via 0x240268, more getters 0x240228..0x240288).
- **Outfits** (SkinManager, `*(0x51c330)`): 45 `Skin*` at +8; per skin +0x18 character key, +0x1c skin key, +0x24
  flags (4 = unlocked for driving, 8 = on foot, 0x40 = the Japanese-only entry). 26 entries are each character's
  `"default"` outfit (unlocked from the start), 16 are the unlockable ones. Equip 0x27b168 (char, skin, 0 driving /
  1 on foot); load 0x27a260, reset 0x27b020; equip routine 0x279e38 sets settings+0x20c when a Crash outfit is equipped.
- **Cars** 0x5bbca8 (8 x {character key, tier bits}); **Die-O-Ramas** 0x5bbcf0 (36 x {name, completed, refs});
  **named flags** vector 0x5bb5a8 {begin, end, cap} of {key, bits} (bit 0 = value).
- **Cheat codes** (CheatManager table 0x50f310, 8 x {4 buttons, handler}; entered with L1+R1 held):
  Disable HUD 0x50f3cc (handler 0x18c690), "disable death" = free camera (settings+0x640), hidden bumper camera
  0x50f3c8 (handler 0x18c868), toy cars (+0x654), high speed (+0x658), KO on rival collision 0x50f3c4,
  chicken heads 0x50f3b4 (the 40 park visitors, NPCHeadAnimator / ctor 0x215ca0), Japanese Crash (handler 0x18c7a8).
- **Movies**: Bink (`fmv_01.bik`..., logos) from `MOVIES.RCF`, decoded in software, VU0 assisted.
  `movie::RenderLoop` (MovieRenderLoop vtable 0x55eb4c; type-info function 0x2090a0) steps at 0x209178 ~30x/s while one
  plays -- the runtime uses that as "a movie is on screen".
- **Cameras** (pure3d camera, vtable 0x581f20): `SetFOV(fov f12, aspect f13)` 0x2F2DD0 stores fov +0x14, aspect +0x18;
  `SetAspect` 0x2F34F8. The aspect is relative: 1.0 = the full 4:3 screen; the rear-view mirror uses a negative aspect
  (flipped). The race view setup is 0x2DA588 (two cameras at this+0x7c/+0x80), its boost path 0x2DACD0.
- **VU1/VU0 microcode**: main VU1 program = `.vutext` 0x506380 (0x2660 bytes, micro 0). Nine overlays, (start, end)
  pairs in the table at 0x51c6c8, loaded at micro 0x4cc (byte 0x2660). Overlay 4 (0x50ccb0, instancing: coins, crates,
  props) is followed in micro memory by per-instance 22-pair matrix blocks the game uploads for every item. VU0's one
  program is two pieces: file offsets 0x40f048 (0x800 bytes) and 0x40f850 (0x30e). See `games/cttr/make_vu_images.py`.
- **Instancing**: `pure3d::ps2ExtInstancing` (vtable 0x582f30) Begin 0x33fd68 / Draw 0x340020 / End 0x342430, called
  from OnFootRenderer 0x22ef50/0x22f008. On-foot update = 0x22f100.
- **Sound**: Radical's sequencer (`audio::Seq*`, `audio::Clip`) on SPU2 voices through AUDIO.IRX. Clip files are
  `sound/rsd\<h>\<%8.8x hi><%8.8x lo>.rsd`; level music = 8-stem RSD6 files streamed into IOP RAM. AUDIO.IRX status
  block at `*(0x51f438)`.
- **Lua number formatting** goes through the game's own `sprintf` (0x4f5ec0) and soft-float routines -- recompiler bugs
  there showed up as wrong file paths (`uber3e+00`).

## 6. Runtime parts that matter for CTTR (where to look)

| Area | Files | Notes |
| --- | --- | --- |
| Guest dispatch, threads, fibers | `ps2_runtime.cpp` | `dispatchGuestBranch` (lean CTTR path `[leandispatch]`), guest lock + handoff `[lockhandoff]`, `[depthmap]`. |
| Kernel: interrupts, vblank, timers | `Kernel/Syscalls/Interrupt.cpp` | PAL 50 Hz `[palvblank]`, callback-before-waiter order `[syncvorder]`, EE timer flags `[eetimerirq]`, pause `[focuspause]`. |
| Memory, DMA, MFIFO, scratchpad | `ps2_memory.cpp` | `[sprdma]`, `[mfifo]`, `[dmaepend]`, re-entrant queue `[reentry]`, async kick worker `[asyncirq]`, I/O registers as an array `[ioregfile]`. |
| VIF | `ps2_vif1_interpreter.cpp` | I-bit interrupts `[vif1irq]`, unpack fast path `[viffast]`, V4-5 `[v45]`, V2/V3 `[v23w]`, `[mpgsame]`. |
| VU0/VU1 | `ps2_vu1.cpp`, `vu1_jit_ops.inc`, `tools/gen_vu1.py` | MAC pipeline `[macpipe]`, stalls `[vustall]`, EFU ops `[efu]`, WAITQ pairing, JIT prefix specs `image:extent@limit` `[jitprefix]`, `[maclive]`. |
| GS | `ps2_gs_pgs.cpp`, `third_party/parallel-gs` | Anisotropic filtering in the ubershader `[aniso]`, live render scale `[pgslive]`, frame queue `[frameq]`. |
| Present, aspect, pacing | `ps2_runtime.cpp` (present loop) | `[cttraspect]`, `[movieaspect]`, `[cttrmods]` widescreen present, `[pacehold50]`, screenshot `[screenshot]`. |
| IOP / sound / disc | `Kernel/Syscalls/CttrIop.cpp`, `Kernel/Iop/*`, `Kernel/Stubs/CD.cpp`, `Kernel/Stubs/SIF.cpp` | `[cdiopm]`, `[getotheriop]`, `[staleloop]`, `[cttrvol]`. |
| Pad, memory card | `Kernel/Stubs/Pad.cpp`, `Kernel/Stubs/MemoryCard.cpp`, `pad_config.cpp` | DS2 ID 0x79, `[mcgetdir]`, `[mcrename]`, test pipe `[padpipe]`. |
| Settings window (F1) | `ps2_settings_overlay.cpp` | CTTR-only pages (Display, Graphics, Audio, Controls, Cheats, Mods, Help), `settings.toml`. |
| Game hooks | `game_overrides.cpp` | `cttrCheatFrame` (limiter), `[cttrcheats]`, `[japanoutfit]`, `[outfitcount]`, `[movieaspect]`, `[cttrmods]`, diagnostics. |
| Crash reports | `ps2_crashreport.cpp`, `ps2_win_timer.cpp`, `Kernel/Debug/HostStack.cpp` | `<game folder>\crashes\<stamp>_<reason>\` with a minidump; hang watchdog. |

The bracketed tags (`[efu]`, `[mfifo]`, ...) are greppable: every change is marked in the code with its tag, and
`DEVLOG.md` explains each one.

### Recompiler fixes that any PS2 game needs (all in `ps2xRecomp`)

MMI lane/operand fixes `[mmiperm]` `[mmihilo]`, LWU zero-extension `[lwu]`, 64-bit zero-compare branches `[br64]`,
CVT.W.S truncation `[cvtw]`, divide-by-zero sign `[divsign]`, COP2 control register numbering `[cop2creg]`, aligned
128-bit accesses, COP0 Count. A recompiler change needs `regen.ps1` (or a full regen) to reach the generated code.

## 7. Features built for the port (F1 window)

| Feature | Setting (settings.toml) | Code |
| --- | --- | --- |
| Internal resolution (paraLLEl-GS super-sampling 1/4/8/16) | `video.render_scale` | `[cttrres]`, `[pgslive]` |
| Anisotropic filtering | `video.anisotropic` | `[aniso]` |
| Aspect ratio (Auto 4:3 / Fit / 4:3 / 16:9 / 10:7) | `video.aspect_mode` | `[cttraspect]` |
| Movies shape (same / 4:3 / fill) | `video.movie_aspect` | `[movieaspect]` |
| Skip intro movies (the game's own option) | `video.skip_intro_movies` | `[skipintro]` |
| Language (the PS2 system language: 1 En, 2 Fr, 3 Es, 4 De, 5 It, 6 Nl) | `video.language` | `[cttrlang]` |
| Pause + mute in background | `video.pause_unfocused` | `[focuspause]` |
| Window size/position memory | `video.window_w/_h/_x/_y` | `[winmem]` |
| Volume | `audio.master_volume` | `[cttrvol]` |
| Cheat codes as switches, progress unlocks | `cheats.cttr_toggles` | `[cttrcheats]` |
| True widescreen mod | `mods.widescreen` | `[cttrmods]` |
| F10 performance readout, frame meter, F11 fullscreen, F8 minimise, F12 screenshot | various | `[perfhud]`, `[screenshot]` |

## 8. Debugging and testing toolkit

Everything is env-gated (no cost when unset). The most useful:

- **Logs**: `PS2X_PRINTF=1` (game prints), `PS2X_FRAMEPACE=1` (vblanks per game frame, 50 frames per line: all `2`
  = locked 25 fps), `PS2X_FNLOG=<hex>[,...]` (calls of guest functions, `PS2X_FNLOGMAX`, `PS2X_FNRET`),
  `PS2X_CTTRDELIVER=1`, `PS2X_MCLOG=1`, `PS2X_RPCLOG=<sid>`, `PS2X_SPU2LOG`, `PS2X_SPU2VOICES=1`, `PS2X_CDREQLOG=1`.
- **Function tracing/patching**: `PS2X_CHICKENTRACE=1` + `PS2X_TRACEFNS=<hex>,...` (count calls of up to 8 functions,
  printed every 5 s), `PS2X_RETFN=<hex>` (make functions return at once), `PS2X_INVERTTRACE=1`, `PS2X_WIDELOG=1/2`.
- **Memory**: `PS2X_RAMTRIGGER=<file>` (when the file appears: dump EE RAM, IOP RAM, SPU RAM), `PS2X_VWATCH=<value>`,
  `PS2X_ADDRWATCH`, `PS2X_STORETRACE`, `PS2X_VALTRACE`.
- **Graphics**: `PS2X_PGS_CAPTRIGGER=<file>` (capture GS stream + VRAM), `PS2X_PGS_GIFCAP` + `games/cttr/gifparse.py`,
  `PS2X_PGS_VRAMRAW` + `gsmem.py` (textures).
- **VU**: `PS2X_VUJIT_VERIFY=1` (interpreter alongside every JIT run, reports mismatches), `PS2X_VUJIT=0`,
  `PS2X_VUMICRO=1`/`PS2X_VUMISSDUMP=1` + `PS2X_VUMICRO_DIR` (dump microcode images for `gen_vu1.py`), `PS2X_BIGKICK`,
  `PS2X_REPLAY_ENTRY` + `PS2X_VUDIFF=1` (replay one VU run with every register change), `games/cttr/vudis.py`.
- **Performance**: F10 readout, `PS2X_EEPROF=2` (+`_SYM=1`) sampling profiler of the game thread, `PS2X_ALLOCPROF=1`
  (who calls operator new, with call chains), `PS2X_FRAMEPROF=1`.
- **Crashes/hangs**: `PS2X_CRASHTEST=exception|hang`, `PS2X_HANGSECS=<s>`, `PS2X_HOSTSTACK=<s>` (all native stacks).
- **Debug dump paths** in `ps2_vu1.cpp` write to a relative `dumps\` folder: create `dumps\bigkick`, `dumps\hazewatch`,
  `dumps\hazenear` first.

**Scripted tests** (PowerShell, `games/cttr/*.ps1`): they assume a test folder `C:\CTTR_test` (with `data\SLES_534.39`
and a *copy* of a save in `savedata\`) and the repo at `C:\CTTR-Recomp`; edit the paths at the top.
- `PS2X_PADPIPE=<file>`: every line appended presses buttons (`cross:0.3`, `up+cross:2`, `screenshot`).
  `press.ps1`, `drive_to_park.ps1` (boot -> Resume -> park -> random walk), `drive_to_race.ps1` (boot -> a 1-player
  Tiki Turbo race via Multiplayer), `ab_park.ps1` / `ab_race.ps1` (A/B two builds), `shot.ps1` (PNG of the game
  window only).
- Always test on a copy of the save: progress tools and the game's auto-save write to it.
- Set `PS2X_FOCUSPAUSE=0` in scripted runs, or the game pauses whenever another window has focus.
- A **locked Windows session** throttles the GPU (paraLLEl-GS flush ~350 ms/swap) and freezes runs; screenshots come
  back blank. Check `nvidia-smi` / the F10 readout before blaming a change for a slowdown.
- PCSX2 is the reference for "what should happen"; compare RAM/VRAM/savestates (the log shows how the SPU2 and EE RAM
  dumps were compared).

## 9. Status, known issues, open ends

Playable start to finish as far as tested: boot, movies, menus, saves, the park, races, sound and music,
PAL-exact 25 fps races and park at "2x + 8x Smoothing" on a mid-range laptop (RTX 3050 class).

Open / ideas:
- **Menus at "2x + 8x Smoothing"** run ~96% speed on some full-screen menus (GPU cost of 8 samples; the present waits
  on the paraLLEl-GS flush + readback inside `onSwap`). Fix idea: don't wait there.
- **Overlay 4 tail**: a disc-only build compiles overlay 4 below 0x2bd0; the per-instance blocks above run on the
  interpreter path (correct; this is also what the reference build does). `PS2X_OV4_LIVE=<16 KB dump>` adds one more
  full-image program (one specific upload; negligible).
- **"Run and Gun" invert**: once seen (gun seat aim always inverted), not reproduced; `PS2X_INVERTTRACE=1` shows which
  code reads the game's Invert X/Y options.
- **50 fps races** (PCSX2's 50/60 FPS patch = limiter object +0x9c 2 -> 1): tried and removed -- on the test machine
  races stayed ~25 fps even at native resolution (the VU1/kick emulation is the limit). A faster CPU or a faster VU
  path could make it worthwhile; the game is variable-step (the update's dt = elapsed vblanks).
- **Local 3-4 players**: the PS2 version has no multitap support (no MTAPMAN.IRX, no mtap code): 2-player split screen
  only. 3-4 players would be a new feature.
- **Subtitles**: the game has no subtitle option.
- **Upstream PS2Recomp** has moved on (ps2xIOP, address bindings for stripped ELFs, ...). Cherry-picking it is an
  option; do it piece by piece with a regen and the park/race A/B after each step (see `DEVLOG.md`).
- Linux/macOS: the runtime has the code paths (from BT3); the CTTR build scripts and launcher are Windows-only.

## 10. Making mods and features: where to start

- **Hook a game function**: in `game_overrides.cpp`, `runtime.lookupFunction(addr)` returns the recompiled function,
  `runtime.replaceFunction(addr, &yourFn)` installs a wrapper; call the original from it. Read/modify registers through
  `R5900Context` (`getRegU32(ctx, 4)` = a0, `ctx->f[12]` = f12). Install in `applyCttrDiagnostics` (runs at ELF load
  for CTTR). The widescreen mod (`cttrWideSetFov`) and the movie detector (`cttrMovieStep`) are small examples.
- **Per-frame logic**: add it to `cttrCheatFrame` (the frame limiter hook; runs on the game thread, safe to touch
  game state). The cheats/progress code shows reading/writing tables and calling game functions (`call(fn, a0, ...)`).
- **New F1 settings**: `include/ps2_settings_overlay.h` (Settings struct), `ps2_settings_overlay.cpp` (load/save TOML,
  the page code, `drawF1Window`), an atomic the game side reads.
- **Finding things**: Ghidra with the emotionengine-reloaded plugin on `games/cttr/work/SLES_534.39`;
  `ps2xRecomp/tools/ghidra/DecompileAt.java`; the generated C++ in `games/cttr/work/output/` carries the original MIPS as
  comments (`// 0x2da664: 0x3c013f80  lui $at, 0x3F80`) -- grep it for constants and addresses; RTTI names for
  classes; `PS2X_TRACEFNS` / `PS2X_FNLOG` to see what runs when.
- **PCSX2 patches** for this disc (github.com/PCSX2/pcsx2_patches, `SLES-53439_E2FF6D3D.pnach`) are a good map of
  interesting addresses (widescreen, 50/60 fps, 480p, PSP content unlock); in this port, apply them as hooks rather
  than instruction patches (the code is compiled ahead of time).
- **Game data**: the RCF archives hold the Pure3D `.p3d` files, Lua scripts and sound; reading them needs an RCF
  extractor (community tools exist for Radical's RCF format). Nothing in the port reads modified data yet; a data-mod
  path would go through the CD layer (`Kernel/Stubs/CD.cpp`, `CttrIop.cpp` `readDisc`).

## 11. Credits, licence, how this was made

- Built on [PS2Recomp](https://github.com/ran-j/PS2Recomp) (ran-j, GPL-3.0) and the *Budokai Tenkaichi 3* port
  BT3_PC (z3xox, RexxColder and contributors, GPL-3.0); [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs)
  (Arntzen Software, LGPL-3.0-or-later) with [Granite](https://github.com/Themaister/Granite); raylib, Dear ImGui, SDL2,
  ffmpeg, the Russo One font (OFL); the PCSX2 project's patch database for the widescreen research.
- The CTTR port (runtime fixes, CTTR hooks, tools, this documentation) was developed with heavy AI assistance
  (Anthropic's Claude Opus 5.5), directed and play-tested by the project's owner, who is not maintaining it further:
  it is handed to the community to continue.
- Licence: GPL-3.0 (`LICENSE`). No game code, data or media is included; the game is recompiled from your own disc.
  *Crash Tag Team Racing* is (c) its rights holders (developed by Radical Entertainment); this project is not affiliated
  with or endorsed by them.
