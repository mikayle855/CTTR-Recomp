#!/usr/bin/env python3
"""make_vu_images.py -- build the VU microcode images the static VU JIT is generated from, out of YOUR SLES_534.39.

    python games/cttr/make_vu_images.py <SLES_534.39> <out_dir> [--ov4-live <16 KB dump>]

Writes <out_dir>/*.bin and <out_dir>/specs.txt ("image:extent_hex" per line), the arguments for
ps2xRuntime/tools/gen_vu1.py, which turns them into ps2xRuntime/src/lib/vu1_jit_gen.inc (setup.py runs both).

How CTTR loads VU code (see docs/DEVELOPER_GUIDE.md, "VU1/VU0 microcode"):
  * VU1 main program: ELF .vutext at 0x506380, 0x2660 bytes, loaded at micro address 0.
  * 9 VU1 overlays, listed as (start, end) pairs in the table at 0x51c6c8, each loaded at micro 0x4cc
    (byte 0x2660) on top of the main program. Each image = main program + one overlay, matched over its own extent.
  * Overlay 4 (0x50ccb0) runs past its table end into code the game GENERATES at run time (a 12-pair block repeated
    with different fields). Only the part in the ELF can be built here; programs in the generated tail run on the
    VU1 interpreter (correct, slower). --ov4-live takes a 16 KB micro-memory dump of overlay 4 from a running game
    (e.g. the PS2X_VUMICRO=1 dumps) to cover the tail too.
  * VU0: one program, uploaded in two pieces: 0x800 bytes from ELF file offset 0x40f048 to micro 0, and 0x30e bytes
    from 0x40f850 to micro 0x800 (an 8-byte DMA tag sits between them in the file).
"""
import struct
import sys
from pathlib import Path

MAIN_VA, MAIN_SIZE = 0x506380, 0x2660
OVERLAY_TABLE, OVERLAYS = 0x51c6c8, 9
VU0_PIECES = ((0x40f048, 0x800), (0x40f850, 0x30e))   # file offsets (not addresses), sizes


def main() -> None:
    args = sys.argv[1:]
    live4 = None
    if '--ov4-live' in args:
        i = args.index('--ov4-live')
        live4 = Path(args[i + 1]).read_bytes()
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    elf = Path(args[0]).read_bytes()
    out = Path(args[1])
    out.mkdir(parents=True, exist_ok=True)
    phoff = struct.unpack_from('<I', elf, 0x1c)[0]
    segs = [struct.unpack_from('<IIIIII', elf, phoff + k * 32) for k in range(struct.unpack_from('<H', elf, 0x2c)[0])]

    def fo(va: int) -> int:
        for _t, off, v, _p, fs, _ms in segs:
            if v <= va < v + fs:
                return off + va - v
        raise ValueError(f'address 0x{va:x} is not in the ELF')

    def u32(va: int) -> int:
        return struct.unpack_from('<I', elf, fo(va))[0]

    main_prog = elf[fo(MAIN_VA):fo(MAIN_VA) + MAIN_SIZE]
    specs = []
    for k in range(OVERLAYS):
        start, end = u32(OVERLAY_TABLE + 8 * k), u32(OVERLAY_TABLE + 8 * k + 4)
        img = bytearray(0x4000)
        img[:MAIN_SIZE] = main_prog
        body = elf[fo(start):fo(start) + (end - start)]
        img[MAIN_SIZE:MAIN_SIZE + len(body)] = body
        extent = MAIN_SIZE + len(body)
        name = f'cttr_ov{k}_{start:x}.bin'
        (out / name).write_bytes(img)
        if k == 4:
            # [jitprefix] compiled below the table end only; the per-instance blocks the game uploads above it
            # (instancing: coins, crates, props) run on the interpreter's per-op path (vujit::jitRunOutside)
            specs.append(f'{(out / name).as_posix()}:4000@{extent:x}')
            if live4 is not None:
                if len(live4) != 0x4000 or live4[:extent] != bytes(img[:extent]):
                    sys.exit('--ov4-live: not a 16 KB overlay 4 image of this ELF')
                (out / 'cttr_ov4_live.bin').write_bytes(live4)
                specs.append(f'{(out / "cttr_ov4_live.bin").as_posix()}:4000')
            continue
        specs.append(f'{(out / name).as_posix()}:{extent:x}')
    vu0 = bytearray(0x1000)
    pos = 0
    for off, size in VU0_PIECES:
        vu0[pos:pos + size] = elf[off:off + size]
        pos += size
    (out / 'cttr_vu0.bin').write_bytes(vu0)
    specs.append(f'{(out / "cttr_vu0.bin").as_posix()}:1000')
    (out / 'specs.txt').write_text('\n'.join(specs) + '\n')
    print(f'{len(specs)} VU images in {out}')


if __name__ == '__main__':
    main()
