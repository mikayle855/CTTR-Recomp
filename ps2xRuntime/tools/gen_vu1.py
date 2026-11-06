#!/usr/bin/env python3
"""gen_vu1.py -- [vu1jit] static VU1 recompiler generator.

Input : the 16 KB microcode images dumped by PS2X_VUMICRO=1 (work/vumicro_<hash>.bin) plus their program extents.
Output: src/lib/vu1_jit_gen.inc -- one C++ function per distinct program body, a label per instruction pair,
        each pair = calls of the compile-time specialised helpers in vu1_jit_ops.inc (jitUpper<W>/jitLower<W>),
        branches = gotos with the delay-slot pair emitted before the jump, JR/JALR = computed goto, E-bit =
        delay-slot pair then return. Anything a helper does not implement falls back to the interpreter via
        vu.jitSlowUpper()/jitSlowLower(), followed by the interpreter's vf0/vi0 reset.
Usage : gen_vu1.py <out.inc> <image.bin:extent_hex> [...]
"""
import sys, struct, hashlib, re

# [vuqlazy] the emitted pipeline snippets. qDead / cDead hold the value of cyc at which a pending Q / clip
# commits (0 = nothing pending), so a straight-line pair costs NOTHING instead of two counter loads.
#   *_RESOLVE: commit if due (enough for a pure reader of st.q / st.clip)
#   *_SYNC   : make the COUNTER exact too (st.qWait / g_clipWait), for ops that read it and at every return
#   *_ARM    : re-derive the deadline after an op that may have written the counter
Q_RESOLVE = 'if (qDead && cyc >= qDead) { st.q = st.pendingQ; st.qWait = 0u; qDead = 0u; }'
Q_SYNC    = 'if (qDead) { if (cyc >= qDead) { st.q = st.pendingQ; st.qWait = 0u; qDead = 0u; } else st.qWait = qDead - cyc; }'
Q_ARM     = 'qDead = st.qWait ? cyc + st.qWait : 0u;'
C_SYNC    = 'if (cDead) { if (cyc >= cDead) { st.clip = g_pendingClip; g_clipWait = 0u; cDead = 0u; } else g_clipWait = cDead - cyc; }'
C_ARM     = 'cDead = g_clipWait ? cyc + g_clipWait : 0u;'
PAIRC = 'g_vu1PairCount.fetch_add(cyc, std::memory_order_relaxed); '   # [vupairs] once per run, not per pair
PIPE_EXIT = Q_SYNC + ' ' + C_SYNC + ' '
# the fallback sites call the shared helpers instead of inlining the snippets: the fallback is cold, and
# inlining it at ~12k sites doubled the generated source (11.5 -> 24.5 MB) for no codegen benefit
PIPE_SLOW = 'vujit::jitPipeSync(st, cyc, qDead, cDead); '     # before an interpreter fallback: it may read either
PIPE_REARM = 'vujit::jitPipeArm(st, cyc, qDead, cDead); '     # ...and after it, because it may have written either

# [jitmacpipe] set per program by gen_program: it reads MAC flags, so its pairs run the interpreter's 4-cycle flag
# ring (vu1_jit_ops.inc jitMacTick/jitMacPush) and every exit drains it (jitMacFlush)
NEEDS_MAC = False
# [vu0jit] the code memory's pc mask, set per program from its image: 0x3FFF for VU1 (16 KB), 0xFFF for a VU0 microprogram (4 KB)
PCMASK = 0x3FFF
def exit_code():
    return (PIPE_EXIT if MODE == 'lazy' else '') + ('vujit::jitMacFlush(st, cyc); ' if NEEDS_MAC else '')

BRANCH_OPS = {0x20: 'B', 0x21: 'BAL', 0x24: 'JR', 0x25: 'JALR', 0x28: 'IBEQ', 0x29: 'IBNE', 0x2C: 'IBLTZ', 0x2D: 'IBGTZ', 0x2E: 'IBLEZ', 0x2F: 'IBGEZ'}

# [vuqlazy] Which pairs can OBSERVE the Q / clip pipelines. The lazy scheme below does nothing per pair
# and makes the state exact only here, so these sets must stay in step with vu1_jit_ops.inc:
#   Q is read by the q-broadcast upper ops (MULq/ADDq/MADDq/SUBq/MSUBq and their ACC "A" forms);
#   st.qWait is read/written by the 0x40-group lower ops DIV/SQRT/RSQRT/WAITQ;
#   st.clip is read by FCAND/FCOR/FCGET and read+written by the CLIP upper op (which owns g_clipWait).
# Anything the helpers do not implement goes to the interpreter, and THAT path syncs at its call site,
# so an op missing from these sets is still safe as long as no jit HELPER gains a new q/clip read.
Q_BC_OPS = {0x1C, 0x20, 0x21, 0x24, 0x25}          # jOP for op < 0x3C, jSOP for the "A" group
AUTOLABEL = True   # [autolabel] see label_set; False = the old full jump table for unlisted programs
CLIP_SOP = 0x1F                                     # jSOP of the CLIP upper op
CLIP_READ_LOWER = {0x10, 0x11, 0x12, 0x13, 0x1C}    # FCEQ / FCSET / FCAND / FCOR / FCGET (lower opHi); [jitflagops]
# [macelide] MAC-flag readers. commitF computes the Z/S nibbles on EVERY vector commit -- two SSE
# compares, two movemasks, two table lookups and a status write, wrapped around math that is often
# a single instruction. Measured on the user's splitscreen fight: turning it off took vu1 from
# 15.15 to 11.34 ms/swap (-25%) and 42.9 -> 45.7 fps. No BT3 microprogram reads these: the
# clippers use CLIP + FCAND/FCGET, which is the separate CLIP register. Decide per program so an
# un-recompiled microprogram running on the interpreter keeps its flags.
MAC_READ_LOWER = {0x18, 0x1A, 0x1B, 0x14, 0x16, 0x17}   # FMEQ / FMAND / FMOR, and FSEQ / FSAND / FSOR (status Z/S come from the same flags)

