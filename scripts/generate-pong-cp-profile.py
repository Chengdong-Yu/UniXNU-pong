#!/usr/bin/env python3
"""Translate Nothing's gen7-4-0 register facts into the Pong CP build input."""
import argparse
from pathlib import Path
import re

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference-dir',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
registers=(a.reference_dir/'gen7_reg.h').read_text()
catalog=(a.reference_dir/'adreno-gpulist.h').read_text()
source=(a.reference_dir/'adreno_gen7.c').read_text()
values={k:int(v,16) for k,v in re.findall(r'^#define\s+(GEN7_\w+)\s+(0x[\da-fA-F]+)\s*$',registers,re.M)}
def array(text,name):
    return re.search(r'\b'+name+r'\[\]\s*=\s*\{(.*?)\n\};',text,re.S).group(1)
lines=['/* SPDX-License-Identifier: GPL-2.0-only */',
       '/* Register tables from the matching NothingOSS SM8475 gen7 sources. */',
       '#ifndef PONG_ADRENO_CP_PROFILE_H','#define PONG_ADRENO_CP_PROFILE_H',
       '#include <stdint.h>']
lines += [f'#define {k} 0x{v:x}u' for k,v in sorted(values.items())]
for name,vendor in [('PongCpHwcg','gen7_0_0_hwcg_regs'),('PongCpGbif','gen7_0_0_gbif_regs')]:
    pairs=re.findall(r'\{\s*(GEN7_\w+),\s*(0x[\da-fA-F]+)\s*\}',array(catalog,vendor))
    if not pairs:raise RuntimeError('missing '+vendor)
    lines += [f'static const uint32_t {name}[][2]={{']
    lines += [f'    {{{reg},{value}}},' for reg,value in pairs]
    lines += ['};']
lines += ['static const uint32_t PongCpProtect[][2]={']
for slot,start,end,noaccess in re.findall(r'\{\s*GEN7_CP_PROTECT_REG\s*\+\s*(\d+),\s*(0x[\da-fA-F]+),\s*(0x[\da-fA-F]+),\s*([01])\s*\}',array(catalog,'gen7_0_0_protected_regs')):
    value=int(start,16)|((int(end,16)-int(start,16))<<18)|(int(noaccess)<<31)
    lines += [f'    {{GEN7_CP_PROTECT_REG+{slot},0x{value:08x}u}},']
lines += ['};','static const uint32_t PongCpRestore[]={']
lines += ['    '+reg+',' for reg in re.findall(r'\bGEN7_\w+\b',array(source,'gen7_pwrup_reglist'))]
lines += ['};','#endif','']
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text('\n'.join(lines))
print(a.output)
