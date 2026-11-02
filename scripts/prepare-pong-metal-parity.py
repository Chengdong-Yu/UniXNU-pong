#!/usr/bin/env python3
"""Prepare a separate pinned translator tree for Pong's iPadOS18 Metal bridge.

The input is the public steelbrain/metal2vulkan Git repository. Existing dirty
translator worktrees are never reset. Applies the feature branch's ordered
patches and the matching guest format ABI adapter; no firmware or images.
"""
import argparse,io,json,subprocess,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
PIN='624bab3918de5b978e1101517abca2b4fa426565'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--repository',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args()
if a.out.exists():p.error('output already exists; preserve it or choose a new path')
source=subprocess.check_output(['git','archive',PIN],cwd=a.repository)
a.out.mkdir(parents=True)
with tarfile.open(fileobj=io.BytesIO(source)) as archive:
    archive.extractall(a.out,filter='data')
patches=sorted((ROOT/'patches/metal2vulkan/parity114').glob('*.patch'))
for patch in patches:
    subprocess.run(['git','apply','--whitespace=nowarn',str(patch)],cwd=a.out,check=True)
(a.out/'.pong-parity.json').write_text(json.dumps(dict(translator=PIN,guest='22H374',
    metal='b82fa2751d62d3b0a8b9330a58d1506d9cdf0963',patches=[p.name for p in patches]),indent=2)+'\n')
print(a.out)
