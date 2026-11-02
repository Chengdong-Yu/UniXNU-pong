#!/usr/bin/env python3
"""Prepare the complete, pinned A730/22H374 Mesa source in a new directory.

The patch includes the public Darwin backend and Pong's accumulated fixes.
Existing source checkouts are read only; no firmware or system images are used.
"""
import argparse
import io
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
PIN = '85fda3b36405bcaea8bffc64aa4f2651dc90770d'
PATCH = 'patches/mesa/parity167/pong-darwin22h374.patch'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    destination = args.out.resolve()
    if destination.exists():
        parser.error('output already exists; preserve it or choose a new path')
    source = subprocess.check_output(['git', 'archive', PIN], cwd=args.repository)
    destination.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(source)) as archive:
        archive.extractall(destination, filter='data')
    subprocess.run(['git', 'apply', '--whitespace=nowarn', str(ROOT / PATCH)],
                   cwd=destination, check=True)
    (destination / '.pong-mesa.json').write_text(json.dumps({
        'schema': 1, 'mesa': PIN, 'guest': '22H374', 'native_abi': 1,
        'patch': PATCH, 'snapshot': 'services167',
    }, indent=2) + '\n')
    print(destination)


if __name__ == '__main__':
    main()
