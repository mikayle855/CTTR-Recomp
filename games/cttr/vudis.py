#!/usr/bin/env python3
"""Disassemble VU micro code (upper/lower pairs).

  vudis.py <code.bin> [start hex] [count] [--offset N]

<code.bin> is raw micro memory (VU1 16 KB, VU0 4 KB); --offset skips a header (e.g. a [bigkick] dump: the code
follows the 32-byte header, VF/VI registers and the data memory).
"""
import argparse
import struct

BC = "xyzw"
UPPER = {}
for i, n in enumerate(["ADD", "SUB", "MADD", "MSUB", "MAX", "MINI", "MUL"]):
    for b in range(4):
        UPPER[i * 4 + b] = (n, b)
UPPER.update({0x1C: ("MULq", None), 0x1D: ("MAXi", None), 0x1E: ("MULi", None), 0x1F: ("MINIi", None),
              0x20: ("ADDq", None), 0x21: ("MADDq", None), 0x22: ("ADDi", None), 0x23: ("MADDi", None),
              0x24: ("SUBq", None), 0x25: ("MSUBq", None), 0x26: ("SUBi", None), 0x27: ("MSUBi", None),
              0x28: ("ADD", None), 0x29: ("MADD", None), 0x2A: ("MUL", None), 0x2B: ("MAX", None),
              0x2C: ("SUB", None), 0x2D: ("MSUB", None), 0x2E: ("OPMSUB", None), 0x2F: ("MINI", None)})
USPEC = {}
for i, n in enumerate(["ADDA", "SUBA", "MADDA", "MSUBA"]):
    for b in range(4):
        USPEC[i * 4 + b] = (n, b)
for b, n in enumerate(["ITOF0", "ITOF4", "ITOF12", "ITOF15"]):
    USPEC[0x10 + b] = (n, None)
for b, n in enumerate(["FTOI0", "FTOI4", "FTOI12", "FTOI15"]):
    USPEC[0x14 + b] = (n, None)
for b in range(4):
    USPEC[0x18 + b] = ("MULA", b)
USPEC.update({0x1C: ("MULAq", None), 0x1D: ("ABS", None), 0x1E: ("MULAi", None), 0x1F: ("CLIP", None),
              0x20: ("ADDAq", None), 0x21: ("MADDAq", None), 0x22: ("ADDAi", None), 0x23: ("MADDAi", None),
              0x24: ("SUBAq", None), 0x25: ("MSUBAq", None), 0x26: ("SUBAi", None), 0x27: ("MSUBAi", None),
              0x28: ("ADDA", None), 0x29: ("MADDA", None), 0x2A: ("MULA", None), 0x2C: ("SUBA", None),
              0x2D: ("MSUBA", None), 0x2E: ("OPMULA", None), 0x2F: ("NOP", None)})
