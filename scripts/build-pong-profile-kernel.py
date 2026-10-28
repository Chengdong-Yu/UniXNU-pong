#!/usr/bin/env python3
"""Compile the community Pong Darwin24 aggregate with resolved format2 inputs."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tomllib


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', required=True, type=Path)
    p.add_argument('--artifact-dir', required=True, type=Path)
    p.add_argument('--config-path', type=Path)
    p.add_argument('--hardware-dtb', type=Path)
    p.add_argument('--jobs', type=int, default=4)
    a = p.parse_args(); source = a.source.resolve()
    local = source / 'local.toml'
    opts = tomllib.loads(local.read_text()).get('pong_build', {}) if local.exists() else {}
    command = [sys.executable, str(source / 'scripts/build-ipados18-pong-macos.py'),
               '--artifact-dir', str(a.artifact_dir.resolve()), '--jobs', str(a.jobs), '--skip-image-verification']
    for key in ('tree', 'developer_dir', 'volume_layout'):
        if opts.get(key):
            path = Path(opts[key]).expanduser()
            if not path.is_absolute(): path = source / path
            command += ['--'+key.replace('_','-'), str(path.resolve())]
    for key in ('config_path', 'hardware_dtb'):
        if getattr(a, key): command += ['--'+key.replace('_','-'), str(getattr(a,key).resolve())]
    subprocess.run(command, cwd=source, check=True)
    print(json.dumps(dict(artifact_dir=str(a.artifact_dir.resolve()), command=command), indent=2))


if __name__ == '__main__':
    main()
