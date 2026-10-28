#!/usr/bin/env python3
"""Track OS writes in sparsebundle bands; retain volume size and all system files."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from sparsebundle import describe, snapshot, storage_for


def dump(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def begin(a):
    prepared = a.prepared.resolve()
    raw = prepared / 'pong-ipados18-system.cdr'
    storage = storage_for(raw)
    baseline = a.baseline_install.resolve()
    installed = json.loads(baseline.read_text())
    if installed.get('state') != 'complete' or not (
            installed['completed'].get('OS') or installed.get('os_generation_complete')):
        raise RuntimeError('completed OS installation receipt required')
    os_window = next(w for w in installed['layout']['windows'] if w['name'] == 'OS')
    if storage is None:
        if str(raw) != os_window['image'] or raw.stat().st_size != os_window['size']:
            raise RuntimeError('baseline does not describe this prepared raw OS')
        bundle = prepared / 'pong-ipados18-system.sparsebundle'
        if bundle.exists():
            raise RuntimeError('preserving existing unregistered sparsebundle')
        # UDTO source has no checksums; UDSB has raw writable bands, no
        # compression checksum or host/device image comparison.
        subprocess.run(['hdiutil', 'convert', str(raw), '-format', 'UDSB',
                        '-o', str(bundle)], check=True)
        band, size = describe(bundle)
        if size != os_window['size']:
            raise RuntimeError('conversion changed logical OS geometry; raw base retained')
        storage = dict(format='pong-os-sparsebundle-v1', ready=True, image=str(bundle),
                       raw_base=str(raw), logical_bytes=size, band_bytes=band,
                       last_install=str(baseline), revision=baseline.parent.name,
                       pending=None)
    if Path(storage['last_install']) != baseline or storage.get('pending'):
        raise RuntimeError('baseline differs from installed generation or update is pending')
    a.out.mkdir(parents=True, exist_ok=False)
    bands = snapshot(storage['image'])
    dump(a.out / 'before.json', dict(build='22H374', revision=a.revision,
         base_install=str(baseline), serial=installed['serial'],
         geometry=installed['geometry'], window=os_window, image=storage['image'],
         band_bytes=storage['band_bytes'], logical_bytes=storage['logical_bytes'],
         bands=bands, created_utc=datetime.now(timezone.utc).isoformat()))
    storage['pending'] = str(a.out.resolve())
    dump(prepared / 'os-storage.json', storage)
    # ExFAT timestamps have coarse granularity. No image reads or hashes.
    time.sleep(2.2)
    print('OS write tracking ready; raw baseline retained', flush=True)


def finish(a):
    before = json.loads((a.out / 'before.json').read_text())
    storage = storage_for(a.prepared.resolve() / 'pong-ipados18-system.cdr')
    if storage['pending'] != str(a.out.resolve()):
        raise RuntimeError('update generation differs')
    current = snapshot(before['image'])
    changed = sorted(int(k, 16) for k in set(before['bands']) | set(current)
                     if before['bands'].get(k) != current.get(k))
    ranges = []
    for index in changed:
        offset = index * before['band_bytes']
        amount = min(before['band_bytes'], before['logical_bytes'] - offset)
        if amount <= 0:
            raise RuntimeError('dirty band outside OS')
        if ranges and ranges[-1][0] + ranges[-1][1] == offset:
            ranges[-1][1] += amount
        else:
            ranges.append([offset, amount])
    # Timestamp tracking cannot prove that an unreported APFS metadata band
    # stayed unchanged. A complete OS update streams the same logical image
    # without relying on that assumption, with no hashes or readback.
    if a.full_transfer:
        ranges = [[0, before['logical_bytes']]]
    plan = dict(format='pong-os-delta-v1', build='22H374',
                revision=before['revision'], base_install=before['base_install'],
                serial=before['serial'], geometry=before['geometry'],
                window=before['window'], image=before['image'],
                band_bytes=before['band_bytes'], logical_bytes=before['logical_bytes'],
                ranges=ranges, dirty_bands=len(changed), bands=current,
                transfer_bytes=sum(n for _, n in ranges), storage_record=storage['record'],
                full_transfer=a.full_transfer,
                tracking='complete logical OS' if a.full_transfer else
                         'band modification timestamps and sizes; no image hashes/comparison')
    dump(a.out / 'delta.json', plan)
    print(json.dumps({k: plan[k] for k in ('revision', 'dirty_bands', 'ranges',
                                          'transfer_bytes', 'logical_bytes')}, indent=2))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=('begin', 'finish'))
    p.add_argument('--prepared', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--baseline-install', type=Path)
    p.add_argument('--revision')
    p.add_argument('--full-transfer', action='store_true',
                   help='finish a complete OS transfer plan without image comparison')
    a = p.parse_args()
    if a.mode == 'begin' and (not a.baseline_install or not a.revision):
        p.error('begin requires --baseline-install and --revision')
    globals()[a.mode](a)


if __name__ == '__main__':
    main()
