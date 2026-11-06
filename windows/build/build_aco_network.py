#!/usr/bin/env python3
"""Build the SPIR-V an architecture's ACO records were made from (ACO Mode), from windows/shaders/<arch>/pipelines.json,
compiled as written (glslang, as the Linux network is built) - not the Windows driver's preprocessed form that
build_network.py makes. RDNA3's network (rdna3: gfx11, e4m3 values as FP16 through the FP16 matrix instructions) runs
as ACO's machine code only: windows/data/aco/rdna3/records holds one record per pipeline, found by its SPIR-V's hash.
(RDNA4's records belong to the Linux network: linux/build/build_network.py rdna4.)

    build_aco_network.py <arch> [--experimental | --optimized] --out DIR [--check DIR]

Writes g_<name>.spv + the marker files, g_<name>_grade.spv (style_grade) and temporal/ (the temporal variants, their
_grade twins and the motion estimator). --check DIR compares every SPV and marker with DIR byte for byte.

--experimental / --optimized: pipelines.json with pipelines-<overlay>.json applied - "change" adds defines to the
pipeline of that name, "new" builds extra pipelines from a base one's defines plus its own - and, in that folder,
fixed-attention.txt (the pipelines whose fixed probability tables the host writes), const-heads.txt when the overlay
marks constant heads (NR_CONSTH) and block-routes.txt when it routes blocks to pipelines of their own
("<suffix> <kernel>... : <block>..."). The overlay carries no runtime/ passes: they are the default network's.
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

R = Path(__file__).resolve().parents[2]
MOTION = ['motion_luma', 'motion_estimate']


def glslang(arch, src, defines, out):
    cmd = [str(R / 'toolchain/glslang/bin/glslang'), '-V', '--target-env', 'vulkan1.3',
           '-I' + str(R / 'windows/shaders' / arch / 'include'), '-I' + str(R / 'windows/shaders/passes/include')]
    cmd += ['-D' + d for d in defines] + [str(src), '-o', str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'glslang failed on {src}:\n{r.stdout}{r.stderr}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('arch')
    ap.add_argument('--experimental', action='store_true')
    ap.add_argument('--optimized', action='store_true')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--check', type=Path)
    a = ap.parse_args()
    src = R / 'windows/shaders' / a.arch
    table = json.loads((src / 'pipelines.json').read_text())
    if a.experimental + a.optimized > 1:
        sys.exit('one of --experimental, --optimized')
    overlay = 'experimental' if a.experimental else 'optimized' if a.optimized else ''
    if overlay:
        ov = json.loads((src / f'pipelines-{overlay}.json').read_text())
        default = table['pipelines']

        def derive(e, c):
            return dict(e, defines=[x for x in e['defines'] if x not in c.get('remove', [])] + c.get('add', []))
        assert not set(ov['change']) - set(default), f'pipelines-{overlay}.json changes unknown pipelines'
        table['pipelines'] = {k: derive(e, ov['change'].get(k, {})) for k, e in default.items()}
        table['pipelines'].update({k: derive(default[c['base']], c) for k, c in ov['new'].items()})
    out = a.out
    if out.exists():
        shutil.rmtree(out)
    (out / 'temporal').mkdir(parents=True)
    pipelines = table['pipelines']
    # post_no_out1 in the manifest tells the host that the post kernels never write their second output (it makes
    # that image 1x1): it has to say what they are built with.
    consts = dict(r.split() for r in table['markers']['shader-constants.txt'][1:])
    if consts.get('post_no_out1') == '1' and 'NR_POST_NO_OUT1=1' not in pipelines['fswinimagepost32']['defines']:
        sys.exit('pipelines.json: post_no_out1 1, but fswinimagepost32 is built without NR_POST_NO_OUT1=1')
    for name, e in pipelines.items():
        glslang(a.arch, src / e['source'], e['defines'], out / f'g_{name}.spv')
    for name, text in table['markers'].items():
        (out / name).write_text('\n'.join(text) + '\n' if isinstance(text, list) else text + '\n')
    for name, v in table['variants'].items():
        base = pipelines[v['base']]
        glslang(a.arch, src / base['source'], base['defines'] + v['add'], out / 'temporal' / f'{name}.spv')
    grade = table['style_grade']
    for name in grade['pipelines']:
        e = pipelines[name]
        glslang(a.arch, src / e['source'], e['defines'] + grade['add'], out / f'g_{name}_grade.spv')
    for name in grade['variants']:
        base = pipelines[table['variants'][name]['base']]
        glslang(a.arch, src / base['source'], base['defines'] + table['variants'][name]['add'] + grade['add'],
                out / 'temporal' / f'{name}_grade.spv')
    for k in MOTION:
        glslang(a.arch, R / 'windows/shaders/passes' / f'{k}.comp', [], out / 'temporal' / f'{k}.spv')
    shutil.copy2(out / 'shader-constants.txt', out / 'temporal' / 'shader-constants.txt')
    if overlay:
        fixp = sorted(k for k, e in pipelines.items() if 'NR_FIXP=1' in e['defines'])
        (out / 'fixed-attention.txt').write_text('\n'.join(fixp) + '\n')
        consth = sorted(k for k, e in pipelines.items() if 'NR_CONSTH=1' in e['defines'])
        if consth:
            (out / 'const-heads.txt').write_text('\n'.join(consth) + '\n')
        if ov.get('routes'):
            lines = []
            for r in ov['routes']:
                assert all(f'{k}{r["suffix"]}' in pipelines for k in r['kernels']), r
                lines.append(' '.join([r['suffix']] + r['kernels'] + [':'] + [str(b) for b in r['blocks']]))
            (out / 'block-routes.txt').write_text('\n'.join(lines) + '\n')
    print(f'{out}: {len(pipelines) + len(grade["pipelines"])} network pipelines, '
          f'{len(table["variants"]) + len(grade["variants"]) + len(MOTION)} temporal')
    if a.check:
        bad = [str(p.relative_to(out)) for p in sorted(out.rglob('*')) if p.is_file()
               if not (a.check / p.relative_to(out)).is_file() or (a.check / p.relative_to(out)).read_bytes() != p.read_bytes()]
        print('check against', a.check, ':', 'identical' if not bad else 'DIFFERENT: ' + ' '.join(bad))
        sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
