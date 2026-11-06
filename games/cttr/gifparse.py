#!/usr/bin/env python3
"""Decode a GIF capture written by the runner with PS2X_PGS_GIFCAP=<swap>[,<count>].

The file is a run of records {u32 path, u32 bytes, bytes of GIF data}. This walks the GIFtags, tracks the GS
registers that decide where a primitive lands (PRIM, XYOFFSET, FRAME, SCISSOR, TEST, ZBUF, PRMODECONT) and
prints a summary of the drawing kicks:

  gifparse.py gifcap_3000.bin [--kicks N] [--regs]

--kicks N   also list the first N vertex kicks with their window coordinates
--regs      list every A+D register write
"""
import argparse
import collections
import struct

REGNAMES = {
    0x00: "PRIM", 0x01: "RGBAQ", 0x02: "ST", 0x03: "UV", 0x04: "XYZF2", 0x05: "XYZ2", 0x06: "TEX0_1", 0x07: "TEX0_2",
    0x08: "CLAMP_1", 0x09: "CLAMP_2", 0x0A: "FOG", 0x0C: "XYZF3", 0x0D: "XYZ3", 0x14: "TEX1_1", 0x15: "TEX1_2",
    0x16: "TEX2_1", 0x17: "TEX2_2", 0x18: "XYOFFSET_1", 0x19: "XYOFFSET_2", 0x1A: "PRMODECONT", 0x1B: "PRMODE",
    0x1C: "TEXCLUT", 0x22: "SCANMSK", 0x34: "MIPTBP1_1", 0x35: "MIPTBP1_2", 0x36: "MIPTBP2_1", 0x37: "MIPTBP2_2",
    0x3B: "TEXA", 0x3D: "FOGCOL", 0x3F: "TEXFLUSH", 0x40: "SCISSOR_1", 0x41: "SCISSOR_2", 0x42: "ALPHA_1",
    0x43: "ALPHA_2", 0x44: "DIMX", 0x45: "DTHE", 0x46: "COLCLAMP", 0x47: "TEST_1", 0x48: "TEST_2", 0x49: "PABE",
    0x4A: "FBA_1", 0x4B: "FBA_2", 0x4C: "FRAME_1", 0x4D: "FRAME_2", 0x4E: "ZBUF_1", 0x4F: "ZBUF_2",
    0x50: "BITBLTBUF", 0x51: "TRXPOS", 0x52: "TRXREG", 0x53: "TRXDIR", 0x54: "HWREG", 0x60: "SIGNAL",
    0x61: "FINISH", 0x62: "LABEL",
}
PRIMNAMES = ["point", "line", "linestrip", "tri", "tristrip", "trifan", "sprite", "prim7"]
VERTS_PER_PRIM = [1, 2, 2, 3, 3, 3, 2, 1]   # vertices before the first kick draws


