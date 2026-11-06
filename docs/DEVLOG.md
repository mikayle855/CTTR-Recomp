# CTTR port -- development log

The working log kept while bringing Crash Tag Team Racing (PAL, SLES-53439) up on PS2Recomp, in the order things
happened. It is detailed and sometimes superseded by later entries (look for FIXED / Correction / REMOVED); for an
organised overview start with `DEVELOPER_GUIDE.md`. "Playtest" / "tester" = a person playing the build and reporting.
Paths like `<test dir>` were local test folders; the scripted test helpers in `games/cttr` expect `C:\CTTR_test`.


Target: *Crash Tag Team Racing* (PS2, Europe/Australia, SLES-53439, v1.01, PAL), built on the same
PS2Recomp tree as the BT3/BT4 port. This is a plain native port of the game's own code, with no mods or extras.
Like BT3, the code is generated at build time **from your own disc image**; nothing from the game is committed.

## Disc / ELF facts (checked 2026-10-20)

- Boot ELF `SLES_534.39`, sha256 `8c0816196d16c76470f4443a49f24148873cfb68becb27ba25f461af8280eec2`.
- Entry 0x100008. `.text` 0x100000–0x506380 (4.2 MB), `.vutext` 0x506380 (34 KB of VU microcode),
  `.data`/`.rodata`/`.bss` up to ~0x62392c. No separate overlay files (unlike BT3's `DBZP.BIN`).
- Stripped: no symbol table. C++ RTTI type names survive in old g++ 2.9x mangling
  (`Q26pure3d14ParticleSystem` = `pure3d::ParticleSystem`).
- Engine: Radical Entertainment **Pure3D**. Game data is in `.RCF` archives.
- The disc ships network IRX modules (DEV9, SMAP, eenet). The eenet/sppp/pppoe code is recompiled as-is.

## Build inputs (committed)

- `functions.csv`: 23,224 function rows. They come from Ghidra auto-analysis (17,366), functions created
  at every code address the ELF builds or stores as a pointer that sat outside all functions (3,666: vtable
  methods, interrupt/alarm/thread handlers), and functions created in the remaining gaps (2,178). The SDK names
  recognised by `ps2_analyzer` are written in (1,171).
- `config.toml.in`: 200 stubs (kernel syscalls + the SDK libraries the runtime implements on the host:
  sceCd, scePad, sceMc, sceSif, sceGs, sceDma, sceSd, ...; `kprintf` is bound to `scePrintf`) and the analyzer's
  `[mmio]` table.

To regenerate them:

1. Run Ghidra headless (the emotionengine-reloaded plugin gives the `r5900:LE:32:default` language). This takes
   about 4 minutes:
   ```
   analyzeHeadless.bat games\cttr\work\ghidra CTTR -import games\cttr\work\SLES_534.39 -overwrite ^
     -processor r5900:LE:32:default -scriptPath ps2xRecomp\tools\ghidra ^
     -postScript ExportPS2Functions.java games\cttr\work\ghidra_config.toml games\cttr\work\ghidra_functions.csv
   ```
2. Find the pointer-only entries, then create them and fill the gaps:
   ```
   python games\cttr\find_code_pointers.py games\cttr\work\SLES_534.39 games\cttr\work\ghidra_functions.csv games\cttr\work\code_pointers.txt
   analyzeHeadless.bat games\cttr\work\ghidra CTTR -process SLES_534.39 -scriptPath ps2xRecomp\tools\ghidra ^
     -postScript CreateFunctionsAt.java games\cttr\work\code_pointers.txt -postScript FillCodeGaps.java ^
     -postScript ExportPS2Functions.java games\cttr\work\ghidra_config.toml games\cttr\work\ghidra_functions.csv
   ```
   Order matters: create the pointer targets first, then fill the gaps. Otherwise the gap filler starts a
   function on stray bytes in front of a real entry point (it did for the VIF1 handler at 0x350c00).
3. Run the analyzer: `build\ps2xAnalyzer\ps2_analyzer.exe games\cttr\work\SLES_534.39 games\cttr\work\analyzer.toml`
4. Merge: `python games\cttr\make_config.py games\cttr\work\ghidra_config.toml games\cttr\work\ghidra_functions.csv games\cttr\work\analyzer.toml`

`ps2xRecomp/tools/ghidra/DecompileAt.java <out.c> <hexaddr>...` decompiles the functions containing given
addresses, which is handy for reading a crash site.

## Build

```
powershell -ExecutionPolicy Bypass -File games\cttr\build.ps1 "<path to your ISO>"            # everything
powershell -ExecutionPolicy Bypass -File games\cttr\build.ps1 "<path to your ISO>" --gen-only # sources only
powershell -ExecutionPolicy Bypass -File games\cttr\build.ps1 --skip-setup --jobs 4           # recompile only
```

`build.ps1` loads the VS environment (clang-cl + Ninja) and runs `setup.py`. `setup.py` writes an empty overlay
function table into `ps2xRuntime/src/runner_overlay`, because the runtime links against one and CTTR has no overlay.

## Run

The disc contents go in a folder with the ELF (`<test dir>\data` here). Saves go in its sibling `savedata`.
```
set PS2X_EXEDIR=<test dir>\
set PS2X_NODEFAULTS=1
set PS2X_CD_IMAGE=<path to your ISO>
build\ps2xRuntime\ps2EntryRunner.exe <test dir>\data\SLES_534.39
```
`PS2X_NODEFAULTS=1` turns off the ~30 renderer tweaks the runner applies by default; those are tuned for BT3.

`games\cttr\run.ps1 -Seconds 30 -Env "PS2X_A=1;PS2X_B=2"` does a timed test run and keeps the first 4 MB of the log.

## Runtime fixes made for CTTR

- **BT3 tick pumps gated** (`ps2_runtime.cpp`, `g_ps2xIsBt3`): the runtime force-ran BT3's CRI ADX ticks
  (0x28a3b0, 0x28a530) on a copy of the main context. In CTTR those addresses are the middle of a static
  constructor, so it overwrote saved `$ra` slots on the main stack and boot jumped into `.bss`.
- **Scratchpad DMA addresses** (`Kernel/Stubs/Helpers/Support.h`, `toDmaPhys`): a plain 0x7000xxxx source was
  masked to 0x1000xxxx (GIF registers). The GS reset packet never arrived and the game spun on CSR.FINISH.
- **GS CSR FIFO reads "empty"** (`ps2_memory.cpp`, `gsCsrGuestValue`): Pure3D's SyncGSPath() waits for an idle FIFO.
- **GetRomName** (`Kernel/Syscalls/System.cpp`): it has one argument. Treating garbage `$a1` as a length crashed.
- **D_CTRL starts at DMAE=1, and channels kicked while DMAE=0 start when it is set again** (`ps2_memory.cpp`,
  `[dmaepend]`). Pure3D kicks VIF1 with D_CTRL=8, D1_CHCR, D_CTRL=9. Dropping that start lost every frame.
- **Scratchpad DMA (ch8/ch9) and MFIFO** (`ps2_memory.cpp`, `[sprdma]` `[mfifo]`): these were not implemented at
  all. Pure3D builds display lists in SPR and streams them through the MFIFO ring (D_RBOR/D_RBSR) into VIF1.
- **CTTR's IOP driver (HLE)** (`Kernel/Syscalls/CttrIop.cpp`): the game ships its own IOP module for sound and
  streamed file reads. The HLE says "ready", serves open/read/close from the ISO, and accepts audio commands
  silently. `PS2X_CTTRIOP_LOG=1` logs every command.
- **Pure library code is no longer stubbed** (`make_config.py`, `STUB_EXCLUDE`): sceVif1Pk*/sceGifPk* packet
  builders, libc and maths run as the game's own code. Host versions of some packet builders broke the DMA tags of
  the frame-end packet that carries GS FINISH.
- `PS2X_PRINTF=1` shows the game's own debug prints (Pure3D reports its sanity-check failures this way).
  `PS2X_GSFINISHLOG=1` traces CSR writes, GS FINISH, DMA starts, D_CTRL, VIF1 commands, DIRECT payloads, SPR DMA
  and the MFIFO drain.
- **Debug switches** for bring-up: `PS2X_SPCHECK=1` names calls that come back with a different `$sp` or to the
  wrong place. `PS2X_SPTRACE=1` prints the last 256 dispatches with `$sp` at the first bad jump. `PS2X_ADDRWATCH`
  / `PS2X_STORETRACE` (with `PS2X_ADDRWATCH_ALL=1`) work for CTTR through the "CTTR diagnostics" override.

- **Game heap = hardware layout** (`ps2_runtime.cpp` loadELF `[cttrheap]`, `Kernel/Syscalls/System.cpp` SetupHeap /
  EndOfHeap). crt0 calls `SetupHeap(end, -1)` with a 64 KB main stack, then mallocs ~26 MB (`sbrk(0x19a4000)`). With
  the runtime's 0x1F00000 cap that failed, every later allocation came back as an offset from 0 (disc reads into
  `.text`, the Bink audio tables over `sbrk`'s own pointer, a clobbered singleton, a jump to 0x23f5c). CTTR now gets
  heap end 0x1FF0000 (RAM - main stack), and the runtime's own heap/callback stacks live at 0x80000-0x100000.
- **IOP clock** (`CttrIop.cpp` ticker thread): timer ticks (type 4, every 4 ms) and audio data requests (type 5, one
  per 0x800 bytes per active channel, paced as 48 kHz 16-bit stereo; `PS2X_CTTRIOP_AUDIO_BPS` overrides). Audio is
  still not played.
- **COP0 Count advances** (`ps2xRecomp/src/lib/cop0_translator.cpp` + `ps2xCop0Count()` in `ps2_runtime.cpp`):
  MFC0/MTC0 Count now use a 294.912 MHz clock derived from the host clock. The context field never moved, so the
  game's millisecond clock `FUN_00468628` stood still and the Bink intro movie stopped after ~24 frames.
- **XGKICK bounded** (`ps2_vu1.cpp` `[kickbound]`): a kick larger than VU1 memory (16 KB) is dropped and logged. One
  CTTR program (pc 0x15e8, vi1=0x3e4) kicks vertex data as a tag; copying that 1.3 MB "packet" froze the main thread.
  Root cause still open.
- `PS2X_HOSTSTACK=<seconds>` (`Kernel/Debug/HostStack.cpp`, Windows): prints every runner thread's native stack
  (PDB names and lines) after that many seconds. Use it for host-side hangs.

## Status (2026-10-24)

Boot -> Bink intro -> memory-card check -> frame loop at ~30 fps. Fixed since the 23rd (each is a hardware-behaviour
or recompiler bug; most would hit any game):

- **VIF1 interrupt (I bit)** (`ps2_vif1_interpreter.cpp` `[vif1irq]`, `Kernel/Syscalls/Interrupt.cpp`
  `ps2xRaiseIntcSync`): a VIFcode with bit 31 now raises INTC 5 in stream order. Pure3D starts every PATH3 texture
  upload from that handler (FUN_00350c00, VIF1_MARK selects the GIF chain), so no texture or movie frame reached the
  GS before. VIF0/VIF1 register reads now return the interpreter's live state (`[vifregs]`; MARK read 0xffff).
- **Re-entrant DMA queue** (`ps2_memory.cpp` processPendingTransfers `[reentry]`): the handler's GIF kick re-entered
  it mid-iteration (use-after-free, crash in ntdll).
- **VU MTIR** (`ps2_vu1.cpp`, `vu1_jit_ops.inc` `[mtirfsf]`): the source field is fsf (bits 21-22), not a dest mask.
  CTTR's batch descriptor got a garbage vertex count (the `[kickbound]` megabyte kicks, PRIM 0x6ff).
- **Recompiler, MMI** (`ps2xRecomp/src/lib/mmi_translation_helpers.cpp` `[mmiperm]` `[mmihilo]`): PEXEW PEXCW PROT3W
  PEXEH PEXCH PREVH PCPYH PABSW PABSH read rs instead of rt (and most had the wrong lane order) -- PCPYH is how
  memset spreads its fill byte; PINTH/PINTEH interleaved the wrong halves; PADDUH/PSUBUH did not saturate; PMULTH
  summed instead of 8 products; PMFHL/PMTHL/PMFHI/PMFLO/PMTHI/PMTLO ignored the upper 64 bits of HI/LO (hi1/lo1).
- **128-bit accesses are aligned** (`ps2xRuntime/include/ps2_runtime_macros.h` READ128/WRITE128, Load128/Store128):
  LQ/SQ/LQC2/SQC2 ignore the low 4 address bits on hardware. Code that reads an unaligned matrix as aligned LQs +
  QFSRV (Pure3D's matrix multiply FUN_0039c5f0) got it shifted twice: the camera/transform matrices were garbage and
  every vertex landed on X = 2048 (zero-width triangles, black screen). Header change = full rebuild.
- **IOP/sound driver clock, COP0 Count, heap layout, XGKICK bound**: see the 2026-10-23 entries above.
- Display: vblank scanout for CTTR (`[cttrscan]`); BT3 picture edits in the paraLLEl-GS feed are BT3-only.

Tools added on the way (all env-gated): `PS2X_HOSTSTACK=<s>` and `[crashstack]` (native stacks), `PS2X_PGS_FRAMETRACE`,
`PS2X_PGS_VRAMDUMP=<page>` + `PS2X_PGS_DUMPDIR`, `PS2X_UNPACKLOG=<s>`, `PS2X_VUREGDUMP=<n>` (+`_PC=<hex>`),
`[vutrace]` (VU1 block trace at the first bad kick), `PS2X_VU0TRACE=<n>` (+`_EEPC=lo-hi`), `PS2X_VALTRACE=<v,..>`
(now 64/128-bit stores, lanes), `PS2X_WATCH_VAL=<hex>`; scripts `games\cttr\shot.ps1` (window PNG),
`games\cttr\regen.ps1` (regenerate after a recompiler change, rewriting only changed sources),
`games\cttr\patch_mmiperm.py`, scratch VU1 disassembler (see the session notes).

### 2026-10-24 (later): picture, sound, PAL timing

Boot now shows the Sierra Bink logo and the front end's auto-save notice (3D panel, text, OK button) in the window.

- **VU0/VU1 decode cache collision** (`ps2_vu1.cpp` `[vu0decode]`): VU0 microprograms (Bink's decoder) run through
  the same interpreter as VU1, and VU1 programs also run on the EE thread when VIF1 DMA is processed there. The
  per-thread pre-decode cache was keyed by pc and validated only by the VU1 code generation, so VU1 executed VU0's
  decoded instructions at the same addresses: VU1 program 0x230 took a VU0 branch at 0x298 (really an IADDIU), looped
  0x270 -> 0x0 until its 65536-cycle budget ran out, and never kicked a frame. VU0 has its own cache now, validated by
  the instruction words (VIF0 MPG and direct EE stores write VU0 code without a generation bump).
- **Sound** (`Kernel/Syscalls/CttrIop.cpp`): the driver drives no SPU2 voices; channel N is core N playing a looping
  0x1000-byte block transfer of 48 kHz 16-bit PCM (0x200 bytes L / 0x200 R). Each refilled half is read back from IOP
  RAM after the game's handler DMAs it and mixed to an SDL device; MVOL (volume-slide format, 0x3fff = 1.0) and BVOL
  (plain s16, 0x7fff = 1.0) from cmd 5 are applied, core 0 through core 1's MVOL. Requests are paced by the device's
  buffer level. `[cttraudio]` prints blocks/peak/underruns every 10 s; `PS2X_CTTR_AUDIO=0` = silent.
- **PAL vblank** (`Kernel/Syscalls/Interrupt.cpp` `[palvblank]`, set from `sceGsResetGraph` / `SetGsCrt`): 50 Hz for
  PAL, 59.94 Hz for NTSC (was a fixed 60 Hz). `PS2X_VBLANK_US` still overrides.
- BT3 status probes reading BT3 addresses no longer run for CTTR (`ps2_runtime.cpp`).

New tools: `PS2X_PGS_GIFCAP=[b]<swap>[,<count>]` (`b` = count only swaps that carried GIF data) writes the GS stream
to `PS2X_PGS_DUMPDIR`, decoded by `games\cttr\gifparse.py` (draws per FRAME/prim, window-space ranges, register values);
`PS2X_VIFSEQ=<skip>,<count>` (VIF1 UNPACK/MSCAL/MSCNT/ITOP/BASE/OFFSET in order, full 30-qw constant blocks);
`PS2X_VURUNTRACE=<skip>,<n>` (block trace of whole VU1 runs); `[vucap]`/`[vuspin]` (always on: a VU run that ends
without its E bit, with its last blocks and instructions); `PS2X_DIVZERO=<n>` (VU DIV by zero with operands and input);
`PS2X_VUREGDUMP_PC=ffff` + `PS2X_VUREGDUMP_SKIP=<n>`; `PS2X_VUMICRO=1` + `PS2X_VUMICRO_DIR` (distinct microcode images).

Open items: pad input on the front end; the window stretches the 4:3 picture to 16:9; audio underruns (6-17% of
frames at times); VIF UNPACK V2/V3 missing lanes (unused by CTTR so far).
(Pad, 4:3 and V2/V3 lanes are fixed below.)

### 2026-10-24 (evening): menus to the hub

The game now goes boot -> movies -> title -> memory card -> Single Player -> loading -> the park hub (Crash on foot,
textured world, minimap) at a steady 50 fps.

- **Pad**: a DualShock 2 in pressure mode must report ID 0x79 (was 0x73, so the front end ignored every button).
- **paraLLEl-GS window coords** (`[pgswrap]`): XYOFFSET subtraction wraps to 16-bit signed like hardware (green stripes).
- **PAL display**: `SMODE1.CMOD` = PAL so all 512 lines are scanned out; `[cttr43]` presents 4:3.
- **AUDIO.IRX in an IOP R3000 VM** (`Kernel/Iop/IopModuleVm.*`, `Kernel/Iop/Spu2.*`): Radical's sound driver runs as
  real IOP code; its libsd calls drive an SPU2 voice model mixed with the stream output.
- **Guest lock handoff** (`ps2_runtime.cpp` `[lockhandoff]`): the guest-execution lock is an unfair host mutex. A busy
  guest thread relocked it between dispatches before waiters woke, starving the IOP driver's reply thread for up to
  10 s (`[cttrdeliver]` measured it). Loading took minutes and audio ticks stalled. A thread that has held the lock
  over 1 ms while others wait now lets one take it (2 ms bounded). Delivery latency: ~1 ms. `PS2X_LOCKHANDOFF=0` = old.
- **VIF UNPACK V4-5** (`ps2_vif1_interpreter.cpp` `[v45]`): RGBA 5:5:5:1 is expanded to 8-bit lanes (R/G/B << 3,
  A << 7). Unscaled, every gameplay vertex colour was ~1/8 brightness and alpha 0/1: the world rendered black.
- **VIF UNPACK V2/V3** (`[v23w]`): V2 fills Z,W with X,Y; V3 takes W from the next source element (as PCSX2).

- **VU1 cycle budget** (`ps2_runtime.cpp` `[vubudget]`): 65536 cut long vertex loops short at the park transition
  (`[vucap]` with vi1 still advancing); now 4M (`PS2X_VU1_BUDGET`).
- **EE CVT.W.S** (recompiler `fpu_translator.cpp` `[cvtw]`): truncates toward zero and saturates, as hardware (was
  round-to-nearest). Emitted inline; `regen.ps1` regenerated the ~300 affected functions.
- **VU WAITQ pairing** (`ps2_vu1.cpp` `[waitqpair]`, also `gen_vu1.py`): WAITQ stalls the pair's issue, so the paired
  upper op reads the new Q. CTTR's polygon clipper (`addq.x vf10, vf0, Q | waitq` at 0x1ea8) got the previous
  division's factor; clipped vertices landed far outside the frustum and wrapped across the screen. This was the
  "camera inside giant flat polygons" hub.
- **VU MAC/status flag latency** (`[macpipe]`): FMAC flags become visible to FMAND/FMEQ/FMOR/FSxx 4 cycles after
  issue (the world culler tests Sz of the op 4 slots back). `PS2X_MACPIPE=0` = instant. The JIT models flags as
  instant, so programs that read MAC run interpreted while this is on (both CTTR JIT programs do).

- **EE FPU rounding** (`[eeround]`): guest threads (main included) run with MXCSR round-toward-zero + FTZ/DAZ like
  the EE FPU; on for CTTR, `PS2X_EEROUND=0/1` overrides.
- **Memory card** (`Kernel/Stubs/MemoryCard.cpp`): `sceMcGetDir` treats `maxent` as unsigned (CTTR passes -1 =
  no limit; we returned 0 entries) and matches a wildcard-free name as one entry in its parent (`"BESLES-53439-C1"`
  is the folder itself, only `"dir/*"` lists it) `[mcgetdir]`; `sceMcRename`'s new name is a bare name in the old
  entry's folder `[mcrename]` (CTTR writes `~icon.sys` and renames it to `icon.sys` last; without that the save
  showed as CORRUPTED). Saves now persist and load at boot. `PS2X_MCLOG=1` traces every card call (repeats folded).
- BT3's settings dump (`bt3_settings_dump.log`) is no longer written into CTTR's save folder.

- **MPG re-sends** (`ps2_vif1_interpreter.cpp` `[mpgsame]`): an upload that leaves VU1 code unchanged no longer bumps
  the code generation (~40% of CTTR's uploads re-send identical code; each one flushed the pre-decode cache).

Performance (2026-10-24, superseded below): "80 game frames/s in the park" was wrong -- the on-foot update
(`0x22f100`, OnFootRenderer's frame) ran ~5 times a second. CTTR's main thread runs a test script->VIF1 DMA, the
VIF unpacks and every VU1 program inline (`0x395a70` = right after the D8_CHCR kick), and that was 85% of it.

Performance (2026-10-25/26): park on-foot update 5/s -> ~14.4/s (PAL target 25/s, the park waits 2 vblanks a
frame). Measure with `PS2X_FNLOG=22f100` (counts every 10 s; `[fnlog]` in `ps2_runtime.cpp` logs any guest function's
calls) and `PS2X_EEPROF=2` (host sampling profiler of the game thread).
- **VU1 JIT per overlay** (`games/cttr` images built from the ELF, generated by `ps2xRuntime/tools/gen_vu1.py`):
  the main program (`.vutext` 0x506380, 0x2660 bytes, micro 0) plus each of the 9 overlays the table at 0x51c6c8
  lists (start/end pairs, loaded at micro 0x4cc), each matched over its OWN extent so the bytes earlier overlays
  leave behind don't matter. Overlay 4 (0x50ccb0) runs past its table end into code the game builds by repeating a
  12-pair block, so its program is generated from a live dump with a 16 KB extent. Inputs: `<test dir>\vu1img\
  specs3.txt` (+ the images there). `PS2X_VUJIT_VERIFY=1` (interpreter alongside every JIT run): 0 mismatches
  in 2.3M runs.
- **MAC flag pipeline in the JIT** (`[jitmacpipe]`, `vu1_jit_ops.inc` jitMacTick/Push/Flush + gen_vu1.py): both
  CTTR programs read MAC flags, so they used to run interpreted while `[macpipe]` was on. FSEQ/FSAND/FSOR now count
  as flag readers too (status Z/S come from the same flags).
- **VU0 microprogram JIT** (`[vu0jit]`): CTTR's one VU0 program (~16k runs/s) is generated too (pc mask from the
  image size); a VU0 slot is re-matched whenever its code memory changes (VU0 code is written without a VU1
  generation bump).
- **JIT matching by memcmp** (`[jitmatch]`): programs carry their bytes; the per-upload FNV of every program was
  ~10% of the game thread.
- **VIF unpack fast path** (`ps2_vif1_interpreter.cpp` `[viffast]`): V2/V3/V4-32 and V4-5, mode 0, CL >= WL,
  masked or not (~90% of CTTR's vectors, `PS2X_VIFHIST=1`); `PS2X_VIFVERIFY=1` compares it with the generic path
  (3.8M unpacks, 0 mismatches); `PS2X_VIFFAST=0` disables.

Test driving: `PS2X_PADPIPE=<file>` + `games\cttr\press.ps1` (press keys, then screenshot) or
`games\cttr\drive_to_park.ps1` (boot -> park -> random walk). `PS2X_PGS_CAPTRIGGER=<file>` captures GS stream + VRAM
when the file appears. `run.ps1` now stops only the runner it started.

- **64-bit zero-compare branches** (recompiler `control_flow_emitter.cpp` `[br64]`): BLTZ/BGEZ/BLEZ/BGTZ (and the
  likely/link forms) tested only the low 32 bits. libgcc's soft-double `_fpadd_parts` tests a 64-bit fraction
  difference, so 1.0 - 1.5 gave 7.5; log10 broke, the game's `sprintf("%.5g")` printed 3 as "3e+00", Lua built the
  car path `art/cars/uber3e+00/...` and the race load waited forever. Found with `PS2X_LOADPROBE=1` (load request
  list) and `PS2X_NUMFMT=1` (Lua number formatting + a self-test of the game's soft-float routines). ~2000
  functions regenerated.
- **Divide-by-zero sign** (`[divsign]`, recompiler `fpu_translator.cpp` + `vu_translation_helpers.cpp`, runtime
  `ps2_vu1.cpp` vuDivZero + `vu1_jit_ops.inc`): EE DIV.S and VU DIV give +/-Fmax with the sign of num XOR den (was the
  numerator's sign only, so x / -0 came out +Fmax); VU RSQRT by zero gives +/-Fmax, or +/-0 when num is 0 too (was
  always +Fmax). As PCSX2. Suspected cause of walking through some walls/platforms (slab tests divide by direction
  components that are 0 or -0 on axis-aligned geometry). 466 functions regenerated.
- **Second controller** (`[padconn]`): port 2 reports "no controller" unless player 2 has its own device; the test
  pipe/script drive port 1 only (they used to mirror into port 2).

Debugging recipe that found the VU bugs: `PS2X_BIGKICK=<n>` (+ `PS2X_BIGKICK_AFTER=<s>`) dumps VU1 memory/registers/code
of kicks that draw a screen-sized triangle, plus the run's entry state (`.entry`); `PS2X_REPLAY_ENTRY=<file.entry>
PS2X_VUDIFF=1` replays that run at startup with every register change per instruction; `games\cttr\vudis.py`
disassembles VU micro code.

New tools: `PS2X_CTTRDELIVER=1` (IOP reply counts, queue->handler latency, guest-lock wait per 10 s);
`PS2X_PADSCRIPT=<sec>:<button>[:<hold>],...` (wall-clock button presses from the first pad read);
`games\cttr\gsmem.py` (decode CT32/T8/T4 textures from a `PS2X_PGS_VRAMRAW` dump).

## Open issues (2026-10-26)

- **FIXED 2026-10-31 (see "No music: FIXED"):** ~~No music anywhere~~ (menus too; SFX work). Movie audio uses CttrIop stream channel 1 and works (`[cttrstream]` every
  5 s). Music is Radical's sequencer (audio::Seq*) playing audio::Clip: at the title screen its clip events are built
  (0x3d4ff8 -> ctor 0x3d5058) and destroyed at once (dtor 0x3d5088, from 0x3e4f28), so the sequence races through;
  the clips themselves (ctor 0x3c23d8, created at 0x2819b8) are released at once (0x3c2660 via 0x3cb0f0) and no disc
  read follows. Next: follow 0x2819b8 into the stream open. Reference pair: PCSX2 title-screen savestate RAM
  `<test dir>\pcref\title_ee.bin` vs ours `<test dir>\ramt.bin` (88% of the heap identical).
- **Invisible coins/crates/juice/platform** (they exist: coins get collected; drawn by OnFootRenderer).
  Investigated 2026-10-30, see "Invisible objects: findings (2026-10-30)" below. The `ps2LightmapShader` census lead
  was wrong: by vtable (0x583f08) there are 0 lightmap and 0 layered shaders in PCSX2's title and park dumps too, and
  the ps2SimpleShader counts match (136 PCSX2 / 137 ours at the title).
- **Walking through some walls**: `[divsign]` may fix it; tester's spot = the green column ahead of the wooden arrow
  platform in the park.
- Performance: park ~14/s on-foot updates (target 25); `PS2X_ASYNC_KICK=1` hangs at boot (main thread at 0x395a70).

### Playtest (2026-10-27)

- **Invisible but interactive objects confirmed.** Crates and coins are in the game but don't draw. Crates can still
  be broken and coins collected at the spots where they appear in PCSX2 (tester compared against a PCSX2 playthrough).
- **Hub starting-area platform: same problem.** In single player, the platform in the starting area appears after you
  jump and press the floor button (cross, then circle). It doesn't draw, but it's solid: the tester stood on it and
  rode it to the main park area. So the missing draw path covers moving/triggered platforms too, not just pickups.
  Collision and logic are fine; only the rendering is missing (see the `ps2LightmapShader` census lead above).
- **Game runs too fast, like 2x-3x speed.** New issue. This happens even though the measured park rate is ~14
  on-foot updates/s, below the 25 target. So the game's time step is probably too large, not the frame rate too high.
  Suspects: the game's clock source (COP0 Count, which advances since 2026-10-23, EE timers, or the vblank count)
  running faster than the real PS2 relative to the frames we actually show, or the PAL 50 Hz vblank being delivered
  faster than real time. Check: log the game's per-frame delta time and compare it with PCSX2.
- **FIXED 2026-10-30 (`[eetimerirq]`, Interrupt.cpp), confirmed by the tester's playtest the same day:** the runtime raised OVFF+EQUF on all four EE
  timers and ran their INTC handlers every vblank. CTTR's SDK clock (`GetTimerSystemTime` 0x4ed398 = overflow count at
  0x536890 << 16 | T2 COUNT; `_InitSys` calls `InitTimer(2)` = Timer2 at BUSCLK/256) overflows 576000/65536 = 8.79
  times/s on hardware, but gained one overflow per vblank = 50/s, so game time ran 5.7x fast (the game's dt clamp
  probably made it look 2-3x). Now, for CTTR, each vblank reads every timer's MODE (which advances COUNT at its clock
  and sets EQUF/OVFF) and runs the handlers only when a flag is set and enabled (CMPE bit 8 / OVFE bit 9). Measured:
  8.80 overflows/s (logged every 5 s as `[eetimerirq]`). `PS2X_TIMERIRQ=0` restores the old delivery for A/B. Timer
  alarms (compare matches) now fire up to one vblank (20 ms) late; Timer0 has no flag emulation (logged if a game
  enables its interrupts).
- **[FIXED 2026-11-04, `[efu]`, CONFIRMED in playtest]** (see "Fog / god-ray flicker" below) **Fog flashes and flickers**, as if it isn't loading correctly. New issue. Suspects: GS fog state (the FOG
  register / per-vertex F value, FOGCOL, PRIM.FGE) set wrongly or only on some frames, a fog value the VU1 programs
  compute wrongly (they're JIT-compiled now, so A/B with the JIT off), or a draw-order/timing race. Check: capture
  two consecutive frames with PS2X_PGS_GIFCAP, compare FOG/FOGCOL/FGE in both, and compare with PCSX2.
- **[RESOLVED 2026-11-01, playtest: "doesn't seem like an issue anymore"; no dedicated fix, probably fixed by a later change such as [eetimerirq], [lwu] or [cop2creg]]** **Clipping through some objects/platforms still happens** (playtest report + screenshot
  (screenshot) 2026-10-27 184414.png`). In the park, Crash is sunk waist-deep into
  the top of a large round tree-stump platform (the one under the two bent palm trees, near the wooden railing), on the way to
  Mystery Island.
  This was on the test build: ps2EntryRunner.exe built 2026-10-26 08:56, after the `[divsign]`
  source changes (07:37), so the build includes `[divsign]`.
  So `[divsign]` didn't fully fix collision, or this is a different bug (the earlier report was walls; this one is a
  floor/top surface). Note it's only *some* objects: compare which collision meshes fail (static world vs. prop/instance
  collision) and check the collision code's float paths (DIV/SQRT/RSQRT, CVT, MMI/VU0 macro mode) against PCSX2
  at this spot.
  Second example ((screenshot) 2026-10-27 185029.png`): in the darker area with the
  twisted blue trees, on the way to Happily Ever Faster, Crash is sunk up to his head inside a big raised ledge (pink top, green sides). He walked into its
  side and through it. So both side walls and top surfaces of solid objects fail.

### No music: FIXED (2026-10-31, `[lwu]`)

Root cause: the recompiler translated **LWU** with `SET_GPR_U32`, which sign-extends; LWU must zero-extend. CTTR's
newlib vfprintf (0x4fa3a0, %x path in sub_004FB7A0) reads a plain `%x` argument with LWU, so `%8.8x` of a word with
bit 31 set printed 16 digits. The clip file names are built as `sound/rsd\<h>\<%8.8x hi><%8.8x lo>.rsd` (0x3c3cc0, two
sprintf calls 8 bytes apart), so they came out as `...ffffffff2d2857ae.rsd` / `...6d46f481ffffffff.rsd`, the files
were not found, every clip load failed (0x3c20e0 status != 2), the sequencer's clip actions had no clip
(0x3d5428 lookup = 0) and were deleted at start. Fix: `instruction_translator.cpp` OPCODE_LWU ->
`SET_GPR_U64(rt, (uint64_t)(uint32_t)READ32(...))`; `regen.ps1` rewrote the 65 functions that use LWU.
Verified: clip paths correct (`sound/rsd\e\eb5c8ae42d2857ae.rsd`), 31 audio::Clip loaded (PCSX2 31), a looping
2-voice music player alive, 4 voices playing in the menus with `[spu2out]` voice peaks ~22000-39000, and in the park
14-15 voices playing with ~1.5 MB/10 s streamed. Side effect seen: the park minimap now shows its icons.
New: `[spu2out]` (CttrIop.cpp) prints the SPU2 voices' peak level per core every 10 s; `PS2X_FNRET` logs return
values (unreliable for functions that end in a tail call); fnlog/fnret lines carry `t=` seconds.

**Wrong/looping frontend music: FIXED (2026-10-31, `[cdiopm]`, awaiting playtest).** Root cause: `sceCdReadIOPm`
(Stubs/CD.cpp) was plain `sceCdRead`, so sectors meant for IOP RAM went into EE RAM. The sound loader reads
whole-sector clip chunks straight into AUDIO.IRX's transfer buffer (IOP 0x82000) before each LoadBuffer (odd-sized
chunks go EE 0x884e00 -> sceSifSetDma instead, which worked). So every 32 KB chunk of the 1 MB frontend music clip
(`6ee8a9cff93c9191.rsd`, "femusic 24k clip edit", sound\scripts\musicrontend.p3d "music_splash") was loaded from
stale buffer data: all 30 chunks per channel identical, i.e. the same ~1.2 s repeated. After the fix all 738 used
sound pages match PCSX2's title dump byte for byte. Notes: the 0.87 s clip on entering the menu (f8fc054efd1f05f3)
is a real short jingle; the music keeps playing on voices 1+2 through the menus. New debug: `PS2X_FNLOGGATE=<file>`,
`PS2X_FNLOGMEM=<n>`, `PS2X_SIFDMALOG=1`, RAM trigger also writes `.iop`/`.spu`, `PS2X_SPU2LOG` page-jump lines.

**Constant sharp ringing after the cdiopm fix: FIXED (2026-10-31, Iop/Spu2.cpp decodeBlock).** A loop-start flag in
the ADPCM data overrode an LSAX written by software since key-on. On the SPU2 (and PCSX2's LoopMode) a software LSAX
write sticks, so AUDIO.IRX's page chaining was undone and voices looped one 2 KB page: 23k-91k stale loops per 10 s
before, ~150 after (normal: ambient loops and voices parked in the silence page). Tester confirmed.

**Level music missing / dropping out: FIXED (2026-10-31, `[getotheriop]`, Stubs/SIF.cpp, awaiting playtest).**
Symptom: loading a save straight into Mystery Island gave no music (only SFX); leaving and re-entering sometimes
brought it back; music also seemed to stop around NPC dialog. Level music (Mystery Island: `sound\rsd\7\
7c190bcf931fa914.rsd` in ONFOOT.RCF, RSD6 8 channels 32 kHz = 8 stems on voices 3-10) and dialog lines stream from
disc into an IOP ring buffer (SoundMemoryController block 0x23b6d, 512 KB; IOP 0xb8000 in PCSX2, 0x137000 here;
`core::Request` mem 2). The file system then pulls each piece into an EE buffer with `sceSifGetOtherData`, sends it
with sceSifSetDma to AUDIO.IRX's second transfer buffer (IOP 0x8a000) and LoadBuffer splits the 8 channels into
sound pages. Our `sceSifGetOtherData` read its source from EE RAM, so the stems were fed game code (EE
0x137000..: verified byte for byte), the player never got going and no further disc reads followed. Now the source is
read from IOP RAM (except the generic HLE sound-driver status block, which lives in EE RAM). After the fix a direct
load streams ~1.5 MB/10 s with all 8 stems playing. New debug: `PS2X_SPU2VOICES=1` (every keyed voice every 10 s),
`PS2X_CDIOPMLOG=1` (every IOP disc read with time; big reads also print stack callers and the request struct),
`PS2X_CDREQLOG=1` (each file-thread disc read with its core::Request: File, buffer, size, memory space, file name),
`PS2X_RPCLOG` cap raised to 3000, `PS2X_SIFDMALOG` now logs only destinations from IOP 0x8a000 up (first 5000).
Not a bug: Crash makes no greeting sound when NPC dialog opens (same on PCSX2, tester checked).

**"Music too soft at times" (playtest 2026-10-31): investigated, no emulation fault found yet.** The on-foot
music is positional: of the 8 stems, ch0/ch1 are a steady stereo bed (VOL 0x2dea hard L / hard R) and ch2-7 are faded
by the game from where Crash stands, often to near zero or small negative (phase-inverted) values such as 0x7f3c.
PCSX2 savestates show the same pattern and the same raw values (e.g. 1c1e/70bc; a fixed VOL is a 15-bit signed level,
`int16(v << 1)`, as PCSX2's V_VolumeSlide does). Checked against PCSX2 state: core0 voices are dry only (no reverb),
MVOL 0x3fff = ~1.0 on both cores, and each stem voice plays its own channel of the right file. Our SPU RAM pages
matched to ONFOOT5's 8ec703e10a3aaf43.rsd channel by channel, and the music starts about 0.8 s after the ambience at
load. Note: the test save's spawn follows the tester's last save; after the 2026-10-31 playtest it loads the park hub
(ONFOOT5, music 8ec7), not Mystery Island (ONFOOT, 7c19). PCSX2 SPU2.bin in a .p2s: sound RAM is the first 2 MB, the
cores follow at +0x10008 (Index, then 24 voice gates of DryL/DryR/WetL/WetR, ...), the voice structs are 0x108 apart,
and the VOL reg/value pairs sit at +8. `[spu2voices]` lines now end in `peak` (peak decoded sample of that voice
since the last line).

**Update (later 2026-10-31): the soft/late/missing music is stream starvation after an area change.** A same-spot A/B
at the Mystery Island T-shirt shop (PCSX2 savestate vs our RAM dump) showed PCSX2 playing all 8 stems while ours played
none. Loading straight into an area, the music file is read in 32-sector chunks and `stale loops` stays 0. After
walking into another area (heap laid out differently: stem rings at ~0x8c000-0xe3000 in SPU RAM, the same place PCSX2
has them), the chunks are 16+1 sectors, the game issues one about every 285 ms (~3 game updates at ~10/s in the park,
sometimes 750 ms), which is ~112 KB/s against the 146 KB/s the 8 stems consume (32 kHz ADPCM). The stems replay stale
ring pages (`stale loops` 150-380 per 10 s), drop out and restart, and after the change back the stems took ~40 s to
start. The read is synchronous (`FUN_003a9798`: sceCdRead + sceCdSync(0)); the pipeline is paced by game updates, so
the fix is the slow update rate (performance task), not the SPU. Not the cause: the sound player's one-current-stream
slot at 0x51c348 (traced play 0x282478 / stop 0x284ea8; no stream start was refused). Reproduce without the tester:
`trans_test.ps1` in a test script: Resume -> Enter the Park lands in Mystery Island; skip the intro with X, then hold
DOWN ~6 s to walk into the park (UP walks to the race-1 door).

**Correction (2026-11-01): the starvation diagnosis above was wrong.** `stale loops` also counted voices parked on
AUDIO.IRX's silence page (LSAX 0x6000-0x67ff loops there by design); it now counts only real replays (`[staleloop]`,
`Spu2.cpp`). With that, a full transition run (`tr3.log`) shows 0 replays in Mystery Island and in the park, the 8 stems
playing in every sample, and only a short gap at the area load. The "stems missing for ~40 s" in `tr2.log` was also a
misread: they were playing at pitch 0x0ab7 (not 0x0aaa, which the filter looked for) while English voice clips
streamed. Disc reads are ~370 sectors/10 s in both areas (same in the good and the "bad" run). Measured rates (no
profiler): Mystery Island ~16 on-foot updates/s, park ~22/s (target 25). Performance is still worth doing but is not
the music fix. The tester's "no stems" RAM snapshot (`rmi.*`, shop) stays unexplained; not reproduced in 3 scripted runs.

`PS2X_ASYNC_KICK=1` (VIF1/VU1 on a worker thread) crashes CTTR: `processPendingTransfers` from `mfifoDrain` on the
game thread races the worker, because CTTR's VIF1 I-bit interrupt handler (FUN_00350c00, raised by
`ps2xRaiseIntcSync` from inside `processVIF1Data`, i.e. on the worker) kicks PATH3 texture DMA, touching the
pending-transfer vectors and `m_ioRegisters` from two threads. Async for CTTR needs the I-bit handler run on the game
thread while the worker waits, with the handler's PATH3 data processed before the VIF1 stream resumes.
Profile (park, `PS2X_EEPROF=2`): VU1 main program JIT `vu1jit_48f9f351` 21-30% of the game thread, guest EE code ~34%,
VIF ~5%, VU1 interpreter ~3%, heap alloc ~3%.

**Async kick WORKS (2026-11-01, `[asyncirq]` in ps2_memory.cpp):** the kick worker gets its own three pending-transfer
queues (`t_workerPending`), so the PATH3 kicks CTTR's VIF1 I-bit handler makes on the worker run inline, in stream
order, without touching the game thread's queues; the worker owns the GIF arbiter; `m_ioRegisters` is pre-reserved so
worker register writes never rehash it. Measured with `PS2X_ASYNC_KICK=1` (as5.log, trans_test route): on-foot update
0x22f100 a steady **33.3/s in Mystery Island and the park** (sync: ~16 / ~22). Gameplay speed is unchanged: the game is
variable-step (the update's f13/f14 = elapsed vblanks, ~1.5 at 33/s); the Mac port notes say the NTSC park holds
30 fps, so PAL hardware would be ~25. Race (Tiki Turbo, race_as.log) runs and renders correctly; game thread
EE 66% (sync 36%, the rest was VU1 JIT inline), kick worker ~60% busy. No errors in either log. Enabled in
the test launcher (set it to 0 to compare). Pending playtest.
Playtest: "runs much smoother". PCSX2 OSD in the park: **FPS 24.98**, VPS 49.97. Our 33 was a bug, `[syncvorder]`
(Kernel/Syscalls/Interrupt.cpp): CTTR's frame limiter 0x343af0 (mode at obj+0x9c = 2 in game) reads a vblank count at
0x51c744 that its sceGsSyncVCallback 0x344b90 bumps (and stamps COP0 Count at 0x51c740); count 0 -> 2x sceGsSyncV,
1 -> 1x, 2+ -> none; then zeroes it. Our vblank tick woke the sceGsSyncV waiter BEFORE running the callback, so the
waiter zeroed the count and the callback then counted the waking vblank into the next frame -> 1/2-vblank frames,
33/s. Now (CTTR) the callback runs first, as the interrupt does on hardware: **25.0/s** in MI and the park (as6.log).
`PS2X_FPSUNLOCK=1` = old order (~33 fps), exposed in the launcher at the tester's request (default 0 = PS2-exact).

**Remaining music problems (playtest 2026-10-31, still open):** menu theme does not play, music sometimes
loops/repeats, NPC dialog plays a strange sound instead of speech. Findings so far:
- Title screen now matches PCSX2 (two looping players 1-ch + 2-ch in state 1, a one-shot every ~1.7 s).
- On Start, the title sequence stops and the menu sequence plays ONE clip (hash 0xfd1f05f3, clip type 0x00030001 like
  the title loop 2d2857ae). Its voice plays ~0.75 s (the pages loaded at boot) and then reaches AUDIO.IRX's silence
  page; the player sees its voice in the silent page and stops (0x3e9e24). No `LoadBuffer` (RPC 0x11 #0x13) and no disc
  read (`sceCdRead` 0x49ab18, only caller 0x3a9850) happen after Start, so the rest of the clip is never fetched.
  Working theory: type-3 clips keep only their start resident and stream the rest from disc; the menu/title path
  never starts that stream (in the park, 64 KB reads into 0x884e00 and LoadBuffer run constantly).
- The game's audio tick (0x3c5d40) sends PageNexts / LoadBuffer / ClearBuffer when flags 2/4/1 are set at
  `*(0x51f438)+0x2a20` (set when a disc read has landed).
- AUDIO.IRX semantics (from (a reference source tree) notes): server 0x11 = load (0 init, 0x11 ClearBuffer,
  0x12 UpdateAudioPageNexts, 0x13 LoadBuffer), 0x12 = voices (0x10 per frame: answers silent-page bits + NAX per
  voice). SPU2 memory is cut into pages (page 0 = silent loop at 0x6000); a voice loops on its page until the voice
  thread points LSAX at the next page.
- Measured: real chunk replays are rare; most "stale loops" are voices parked in the 0x6000 silence page. The silence
  mask differs from PCSX2 for never-used voices (ours 0, PCSX2 1); not yet shown to matter.
- New switches: `PS2X_RPCLOG=<sid hex>` (IOP VM RPC calls with time and first 8 words), `[spu2] ... stale loops N` and
  `PS2X_SPU2LOG` stale-loop lines, `[spu2out]` voice peaks.

Earlier investigation notes (kept for reference):
- Music is Radical's sequencer playing clips on **SPU2 voices through AUDIO.IRX** (not the CttrIop PCM stream
  channels: ch0 is never switched on, ch1 is movie audio). AUDIO.IRX imports only libsd SetParam/SetSwitch/GetSwitch/
  SetAddr/GetAddr/VoiceTrans/VoiceTransStatus: no SPU2 interrupts. It polls.
- Clip data is streamed into SPU2 RAM in 2 KB chunks (voice 0 core 0: SSA 0x6800, then LSAX 0x7000, 0x7800, 0x8000,
  ...); each chunk ends with an end+repeat ADPCM block, so ENDX is set at every chunk end (correct, PCSX2 does it too).
- AUDIO.IRX reports a status block to the EE (`g = *(0x51f438)`): `g+0x28c0` = a 1 kHz tick (measured ~1003/s, real
  time), `g+0x28c4/+0x28c8` = per-core "voice ended" masks. It does NOT pass ENDX through: for each voice it reads NAX
  (GetAddr 0x22xx) and sets the bit only if NAX lies in its silence region (AUDIO.IRX 0x624: [*(0x2e18), +*(0x2e24))).
- EE stream player (allocated by 0x3e8668, +0xb4 = 0x3e5dc0 identifies one in a dump): state at +8, loop flag +0xc
  (set by 0x3ea298 from SeqEventClip+8). Its update 0x3e9da8 stops it (state 0) when it is in state 1, not looping,
  and its voice's bit is set in that mask. The first title clip (one-shot, loop 0) plays its 4 prefetched chunks
  (0x6800..0x8800), then AUDIO.IRX points LSAX at 0x12f000 (the clip's 3392-byte tail / silence area), the player
  sees "ended" and the SeqActionClip (0x3d5058, vtable 0x592358) goes 3 -> 5 -> 0 and is deleted (0x3e4f28).
- Every later clip action starts (0x3d5108) without a clip/player (+0x14/+0x18 null) and is deleted at once: the
  action update (0x3d4730, state 1) prepares only the head of the pending queue (s4+0x40), so later actions are never
  prepared in time. Census at the title: PCSX2 31 audio::Clip / 17 SeqActionClip / 15 SeqActionSilence and three
  players alive on voices 0-3; ours 3 / 0 / 0 (37 bare SeqAction = destroyed). The clip cache (`*(0x51c340)`) is not
  the cause: same budget (+0x58 0x1aa602), our usage +0x6c is only 0x8000 vs PCSX2 0x145000.
- Next: compare the first clip's length with PCSX2 (is 4 chunks + tail all of it, or does the EE stop feeding
  chunks?), and follow why the sequencer's next clip actions are not prepared before their start time (queue at
  SeqPlayer+0x40, 0x3d5400 head, 0x3e55a0 remove). Switches: `PS2X_SPU2LOG=1` now also logs core-0 voice-0
  SSA/LSAX/NAX writes, ENDX events with blocks since key-on, and ENDX reads/writes.

### Backlog (agreed, not started)

- DONE 2026-11-04: fog flashes/flicker (`[efu]`, see "Fog / god-ray flicker" below).
- Quality-of-life (suggested 2026-11-01, playtest: "add this to the backlog"):
  1. DONE 2026-11-01 (`[cttrmenu]`, `[cttrvol]`): CTTR-only F1 menu. Hidden for CTTR: Cheats / Mods / Online tabs, BT3
     widescreen + HUD placement, native-renderer 2D filter, fight effects (shadows, outline, glow, distance blur), "Not
     offered". The Volume slider now really controls CTTR's sound (CttrIop SPU2 mixer x PS2AudioBackend master volume;
     music and effects share SPU2 voices, so one level). CTTR Help text.
  2. CHECKED 2026-11-01, nothing to speed up: a level load (Enter the Park -> Mystery Island) takes ~5 s (CD reads are
     instant); the "~45 s" was only the test script's wait. Boot to "Load Successful" ~31 s is the logo movies, and
     the game itself skips them with X (title in ~15 s). A turbo would only fast-forward the original logos/audio.
  3. DONE 2026-11-04: launcher <test dir>\CTTR.exe (games\cttr\launcher), icon, no console, desktop shortcut.
  4. DONE 2026-11-04 (`[skipintro]`): F1 > Display > Start-up "Skip intro movies" (the game's own option, default off).
  5. CHECKED 2026-11-04: vibration reaches SDL (127 requests per race; tester to feel-test); Controls page in new style.
  6. DONE 2026-11-04 (`[savebackup]`): the launcher backs up the save + settings at each start (keeps 10).
- Kept by decision 2026-11-04: env-gated diagnostics PS2X_SKINDUMP / PS2X_PROGRESSLOG / PS2X_CHEATTEST.
- Optional improvements (playtest: "add to the notes for later", 2026-11-04; "keep these in the backlog for now", 2026-11-05 = parked), in suggested order:
  1. DONE 2026-11-04, CONFIRMED in playtest 2026-11-05 ("menus are smooth now", Driving Games door fine, races feel fine):
     menu smoothness at "2x + 8x Smoothing". Level Options screen,
     render_scale 3: 27.6 -> 49.4 pictures/s shown. Two causes:
     - `[pacehold50]` (ps2_runtime.cpp): the hold engaged on 50 fps menus (arrival-gap EMA > 24 ms with the GPU busy)
       and dropped every other picture. CTTR now also counts content as fast when the game itself runs > 37.5 fps
       (g_ps2xGameFrames); this can only turn the hold off, so races (25 game fps, 50 scanouts/s) are unchanged.
       PS2X_PACEHOLD50=0 = old rule.
     - `[frameq]` (ps2_gs_pgs.cpp): the async readback kept ONE picture; a picture the present loop had not taken
       yet was overwritten (late loop pass, or two readbacks completing in one swap). CTTR now queues up to 2
       (oldest first, oldest dropped beyond that). PS2X_PGS_FRAMEQ=0 = old behaviour.
     Race check (perf_race.ps1): 25 fps, 50 pictures/s shown, late frames 36 vs 31 baseline (noise).
     Menu game-frame lateness at render_scale 3 was 113/1763 (earlier runs 69-98): watch it; item 2 is the real fix.
  2. Menu speed at "2x + 8x Smoothing" (96%): don't wait on the paraLLEl-GS flush + readback inside onSwap
     (ps2_gs_pgs.cpp; 7 ms per swap on Level Options). Bigger, riskier, menus only.
  3. DONE 2026-11-06 (`[movieaspect]`): F1 > Display > Movies: "Same as the picture" (default, as before) / "Always 4:3
     (their original shape)" / "Fill the window" (video.movie_aspect 0/1/2). Movies are Bink (MOVIES.RCF fmv_0x.bik,
     logos), decoded in software; movie::RenderLoop's per-frame step 0x209178 (MovieRenderLoop vtable 0x55eb4c; found
     via its type-info function 0x2090a0 -> name 0x55ed10) runs ~30x/s only while one plays (PS2X_TRACEFNS count:
     150 per 5 s during the Sierra/Radical logos, 0 after). The present (ps2_runtime.cpp [cttraspect]) uses the Movies
     shape while that ran in the last 250 ms. Test: aspect 16:9 + movies 4:3 -> logo movie pillarboxed 4:3, the
     memory-card notice after it 16:9.
  4. DONE 2026-11-06 (`[allocprof]`, `[depthmap]`): PS2X_ALLOCPROF=1 (main.cpp replaces operator new; ps2_win_timer.cpp
     records 6-frame call chains, prints the top sites every 10 s). Race: ~440k operator new/s. Fixed the two biggest:
     PS2Runtime::enter/leaveGuestExecution inserted+erased a thread_local unordered_map node per guest entry (now a
     4-slot GuestDepthMap, no heap; also copied per fiber switch) and the VIF1 MFIFO drain grew its vector 16 B at a
     time (now reserved). Measured with the profiler on: ~190k -> ~88k/s. Left: queueCompletedDmacCause,
     GifArbiter::takeQueue, the PENDING_* swap in processPendingTransfers (~13k/s each; the first two need a header
     change = full rebuild). Race after: all frames 2 vblanks (full speed).
     NOTE for tests: the F1 "pause when in background" pauses scripted runs when the tester uses the PC -> tests set
     PS2X_FOCUSPAUSE=0. A LOCKED PC throttles the GPU (flush 344 ms/swap) and freezes runs; shot.ps1 then returns blank.
  5. DONE 2026-11-06 as a MOD (`[cttrmods]`): new F1 > Mods page (settings [mods] widescreen, off by default), "True
     widescreen (16:9)". Based on PCSX2's 16:9 patch for this disc (github PCSX2/pcsx2_patches
     SLES-53439_E2FF6D3D.pnach, Arapapa/nemesis2000/PeterDelta: 0x2DA664/0x2DA688 lui 0x3F80 -> 0x3FAB, turbo fix =
     NOP the SetFOV call at 0x2DAF98). Here: the view setup 0x2DA588 gives its two cameras (this+0x7c/+0x80, pure3d
     camera vtable 0x581f20) the viewport aspect via SetAspect 0x2F34F8 -> SetFOV(fov f12, aspect f13) 0x2F2DD0 (slot
     0x44); its boost path 0x2DACD0 calls SetFOV directly. Hooks: 0x2DA588 marks "in view setup", 0x2F2DD0 multiplies
     the aspect by 4/3 while marked (boost keeps its fov change, unlike the PCSX2 patch). The present shows 16:9; movies
     stay 4:3 unless Display > Movies says otherwise. HUD/menus are 4:3 art, so they are stretched. Test: race start
     4:3 vs mod -> more track at both sides, car/scenery unstretched, 25 fps 100%. Boost path not driven in the test.
     PS2X_WSPROBE=1 prints the cameras (diagnostic; disables the mod hooks).
     FIX 2026-11-06 (playtest: "the screen flickers in and out", only with the mod on, also in some in-game videos): the
     first version widened only inside 0x2DA588; in the park the camera's own update (SetFOV caller ra 0x39185c) passes
     a fresh aspect 1.0 every frame outside it -> wide/4:3 alternation. In this game the aspect is RELATIVE (1.0 = the
     full 4:3 screen). Now every camera's SetFOV 0x2F2DD0 gets aspect x 4/3 (race/park/cutscene cameras, rear-view
     mirror -- negative aspect -3.0769, flipped -- and menu models), except a value the hook itself produced for that
     camera (callers passing the stored aspect back, e.g. 0x2f3a6c/0x2f3d60/0x2f3d98), so nothing compounds.
     PS2X_WIDELOG=1 logs each distinct (caller, camera, aspect in/out, stored); =2 observes without widening.
     Park (wide_park.ps1) and race: every call ends at 1.3333 (mirror -4.1025), stored values constant, screenshot
     bursts steady; race 25 fps 100%. Movie shape hold 250 -> 700 ms (a streaming stall must not flip 16:9/4:3).
  6. REMOVED 2026-11-06 (playtest: "if it doesn't behave well we can remove it"): "50 fps races" mod (PCSX2 50/60 FPS
     patch: limiter object 0x7B3A40 +0x9c vblanks/frame 2 -> 1). The switch worked (some 1-vblank frames) but races
     stayed ~25 fps at 2x+8x AND at native resolution (pgs flush 0.56 ms/swap): the CPU-side emulation (VU1/kick
     worker ~60% busy at 25 fps) is the limit. No gain on this PC, only timing risk.
- Port enhancements, agreed 2026-11-05 ("add 1 to 6"), worked one at a time in this order:
  1. DONE 2026-11-05 (`[focuspause]`, waiting for the tester's playtest): pause + mute when the window loses focus. F1 >
     Display > Window "Pause when in background" (video.pause_unfocused, default on, CTTR only). The overlay sets
     g_ps2xFocusPaused when the window is unfocused/minimised (only after it has had focus once); the vblank worker
     (Interrupt.cpp) then delivers no vblanks (dropped, not caught up), so the game and its sound/timer threads stand
     still, and the SPU2 output (CttrIop.cpp) plays silence without advancing voices or the music stream.
     PS2X_FOCUSPAUSE=0 = never; PS2X_FOCUSPAUSE_SIM=<file> = scripted test (background while <file> exists).
     Test (focus_test.ps1, race, scale 3): 0 fps + frozen picture for 14 s, game clock stopped (framepace t 168.7 ->
     171.1), race resumed in place, no errors; real save hash unchanged.
     Follow-up (playtest: ~72% for a few seconds after a resume): the CttrIop ticker kept queueing IOP ticks/stream requests
     while paused (up to 64), worked off in a burst after the resume. Now it sends nothing while paused. Retest
     (focus_test2.ps1): one 5-vblank frame at the resume, then all frames 2 vblanks (full speed).
- Invert bug (tester 2026-11-05): the game's own Invert Y option has no effect in the race mode "Run and Gun" (always
  inverted there); works on foot in the park and in races. Option rows (OptionScreen 0x2330d8): profile *(0x51c220)
  getters invert X 0x240268 (+0x44?), invert Y 0x240258 (+0x40). Readers: 0x11cec0 (beql+neg), 0x1bf178/0x1bf278
  (X+Y), 0x1cafa0/0x1cb0f8 (bnel+neg), 0x21c040/0x21c650 (X+Y); recompiled branch-likely code checked = correct.
  `[inverttrace]` PS2X_INVERTTRACE=1 logs getter call sites + values every 5 s; launcher <test dir>\Play CTTR (invert
  trace).bat (temporary) -> invert.log. If the PS2 game behaves the same: tester wants an optional fix (F1 setting /
  mod), off by default.
  Trace session 2026-11-06 (invert.log): only 0x21c040/0x21c650 read it at play time (25/s, followed off -> on; most
  likely the gun-seat aim), 0x233740 area = option screen. Playtest: gun seat on a joined car "worked this time".
  NOT REPRODUCED; parked. Desktop shortcut "CTTR invert test.lnk" kept until it shows again.
  2. DONE 2026-11-06 (`[crashchain]`, `[crashreport]`, `[crashnotify]`): crash report on an unexpected exit. The
     report code existed (ps2_crashreport.cpp: <EXEDIR>\crashes\<stamp>_<reason>\ crash.dmp + info.txt) but:
     HostStack.cpp's filter (installed at ELF load) REPLACED main's full handler, so a crash in play wrote no report ->
     it now hands over to it; the hang watchdog and PS2X_CRASHTEST counted only BT3 frames (never armed for CTTR) ->
     BT3 + CTTR frames, and a [focuspause] pause is not a hang; info.txt names module+offset. Launcher (CTTR.exe) now
     waits for the game: a new report + nonzero exit code -> copies the log tail (256 KB) to run_tail.log and shows
     where the report is. A preset PS2X_EXEDIR now also moves the ELF and logs (tests never touch the real save).
     Tests: PS2X_CRASHTEST=exception via the launcher -> report with dump, info, run_tail.log ([wincrash] stack),
     message; PS2X_CRASHTEST=hang + PS2X_HANGSECS=10 -> "hang" report at frame 582; 21 s pause with HANGSECS=10 ->
     no report.
  3. DONE 2026-11-06 (`[screenshot]`): F12 saves the game picture (no letterbox bars, no Settings window / readouts:
     grabbed in the present loop before they are drawn) as PNG to <Pictures>\Crash Tag Team Racing\CTTR_<date>_<time>.png
     (encoded on a worker thread); a 2.5 s notice says where. Help page lists F12. Test: pad pipe line "screenshot"
     (same request as F12) in a race -> 960x720 PNG of the picture only.
  4. CHECKED 2026-11-06, no change needed: loading is not throttled. sceCdRead (Kernel/Stubs/CD.cpp) copies the whole
     request from the image at once, the CttrIop block reads (type 0) served 0 requests in a boot -> race run
     (PS2X_CTTRDELIVER=1), and a race load (YES -> track flyover) takes under 5 s (loadtest\loading.png 5 s after YES
     already shows the flyover). Loads are bound by the game's own work, not by the disc.
  5. DONE 2026-11-06 (`[portable]`): games\cttr\make_portable.ps1 builds <repo>\dist\CTTR (326 MB): CTTR.exe
     (launcher), runtime\ (exe + DLLs + pdb), assets\, README.txt; no disc image, game file or save. The launcher
     detects runtime\ps2EntryRunner.exe beside it and keeps everything in its own folder (EXEDIR = working dir =
     the folder, so savedata\ also holds the controller bindings); the disc image comes from PS2X_CD_IMAGE, disc.txt,
     a *.iso in the folder that has SLES_534.39, or a one-time file picker (saved to disc.txt); data\SLES_534.39 is
     copied from the disc's ISO9660 root on first start. Test: fresh folder + disc.txt -> ELF extracted (identical to
     <test dir>\data copy), game booted to the new-memory-card notice, save inside the folder; real save unchanged.
     Re-run make_portable.ps1 after each build to refresh it (keeps savedata\, data\, disc.txt).
  6. DONE 2026-11-06 (`[winmem]`): it remembered only a size picked in F1. Now the windowed size AND position are
     tracked (once still for 1 s, CTTR only), saved as video.window_w/_h/_x/_y and restored at start and when leaving
     full screen, only if the title bar would land on a connected monitor (else centred as before). Test
     (winmem_test.ps1): moved/resized to 150,90 1100x660, closed normally, restarted -> 150,90 1100x660.
  7. NOT POSSIBLE as a plain port (checked 2026-11-06): the PS2 version has no multitap support. The disc's IRX\
     folder has PADMAN/SIO2MAN but no MTAPMAN.IRX, and the ELF names only sio2man.irx / padman.irx (no mtap strings,
     no sceMtap* functions). PS2 CTTR is 2-player split screen; 3-4 players were other consoles. Adding them would be a
     new game feature (a mod). Not started.
  8. DONE 2026-11-06 (`[cttrlang]`): F1 > Display > Start-up > Language (English, Francais, Espanol, Deutsch,
     Italiano, Nederlands; video.language = PS2 language code 1/2/3/4/5/6, default 1; next start). The runtime's OSD
     config (Kernel/Syscalls/Helpers/Runtime.h ensureOsdConfigInitialized) always said English; it now takes
     PS2X_LANGUAGE (0-7) or the setting (g_ps2xOsdLanguage, set in preloadSettings before the game asks). The game
     then loads that language's RCF (text and speech). Tests: PS2X_LANGUAGE=2 -> "UN JOUEUR" signpost; settings
     language = 4 -> "EINZELSPIELER", "WAHLEN/ZURUCK". Header edit = 31 runtime files rebuilt (minutes).
- FIXED 2026-11-06 (`[outfitcount]`, playtest report: brand-new save showed "26 outfits unlocked" in F1 > Cheats): the outfit
  table (SkinManager, PS2X_SKINDUMP) holds 26 "default" outfits (skin key cttrKey("default") = 0x5c13d641, flags 0x3f,
  unlocked from the start, game behaviour) + 16 extra outfits (flags 0 on a new save). The F1 count included the
  defaults; it now counts only the extras: fresh save 0 of 16, the tester's save (copy) 16 of 16.
- Mods page excludes by decision (2026-11-06): subtitles, 3-4 player split screen, invert gun-seat fix.
- Tester-side checks: CONFIRMED 2026-11-05 by playtest: KO, vibration, launcher, skip intro (skips logos only, by design),
  High speed (player car only, by design), Chicken heads (hub park visitors only, by design). Menu smoothness
  also confirmed. Still open (optional): PCSX2 .pnach test.
- DONE 2026-11-04 (agreed): removed a desktop test shortcut (the launcher replaces it) and the 11
  fog/F9-capture debug .bats in <test dir>\; the test launcher .bat kept as a fallback.

### Settings: Internal resolution + Anisotropic filtering (2026-11-01, `[cttrres]` + `[aniso]`, CONFIRMED in playtest 2026-11-01)

Request: PCSX2-style Internal Resolution and Anisotropic Filtering. Both are in the in-game Settings window
(F1 or Shift+Tab), saved to `savedata\settings.toml` (`render_scale`, `anisotropic`), and apply at once.
- **Internal resolution** (Display tab, CTTR-only branch of `drawF1Window`): Native / 2x / "3x" / 4x = paraLLEl-GS
  super-sampling 1 / 4 / 8 / 16 samples (`ps2x_pgs::setRenderScale`, live backend re-create `[pgslive]`). 4x is the
  backend's maximum (SuperSampling::X16); "3x" is 8 samples shown at 2x. PCSX2's 5x-12x are not possible with
  paraLLEl-GS. Measured 2026-11-01 (MI, RTX 3050 laptop): 1x 0.6 ms GPU/swap, 2x 1.5 ms, 4x 2.5 ms, still 50 swaps/s.
  The BT3 behaviour of resetting the scale when switching full screen is off for CTTR.
- **Anisotropic filtering** (Graphics tab): Off / 2x / 4x / 8x / 16x. paraLLEl-GS emulates the GS sampler exactly
  and has no such option, so it is added to the ubershader (`sample_texture`): screen-space UV derivatives from the
  primitive setup (barycentrics are affine: di/dx = setup.b.x * 2^(3 - rate_log2) * inv_area), mip chosen from the
  footprint's minor axis, up to N bilinear taps along the major axis. Only bilinear + perspective (STQ) primitives,
  so HUD / text / sprites keep the console's exact sampling. Plumbing: `Hacks::anisotropic` -> `RenderPass` ->
  `ShadingDescriptor.anisotropic` (push constant). `PS2X_PGS_ANISO=<n>` sets it from the launcher; `=100` is a debug
  view (red = taps/16, green = mip/4). Most CTTR textures have no mipmaps, so the effect is smoother, steadier distant
  ground (less shimmer) rather than extra sharpness.
- Shader bank: `gs/shaders/slangmosh.hpp` is regenerated with Granite's slangmosh, now built at
  `<build dir>\slangmosh.exe` (`cd gs && slangmosh --namespace ParallelGS shaders/slangmosh.json
  --output shaders/slangmosh.hpp --output-interface shaders/slangmosh_iface.hpp -O --strip`). Rebuilding the old
  sources gives byte-identical SPIR-V. Backup of the previous bank: `<test dir>\slangmosh.hpp.pre_aniso`.
- Benchmarks 2026-11-01 (warm shader cache, RTX 3050 laptop, `bench_route` MI + race; game rate = 0x343af0 calls):
  1x: MI GPU 0.65 ms, race 0.5 ms; 4x + 16x aniso: MI 2.0 ms, race 1.5-4.4 ms, menus ~11 ms (1x 0.5); always 50
  swaps/s. Game rate is the same at 1x and 4x+16x (MI ~19-20/s, race ~20-24/s on these routes: CPU-bound, not the
  settings). Tap-count fix: no extra taps when the footprint is < 1 texel (magnified surfaces had taken 16 taps).
  First run after a shader-bank change: driver pipeline compiles cause brief hitches; later runs are smooth.
- Note for perf tests: another GPU-heavy program running alongside (99% GPU) drops the game to 10-15
  swaps/s; check `nvidia-smi` before blaming a change.

### Black HUD coin icon: FIXED (2026-11-01, `[cop2creg]`, CONFIRMED in playtest 2026-11-01)

Playtest report: the spinning coin in the top-right HUD (coin counter) drew as a black disc; PCSX2 shows a gold coin.
Cause: the recompiler numbered the COP2 control registers wrongly for CFC2/CTC2 (ps2xRecomp `vu_translator.cpp`,
enum in `instructions.h`: I = 4, R = 3, CMSAR0 = 7, ...). Hardware: 0-15 = VI00-VI15, 16 Status, 17 MAC, 18 Clip,
20 R, 21 I, 22 Q, 26 TPC, 27 CMSAR0, 28 FBRST, 29 VPU-STAT, 31 CMSAR1. The lighting setup `func_33D178` computes the
ambient colour ((emissive + group ambient * material) * 255), then clamps it with `ctc2 $t0,$vi21` (I = 255.0) +
`vminii.xyzw vf1,vf1,I`; the CTC2 wrote a dead `vu0_info` field, so the clamp used a stale I (sqrt 2 or 2 pi left by
other code) and every ambient became 1.41 / 6.28 -> vertex colours (1,1,1,1) / (6,6,6,6) -> modulate = black. The
coin's lights have zero directions, so the ambient is all it gets. Fix: CFC2/CTC2 switch rewritten to the hardware
numbering (9 functions regenerated: sub_0033D178, sceVu0MemReadQ (VI01 was going to the MAC flags), sceDevVu0/1Pause,
sceDevVu0/1Sync, sceVuCheckBusy, sub_00496B68, sub_0033AB68). Verified: coin gold, the HUD gem lit, no more tiny
colours (probe), a race OK.
How it was found: GS capture of the HUD (`PS2X_PGS_CAPTRIGGER`, gifparse now prints `tfx`/`tcc`) -> coin drawn with
RGBA 1,1,1,1 modulate; `PS2X_DIMKICK=1` (ps2_vu1.cpp xgkickImpl) logs VU1 packets with tiny grey colours + registers
and dumps VU1 code/data to <test dir>\dimkick_<n>_*.bin; `PS2X_RAMTRIGGER` EE RAM dump -> light block in the
scratchpad (0x70003a20, 30 qw); `PS2X_VWATCH=0x3fb504f3` (store of sqrt 2) -> 0x33d2c8.

### Invisible objects: FIXED (2026-11-01, `[vustall]` + `[jitprefix]`)

Playtest 2026-11-01: CONFIRMED, coins and crates load in fine.

Coins, crates and the other instanced props are drawn by `pure3d::ps2ExtInstancing` (CoinDrawable/CrateDrawable at
manager +0x20/+0x18, getters 0x230348/0x230340; OnFootRenderer 0x22ef50/0x22f008 -> Begin 0x33fd68 / Draw 0x340020 /
End). Per item the EE MPGs a table of 22-pair matrix blocks at VU1 0x2bd0+ (LOI; the upper op sees the OLD I --
confirmed, the generator lays the values out that way). The instancing overlay's loop (0x26b0..0x2b40, in
`cttr_ov4_live.bin`) calls each block, then culls by distance:
`2728: sub.x vf5, vf2, vf3` / `2730: abs.x vf0, vf5 | fmeq vi1, 0x80` / `2738: ibeq vi0, vi1, skip`.
On hardware the ABS reads vf5 while the SUB is in flight, the PAIR STALLS until it lands, and by then the SUB's sign
flag is in the MAC register. We ran one pair per cycle (the `[macpipe]` 4-cycle delay without stalls), so FMEQ saw an
older op's flags, every instance was "out of range" and was skipped: invisible but collectable/solid.
Fix:
- `[vustall]` (ps2_vu1.cpp): VF component ready times (FMAC/LQ/MOVE/MFIR... usable 4 cycles after issue); a pair
  reading a component in flight waits while Q/clip/MAC tick (`vuPairVfUse`, decode cached per pc). `PS2X_VUSTALL=0` = off.
- gen_vu1.py: the same model computed STATICALLY (data flow over fall-through + static branch edges, join = max);
  stalled pairs get `++cyc; jitMacTick` ticks (1671 sites). All programs regenerated.
- `[jitprefix]`: every instancing upload is a new image, so it ran interpreted (MI fell to ~9-15 updates/s once the
  items really drew). New spec form `image:extent@limit` -- `cttr_ov4_live.bin:4000@2bd0` -- compiles only below
  0x2bd0, matches on [0, 0x2bd0), and runs anything at/above it on the interpreter's per-op paths
  (`vujit::jitRunOutside`, vu1_jit_ops.inc) until control is back below. Regenerate with:
  `cd <test dir>\u1img && python gen_vu1.py <inc> $(cat specs3.txt) cttr_ov4_live.bin:4000@2bd0`
  (pre-change .inc kept as `vu1_jit_gen.inc.pre_stall`).
Result: crates/coins visible in MI and the park; MI and park 25 updates/s; race OK (more island props visible too);
`PS2X_VUJIT_VERIFY=1`: 0 mismatches in ~3.5M runs, 0 unmatched microcodes.
Debug switches added: `PS2X_INSTDUMP=<n>[,<skip>]` (per-instance matrices, `[instsetup]` batch header/ITOP, and a
pair trace `[insttrace]` of the run hitting instance #skip), `PS2X_RAMTRIGGER` RAM dumps for header comparisons.
Things that were checked and are NOT the cause: LOI order, the JIT matching, ITOP/batch headers (scale 1024 headers
are normal), GS positions (gifparse's off=(0,0) before the frame's XYOFFSET write misled one A/B).

### Invisible objects: findings (2026-10-30)

- Asset load is not the problem. In the park, a class census by vtable (PCSX2 `pcref/eeMemory.bin` vs ours) matches,
  e.g. 129 StaticProps each, and the 306 pure3d::Geometry name uids are the same set. The count gaps (Geometry 589 vs
  469 etc.) are stale copies in freed memory. The StaticProp objects are byte-identical at the same addresses.
- Shaders: the shader-chunk loader (0x3634cc) passes the name from the data to the factory (0x390540, table at
  0x51ddb0, falls back to entry 0 "simple"). The data really asks for "simple"/"spheremap"/"shadow", so no lookup
  fails. The MMI strcmp at 0x4f4594 translates correctly.
- Instancing is how static props are drawn: `pure3d::ps2ExtInstancing` (vtable 0x582f30). OnFootRenderer
  (0x22ef6c/0x22f03c) and 0x247f20 call Begin 0x33fd68(geometry) -> Draw 0x340020(matrix group) -> End 0x342430
  about 9 times per frame. Draw loops over the group's items (0x342648) and, per item, uploads ~5 KB of VU1
  microcode with an MPG (0x33a268: FLUSHA + MPG chunks, load address 0x57a = byte 0x2bd0) and emits the draw
  (0x33ff50). The uploaded code is a table of 22-instruction blocks, one per instance: ADDi/MULi with I-bit
  immediates build the instance matrix (identity + translation, e.g. (57,-9,336), scale 1), then `jr vi15`. The
  main renderer program calls into that table (e.g. `bal vi10, 0x3b70`). Each item uploads different code, so
  every image is new (4810 distinct images dumped in one run) and runs interpreted. The interpreter's LOI order
  (upper sees the old I) is what gives a sensible matrix.
- A/B at the park spawn: `PS2X_RETFN=340020` (skip the instanced draw) leaves the picture and the GS stream
  unchanged (~26.7k vs ~27.1k vertex kicks per 3-frame capture; the `fbp=0x0` rows in gifparse output are
  primitives seen before the capture's first FRAME write). So instancing currently adds nothing to the frame
  there. Not settled: whether any instanced prop should be in view at the spawn at all. `PS2X_VUJIT=0` looks the
  same at the spawn. Next: get a PCSX2 screenshot of a spot with crates/coins (or the start platform after the
  floor button) and repeat the RETFN / VUJIT=0 A/B there; then trace one instance block's matrix through the main
  program (`PS2X_VURUNTRACE`) to see where its vertices go (clipped? culled?).
- New debug switches: `PS2X_RETFN=<hex>[,...]` (those guest functions return at once, v0=0), `PS2X_FNLOGMAX=<n>`
  (per-call lines for `PS2X_FNLOG`; a0/a1 that point at C strings are printed as text), `PS2X_VUPCSTAT=1` (VU1
  runs and XGKICKs per entry byte address every 10 s). Note: entry 0xb0 is a setup program that never kicks by
  design.
- `drive_to_park.ps1` waits were tuned to the old too-fast game; it now uses real-time waits and one more cross
  before the dialog. If a run stalls on "Do you want to enter the park?", a press was lost in a menu transition
  (the game is fine; press cross by hand).

## References

- Game wiki (area/track names, hub layout, collectibles; useful for describing test spots and checking expected
  behaviour): https://crashbandicoot.fandom.com/wiki/Crash_Tag_Team_Racing
- GameFAQs PS2 guide (in-depth walkthrough: hub areas, collectible locations, progression):
  https://gamefaqs.gamespot.com/ps2/927300-crash-tag-team-racing/faqs/41187
- Crashmania game page: https://www.crashmania.net/en/games/crash-tag-team-racing/

## Things to watch

- VU1: CTTR's programs are JIT-compiled now (see Performance); a new VU1 image that matches none runs interpreted
  (correct, slower) -- `PS2X_VUMISSDUMP=1 PS2X_VUMICRO_DIR=<dir>` dumps it for gen_vu1.py.
- The launcher (`ps2xRuntime/src/launcher/`) is BT3/BT4-specific. Run the runner directly.
- BT3's fixes in `game_overrides.cpp` are keyed on `SLUS_216.78` and stay inactive. CTTR fixes go there, keyed on
  `SLES_534.39`.
- PAL: the game runs at 50 Hz.

## Future idea (not now): sync with upstream PS2Recomp (noted 2026-10-27)

- Upstream: https://github.com/ran-j/PS2Recomp (GPL-3.0). It's the base our tree
  is built on: this tree <- BT3_PC (c1b3r0s) <- PS2Recomp. Upstream has kept moving
  since our fork. A newer local clone is at `(a local clone)` (c5a9d02, 2026-09-30).
- Upstream has **ps2xIOP**, which we don't have: an R3000A IRX executor with a virtual
  IOP kernel and HLE fallbacks. It could replace or simplify our hand-made IOP pieces
  (CttrIop.cpp HLE, the AUDIO.IRX R3000 VM, the guest-lock handoff). Also new upstream:
  address bindings for stripped ELFs (`handler@0xADDRESS`), `ret0`/`ret1`/`reta0`
  return handlers, game override hooks, relocation-symbol auto-binding, and SYSCALL
  dispatch by encoded ID before `$v1`. Upstream's own README still calls VU/GS
  performance very poor, so keep our VU1/VU0 JIT, MAC pipe and paraLLEl-GS work.
- Not implemented yet. If we do it later: diff upstream's ps2xRecomp/ps2xRuntime against
  ours and cherry-pick piece by piece. Don't merge wholesale: our runtime has many CTTR
  and BT3 fixes, and a recompiler change means a regen (regen.ps1) or a full rebuild
  (~80 min, `--jobs 1-2`). Check each picked change with a boot to the hub plus the
  park A/B (ab_park.ps1).
- Also checked 2026-10-27: github.com/InconspicuousCactus/ps2-recompiler. **Not useful.**
  It's an early experiment (2 commits) with no graphics, sound or input, and no overlay
  support. It leans on debug symbols, but CTTR is stripped. It has no license, so we
  can't reuse its code. Its only test game (Shadow of the Colossus) crashes during
  overlay loading. Our tree is far ahead of it.

## CPU bottleneck pass (2026-11-02)

- **[maclive] VU1 MAC-flag liveness (gen_vu1.py `mac_dead_writers`)**: flag writes no FMEQ/FMAND/FMOR, exit or
  computed jump can observe are emitted as `jitUpper<W, false>` (6440 sites; 532/698 in 48f9f351). Mystery Island
  went from ~20 fps (196-202 per 10 s) to a locked 25 (250/10 s); KickWorker now ~33% idle there.
  Verify: vf/vi/acc/MAC/clip/data all exact; only `status` sticky bits differ (no CTTR VU1 program reads status --
  the generator keeps every writer live if one ever does).
- **[autolabel] + native MAX/MINI/flag ops + [jitbail]/[jitmiss]**: 0 verify mismatches in 4.5M runs; the only
  remaining miss (4725e062 0x2bd0) is in vu1img/jit_targets.txt.
- **[gamegate] PS2X_GSBUSYCLR was BT3-only logic running in CTTR**: it cleared bit 8 of guest word 0x1be0384 every
  64 main-thread branches (CTTR's 0x2ff10c = 0x7fbe0370). Now gated to BT3. The BT3 overlay-loop probe (locked add
  per call into 0x334c00-0x360000, hot CTTR code) is gated too.
- **Races are still ~20 fps**: EE-bound on the game thread, not VU1 (KickWorker ~66% idle). Per-thread profile with
  real names (`PS2X_EEPROF_SYM=1`): dispatchGuestBranch ~11%, ps2TraceGuestWrite ~5% (the inline store hook not
  inlined by clang-cl; fixing it means touching ps2_runtime.h = full game rebuild), heap alloc ~4%, io-register
  unordered_map ~2-3%. TODO: audit the other default-on PS2X_* switches for BT3 logic that is not game-gated.
- **Aspect ratio** (F1 > Display): Auto Standard / Fit to Window / 4:3 / 16:9 / 10:7, saved as video.aspect_mode;
  16:9 verified stretching to the window.

## Fog / god-ray flicker: FIXED (2026-11-04, `[efu]`, CONFIRMED in playtest)

- The light-shaft billboards (Fairy hub, Mystery Island bridge) are turned to face the camera by VU1 code (image B,
  ERLENG / ESUM / ESQRT around B:0x10f0-0x1230). The port's VU lower EFU ops ESADD, ERSADD, ESUM, ESQRT, ERSQRT,
  ESIN, EATAN/EATANxy/EATANxz and EEXP were empty, so P kept a stale value and the shaft quads got a wrong width
  that changed with whatever ran before: the haze flashed bright/dark. Implemented in ps2_vu1.cpp (`[efu]`).
  The EE side was clean (PS2X_GODRAY probe: world matrix unit scale every frame).

## Performance pass (2026-11-03/04, playtest: "greatly increased")

Races went from ~19.7 fps to a locked 25 (PS2 rate); hub on foot locked 25; loads 2-4 s; input read live (no queue).
- `[ioregfile]` (ps2_memory.cpp): I/O registers 0x10000000-0x1000FFFF in a dense array instead of an unordered_map.
- `[leandispatch]` (ps2_runtime.cpp dispatchGuestBranch): CTTR-only lean path when no probe env is set
  (PS2X_LEANDISPATCH=0 = old path).
- `[tracewrite]` (ps2_runtime.h): the inline store hook only does a 3-load check; the rest moved to
  ps2TraceGuestWriteSlow. `[ramfast]`: isSpecialAddress returns early for RAM addresses. (Header change = full
  rebuild; build with `-- -j6` on 16 GB, a parallel build was killed for low memory.)
- `[chainpool]`: DMA chain buffer reuse (no measurable gain, kept).
- log_level: 1 (the default) turns on PS2X_PROFILE / PS2X_MCLOG / PS2X_SCHED_DEBUG. Races: 24.3-24.7 fps with them,
  24.95 without (7 late frames in 1900). The tester's settings now have log_level = 0.
- Not needed: PS2X_ASYNC_SYNCRELAX=2 (game/worker overlap; texture hazard in BT3).
- Low priority: the in-hub "Driving Games" menu (race door 2) runs ~48 fps / 96% speed (menu targets 50).
  Measured 2026-11-04 (Level Options screen, 50 swaps/s): only at Internal resolution "2x + 8x Smoothing" (render_scale 3):
  paraLLEl-GS flush+vsync 7 ms per swap, 69 of 1754 frames late. Plain 2x: 2.1 ms, 0 late; Native: 0.6 ms, 0 late.
  So it is the GPU cost of 8 samples on that full-screen menu, not game code. Possible fix later: don't wait on the
  pgs flush/readback inside onSwap. Races at the same setting are 24.95/25 fps.
- Measuring: scratchpad perf_race.ps1 / perf_hub.ps1 on a save copy (<test dir>), PS2X_FRAMEPACE=1, fpstat.py.

## Crash's outfits reverting: FIXED (2026-11-04, `[japanoutfit]`, CONFIRMED in playtest)

- Not a save bug. The save keeps outfits (SkinManager section: 4 bytes per skin = driving / on foot unlocked,
  equipped driving / on foot; load 0x27a260, reset 0x27b020). The F1 "Japanese Crash outfit" switch was enforced every
  frame, but the game's equip routine (0x279e38) sets settings+0x20c to "Crash wears JapanDefault" whenever a Crash
  outfit is equipped for driving. Picking another Crash outfit cleared it and the cheat re-ran the code's handler
  (0x18c7a8), re-equipping JapanDefault (also at every boot from the saved toggle).
- Now: switching it on runs the code once; off equips crash/default for driving and on foot (0x27b168); the bit is
  never saved in cttr_toggles. Skin keys are the lowercase h*31 hash. Debug: PS2X_SKINDUMP=1 prints the outfit table.

