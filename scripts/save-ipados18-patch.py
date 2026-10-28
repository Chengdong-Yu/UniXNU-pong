#!/usr/bin/env python3
"""Record the Darwin 24 source changes without reading or hashing images."""
from pathlib import Path
import argparse, subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,default=Path('/Volumes/PongDev/build/Pong-iPadOS18-native/tree/.cache/xnu-work'))
a=p.parse_args();root=Path(__file__).resolve().parents[1]
revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=a.source,text=True).strip()
if revision!='43a90889846e00bfb5cf1d255cdc0a701a1e05a4':raise SystemExit('Wrong source revision; expected xnu-11417.140.69')
subprocess.run(['git','add','-u'],cwd=a.source,check=True)
out=root/'patches/xnu-ipados18/0001-pong-darwin24.patch'
with out.open('wb') as f:subprocess.run(['git','diff','--binary','HEAD'],cwd=a.source,stdout=f,check=True)
print(out)
