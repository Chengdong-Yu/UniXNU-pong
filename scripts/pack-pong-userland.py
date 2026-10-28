#!/usr/bin/env python3
"""Pack a locally prepared 22H374 userland into the archive a CI run expects.

A local build reads the prepared userland straight out of the directory that
`userland/build-ipados18.py prepare/apply/finish` filled in. This script turns
that directory, plus the SystemOS image `userland/build-systemos18.py` writes
somewhere else, into one archive with the layout the workflow stages, so the
same inputs can be handed to a GitHub Actions run without putting any Apple
image in Git.

Example, after finishing a prepared userland locally:

    python3 scripts/pack-pong-userland.py \
      --prepared ../private-inputs/22H374/prepared \
      --systemos out/ipados18/systemos/pong-ipados18-systemos.cdr \
      --output ../private-inputs/pong-userland.zip

Nothing is copied: the two records and three images go into the archive at the
top level, and zip picks the deflate level Python defaults to. The three images
are APFS containers, so the archive stays close to their size.
"""
import argparse
import hashlib
import json
import zipfile
from pathlib import Path

RECORDS = ('result.json', 'apply-complete.json')
IMAGES = ('pong-ipados18-system.cdr', 'pong-ipados18-data.cdr')
SYSTEMOS = 'pong-ipados18-systemos.cdr'


def check_file(path, what):
    path = path.expanduser().resolve()
    if not path.is_file():
        raise SystemExit('refusing to pack: missing ' + what + ': ' + str(path))
    if path.stat().st_size == 0:
        raise SystemExit('refusing to pack: empty ' + what + ': ' + str(path))
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--prepared', type=Path, required=True,
                        help='the directory build-ipados18.py finish wrote: result.json, '
                             'apply-complete.json, pong-ipados18-system.cdr and '
                             'pong-ipados18-data.cdr')
    parser.add_argument('--systemos', type=Path, required=True,
                        help='the .cdr build-systemos18.py wrote')
    parser.add_argument('--output', type=Path, required=True,
                        help='zip to write; the images themselves stay local')
    args = parser.parse_args()
    prepared = args.prepared.expanduser().resolve()
    if not prepared.is_dir():
        raise SystemExit('refusing to pack: not a directory: ' + str(prepared))
    files = {}
    for name in RECORDS + IMAGES:
        files[name] = check_file(prepared / name, name)
    files[SYSTEMOS] = check_file(args.systemos, SYSTEMOS)
    result = json.loads((prepared / 'result.json').read_text())
    if result.get('build') != '22H374':
        raise SystemExit('refusing to pack: result.json names build %r, and the workflow '
                         'builds the 22H374 userdata layout' % result.get('build'))
    for name in IMAGES + (SYSTEMOS,):
        size = files[name].stat().st_size
        if size < 4096 or size % 4096:
            raise SystemExit('refusing to pack: %s is %d bytes; every window input must be a '
                             'raw disk of at least 4096 bytes in 4096-byte blocks'
                             % (name, size))
    storage = prepared / 'os-storage.json'
    if storage.is_file() and json.loads(storage.read_text()).get('ready'):
        raise SystemExit('refusing to pack: os-storage.json records a tracked sparsebundle OS '
                         'generation, and the layout script sizes the raw image. Finish the '
                         'generation first, or pass the plain pong-ipados18-system.cdr.')
    output = args.output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    written = []
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name in RECORDS + IMAGES + (SYSTEMOS,):
            path = files[name]
            archive.write(path, name)
            written.append({'path': name, 'bytes': path.stat().st_size})
        archive.writestr('userland-manifest.json', json.dumps({
            'device': 'Nothing Phone (2) / Pong / SM8475',
            'build': result.get('build'),
            'layout': 'result.json, apply-complete.json, the OS, SystemOS and Data images',
            'files': written,
            'note': 'Prepared locally from the 22H374 restore. The volume UUIDs are read out '
                    'of these images by the run, and the kernel compiles them in, so this '
                    'archive belongs to the kernel built from the same run. Do not commit the '
                    'archive or its contents.',
        }, indent=2) + '\n')
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    print(json.dumps({'archive': str(output), 'bytes': output.stat().st_size,
                      'sha256': digest, 'files': len(written)}, indent=2))


if __name__ == '__main__':
    main()
