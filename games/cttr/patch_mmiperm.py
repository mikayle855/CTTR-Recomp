"""Bring already-generated sources in line with the [mmiperm] recompiler fix (ps2xRecomp mmi_translation_helpers.cpp)
without regenerating and rebuilding all ~23k functions: rewrite each affected statement from the instruction word
recorded in the comment line above it. Running setup.py again produces the same text.
    python games/cttr/patch_mmiperm.py ps2xRuntime/src/runner"""
import glob, os, re, sys

COMMENT = re.compile(r'^\s*// 0x[0-9a-f]+: 0x([0-9a-f]+)\s+(pexew|prot3w|pexcw|pexeh|pexch|prevh|pcpyh|pabsw|pabsh)\b')
WORD = {'pexew': (3, 0, 1, 2), 'prot3w': (3, 0, 2, 1), 'pexcw': (3, 1, 2, 0)}
HALF = {'pexeh': (3, 0, 1, 2), 'pexch': (3, 1, 2, 0), 'prevh': (0, 1, 2, 3)}

def emit(op, raw):
    rt, rd = (raw >> 16) & 31, (raw >> 11) & 31
    if op in WORD:
        return ['SET_GPR_VEC(ctx, %d, _mm_shuffle_epi32(GPR_VEC(ctx, %d), _MM_SHUFFLE(%d,%d,%d,%d)));' % ((rd, rt) + WORD[op])]
    if op in HALF:
        s = '_MM_SHUFFLE(%d,%d,%d,%d)' % HALF[op]
        return ['SET_GPR_VEC(ctx, %d, _mm_shufflelo_epi16(_mm_shufflehi_epi16(GPR_VEC(ctx, %d), %s), %s));' % (rd, rt, s, s)]
    if op == 'pcpyh':
        return ['{ __m128i src = GPR_VEC(ctx, %d); uint16_t l = _mm_extract_epi16(src, 0); uint16_t h = _mm_extract_epi16(src, 4); ' % rt,
                '   SET_GPR_VEC(ctx, %d, _mm_set_epi16(h,h,h,h, l,l,l,l)); }' % rd]
    return ['SET_GPR_VEC(ctx, %d, PS2_%s(GPR_VEC(ctx, %d)));' % (rd, op.upper(), rt)]

changed = 0
for path in glob.glob(os.path.join(sys.argv[1], '*.cpp')):
    with open(path, 'rb') as f:
        text = f.read().decode('utf-8')
    if not any(m in text for m in (' pexew', ' prot3w', ' pexcw', ' pexeh', ' pexch', ' prevh', ' pcpyh', ' pabsw', ' pabsh')):
        continue
    nl = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(nl)
    out, i, edits = [], 0, 0
    while i < len(lines):
        out.append(lines[i])
        m = COMMENT.match(lines[i])
        if m:
            new = emit(m.group(2), int(m.group(1), 16))
            indent = re.match(r'\s*', lines[i + 1]).group(0)
            old = lines[i + 1:i + 1 + len(new)]
            repl = [indent + new[0]] + new[1:]
            if old != repl:
                edits += 1
            out.extend(repl)
            i += 1 + len(new)
            continue
        i += 1
    if edits:
        with open(path, 'wb') as f:
            f.write(nl.join(out).encode('utf-8'))
        changed += 1
        print('%s: %d statement(s)' % (os.path.basename(path), edits))
print('files changed:', changed)
