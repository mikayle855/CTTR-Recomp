# update_upstream.py <upstream checkout>: copy the renderer files from a Tenkaichi3Decomp checkout (port/src/gs),
# re-apply our [ps2x] changes (ps2x_patches/<file>.patch, line-ending tolerant), regenerate shaders.h, record the
# commit. After changing one of their files here, refresh its patch:
#   diff -u <(git -C <checkout> show HEAD:port/src/gs/<file>) <file>  > ps2x_patches/<file>.patch
import re, shutil, subprocess, sys
from pathlib import Path

up = Path(sys.argv[1]) / 'port' / 'src' / 'gs'
here = Path(__file__).resolve().parent
FILES = ['gs_core.c', 'gs_vu1.c', 'gs_draw.c', 'gs_draw.h', 'gs_gl.c', 'gs_gpu.c', 'gs_internal.h',
         'gs_texpack.c', 'gs_texpack.h', 'ui.h']
for f in FILES:
    shutil.copy(up / f, here / f)
for f in (up / 'shaders').glob('*'):
    shutil.copy(f, here / 'shaders' / f.name)
for patch in sorted((here / 'ps2x_patches').glob('*.patch')):
    target = here / patch.name[:-len('.patch')].replace('__', '/')   # shaders__gs.vert.patch -> shaders/gs.vert
    s = target.read_text(encoding='utf-8').replace('\r\n', '\n')
    pt = patch.read_text(encoding='utf-8').replace('\r\n', '\n')
    for h in re.split(r'(?m)^@@.*@@.*\n', pt)[1:]:
        old, new = [], []
        for line in h.split('\n'):
            if line.startswith(' '): old.append(line[1:]); new.append(line[1:])
            elif line.startswith('-'): old.append(line[1:])
            elif line.startswith('+'): new.append(line[1:])
        o, n = '\n'.join(old), '\n'.join(new)
        if s.count(o) != 1:
            sys.exit(f'{patch.name}: a hunk does not apply (fix it by hand): ' + o[:160])
        s = s.replace(o, n)
    target.write_text(s, encoding='utf-8')
    print('patched', target.name)
subprocess.run([sys.executable, str(here / 'gen_shaders.py')], check=True)
commit = subprocess.run(['git', '-C', sys.argv[1], 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
(here / 'UPSTREAM_COMMIT').write_text(commit + '\n')
print('updated to', commit)