def jsop(w):
    return (w & 3) | ((w >> 4) & 0x7C)

def upper_reads_q(up):
    op = up & 0x3F
    return (jsop(up) in Q_BC_OPS) if op >= 0x3C else (op in Q_BC_OPS)

def upper_is_clip(up):
    return (up & 0x3F) >= 0x3C and jsop(up) == CLIP_SOP

def lower_touches_qwait(lo, loi):
    # [qarm-exact] 2026-09-08: exactly the jitted Q writers -- DIV/SQRT/RSQRT/WAITQ (0x40 group, funct 0x3C-0x3F,
    # sub-op 0x38-0x3B, same derivation as the interpreter's funct2). The old rule was the WHOLE 0x40 group
    # (IADD/MOVE/LQI/SQI/...), which re-armed qDead after nearly every pair: the re-arm line was the hottest
    # line of the hottest program (perf annotate). A 0x40-group op that falls back to the interpreter is
    # covered by PIPE_SLOW/PIPE_REARM around jitSlowLower, so the superset bought nothing.
    if loi or ((lo >> 25) & 0x7F) != 0x40: return False
    if (lo & 0x3F) < 0x3C: return False
    sub = (lo & 3) | ((lo >> 4) & 0x7C)
    return sub in (0x38, 0x39, 0x3A, 0x3B)

def lower_reads_clip(lo, loi):
    return (not loi) and ((lo >> 25) & 0x7F) in CLIP_READ_LOWER


def lower_reads_mac(lo, loi):
    return (not loi) and ((lo >> 25) & 0x7F) in MAC_READ_LOWER

def upper_is_nop(up):
    funct = up & 0x3F
    if funct < 0x3C: return False
    sub = (up & 3) | ((up >> 4) & 0x7C)
    return sub in (0x2F, 0x30)

def imm11(w):
    v = w & 0x7FF
    return v - 0x800 if v & 0x400 else v