class Gs:
    def __init__(self, args):
        self.args = args
        self.r = collections.defaultdict(int)
        self.prmodecont = 1
        self.vq = 0                      # vertices queued since PRIM
        self.kicks = 0
        self.listed = 0
        self.stats = collections.Counter()
        self.inside = collections.Counter()
        self.xs = collections.defaultdict(list)
        self.ys = collections.defaultdict(list)
        self.regwrites = collections.Counter()
        self.values = collections.defaultdict(collections.Counter)
        self.region = [float(v) for v in args.region.split(",")] if args.region else None
        if args.offset:   # XYOFFSET set before the capture began: seed both contexts' (in pixels)
            ox, oy = (int(float(v) * 16) for v in args.offset.split(","))
            self.r[0x18] = self.r[0x19] = ox | (oy << 32)
        self.inRegion = collections.Counter()
        self.regionRgba = collections.defaultdict(collections.Counter)   # TEX0 -> RGBAQ (r|g<<8|b<<16|a<<24) counts

    def prim(self):
        if self.r[0x1A] & 1:   # PRMODECONT.AC = 1: attributes from PRIM
            return self.r[0x00]
        return (self.r[0x00] & 7) | (self.r[0x1B] & ~7)

    def write(self, reg, val, kick=True):
        self.regwrites[reg] += 1
        if reg in (0x18, 0x19, 0x40, 0x41, 0x4C, 0x4D, 0x4E, 0x4F, 0x47, 0x48, 0x1A, 0x00, 0x42, 0x43, 0x50, 0x52, 0x53):
            self.values[reg][val] += 1
        if self.args.regs:
            print(f"  {REGNAMES.get(reg, hex(reg)):<11} {val:016x}")
        if reg == 0x00:
            self.vq = 0
        if reg in (0x04, 0x05, 0x0C, 0x0D):
            self.r[reg] = val
            self.vertex(val, drawing=kick and reg in (0x04, 0x05))
            return
        self.r[reg] = val

    def vertex(self, val, drawing):
        p = self.prim()
        ptype = p & 7
        ctx = (p >> 9) & 1
        self.vq += 1
        if not drawing or self.vq < VERTS_PER_PRIM[ptype]:
            return
        if ptype in (0, 1, 3, 6):   # list types restart
            self.vq = 0
        x = val & 0xFFFF
        y = (val >> 16) & 0xFFFF
        off = self.r[0x18 + ctx]
        ofx, ofy = off & 0xFFFF, (off >> 32) & 0xFFFF
        wx, wy = (x - ofx) / 16.0, (y - ofy) / 16.0
        sc = self.r[0x40 + ctx]
        x0, x1, y0, y1 = sc & 0x7FF, (sc >> 16) & 0x7FF, (sc >> 32) & 0x7FF, (sc >> 48) & 0x7FF
        frame = self.r[0x4C + ctx]
        fbp = frame & 0x1FF
        key = (ctx, fbp, PRIMNAMES[ptype], bool((p >> 4) & 1))   # TME
        self.stats[key] += 1
        if x0 <= wx <= x1 and y0 <= wy <= y1:
            self.inside[key] += 1
        if self.region and self.region[0] <= wx <= self.region[2] and self.region[1] <= wy <= self.region[3]:
            tme = (p >> 4) & 1
            self.inRegion[(ctx, fbp, PRIMNAMES[ptype], tme, self.r[0x06 + ctx] if tme else 0, self.r[0x42 + ctx], (p >> 6) & 1)] += 1
            self.regionRgba[self.r[0x06 + ctx] if tme else 0][self.r[0x01]] += 1
        if len(self.xs[key]) < 200000:
            self.xs[key].append(wx)
            self.ys[key].append(wy)
        self.kicks += 1
        if self.listed < self.args.kicks:
            self.listed += 1
            print(f"  kick#{self.kicks} ctx{ctx} {PRIMNAMES[ptype]} tme={(p >> 4) & 1} fbp=0x{fbp:x} raw=({x / 16:.1f},{y / 16:.1f}) "
                  f"off=({ofx / 16:.1f},{ofy / 16:.1f}) win=({wx:.1f},{wy:.1f}) scissor=({x0}..{x1},{y0}..{y1}) "
                  f"test={self.r[0x47 + ctx]:x} zbuf={self.r[0x4E + ctx]:x}")


def packed(gs, reg, lo, hi):
    if reg == 0x0:      # PRIM
        gs.write(0x00, lo & 0x7FF)
    elif reg == 0x1:    # RGBAQ
        r, g, b, a = lo & 0xFF, (lo >> 32) & 0xFF, hi & 0xFF, (hi >> 32) & 0xFF
        gs.write(0x01, r | (g << 8) | (b << 16) | (a << 24))
    elif reg == 0x2:
        gs.write(0x02, lo)
    elif reg == 0x3:    # UV
        gs.write(0x03, (lo & 0x3FFF) | (((lo >> 32) & 0x3FFF) << 16))
    elif reg == 0x4:    # XYZF2 (ADC = bit 111)
        x, y = lo & 0xFFFF, (lo >> 32) & 0xFFFF
        z, f = (hi >> 4) & 0xFFFFFF, (hi >> 36) & 0xFF
        adc = (hi >> 47) & 1
        gs.write(0x0C if adc else 0x04, x | (y << 16) | (z << 32) | (f << 56), kick=not adc)
    elif reg == 0x5:    # XYZ2
        x, y = lo & 0xFFFF, (lo >> 32) & 0xFFFF
        adc = (hi >> 47) & 1
        z = hi & 0xFFFFFFFF
        gs.write(0x0D if adc else 0x05, x | (y << 16) | (z << 32), kick=not adc)
    elif reg in (0x6, 0x7, 0x8, 0x9):
        gs.write(reg, lo)
    elif reg == 0xA:
        gs.write(0x0A, ((hi >> 36) & 0xFF) << 56)
    elif reg in (0xC, 0xD):
        gs.write(reg, (lo & 0xFFFF) | (((lo >> 32) & 0xFFFF) << 16), kick=False)
    elif reg == 0xE:    # A+D
        gs.write(hi & 0xFF, lo)


