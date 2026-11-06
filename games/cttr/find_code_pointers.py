#!/usr/bin/env python3
"""List code addresses the CTTR ELF refers to by pointer that are not inside any known function.

    python games/cttr/find_code_pointers.py <elf> <functions.csv> <out.txt>

Code reached only through a pointer -- interrupt/alarm/thread handlers built with lui+addiu, C++ vtables and
callback tables in .data/.rodata -- is invisible to Ghidra's call-flow analysis, so it lands in the gaps between
functions and the recompiler has no entry for it (AddIntcHandler(5, 0x350c00) found "registered=NO").
A target counts when it is word-aligned inside .text, holds a non-zero word, and lies in a gap. Targets inside an
existing function are left alone: those are mostly switch-table cases. Output: one hex address per line, for
ps2xRecomp/tools/ghidra/CreateFunctionsAt.java.
"""
import bisect
import csv
import struct
import sys

TEXT_LO, TEXT_HI = 0x100000, 0x506380


def main() -> None:
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    elf = open(sys.argv[1], "rb").read()
    ph, = struct.unpack_from("<I", elf, 0x1C)
    _, off, va, _, filesz, *_ = struct.unpack_from("<8I", elf, ph)
    seg = elf[off:off + filesz]

    def word(a: int) -> int:
        return struct.unpack_from("<I", seg, a - va)[0]

    rows = sorted((int(r["Start"], 16), int(r["End"], 16)) for r in csv.DictReader(open(sys.argv[2])))
    starts = [s for s, _ in rows]

    def in_function(a: int) -> bool:
        i = bisect.bisect_right(starts, a) - 1
        return i >= 0 and rows[i][0] <= a < rows[i][1]

    cands = set()
    # 1. Data words (everything loaded after .text) that point into .text.
    for a in range(TEXT_HI, va + filesz - 3, 4):
        w = word(a)
        if TEXT_LO <= w < TEXT_HI and w % 4 == 0:
            cands.add(w)
    # 2. lui rt, hi ... addiu/ori rX, rt, lo within the next 8 instructions.
    for a in range(TEXT_LO, TEXT_HI, 4):
        ins = word(a)
        if ins >> 26 != 0x0F:
            continue
        rt, hi = (ins >> 16) & 31, ins & 0xFFFF
        for b in range(a + 4, min(a + 36, TEXT_HI), 4):
            nxt = word(b)
            op, rs = nxt >> 26, (nxt >> 21) & 31
            if rs == rt and op in (0x09, 0x0D):   # addiu / ori
                lo = nxt & 0xFFFF
                v = ((hi << 16) + (lo - 0x10000 if op == 0x09 and lo & 0x8000 else lo)) & 0xFFFFFFFF
                if TEXT_LO <= v < TEXT_HI and v % 4 == 0:
                    cands.add(v)
                break
            if (nxt >> 16) & 31 == rt and op not in (0x2B, 0x3F, 0x29, 0x28, 0x1F):   # rt overwritten (not a store)
                break

    out = sorted(a for a in cands if word(a) != 0 and not in_function(a))
    with open(sys.argv[3], "w") as f:
        f.write("".join(f"{a:x}\n" for a in out))
    print(f"{len(cands)} code pointers, {len(out)} outside every function -> {sys.argv[3]}")


if __name__ == "__main__":
    main()
