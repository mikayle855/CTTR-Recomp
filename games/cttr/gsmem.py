#!/usr/bin/env python3
"""Decode GS memory dumped with PS2X_PGS_VRAMRAW=<swap> (4 MB, swizzled) into PNGs.

The block/column tables are read from the runtime's own headers (ps2xRuntime/include/runtime/ps2_gs_psm*.h), so this
decodes exactly what the runtime's address functions describe.

  gsmem.py vram.bin ct32 <tbp hex> <tbw> <w> <h> out.png                 # a 32-bit buffer (FRAME fbp*32 = tbp)
  gsmem.py vram.bin t8   <tbp hex> <tbw> <w> <h> out.png <cbp hex>       # 8-bit indexed, CT32 CLUT (CSM1)
  gsmem.py vram.bin t4   <tbp hex> <tbw> <w> <h> out.png <cbp hex> [csa] # 4-bit indexed, CT32 CLUT (CSM1)
"""
import re
import sys
from pathlib import Path

from PIL import Image

HDR = Path(__file__).resolve().parents[2] / "ps2xRuntime" / "include" / "runtime"


def table(header, name):
    text = (HDR / header).read_text()
    m = re.search(name + r"\[(\d+)\]\[(\d+)\]\s*=\s*\{(.*?)\};", text, re.S)
    rows, cols = int(m.group(1)), int(m.group(2))
    nums = [int(v) for v in re.findall(r"\d+", m.group(3))]
    assert len(nums) == rows * cols, name
    return [nums[r * cols:(r + 1) * cols] for r in range(rows)]


BT32, CT32 = table("ps2_gs_psmct32.h", "blockTable32"), table("ps2_gs_psmct32.h", "columnTable32")
BT8, CT8 = table("ps2_gs_psmt8.h", "blockTable8"), table("ps2_gs_psmt8.h", "columnTable8")
BT4, CT4 = table("ps2_gs_psmt4.h", "blockTable4"), table("ps2_gs_psmt4.h", "columnTable4")
BT16, BT16S, CT16 = (table("ps2_gs_psmct16.h", "blockTable16"), table("ps2_gs_psmct16.h", "blockTable16S"),
                     table("ps2_gs_psmct16.h", "columnTable16"))
MASK = 4 * 1024 * 1024 - 1


def addr32(bp, bw, x, y):   # byte address
    page = (bp >> 5) + (y >> 5) * max(bw, 1) + (x >> 6)
    blk = (bp & 31) + BT32[(y >> 3) & 3][(x >> 3) & 7]
    return ((page << 13) + ((blk >> 5) << 13) + (blk & 31) * 256 + CT32[y & 7][x & 7] * 4) & MASK


def addr8(bp, bw, x, y):    # byte address
    page = (bp >> 5) + (y >> 6) * max(bw >> 1, 1) + (x >> 7)
    blk = (bp & 31) + BT8[(y >> 4) & 3][(x >> 4) & 7]
    return ((page << 13) + ((blk >> 5) << 13) + (blk & 31) * 256 + CT8[y & 15][x & 15]) & MASK


def addr4(bp, bw, x, y):    # nibble address
    page = (bp >> 5) + (y >> 7) * max(bw >> 1, 1) + (x >> 7)
    blk = (bp & 31) + BT4[(y >> 4) & 7][(x >> 5) & 3]
    return ((page << 14) + ((blk >> 5) << 14) + (blk & 31) * 512 + CT4[y & 15][x & 31]) & (MASK * 2 + 1)


def addr16(bp, bw, x, y, bt):   # byte address (PSMCT16 / PSMCT16S by block table)
    page = (bp >> 5) + (y >> 6) * max(bw, 1) + (x >> 6)
    blk = (bp & 31) + bt[(y >> 3) & 7][(x >> 4) & 3]
    return ((page << 13) + ((blk >> 5) << 13) + (blk & 31) * 256 + ((y >> 1) & 3) * 64 + CT16[y & 1][x & 15] * 2) & MASK


def word(v, a):
    return v[a] | (v[a + 1] << 8) | (v[a + 2] << 16) | (v[a + 3] << 24)


def rgba(w):
    a = (w >> 24) & 0xFF
    return (w & 0xFF, (w >> 8) & 0xFF, (w >> 16) & 0xFF, min(255, a * 2))


def clut(v, cbp, n, csa=0):
    out = []
    for i in range(n):
        if n == 256:   # CSM1, 16x16: bits 3 and 4 of the index swapped in x
            x, y = (i & 7) | ((i & 0x10) >> 1), (i >> 5) * 2 + ((i >> 3) & 1)
        else:          # 16 colours: 8x2, CSA picks the 16-entry slot
            j = i + csa * 16
            x, y = (j & 7) | ((j & 0x10) >> 1), (j >> 5) * 2 + ((j >> 3) & 1)
        out.append(rgba(word(v, addr32(cbp, 1, x, y))))
    return out


def main():
    v = Path(sys.argv[1]).read_bytes()
    kind, tbp, tbw, w, h, out = sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]), sys.argv[7]
    img = Image.new("RGBA", (w, h))
    px = img.load()
    if kind == "ct32":
        for y in range(h):
            for x in range(w):
                r, g, b, a = rgba(word(v, addr32(tbp, tbw, x, y)))
                px[x, y] = (r, g, b, 255)
    elif kind in ("ct16", "ct16s"):
        bt = BT16 if kind == "ct16" else BT16S
        for y in range(h):
            for x in range(w):
                a = addr16(tbp, tbw, x, y, bt)
                c = v[a] | (v[a + 1] << 8)
                px[x, y] = ((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3, 255)
    elif kind == "t8":
        pal = clut(v, int(sys.argv[8], 16), 256)
        for y in range(h):
            for x in range(w):
                px[x, y] = pal[v[addr8(tbp, tbw, x, y)]]
    elif kind == "t4":
        pal = clut(v, int(sys.argv[8], 16), 16, int(sys.argv[9]) if len(sys.argv) > 9 else 0)
        for y in range(h):
            for x in range(w):
                n = addr4(tbp, tbw, x, y)
                px[x, y] = pal[(v[n >> 1] >> ((n & 1) * 4)) & 0xF]
    img.save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