def fnv(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

import os
MODE = os.environ.get('VU1GEN_QMODE', 'lazy')   # lazy (default) | eager (the old per-pair countdown) | none (ceiling test, WRONG)

# [vustall] FMAC/load data-hazard stalls, computed at generation time. Mirrors the interpreter (ps2_vu1.cpp
# vuPairVfUse / t_vfReady): a VF component written by a pair is usable 4 cycles after issue; a pair reading a
# component still in flight waits, and the Q / clip / MAC pipelines tick through the wait. CTTR's instancing loop
# (`sub.x vf5` then `abs.x vf0, vf5 | fmeq`) needs the wait for FMEQ to see the SUB's sign flag.
STALLS = None   # per pair: cycles the pair waits before issuing (set by gen_program)

def _dmask(w):
    d = (w >> 21) & 0xF
    return ((d >> 3) & 1) | (((d >> 2) & 1) << 1) | (((d >> 1) & 1) << 2) | ((d & 1) << 3)

def pair_vf_use(up, lo):
    """(reads, writes): lists of (vf, component mask bit0=x..bit3=w). Keep in step with ps2_vu1.cpp vuPairVfUse."""
    rd, wr = [], []
    def R(reg, m):
        if reg and m and len(rd) < 3: rd.append((reg, m))
    def W(reg, m):
        if reg and m and len(wr) < 2: wr.append((reg, m))
    uft, ufs, ufd, udm = (up >> 16) & 31, (up >> 11) & 31, (up >> 6) & 31, _dmask(up)
    uop = up & 0x3F
    if uop < 0x3C:
        if uop <= 0x1B: R(ufs, udm); R(uft, 1 << (up & 3)); W(ufd, udm)
        elif uop <= 0x27: R(ufs, udm); W(ufd, udm)
        elif uop <= 0x2F:
            m = 7 if uop == 0x2E else udm
            R(ufs, m); R(uft, m); W(ufd, udm)
    else:
        sop = (((up >> 6) & 0x1F) << 2) | (up & 3)
        if sop <= 0x0F or 0x18 <= sop <= 0x1B: R(ufs, udm); R(uft, 1 << (up & 3))
        elif sop <= 0x17 or sop == 0x1D: R(ufs, udm); W(uft, udm)
        elif sop == 0x1F: R(ufs, 7); R(uft, 8)
        elif sop <= 0x27: R(ufs, udm)
        elif sop <= 0x2E:
            m = 7 if sop == 0x2E else udm
            R(ufs, m); R(uft, m)
    if (up >> 31) & 1:
        return rd, wr
    lft, lfs, ldm = (lo >> 16) & 31, (lo >> 11) & 31, _dmask(lo)
    lop = lo >> 25
    if lop == 0x00: W(lft, ldm)
    elif lop == 0x01: R(lfs, ldm)
    elif lop == 0x40 and (lo & 0x3F) >= 0x3C:
        s = (((lo >> 6) & 0x1F) << 2) | (lo & 3)
        fsf, ftf = 1 << ((lo >> 21) & 3), 1 << ((lo >> 23) & 3)
        if s in (0x30, 0x31): R(lfs, 0xF if s == 0x31 else ldm); W(lft, ldm)
        elif s in (0x34, 0x36): W(lft, ldm)
        elif s in (0x35, 0x37): R(lfs, ldm)
        elif s in (0x38, 0x3A): R(lfs, fsf); R(lft, ftf)
        elif s == 0x39: R(lft, ftf)
        elif s == 0x3C: R(lfs, fsf)
        elif s in (0x3D, 0x40, 0x41, 0x64): W(lft, ldm)
        elif s in (0x42, 0x43): R(lfs, fsf)
    return rd, wr

def compute_stalls(pairs, nlim):
    """Static stall per pair (k < nlim): data flow of 'cycles until each VF component is usable' along fall-through
    and static branch edges, joined by max. Dynamic arrivals (run entries, JR/JALR targets) start with nothing in
    flight, as the interpreter does at a run start."""
    uses = [pair_vf_use(up, lo) for lo, up in pairs[:nlim]]
    def info(k):
        lo, up = pairs[k]
        loi = (up >> 31) & 1
        op = (lo >> 25) & 0x7F
        br = None if loi or op not in BRANCH_OPS else BRANCH_OPS[op]
        return br, (up >> 30) & 1, ((8 * k + 8 + imm11(lo) * 8) & PCMASK) // 8
    succ = [[] for _ in range(nlim)]
    for k in range(nlim):
        br, eb, tgt = info(k)
        prev = info(k - 1) if k > 0 else (None, 0, 0)
        if prev[1]:                      # delay slot of an E-bit pair: the run ends here
            continue
        if prev[0] is not None:          # delay slot of a branch (or reached directly: then it falls through)
            if prev[0] not in ('JR', 'JALR'):
                succ[k].append(prev[2])
            if prev[0] not in ('B', 'BAL', 'JR', 'JALR'):
                succ[k].append(k + 1)
            else:
                succ[k].append(k + 1)    # conservative: the pair may also be entered directly and fall through
            continue
        succ[k].append(k + 1)
    instate = [None] * nlim
    stall = [0] * nlim
    work = [0]
    instate[0] = {}
    for k in range(nlim):                # every pair may be a dynamic arrival point
        if instate[k] is None:
            instate[k] = {}
            work.append(k)
    seen_iter = 0
    while work:
        k = work.pop()
        seen_iter += 1
        st = instate[k]
        rd, wr = uses[k]
        s = 0
        for reg, m in rd:
            for c in range(4):
                if (m >> c) & 1:
                    s = max(s, st.get((reg, c), 0))
        stall[k] = s
        out = {}
        for key, v in st.items():
            v2 = v - s - 1
            if v2 > 0: out[key] = v2
        for reg, m in wr:
            for c in range(4):
                if (m >> c) & 1:
                    out[(reg, c)] = 3
        for n in succ[k]:
            if n >= nlim or n < 0:
                continue
            cur = instate[n]
            changed = False
            for key, v in out.items():
                if cur.get(key, 0) < v:
                    cur[key] = v; changed = True
            if changed:
                work.append(n)
    return stall

def emit_pair_body(out, k, lo, up, pairs, inline_mode):
    """Emit the per-pair code (no label). inline_mode: this pair is emitted as a delay slot -> its own control
    flow (branch/E-bit) is ignored (matches the interpreter: a branch in a delay slot is lost)."""
    pc = 8 * k
    loi = (up >> 31) & 1
    ebit = (up >> 30) & 1
    op_hi = (lo >> 25) & 0x7F
    is_branch = (not loi) and op_hi in BRANCH_OPS
    out.append(f'        // pc 0x{pc:x}  lo=0x{lo:08x} up=0x{up:08x}')
    # [vujit-lean] the cycle bound only matters where a loop can close (a branch / JR / JALR) or the program ends
    # (E-bit); straight-line pairs just count. st.pc is stored only where something may read it: branch pairs
    # (the bound's return), E-bit pairs, and any lower op that can reach the interpreter's slow path or its
    # diagnostics (flag ops, the 0x40 special group incl. XGKICK/E*/R*/WAITP). Plain loads/stores/immediates and
    # the lower NOP word (0x8000033C) never read it.
    mode = MODE
    if is_branch or ebit:
        # the bound is checked BEFORE the pair runs, so this pair's pipeline tick must not have happened:
        # keep cyc as "pairs executed" by incrementing after the check, not inside it
        out.append('        if (cyc >= maxCycles) { ' + exit_code() + PAIRC + f'st.pc = 0x{pc:x}u; return; }}')
    stall = STALLS[k] if STALLS is not None and k < len(STALLS) else 0
    if stall:
        # [vustall] the pair waits for an operand still in flight; the pipelines keep ticking
        out.append(f'        // [vustall] waits {stall} cycle(s)')
        for _ in range(stall):
            out.append('        ++cyc;' + (' vujit::jitMacTick(st, cyc, macRing);' if NEEDS_MAC else ''))
    out.append('        ++cyc;')
    if NEEDS_MAC:
        out.append('        vujit::jitMacTick(st, cyc, macRing);')
    if mode == 'eager':
        out.append('        if (st.qWait > 0u && --st.qWait == 0u) st.q = st.pendingQ;')
        out.append('        if (g_clipWait > 0u && --g_clipWait == 0u) st.clip = g_pendingClip;')
    elif mode == 'lazy':
        # nothing per pair -- only where this pair can observe a pipeline
        if upper_reads_q(up) and not upper_is_nop(up):
            out.append('        ' + Q_RESOLVE)
        if lower_touches_qwait(lo, loi):
            out.append('        ' + Q_SYNC)
        if upper_is_clip(up) or lower_reads_clip(lo, loi):
            out.append('        ' + C_SYNC)
    if not loi and op_hi == 0x40 and (lo & 0x3F) == 0x3F and ((lo >> 6) & 0x1F) == 0x0E:
        # [waitqpair] WAITQ stalls the pair's issue until the divider is done: the paired upper op reads the NEW Q
        out.append('        if (qDead) { st.q = st.pendingQ; st.qWait = 0u; qDead = 0u; }' if mode == 'lazy'
                   else '        if (st.qWait > 0u) { st.q = st.pendingQ; st.qWait = 0u; }')
    plain_lower = loi or lo == 0x8000033C or op_hi in (0x00, 0x01, 0x04, 0x05, 0x08, 0x09)
    if is_branch or ebit or not plain_lower:
        out.append(f'        st.pc = 0x{pc:x}u;')
    out.append('        {')
    out.append('            bool fb = false;')
    slow_pre, slow_post = (PIPE_SLOW, PIPE_REARM) if mode == 'lazy' else ('', '')
    up_t = f'0x{up:08x}u, false' if k in MAC_DEAD else f'0x{up:08x}u'   # [maclive]
    up_code = '' if upper_is_nop(up) else f'if (!vujit::jitUpper<{up_t}>(st)) {{ {slow_pre}vu.jitSlowUpper(0x{up:08x}u); {slow_post}fb = true; }}'
    if is_branch:
        name = BRANCH_OPS[op_hi]
        vit, vis = (lo >> 16) & 0xF, (lo >> 11) & 0xF
        tgt = ((pc + 8 + imm11(lo) * 8) & PCMASK)
        cond = {'B': 'true', 'BAL': 'true',
                'IBEQ': f'((int16_t)st.vi[{vis}] == (int16_t)st.vi[{vit}])', 'IBNE': f'((int16_t)st.vi[{vis}] != (int16_t)st.vi[{vit}])',
                'IBLTZ': f'((int16_t)st.vi[{vis}] < 0)', 'IBGTZ': f'((int16_t)st.vi[{vis}] > 0)',
                'IBLEZ': f'((int16_t)st.vi[{vis}] <= 0)', 'IBGEZ': f'((int16_t)st.vi[{vis}] >= 0)',
                'JR': 'true', 'JALR': 'true'}[name]
        lo_code = []
        if name in ('JR', 'JALR'):
            lo_code.append(f'jtgt = ((uint32_t)(uint16_t)st.vi[{vis}] * 8u) & 0x{PCMASK:X}u;')
        if name in ('BAL', 'JALR') and vit != 0:
            lo_code.append(f'st.vi[{vit}] = {k + 2};')
        lo_code.append(f'br = {cond};')
        lo_code = ' '.join(lo_code)
    elif (lo & 0x7FF) == 0x6FC:
        # [xgkick-native] XGKICK: call the kick service routine directly. It reads only vi[is] and VU memory (no Q/CLIP,
        # no flags), so no pipeline sync and no post-fallback vf0/vi0 reset.
        lo_code = f'vu.xgkickImpl({(lo >> 11) & 0x1F}u, vuData, dataSize, gs, memory);'
    else:
        lo_code = f'if (!vujit::jitLower<0x{lo:08x}u>(st, vuData, dataSize)) {{ {slow_pre}vu.jitSlowLower(0x{lo:08x}u, vuData, dataSize, gs, memory, 0x{up:08x}u); {slow_post}fb = true; }}'
    if loi:
        out.append(f'            {up_code}')
        out.append(f'            {{ const uint32_t w = 0x{lo:08x}u; std::memcpy(&st.i, &w, 4); }}')
    else:
        out.append(f'            if constexpr (vuLowerShouldRunBeforeUpper(0x{up:08x}u, 0x{lo:08x}u)) {{ {lo_code} {up_code} }}')
        out.append(f'            else {{ {up_code} {lo_code} }}')
    out.append('            if (fb) { _mm_storeu_ps(st.vf[0], _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f)); st.vi[0] = 0; }')
    out.append('        }')
    if NEEDS_MAC:
        out.append('        vujit::jitMacPush(cyc, macRing, macNew);')
    if mode == 'lazy':
        if lower_touches_qwait(lo, loi):
            out.append('        ' + Q_ARM)
        if upper_is_clip(up) or ((not loi) and ((lo >> 25) & 0x7F) == 0x11):   # [jitflagops] FCSET zeroes g_clipWait
            out.append('        ' + C_ARM)
    return is_branch, bool(ebit), (BRANCH_OPS[op_hi] if is_branch else None), (((pc + 8 + imm11(lo) * 8) & PCMASK) if is_branch else None)

# [vusuper] Entry PCs each program is actually entered at, from a live fight with PS2X_VUENTRY=1
# (work/drv_VUENT/run.log, 2026-09-04). Keyed by the FNV hash the Prog table uses. WHY THIS EXISTS:
# the generated body gives EVERY pair a label so `goto *jt[st.pc>>3]` can land anywhere, and taking a
# label's address makes the compiler treat that pair as a basic block with unknown predecessors -- so
# nothing, vf registers above all, can stay live across a pair boundary. Only ~5% of pairs are static
# branch targets and only 3-4 PCs per program are ever entered, so labelling just those turns ~91% of
# pairs into straight-line code the compiler can actually optimise across.
# A program NOT listed here keeps a label on every pair (the old behaviour) -- that is the safe default,
# and it is why 0xff81d66058071344 (the destructible-scenery program, which the rig cannot reach) is absent.
VU_ENTRIES = {
    0x0b7f264e0237b48e: [0x0, 0x78, 0xa8, 0x228],
    0x28ce214c0c35fc68: [0x0, 0x78, 0x2e0],
    0x7e8efb7575b22f69: [0x0, 0x80, 0x318],
    0x0492d545dc59311d: [0x0, 0x120, 0x3e8],
    0x05c48715b8434850: [0x0, 0x78, 0xa8],
}

def label_set(pairs, npairs, entries):
    """Pairs that must keep a label: every place control can ARRIVE at from outside its predecessor."""
    if entries is None:
        if not AUTOLABEL:
            return set(range(npairs))        # unknown entry set -> keep the full jump table
        entries = []
        # [autolabel] no census: label every pair control cannot reach by falling through -- the pair after an
        # unconditional transfer's delay slot (B, JR, JALR... and the E-bit stop, where MSCNT resumes) is where a
        # routine or a resume point starts. Anything else that is entered still works: the dispatch default bails to
        # the interpreter ([jitbail]) and logs the pc, so a miss costs speed, never correctness.
        for k in range(npairs):
            lo, up = pairs[k]
            if (up >> 30) & 1:
                entries.append(8 * ((k + 2) % npairs))
            if (up >> 31) & 1:
                continue
            op = (lo >> 25) & 0x7F
            if op in BRANCH_OPS and BRANCH_OPS[op] in ('B', 'BAL', 'JR', 'JALR'):
                entries.append(8 * ((k + 2) % npairs))
        # [autolabel-computed] a JR through a register no BAL/JALR uses as its link is a computed jump (CTTR's skinning
        # routines `ilw vi7, table(vi7); jr vi7` into unrolled 9-pair blocks): label every pair from it to the end of
        # its routine (the next JR), since any of them can be a target
        links = set()
        for k in range(npairs):
            lo, up = pairs[k]
            if (up >> 31) & 1:
                continue
            op = (lo >> 25) & 0x7F
            if op == 0x21 or op == 0x25:                 # BAL / JALR: the link register is vit
                links.add((lo >> 16) & 0xF)
        for k in range(npairs):
            lo, up = pairs[k]
            if (up >> 31) & 1:
                continue
            if (lo >> 25) & 0x7F == 0x24 and ((lo >> 11) & 0xF) not in links:
                j = k + 2
                while j < npairs:
                    entries.append(8 * j)
                    jl, ju = pairs[j]
                    if not ((ju >> 31) & 1) and (jl >> 25) & 0x7F == 0x24:
                        break
                    j += 1
    lab = {0}
    for pc in entries:
        lab.add((pc // 8) % npairs)
    for k in range(npairs):
        lo, up = pairs[k]
        if (up >> 31) & 1:                   # loi: the lower word is an immediate, not an instruction
            continue
        op = (lo >> 25) & 0x7F
        if op not in BRANCH_OPS:
            continue
        nm = BRANCH_OPS[op]
        if nm in ('JR', 'JALR', 'BAL'):
            lab.add((k + 2) % npairs)        # JALR's link value, i.e. where a matching JR returns to
        else:
            lab.add((((8 * k) + 8 + imm11(lo) * 8) & PCMASK) // 8 % npairs)
    return lab

def gen_program(name, image, npairs, entries=None, needs_mac=False, limit=None, extra_labels=frozenset()):
    global NEEDS_MAC, PCMASK, STALLS
    NEEDS_MAC = needs_mac
    PCMASK = 8 * npairs - 1
    pairs = [(struct.unpack_from('<I', image, 8 * k)[0], struct.unpack_from('<I', image, 8 * k + 4)[0]) for k in range(npairs)]
    nlim = npairs if limit is None else limit // 8   # [jitprefix] pairs >= nlim run on the interpreter (jitRunOutside)
    STALLS = compute_stalls(pairs, nlim)
    global MAC_DEAD
    MAC_DEAD = mac_dead_writers(pairs, npairs, nlim) if NEEDS_MAC else set()   # [maclive]
    if NEEDS_MAC:
        print(f'[maclive] {name}: {len(MAC_DEAD)} of {sum(1 for k in range(min(nlim, npairs)) if upper_sets_mac(pairs[k][1]))} flag writers dead', file=sys.stderr)
    out = []
    # [vurestrict] see vu1_jit_ops.inc: st and vuData are separate allocations, and vuData being a
    # uint8_t* otherwise forces a reload of st after every store through it
    out.append(f'static void {name}(VU1Interpreter &vu, VU1State &__restrict st, uint8_t *__restrict vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t maxCycles)')
    out.append('{')
    out.append('    (void)gs; (void)memory;')
    out.append('    uint32_t cyc = 0; bool br = false; uint32_t jtgt = 0; (void)br; (void)jtgt;')
    if NEEDS_MAC:
        # [jitmacpipe] the thread-local flag ring's address, once per run; a stale queued entry from an interpreter run is dropped
        out.append('    MacPipeSlot *const macRing = t_macRing; MacPipeSlot *const macNew = &t_macNew; macNew->valid = false;')
    if MODE == 'lazy':
        out.append('    // [vuqlazy] deadlines, not counters: cyc at which a pending Q / clip commits (0 = idle)')
        out.append('    uint32_t qDead = st.qWait, cDead = g_clipWait; (void)qDead; (void)cDead;')
    lab = label_set(pairs, npairs, entries) | set(extra_labels)
    if nlim < npairs:
        lab |= set(range(nlim, npairs))   # [autolabel] JALR targets in the per-upload region above the prefix limit (instancing table)
    # [vusuper] dispatch only to pairs control can ARRIVE at. A pc outside that set means the census in
    # VU_ENTRIES is stale (a new entry point, or a JR to somewhere we did not predict): say so loudly and
    # hand the run back rather than jumping into the middle of a superblock.
    def dispatch(expr, what):
        if len(lab) == npairs:
            # every pair is a target: the original computed goto is both smaller and what the committed
            # .inc contains, so emit exactly that
            return ['    static void *const jt[' + str(npairs) + '] = { ' + ', '.join(f'&&L_{k}' for k in range(npairs)) + ' };',
                    f'    goto *jt[({expr} >> 3) & {npairs - 1}u];'] if expr == 'st.pc' else \
                   [f'            goto *jt[({expr} >> 3) & {npairs - 1}u];']
        o = [f'    switch (({expr} >> 3) & {npairs - 1}u)', '    {']
        for t in sorted(lab):
            o.append(f'        case {t}u: goto L_{t};')
        # [jitbail] an unpredicted entry / jump target: note it for the census ([jitmiss] -> jit_targets.txt) and
        # finish the run on the interpreter (exact, just slower)
        o.append(f'        default: {{ vujit::jitNoteMiss("{name}", ({expr}) & 0x{PCMASK:X}u); '
                 f'st.pc = ({expr}) & 0x{PCMASK:X}u; ' + exit_code() + PAIRC + 'vujit::t_jitBail = true; return; }')
        o.append('    }')
        return o
    out.extend(dispatch('st.pc', 'entry'))
    warn = 0
    for k in range(npairs):
        lo, up = pairs[k]
        if k >= nlim:
            # [jitprefix] code above the limit changes per upload: run it on the interpreter's paths
            out.append(f'L_{k}:')
            out.append('    {')
            out.append(f'        st.pc = 0x{8 * k:x}u;')
            if MODE == 'lazy': out.append('        ' + PIPE_SLOW)
            out.append(f'        jtgt = vujit::jitRunOutside(vu, st, cyc, 0x{8 * nlim:x}u, vuData, dataSize, gs, memory, '
                       f'{"macRing" if NEEDS_MAC else "nullptr"}, maxCycles);')
            if MODE == 'lazy': out.append('        ' + PIPE_REARM)
            out.append('        if (jtgt == vujit::kJitEnded) { ' + exit_code() + PAIRC + 'return; }')
            for ln in dispatch('jtgt', 'outside-return'):
                out.append(ln)
            out.append('    }')
            continue
        if k in lab: out.append(f'L_{k}:')
        out.append('    {')
        is_branch, ebit, bname, tgt = emit_pair_body(out, k, lo, up, pairs, False)
        nxt = k + 1
        if (ebit or is_branch) and nxt >= nlim:
            print(f'warning: {name}: the delay slot of pair 0x{8 * k:x} lies above the prefix limit', file=sys.stderr)
        if ebit:
            # delay slot pair, then stop
            if nxt < npairs:
                nlo, nup = pairs[nxt]
                if ((nlo >> 25) & 0x7F) in BRANCH_OPS or ((nup >> 30) & 1):
                    warn += 1
                emit_pair_body(out, nxt, nlo, nup, pairs, True)
            # [vujit-ebitbranch] an E-bit on a branch: the program still stops after the delay slot, but the pc it
            # resumes at (MSCNT) is the branch target when the branch was taken -- CTTR ends each batch with
            # `B 0x230` + E so the next MSCNT restarts its loop
            resume = f'0x{8 * (k + 2):x}u'
            if is_branch:
                resume = f'(br ? {"jtgt" if bname in ("JR", "JALR") else f"0x{tgt:x}u"} : {resume})'
            out.append('        ' + exit_code() + PAIRC + f'st.ebit = true; st.pc = {resume}; return;')
        elif is_branch:
            out.append('        if (br) {')
            if nxt < npairs:
                nlo, nup = pairs[nxt]
                if ((nlo >> 25) & 0x7F) in BRANCH_OPS or ((nup >> 30) & 1):
                    warn += 1
                emit_pair_body(out, nxt, nlo, nup, pairs, True)
            if bname in ('JR', 'JALR'):
                d = dispatch('jtgt', 'jump-target')
                for ln in d:
                    out.append(ln if len(lab) == npairs else ('    ' + ln))
            else:
                out.append(f'            goto L_{tgt // 8};')
            out.append('        }')
        out.append('    }')
    out.append('    ' + exit_code() + PAIRC + f'st.pc = 0x{8 * npairs:x}u & 0x{PCMASK:X}u; return;')
    out.append('}')
    return '\n'.join(out), warn

# [maclive] Per-pair MAC liveness inside a program that DOES read the flags. A flag-setting upper op at pair k is
# live when some MAC reader (FMEQ/FMAND/FMOR), a program exit, a computed jump or the interpreted region can be
# reached from k before another flag-setting op has had >= 4 pairs to land (pair distance <= cycle distance, so the
# 4-pair test is conservative under stalls). Dead writers are emitted as jitUpper<W, false> and skip the Z/S
# computation entirely (the arithmetic is unchanged). A program that reads STATUS (FSEQ/FSAND/FSOR: sticky bits
# accumulate every write) keeps every writer live. Anything the search cannot bound is live.
RAW_UPPER_SOPS = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1D, 0x1F, 0x2F, 0x30}   # ITOF/FTOI/ABS/CLIP/NOP
RAW_UPPER_OPS = set(range(0x10, 0x18)) | {0x1D, 0x1F, 0x2B, 0x2F}                            # MAX/MINI families
STATUS_READ_LOWER = {0x14, 0x16, 0x17}
MAC_ONLY_READ_LOWER = {0x18, 0x1A, 0x1B}

def upper_sets_mac(up):
    op = up & 0x3F
    if op >= 0x3C:
        return jsop(up) not in RAW_UPPER_SOPS
    return op not in RAW_UPPER_OPS

def mac_dead_writers(pairs, npairs, nlim):
    def info(p):
        lo, up = pairs[p]
        loi = (up >> 31) & 1
        oh = (lo >> 25) & 0x7F
        br = None if loi else BRANCH_OPS.get(oh)
        tgt = ((8 * p + 8 + imm11(lo) * 8) & PCMASK) // 8 % npairs
        return lo, up, loi, oh, br, tgt, (up >> 30) & 1
    for p in range(min(nlim, npairs)):
        lo, up, loi, oh, *_ = info(p)
        if not loi and oh in STATUS_READ_LOWER:
            return set()   # status reader: sticky bits make every writer observable
    END = -1
    def after(p):
        """continuations of pair p executed normally: (next_pc, pend) where pend lists what follows the next pair
        when that pair is a delay slot (None = not a delay slot)."""
        lo, up, loi, oh, br, tgt, eb = info(p)
        if eb or br in ('JR', 'JALR'):
            return [(p + 1, (END,))]          # end of program / computed target: treated as an observer
        if br in ('B', 'BAL'):
            return [(p + 1, (tgt,))]
        if br:
            return [(p + 1, (tgt, p + 2))]
        return [(p + 1, None)]
    dead = set()
    for k in range(min(nlim, npairs)):
        if not upper_sets_mac(pairs[k][1]):
            continue
        live = False
        seen = set()
        work = [(n, pend, None) for n, pend in after(k)]
        steps = 0
        while work:
            p, pend, wd = work.pop()
            steps += 1
            if steps > 4000:
                live = True; break
            if wd is not None and wd >= 4:
                continue                       # a later writer has landed: k is no longer observable on this path
            if p == END or p >= npairs or p >= nlim:
                live = True; break             # exit / wrap / interpreted region: flags persist or are read there
            key = (p, pend, wd)
            if key in seen:
                continue
            seen.add(key)
            plo, pup, ploi, poh, *_ = info(p)
            if not ploi and poh in MAC_ONLY_READ_LOWER:
                live = True; break
            nwd = 0 if upper_sets_mac(pup) else wd
            nwd = None if nwd is None else nwd + 1
            if pend is not None:               # p ran as a delay slot: its own control flow is ignored
                for q in pend:
                    work.append((q, None, nwd))
            else:
                for q, qp in after(p):
                    work.append((q, qp, nwd))
        if not live:
            dead.add(k)
    return dead

MAC_DEAD = set()   # set per program by gen_program

# [macelide] does this program ever READ the MAC flags? If not, its commits can skip
# computing them entirely. Scanned over the program's own extent only.
def prog_reads_mac(image, extent):
    for off in range(0, extent, 8):
        lo = int.from_bytes(image[off:off + 4], 'little')
        up = int.from_bytes(image[off + 4:off + 8], 'little')
        loi = bool(up & 0x80000000)   # I-bit: the lower word is an immediate, not an opcode
        if lower_reads_mac(lo, loi):
            return True
    return False


# [jitmiss] jump / entry targets the programs bailed at in play ("[jitmiss] <prog> 0x<pc>=<n>" lines from run logs,
# collected into jit_targets.txt next to the images): those pairs get labels so the next build does not bail there
JIT_TARGETS = {}
def load_jit_targets(path='jit_targets.txt'):
    if not os.path.exists(path):
        return
    for ln in open(path, encoding='utf-8', errors='replace'):
        m = re.search(r'(vu1jit_\w+) 0x([0-9a-fA-F]+)', ln)
        if m:
            JIT_TARGETS.setdefault(m.group(1), set()).add(int(m.group(2), 16))

def main():
    load_jit_targets()
    outp = sys.argv[1]
    progs = []
    seen = {}
    for spec in sys.argv[2:]:
        path, ext = spec.rsplit(':', 1)   # rsplit: a Windows path carries its own colon (C:/...)
        limit = None
        if '@' in ext:                    # [jitprefix] path:extent@limit -- recompile below limit, match on [0, limit)
            ext, lim = ext.split('@', 1)
            limit = int(lim, 16)
        image = open(path, 'rb').read()
        extent = int(ext, 16) if limit is None else limit
        body = image[:extent]
        bh = hashlib.md5(body).hexdigest()[:8] + ('p' if limit is not None else '')
        if bh in seen:
            continue
        seen[bh] = True
        progs.append((bh, image, extent, limit))
    # full programs first: the runtime takes the first memcmp match, so an exact image beats a prefix
    progs.sort(key=lambda p: p[3] is not None)
    parts = ['// GENERATED by work/rig/gen_vu1.py -- do not edit. [vu1jit] static VU1 recompiler output.', '']
    total_warn = 0
    for bh, image, extent, limit in progs:
        # [vusuper] REVERTED 2026-09-04: passing VU_ENTRIES here labels only the pairs control can arrive
        # at, but the entry/jump-target set is NOT STABLE between fights (see the ledger) and the measured
        # win was nil. None = label every pair, the safe original behaviour.
        census = frozenset(p // 8 for p in JIT_TARGETS.get(f'vu1jit_{bh}', ()))   # [jitmiss] targets seen at runtime
        code, warn = gen_program(f'vu1jit_{bh}', image, len(image) // 8, None, prog_reads_mac(image, extent), limit, census)
        # [autolabel] a static branch emitted inside another pair's delay slot can target a pair label_set did not
        # mark: add every goto target that has no label and generate again
        for _ in range(4):
            have = set(int(x) for x in re.findall(r'^L_(\d+):', code, re.M))
            missing = set(int(x) for x in re.findall(r'goto L_(\d+);', code)) - have
            if not missing:
                break
            code, warn = gen_program(f'vu1jit_{bh}', image, len(image) // 8, None, prog_reads_mac(image, extent), limit,
                                     frozenset(missing) | frozenset(have) | census)
        total_warn += warn
        parts.append(code)
        parts.append('')
    parts.append('namespace vujit {')
    # [jitmatch] each program's bytes up to its extent: the runtime matches code memory by memcmp, not by hashing it
    for bh, image, extent, limit in progs:
        body = image[:extent]
        rows = [', '.join(f'0x{b:02x}' for b in body[i:i + 32]) for i in range(0, len(body), 32)]
        parts.append(f'static const uint8_t vu1img_{bh}[0x{extent:x}] = {{')
        parts.append(',\n'.join('    ' + r for r in rows))
        parts.append('};')
    parts.append('const Prog kPrograms[] = {')
    for bh, image, extent, limit in progs:
        needs_mac = prog_reads_mac(image, extent)
        parts.append(f'    {{ 0x{extent:x}u, 0x{fnv(image[:extent]):016x}ull, &vu1jit_{bh}, {"true" if needs_mac else "false"}, vu1img_{bh} }},   // body md5 {bh}{"" if needs_mac else "  [macelide] no MAC reads"}')
    parts.append('};')
    parts.append(f'const int kProgramCount = {len(progs)};')
    parts.append('}')
    open(outp, 'w').write('\n'.join(parts) + '\n')
    print(f'wrote {outp}: {len(progs)} programs, {sum(len(i)//8 for _, i, _, _ in progs)} pairs, {total_warn} delay-slot control-flow warnings')

if __name__ == '__main__':
    main()
