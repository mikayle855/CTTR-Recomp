#!/usr/bin/env python3
"""Regenerate the runner sources after a recompiler change, touching only the files whose text changed.

setup.py --gen-only does the same but always rewrites the two generated headers, which every generated source
includes, so the next build recompiles all ~23k functions (hours). Here the headers are copied only when their
content differs. Run inside the VS environment (games\\cttr\\regen.ps1 does that), then build with
build.ps1 --skip-setup.
"""
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "common"))
import build_common as bt3  # noqa: E402

ROOT = HERE.parent.parent
WORK = HERE / "work"


def main() -> None:
    config = WORK / "config.toml"
    if not config.exists():
        bt3.die(f"{config} missing: run build.ps1 \"<iso>\" --gen-only once first")
    print("== building recompiler")
    bt3.cmake_build("ps2_recomp", "2")
    recomp = bt3.find_binary("ps2_recomp")
    out = WORK / "output"
    if out.exists():
        shutil.rmtree(out)
    print("== generating runner sources")
    bt3.run([recomp, config])
    rt = ROOT / "ps2xRuntime"
    bt3.sync_tree(out, rt / "src" / "runner", exclude=("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"))
    for h in ("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"):
        src, dst = out / h, rt / "include" / h
        if not dst.exists() or bt3.sha256_of(src) != bt3.sha256_of(dst):
            shutil.copyfile(src, dst)
            print(f"updated {dst.name}")


if __name__ == "__main__":
    main()
