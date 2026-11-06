#!/usr/bin/env python3
"""Build Crash Tag Team Racing (SLES_534.39, Europe/Australia, PAL, v1.01).

    python games/cttr/setup.py <iso|elf> [--jobs N] [--deploy OUT] [--skip-setup] [--gen-only]

Same pipeline as the BT3 port's setup: extract + verify the ELF, build the recompiler, generate the runner sources,
an empty overlay table (CTTR has no code overlays), the VU JIT from the ELF's microcode, then build the runner.
The game's code is generated locally from YOUR copy of the game; nothing from the game is in this repository.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
WORK = HERE / "work"
BOOT = "SLES_534.39"
ELF_SHA256 = "8c0816196d16c76470f4443a49f24148873cfb68becb27ba25f461af8280eec2"

# Shared build helpers (cmake configure/build, extraction, deploy): games/common/build_common.py.
sys.path.insert(0, str(ROOT / "games" / "common"))
import build_common as bt3  # noqa: E402  (kept the old module alias: the calls below read bt3.<helper>)
BUILD, IS_WINDOWS, DEFAULT_JOBS = bt3.BUILD, bt3.IS_WINDOWS, bt3.DEFAULT_JOBS
die, run, sha256_of, make_writable = bt3.die, bt3.run, bt3.sha256_of, bt3.make_writable
from split_functions import split_csv  # noqa: E402


def write_config(elf: Path, csv: Path, out: Path) -> Path:
    text = (HERE / "config.toml.in").read_text()
    for key, val in (("input", elf.as_posix()), ("output", out.as_posix() + "/"), ("ghidra_output", csv.as_posix())):
        text, n = re.subn(rf'^{key}\s*=.*$', f'{key} = "{val}"', text, count=1, flags=re.M)
        if not n:
            die(f"config.toml.in has no '{key} =' line")
    cfg = WORK / "config.toml"
    cfg.write_text(text)
    return cfg


def main() -> None:
    ap = argparse.ArgumentParser(description="Build (and optionally deploy) Crash Tag Team Racing.")
    ap.add_argument("src", nargs="?", metavar="<iso|elf>",
                    help=f"CTTR PAL ISO or bare {BOOT} ELF (not needed with --skip-setup)")
    ap.add_argument("--jobs", default=DEFAULT_JOBS, metavar="N",
                    help=f"parallel jobs for the ps2EntryRunner build (default {DEFAULT_JOBS})")
    ap.add_argument("--deploy", metavar="OUT", help="after the build, copy the playable tree into OUT")
    ap.add_argument("--skip-setup", action="store_true",
                    help="skip ISO/recompile; only rebuild the runner (+deploy)")
    ap.add_argument("--gen-only", action="store_true",
                    help="stop after generating the runner sources; skip building the runner")
    args = ap.parse_args()
    if args.skip_setup and args.gen_only:
        die("--skip-setup and --gen-only are mutually exclusive")

    elf = WORK / BOOT
    if args.skip_setup:
        if not elf.is_file():
            die(f"--skip-setup requires an existing games/cttr/work/ (no {BOOT} found)")
    else:
        if not args.src:
            ap.print_usage(sys.stderr)
            die(f"missing the CTTR ISO or {BOOT} ELF path")
        src = Path(args.src).resolve()
        if not src.exists():
            die(f"{src} does not exist")
        for f in ("functions.csv", "config.toml.in"):
            if not (HERE / f).is_file():
                die(f"games/cttr/{f} is missing: export it from Ghidra first (see games/cttr/README.md)")
        WORK.mkdir(parents=True, exist_ok=True)

        # 1. Only the boot ELF is needed to build; the RCF archives are read at run time.
        if src.suffix.lower() == ".iso":
            kind, exe = bt3.find_extractor()
            print(f"== extracting {BOOT} from the ISO with {exe}")
            if kind == "tar":
                run([exe, "-xf", src, "-C", WORK, BOOT])
            else:
                run([exe, "x", "-y", f"-o{WORK}", src, BOOT], stdout=subprocess.DEVNULL)
        else:
            shutil.copyfile(src, elf)
        if not elf.is_file():
            die(f"{BOOT} not found in the ISO (is this the Europe/Australia release?)")
        make_writable(WORK)

        # 2. Verify it is the expected PAL ELF.
        got = sha256_of(elf)
        if got != ELF_SHA256:
            print(f"ERROR: ELF sha256 mismatch.\n  expected: {ELF_SHA256}\n  got:      {got}")
            print("Only Crash Tag Team Racing Europe/Australia (SLES-53439) is supported. "
                  "Set PS2X_SETUP_FORCE=1 to continue anyway.")
            if os.environ.get("PS2X_SETUP_FORCE") != "1":
                sys.exit(1)

        # 3. Build the recompiler (same submodule + configure rules as BT3).
        if not os.environ.get("PS2X_SETUP_NO_SUBMODULES") and (ROOT / ".git").exists() and shutil.which("git"):
            try:
                run(["git", "-C", ROOT, "submodule", "update", "--init", "--recursive"])
            except Exception as e:   # noqa: BLE001
                print(f"== submodule fetch failed ({e}); building without the paraLLEl-GS backend")
        print("== building recompiler")
        if not bt3.configured():
            run(["cmake", "-S", ROOT, "-B", BUILD] + bt3.cmake_configure_extra())
        bt3.cmake_build("ps2_recomp", str(os.cpu_count() or 4))
        recomp = bt3.find_binary("ps2_recomp")

        # 4. Generate the runner sources.
        print("== generating runner sources")
        split = WORK / "functions_split.csv"
        split_csv(elf, HERE / "functions.csv", split)
        out = WORK / "output"
        if out.exists():
            shutil.rmtree(out)
        run([recomp, write_config(elf, split, out)])

        # 5. Install into the runtime tree. CTTR has no overlay module, but the runtime links against
        #    BT3's overlay function table: give it an empty one (slot count 0 = every lookup misses).
        print("== installing runner sources")
        rt = ROOT / "ps2xRuntime"
        bt3.sync_tree(out, rt / "src" / "runner",
                      exclude=("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"))
        for h in ("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"):
            shutil.copyfile(out / h, rt / "include" / h)
        overlay = rt / "src" / "runner_overlay"
        overlay.mkdir(parents=True, exist_ok=True)
        for p in overlay.glob("*.cpp"):
            p.unlink()
        (overlay / "overlay_register.cpp").write_text(
            '#include "ps2_runtime.h"\n\n'
            "// Generated by games/cttr/setup.py: CTTR has no overlay module.\n"
            "extern const uint32_t g_ps2OverlayFunctionTableBase = 0u;\n"
            "extern const uint32_t g_ps2OverlayFunctionTableEnd = 0u;\n"
            "extern const uint32_t g_ps2OverlayFunctionTableSlotCount = 0u;\n"
            "PS2Runtime::RecompiledFunction g_ps2OverlayFunctionTable[1] = {};\n")

        # 5b. The static VU1/VU0 JIT (ps2xRuntime/src/lib/vu1_jit_gen.inc): the game's VU microcode, cut out of the
        #     ELF by make_vu_images.py, translated by gen_vu1.py. Without it every VU program runs on the interpreter
        #     (races ~3-4x slower). PS2X_OV4_LIVE=<16 KB dump> adds the run-time-generated tail of overlay 4.
        print("== generating the VU JIT")
        vu_img = WORK / "vu_images"
        cmd = [sys.executable, HERE / "make_vu_images.py", elf, vu_img]
        if os.environ.get("PS2X_OV4_LIVE"):
            cmd += ["--ov4-live", os.environ["PS2X_OV4_LIVE"]]
        run(cmd)
        specs = (vu_img / "specs.txt").read_text().split()
        run([sys.executable, rt / "tools" / "gen_vu1.py", rt / "src" / "lib" / "vu1_jit_gen.inc"] + specs, cwd=HERE)

    if args.gen_only:
        print("--gen-only: runner sources generated (skipping runner build)")
        return

    # 6. Build the game.
    if not bt3.configured():
        run(["cmake", "-S", ROOT, "-B", BUILD] + bt3.cmake_configure_extra())
    else:
        run(["cmake", "-S", ROOT, "-B", BUILD])   # the runner file set changed: re-glob
    print(f"== building ps2EntryRunner (-j{args.jobs}, this takes a while)")
    bt3.cmake_build("ps2EntryRunner", str(args.jobs))
    runner = bt3.find_binary("ps2EntryRunner")

    if args.deploy:
        bt3.deploy_tree(runner, Path(args.deploy).resolve())
        return
    print(f"""
Done. Run with (cmd.exe):

  cd {runner.parent}
  set PS2X_CD_IMAGE=<path to your CTTR ISO>
  {runner} {elf}
""")


if __name__ == "__main__":
    main()
