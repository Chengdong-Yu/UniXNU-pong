#!/usr/bin/env python3
"""Resolve the four profile layers, compile hardware and build Pong Darwin24."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--unixnu', required=True, type=Path)
    p.add_argument('--out', type=Path, default=ROOT / 'out')
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--prepare-only', action='store_true')
    p.add_argument('--defconfig', default='pong_ipados18_defconfig',
                   help='configs/<name> selected before the kernel build')
    a = p.parse_args(); up=a.unixnu.resolve(); out=a.out.resolve()
    spec=importlib.util.spec_from_file_location('pong_format2',ROOT/'scripts/migrate-pong-format2.py')
    migration=importlib.util.module_from_spec(spec);spec.loader.exec_module(migration)
    state=migration.install(up, ROOT.parent/'UniXNU-sm8475-common')
    command=[sys.executable,str(up/'unixnu')]
    common=['-D',str(ROOT),'--out',str(out)]
    subprocess.run(command+['defconfig', a.defconfig, *common],cwd=up,check=True)
    report=json.loads(subprocess.check_output(command+['status','--json',*common],cwd=up,text=True))
    work=Path(report['output']);work.mkdir(parents=True,exist_ok=True)
    (work/'resolved-profile.json').write_text(json.dumps(report,indent=2)+'\n')
    # All selected sources are local and were just resolved. Assemble through
    # the official API without updating unrelated upstream submodules.
    sys.path.insert(0,str(up/'scripts'))
    from unixnu.cli import parser
    from unixnu.config import Context
    from unixnu import feeds,tree,devicetree
    args=parser().parse_args(['status',*common]);ctx=Context(args)
    modules=feeds.resolve(ctx)
    tree.assemble(ctx,modules)
    hardware=devicetree.compile_hardware(ctx)
    result=dict(schema_version=2,source_pins=state,output=str(work),hardware_dtb=str(hardware),
                configuration=report['config_path'],kernel_built=False)
    if not a.prepare_only:
        subprocess.run([sys.executable,str(ROOT/'scripts/build-pong-profile-kernel.py'),
                        '--source',str(ROOT),'--artifact-dir',str(work/'kernel'),
                        '--config-path',report['config_path'],'--hardware-dtb',str(hardware),
                        '--jobs',str(a.jobs)],cwd=ROOT,check=True)
        result['kernel_built']=True
    (work/'PONG_FORMAT2_RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__ == '__main__':
    main()
