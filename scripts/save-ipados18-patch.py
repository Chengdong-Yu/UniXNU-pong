#!/usr/bin/env python3
"""Record the Darwin 24 source changes without reading or hashing images."""
from pathlib import Path
import argparse, subprocess, sys
p=argparse.ArgumentParser(description=__doc__)
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
import pong_paths
p.add_argument('--source',type=Path,default=pong_paths.xnu_work(pong_paths.build_tree()))
a=p.parse_args();root=ROOT
revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=a.source,text=True).strip()
if revision!='43a90889846e00bfb5cf1d255cdc0a701a1e05a4':raise SystemExit('Wrong source revision; expected xnu-11417.140.69')
subprocess.run(['git','add','-u'],cwd=a.source,check=True)
out=root/'patches/xnu-ipados18/0001-pong-darwin24.patch'
with out.open('wb') as f:subprocess.run(['git','diff','--binary','HEAD'],cwd=a.source,stdout=f,check=True)
print(out)