LOWER = {0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU", 0x10: "FCEQ",
         0x11: "FCSET", 0x12: "FCAND", 0x13: "FCOR", 0x14: "FSEQ", 0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR",
         0x18: "FMEQ", 0x1A: "FMAND", 0x1B: "FMOR", 0x1C: "FCGET", 0x20: "B", 0x21: "BAL", 0x24: "JR",
         0x25: "JALR", 0x28: "IBEQ", 0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ"}
LSPEC1 = {0x30: "IADD", 0x31: "ISUB", 0x32: "IADDI", 0x34: "IAND", 0x35: "IOR"}
LSPEC2 = {0x30: "MOVE", 0x31: "MR32", 0x34: "LQI", 0x35: "SQI", 0x36: "LQD", 0x37: "SQD", 0x38: "DIV",
          0x39: "SQRT", 0x3A: "RSQRT", 0x3B: "WAITQ", 0x3C: "MTIR", 0x3D: "MFIR", 0x3E: "ILWR", 0x3F: "ISWR",
          0x40: "RNEXT", 0x41: "RGET", 0x42: "RINIT", 0x43: "RXOR", 0x64: "MFP", 0x68: "XTOP", 0x69: "XITOP",
          0x6C: "XGKICK", 0x70: "ESADD", 0x71: "ERSADD", 0x72: "ELENG", 0x73: "ERLENG", 0x74: "EATANxy",
          0x75: "EATANxz", 0x76: "ESUM", 0x78: "ESQRT", 0x79: "ERSQRT", 0x7A: "ERCPR", 0x7B: "WAITP",
          0x7C: "ESIN", 0x7D: "EATAN", 0x7E: "EEXP"}


def dest(w):
    d = (w >> 21) & 0xF
    return "".join(c for c, bit in zip("xyzw", (8, 4, 2, 1)) if d & bit)


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def upper(w):
    op = w & 0x3F
    ft, fs, fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    flags = "".join(f for f, b in (("[I]", 31), ("[E]", 30), ("[M]", 29), ("[D]", 28), ("[T]", 27)) if w >> b & 1)
    if op >= 0x3C:
        idx = (op & 3) | (((w >> 6) & 31) << 2)
        name, b = USPEC.get(idx, ("?u%02x" % idx, None))
        if name == "NOP":
            return "nop" + flags
        if name == "CLIP":
            return f"clipw.xyz vf{fs}, vf{ft}w" + flags
        if name.startswith(("ITOF", "FTOI", "ABS")):
            return f"{name.lower()}.{dest(w)} vf{ft}, vf{fs}" + flags
        if name == "OPMULA":
            return f"opmula.xyz ACC, vf{fs}, vf{ft}" + flags
        if b is not None:
            return f"{name.lower()}{BC[b]}.{dest(w)} ACC, vf{fs}, vf{ft}{BC[b]}" + flags
        src = {"q": "Q", "i": "I"}.get(name[-1], f"vf{ft}")
        return f"{name.lower()}.{dest(w)} ACC, vf{fs}, {src}" + flags
    name, b = UPPER.get(op, ("?U%02x" % op, None))
    if b is not None:
        return f"{name.lower()}{BC[b]}.{dest(w)} vf{fd}, vf{fs}, vf{ft}{BC[b]}" + flags
    src = {"q": "Q", "i": "I"}.get(name[-1], f"vf{ft}")
    if name == "OPMSUB":
        return f"opmsub.xyz vf{fd}, vf{fs}, vf{ft}" + flags
    return f"{name.lower()}.{dest(w)} vf{fd}, vf{fs}, {src}" + flags


def lower(w, pc):
    if (w >> 25) == 0x40:
        op = w & 0x3F
        it, is_, id_ = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        if op < 0x3C:
            n = LSPEC1.get(op, "?l%02x" % op)
            if n == "IADDI":
                return f"iaddi vi{it}, vi{is_}, {sx((w >> 6) & 31, 5)}"
            return f"{n.lower()} vi{id_}, vi{is_}, vi{it}"
        idx = (op & 3) | (((w >> 6) & 31) << 2)
        n = LSPEC2.get(idx, "?L%02x" % idx)
        ft, fs = it, is_
        fsf, ftf = (w >> 21) & 3, (w >> 23) & 3
        if n in ("MOVE", "MR32"):
            return f"{n.lower()}.{dest(w)} vf{ft}, vf{fs}"
        if n in ("LQI", "LQD"):
            return f"{n.lower()}.{dest(w)} vf{ft}, (vi{fs}{'++' if n == 'LQI' else '--'})"
        if n in ("SQI", "SQD"):
            return f"{n.lower()}.{dest(w)} vf{fs}, (vi{ft}{'++' if n == 'SQI' else '--'})"
        if n in ("DIV", "RSQRT"):
            return f"{n.lower()} Q, vf{fs}{BC[fsf]}, vf{ft}{BC[ftf]}"
        if n == "SQRT":
            return f"sqrt Q, vf{ft}{BC[ftf]}"
        if n == "MTIR":
            return f"mtir vi{ft}, vf{fs}{BC[fsf]}"
        if n == "MFIR":
            return f"mfir.{dest(w)} vf{ft}, vi{fs}"
        if n in ("ILWR", "ISWR"):
            return f"{n.lower()}.{dest(w)} vi{ft}, (vi{fs})"
        if n in ("XTOP", "XITOP"):
            return f"{n.lower()} vi{ft}"
        if n == "XGKICK":
            return f"xgkick vi{fs}"
        if n.startswith("E") and n not in ("ESADD", "ERSADD", "ELENG", "ERLENG", "ESUM"):
            return f"{n.lower()} P, vf{fs}{BC[fsf]}"
        if n == "MFP":
            return f"mfp.{dest(w)} vf{ft}, P"
        return f"{n.lower()} vf{fs}"
    op = w >> 25
    n = LOWER.get(op, "?L%02x" % op)
    it, is_ = (w >> 16) & 31, (w >> 11) & 31
    imm11 = sx(w & 0x7FF, 11)
    if n in ("LQ", "SQ"):
        return f"{n.lower()}.{dest(w)} vf{it if n == 'LQ' else is_}, {imm11}(vi{is_ if n == 'LQ' else it})"
    if n in ("ILW", "ISW"):
        return f"{n.lower()}.{dest(w)} vi{it}, {imm11}(vi{is_})"
    if n in ("IADDIU", "ISUBIU"):
        return f"{n.lower()} vi{it}, vi{is_}, 0x{(w & 0x7FF) | ((w >> 10) & 0x7800):x}"
    if n.startswith(("FC", "FS", "FM")):
        if n in ("FCEQ", "FCSET", "FCAND", "FCOR"):
            return f"{n.lower()} {'vi1, ' if n != 'FCSET' else ''}0x{w & 0xFFFFFF:06x}"
        if n == "FCGET":
            return f"fcget vi{it}"
        if n in ("FSEQ", "FSSET", "FSAND", "FSOR"):
            return f"{n.lower()} vi{it}, 0x{(w & 0x7FF) | ((w >> 10) & 0x800):x}"
        return f"{n.lower()} vi{it}, vi{is_}"
    if n in ("B", "BAL", "IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"):
        tgt = (pc + 8 + imm11 * 8) & 0x3FFF
        pre = {"B": "", "BAL": f"vi{it}, ", "IBEQ": f"vi{it}, vi{is_}, ", "IBNE": f"vi{it}, vi{is_}, "}.get(n, f"vi{is_}, ")
        return f"{n.lower()} {pre}0x{tgt:04x}"
    if n in ("JR", "JALR"):
        return f"{n.lower()} {'vi%d, ' % it if n == 'JALR' else ''}vi{is_}"
    return n.lower()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("code")
    ap.add_argument("start", nargs="?", default="0")
    ap.add_argument("count", nargs="?", type=int, default=64)
    ap.add_argument("--offset", type=int, default=0)
    a = ap.parse_args()
    raw = open(a.code, "rb").read()[a.offset:]
    pc = int(a.start, 16) & ~7
    for _ in range(a.count):
        if pc + 8 > len(raw):
            break
        lo, up = struct.unpack_from("<II", raw, pc)
        lstr = f"loi {struct.unpack('<f', struct.pack('<I', lo))[0]:g}" if up >> 31 else lower(lo, pc)
        print(f"{pc:04x}: {upper(up):<40} {lstr}")
        pc += 8


if __name__ == "__main__":
    main()
