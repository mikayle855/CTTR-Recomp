# games/cttr — Crash Tag Team Racing (PAL) build inputs and scripts

Target: *Crash Tag Team Racing* (PS2, Europe/Australia, SLES-53439, v1.01, PAL), built on PS2Recomp (via the BT3 port's runtime). It runs the game's own code; PC settings and mods (off by default) are layered on top in the runtime.
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

## More

- Overview, game addresses, runtime map, debugging/test tools, open ends, mod starting points:
  [`docs/DEVELOPER_GUIDE.md`](../../docs/DEVELOPER_GUIDE.md).
- The full working log (every fix, how it was found): [`docs/DEVLOG.md`](../../docs/DEVLOG.md).

## Files here

| File | What |
| --- | --- |
| `build.ps1`, `setup.py`, `regen.ps1`/`regen.py` | Build from the ISO; regenerate after a recompiler change. |
| `functions.csv`, `config.toml.in`, `make_config.py`, `find_code_pointers.py` | The function map and recompiler config (and how they were made). |
| `make_vu_images.py`, `jit_targets.txt` | VU microcode images from the ELF for the static VU JIT (`ps2xRuntime/tools/gen_vu1.py`). |
| `launcher/` | The no-console launcher (`CTTR.exe`): portable layout, save backups, crash notice. |
| `make_portable.ps1` | Assemble a self-contained game folder (`dist/CTTR`). |
| `run.ps1` | Timed test run with extra env vars, keeps the log. |
| `press.ps1`, `drive_to_park.ps1`, `drive_to_race.ps1`, `ab_park.ps1`, `ab_race.ps1`, `burst.ps1`, `shot.ps1` | Scripted tests through the pad pipe (`PS2X_PADPIPE`); expect a test folder `C:\CTTR_test` and the repo at `C:\CTTR-Recomp`. |
| `pcsx2_ctl.ps1` | Drive PCSX2 for reference comparisons. |
| `gifparse.py`, `gsmem.py`, `vudis.py`, `patch_mmiperm.py` | GS stream decoder, VRAM texture decoder, VU disassembler, a one-off recompiler patch helper. |