def walk(gs, data, path, problems):
    off = 0
    n = len(data)
    while off + 16 <= n:
        lo, hi = struct.unpack_from("<QQ", data, off)
        off += 16
        nloop = lo & 0x7FFF
        pre = (lo >> 46) & 1
        prim = (lo >> 47) & 0x7FF
        flg = (lo >> 58) & 3
        nreg = (lo >> 60) & 0xF or 16
        regs = [(hi >> (4 * i)) & 0xF for i in range(nreg)]
        gs.stats[("tag", path, ["PACKED", "REGLIST", "IMAGE", "IMAGE"][flg])] += 1
        if pre and flg != 2:
            gs.write(0x00, prim)
        if flg == 0:
            need = nloop * nreg * 16
            if off + need > n:
                problems.append(f"path{path}: PACKED tag overruns its transfer ({need} > {n - off})")
                return
            for _ in range(nloop):
                for reg in regs:
                    dlo, dhi = struct.unpack_from("<QQ", data, off)
                    off += 16
                    packed(gs, reg, dlo, dhi)
        elif flg == 1:
            words = nloop * nreg
            need = ((words + 1) // 2) * 16
            if off + need > n:
                problems.append(f"path{path}: REGLIST tag overruns its transfer")
                return
            i = 0
            for _ in range(nloop):
                for reg in regs:
                    (v,) = struct.unpack_from("<Q", data, off + 8 * i)
                    i += 1
                    if reg < 0xE:
                        gs.write(reg, v)
            off += need
        else:
            gs.stats[("imageqw", path)] += nloop
            off += nloop * 16


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--kicks", type=int, default=0)
    ap.add_argument("--regs", action="store_true")
    ap.add_argument("--offset", help="x,y XYOFFSET in pixels when the capture does not set it (e.g. 1728,1824)")
    ap.add_argument("--region", help="x0,y0,x1,y1 in window pixels: list the textures/prims drawn there")
    args = ap.parse_args()
    raw = open(args.capture, "rb").read()
    gs = Gs(args)
    problems = []
    pos = 0
    nrec = collections.Counter()
    while pos + 8 <= len(raw):
        path, size = struct.unpack_from("<II", raw, pos)
        pos += 8
        nrec[path] += 1
        walk(gs, raw[pos:pos + size], path, problems)
        pos += size
    print("transfers per path:", dict(nrec))
    for p in problems[:20]:
        print("PROBLEM", p)
    print("tags:", {k: v for k, v in gs.stats.items() if k[0] in ("tag", "imageqw")})
    print("\nvertex kicks by (ctx, fbp, prim, tme): count, inside scissor, window x/y range (5th..95th pct)")
    for key, cnt in sorted(((k, v) for k, v in gs.stats.items() if k[0] in (0, 1)), key=lambda kv: -kv[1]):
        xs, ys = sorted(gs.xs[key]), sorted(gs.ys[key])
        pc = lambda a, q: a[min(len(a) - 1, int(q * len(a)))]
        print(f"  ctx{key[0]} fbp=0x{key[1]:x} {key[2]:<9} tme={int(key[3])}: {cnt:7d} kicks, inside {gs.inside[key]:7d} | "
              f"x {pc(xs, .05):.0f}..{pc(xs, .95):.0f} (min {xs[0]:.0f} max {xs[-1]:.0f}) "
              f"y {pc(ys, .05):.0f}..{pc(ys, .95):.0f} (min {ys[0]:.0f} max {ys[-1]:.0f})")
    if gs.region:
        print("\nkicks inside region by (ctx, fbp, prim, tme, TEX0, ALPHA, ABE):")
        for (ctx, fbp, prim, tme, tex0, alpha, abe), n in gs.inRegion.most_common():
            tbp, tbw, psm = tex0 & 0x3FFF, (tex0 >> 14) & 0x3F, (tex0 >> 20) & 0x3F
            tw, th = 1 << ((tex0 >> 26) & 0xF), 1 << ((tex0 >> 30) & 0xF)
            cbp, cpsm, csm, csa, cld = (tex0 >> 37) & 0x3FFF, (tex0 >> 51) & 0xF, (tex0 >> 55) & 1, (tex0 >> 56) & 0x1F, (tex0 >> 61) & 7
            print(f"  {n:6d}  ctx{ctx} fbp=0x{fbp:x} {prim:<8} tme={tme} abe={abe} alpha={alpha:x} | tbp=0x{tbp:x} tbw={tbw} psm=0x{psm:x} "
                  f"{tw}x{th} tcc={(tex0 >> 34) & 1} tfx={('mod', 'decal', 'hl', 'hl2')[(tex0 >> 35) & 3]} "
                  f"cbp=0x{cbp:x} cpsm=0x{cpsm:x} csm={csm} csa={csa} cld={cld}")
            print("          RGBA " + "  ".join(f"{v & 0xFF},{(v >> 8) & 0xFF},{(v >> 16) & 0xFF},{v >> 24} x{c}"
                                              for v, c in gs.regionRgba[tex0 if tme else 0].most_common(4)))
    print("\nregister values (most frequent):")
    for reg in sorted(gs.values):
        top = gs.values[reg].most_common(6)
        print(f"  {REGNAMES.get(reg, hex(reg)):<11} " + "  ".join(f"{v:x} x{c}" for v, c in top))


if __name__ == "__main__":
    main()
