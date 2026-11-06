#!/usr/bin/env python3
"""Shared build helpers for the game folders (games/<game>/setup.py, regen.py).

cmake configure/build, ISO extraction, hashing, syncing the generated sources into the runtime tree, deploying a
portable game folder. Inherited from the BT3 port this tree grew from (github.com/c1b3r0s/BT3_PC), where they lived in
games/bt3/setup.py.
"""
import hashlib
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
# Overridable so a container can generate into a build-local dir (PS2X_BUILD_DIR). Defaults to <repo>/build.
BUILD = Path(os.environ.get("PS2X_BUILD_DIR") or str(ROOT / "build"))
IS_WINDOWS = os.name == "nt"
IS_MACOS = sys.platform == "darwin"
# Generated TUs are huge; high job counts can exhaust RAM (16 GB: keep <= 3).
DEFAULT_JOBS = "3"


def die(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd, **kw) -> None:
    print("+ " + " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def find_extractor():
    # bsdtar reads ISO9660 directly; Windows 10+ ships it as tar.exe. Prefer that one on Windows: Git for Windows'
    # GNU tar can come first on PATH and cannot read disc images.
    if IS_WINDOWS:
        sys_tar = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "tar.exe"
        if sys_tar.is_file():
            return ("tar", str(sys_tar))
    for name in ("bsdtar", "tar"):
        exe = shutil.which(name)
        if exe:
            return ("tar", exe)
    for name in ("7z", "7za"):
        exe = shutil.which(name)
        if exe:
            return ("7z", exe)
    die("need bsdtar/tar (Windows 10+ ships tar.exe) or 7z to extract the ISO")


def make_writable(root: Path) -> None:
    # ISO9660 files extract read-only; the game opens some (e.g. BIN/DBZP.BIN) read-write.
    for p in root.rglob("*"):
        try:
            p.chmod(p.stat().st_mode | stat.S_IWRITE)
        except OSError:
            pass


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sync_tree(src: Path, dst: Path, exclude=()) -> None:
    """rsync -a --checksum --delete equivalent: copy changed files, remove extras."""
    dst.mkdir(parents=True, exist_ok=True)
    src_names = {p.name for p in src.iterdir() if p.is_file() and p.name not in exclude}
    for p in sorted(dst.iterdir()):
        if p.is_file() and p.name not in src_names:
            p.unlink()
    copied = 0
    for name in sorted(src_names):
        s, d = src / name, dst / name
        if not d.exists() or s.stat().st_size != d.stat().st_size or \
           sha256_of(s) != sha256_of(d):
            shutil.copyfile(s, d)
            copied += 1
    print(f"synced {src} -> {dst} ({copied} updated, {len(src_names)} total)")


def find_binary(name: str) -> Path:
    exe = name + (".exe" if IS_WINDOWS else "")
    hits = sorted(BUILD.rglob(exe))
    if not hits:
        die(f"{exe} not found under {BUILD} after build")
    return hits[0]


# Windows builds use the Clang toolset of the Visual Studio Build Tools ("C++ Clang Compiler for
# Windows" + "MSBuild support for LLVM (clang-cl) toolset" in the installer): the static VU1
# recompiler is computed-goto code and the runtime uses GCC/Clang builtins, which MSVC cannot
# compile. PS2X_SETUP_TOOLSET overrides (e.g. "v143" to try plain MSVC).
def cmake_configure_extra() -> list:
    """-T only makes sense for the Visual Studio generator; a Ninja build directory (clang-cl via
    -DCMAKE_CXX_COMPILER=clang-cl) must not get it, or the reconfigure fails."""
    extra = ["-DCMAKE_BUILD_TYPE=Release"]   # explicit: a Windows Ninja/clang-cl configure came up Debug (/Od /RTC1 -MDd)
    # ps2xStudio (the editor tool) fetches four git branches at configure time; a network hiccup there
    # aborted a user's whole game build (imgui_colortextedit populate failed, 2026-09-08). The game does
    # not need it: off unless PS2X_SETUP_STUDIO=1.
    extra.append("-DPS2X_BUILD_STUDIO=" + ("ON" if os.environ.get("PS2X_SETUP_STUDIO") == "1" else "OFF"))
    if IS_MACOS:
        extra += ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"]
        # [pgs] paraLLEl-GS does not build on macOS: its Granite dependency's
        # sleep_until_nsecs() falls back to clock_nanosleep/TIMER_ABSTIME for
        # everything that is not _WIN32, and macOS has neither. CMake enables the
        # backend whenever the submodule checkout exists, and setup.py fetches
        # submodules, so without this a fresh macOS configure breaks the build.
        # PS2X_SETUP_PGS=1 opts back in (needs MoltenVK and a patched Granite).
        if os.environ.get("PS2X_SETUP_PGS") != "1":
            extra.append("-DPS2X_DISABLE_PGS=ON")
        if os.environ.get("MACOSX_DEPLOYMENT_TARGET"):
            extra.append("-DCMAKE_OSX_DEPLOYMENT_TARGET=" + os.environ["MACOSX_DEPLOYMENT_TARGET"])
        if not (BUILD / "CMakeCache.txt").exists() and shutil.which("ninja"):
            extra += ["-G", "Ninja"]
    if not IS_WINDOWS:
        return extra
    cache = BUILD / "CMakeCache.txt"
    if cache.exists():
        gen = ""
        for line in cache.read_text(errors="replace").splitlines():
            if line.startswith("CMAKE_GENERATOR:"):
                gen = line.split("=", 1)[1]
                break
        if "Visual Studio" not in gen:
            return extra
    # Fresh configure. Prefer Ninja + clang-cl when this is a Visual Studio developer prompt (ninja and
    # clang-cl on PATH, VC environment loaded): Ninja compiles every file in parallel, whereas the default
    # Visual Studio generator hands the runner project's ~1000 unity units to MSBuild, which without
    # multi-processor compilation builds them one at a time (a 45-minute scratch build, 2026-09-08).
    # PS2X_SETUP_GENERATOR=vs forces the Visual Studio generator.
    dev_prompt = bool(os.environ.get("VCToolsInstallDir") or os.environ.get("INCLUDE"))
    if (os.environ.get("PS2X_SETUP_GENERATOR", "ninja").lower() != "vs" and dev_prompt
            and shutil.which("ninja") and shutil.which("clang-cl")):
        return extra + ["-G", "Ninja", "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]
    return extra + ["-T", os.environ.get("PS2X_SETUP_TOOLSET", "ClangCL")]


def configured() -> bool:
    """True when the build dir holds a COMPLETED configure: CMakeCache.txt alone is not enough --
    a configure that failed half-way (a FetchContent download error) leaves the cache behind with no
    project files, and `cmake --build` then dies with "MSB1009: Project file does not exist"."""
    if not (BUILD / "CMakeCache.txt").exists():
        return False
    cache_text = (BUILD / "CMakeCache.txt").read_text(errors="replace")
    if IS_MACOS and os.environ.get("MACOSX_DEPLOYMENT_TARGET"):
        desired = os.environ["MACOSX_DEPLOYMENT_TARGET"]
        if not any(line.startswith("CMAKE_OSX_DEPLOYMENT_TARGET:") and line.endswith("=" + desired)
                   for line in cache_text.splitlines()):
            return False
    if IS_MACOS:
        # [pgs] A cache configured before the parallel-gs submodule was fetched
        # has PS2X_DISABLE_PGS=OFF and no PGS targets; reusing it silently keeps
        # the backend enabled on the next build and fails to compile Granite.
        desired_pgs = "OFF" if os.environ.get("PS2X_SETUP_PGS") == "1" else "ON"
        if not any(line.startswith("PS2X_DISABLE_PGS:") and line.endswith("=" + desired_pgs)
                   for line in cache_text.splitlines()):
            return False
    if any((BUILD / f).exists() for f in ("build.ninja", "Makefile", "ALL_BUILD.vcxproj")):
        return True
    return any(BUILD.glob("*.sln"))


def cmake_build(target: str, jobs: str) -> None:
    cmd = ["cmake", "--build", BUILD, "--target", target, "-j", jobs]
    if IS_WINDOWS:
        cmd += ["--config", "Release"]  # multi-config generators (Visual Studio)
    run(cmd)


def copytree_overlay(src: Path, dst: Path) -> None:
    """copy_tree-like that overwrites instead of failing on existing dirs."""
    if src.is_dir():
        dst.mkdir(parents=True, exist_ok=True)
        for child in src.iterdir():
            copytree_overlay(child, dst / child.name)
    elif src.is_file():
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def deploy_tree(runner: Path, out: Path) -> None:
    """Assemble the portable tree in OUT.

    layout: OUT/savedata/ (settings.toml; existing user saves are preserved),
    OUT/assets/ (fonts). No game data is deployed: the launcher's install wizard
    extracts SLUS_216.78 + BIN/ DATA/ IRX/ SYSTEM.CNF from the user's own ISO
    into OUT/data on first run.

    Windows additionally copies the runtime DLLs next to the runner. 
    """
    print(f"== assembling deploy tree in {out}")
    out.mkdir(parents=True, exist_ok=True)

    # settings: default settings.toml only if none deployed yet (user keeps their saves)
    save_dst = out / "savedata"
    save_dst.mkdir(parents=True, exist_ok=True)
    cfg_src = runner.parent / "settings.toml"
    cfg_dst = save_dst / "settings.toml"
    if cfg_src.exists() and not cfg_dst.exists():
        shutil.copy2(cfg_src, cfg_dst)
        print(f"  copied default settings -> {cfg_dst}")

    # fonts + overlay assets
    for a in ("assets",):
        src = runner.parent / a
        if src.exists():
            copytree_overlay(src, out / a)

    # runtime DLLs (Windows) live next to the runner in the build dir; copy them
    # too so the tree is self-contained. build_and_deploy.sh replaces the runner
    # with the self-extracting payload ELF on Linux; here we always place it.
    if IS_WINDOWS:
        for p in runner.parent.glob("*.dll"):
            shutil.copy2(p, out / p.name)
    shutil.copy2(runner, out / runner.name)
    shutil.copymode(runner, out / runner.name)
    print(f"  runner -> {out / runner.name}")
    print(f"Deploy tree ready: {out}")
