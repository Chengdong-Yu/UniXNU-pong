#!/usr/bin/env python3
"""Capture the current, unmounted guest OS as the base for changed-band writes.

This is a filesystem update source, not image verification. No hashes or
host/device comparisons are performed. Capture before begin/stage/finish,
keep the phone in REC, and install only bands changed by the local edit.
"""
import argparse
from datetime import datetime, timezone
import gzip
import json
import os
from pathlib import Path
import plistlib
import struct
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--prepared', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--adb', type=Path, required=True)
    p.add_argument('--serial', required=True)
    p.add_argument('--port', type=int, default=5038)
    a = p.parse_args()
    record = a.prepared.resolve() / 'os-storage.json'
    storage = json.loads(record.read_text())
    if storage.get('pending') or not storage.get('ready'):
        raise RuntimeError('finish the pending OS update before refreshing its base')
    installed = json.loads(Path(storage['last_install']).read_text())
    if installed.get('state') != 'complete' or installed['serial'] != a.serial:
        raise RuntimeError('a completed installation for this phone is required')
    window = next(w for w in installed['layout']['windows'] if w['name'] == 'OS')
    base, size = window['offset'], window['size']
    if base % 1048576 or size % 1048576 or size != storage['logical_bytes']:
        raise RuntimeError('unsupported OS update geometry')
    cmd = [str(a.adb), '-P', str(a.port), '-s', a.serial]
    def shell(text):
        return subprocess.check_output(cmd + ['shell', text], text=True).strip()
    if subprocess.check_output(cmd + ['get-state'], text=True).strip() != 'recovery':
        raise RuntimeError('enter REC before capturing the filesystem update source')
    target = '/dev/block/by-name/userdata'
    geometry = installed['geometry']
    real = shell('readlink -f ' + target)
    if (real != geometry['device'] or real != '/dev/block/sda13' or
            int(shell('blockdev --getsize64 ' + target)) != geometry['bytes']):
        raise RuntimeError('wrong Pong userdata device')
    def unmounted():
        if shell('ls /sys/class/block/sda13/holders') or any(
                line.split()[0] in (real, target) for line in shell('cat /proc/mounts').splitlines()):
            raise RuntimeError('OS update source must remain unmounted')
    unmounted()
    header = subprocess.check_output(cmd + ['exec-out', 'dd', 'if=' + target,
        'bs=4096', 'count=1', 'status=none'])
    if len(header) != 4096 or header[:8] != b'SHNGROOT':
        raise RuntimeError('missing Pong storage layout')
    count = struct.unpack_from('<I', header, 12)[0]
    if count > 20:
        raise RuntimeError('invalid Pong window count')
    windows = {}
    for i in range(count):
        name, offset, amount = struct.unpack_from('<32sQQ', header, 24 + i * 128)
        windows[name.split(b'\0')[0].decode()] = (offset, amount)
    if windows.get('OS') != (base, size):
        raise RuntimeError('the installed OS window changed')
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    bundle = out / 'current-os.sparsebundle'
    bundle.mkdir()
    (bundle / 'bands').mkdir()
    template = Path(storage['image'])
    info = plistlib.loads((template / 'Info.plist').read_bytes())
    band = info['band-size']
    if info['size'] != size or band != storage['band_bytes']:
        raise RuntimeError('sparse filesystem geometry differs')
    for name in ('Info.plist', 'Info.bckup'):
        (bundle / name).write_bytes(plistlib.dumps(info))
    (bundle / 'token').touch()
    process = subprocess.Popen(cmd + ['exec-out',
        f'set -o pipefail; dd if={target} bs=1048576 skip={base//1048576} '
        f'count={size//1048576} status=none | gzip -1 -c'], stdout=subprocess.PIPE)
    done = 0
    try:
        with gzip.GzipFile(fileobj=process.stdout) as stream:
            while done < size:
                amount = min(band, size - done)
                data = stream.read(amount)
                if len(data) != amount:
                    raise RuntimeError('incomplete live filesystem capture; base unchanged')
                # Sparse representation only; never compare against an older
                # image, and never write captured data back to the phone.
                if data != bytes(amount):
                    (bundle / 'bands' / f'{done//band:x}').write_bytes(data)
                done += amount
                if done % (512 << 20) == 0:
                    print(f'Current OS update base: {done}/{size}', flush=True)
            if stream.read(1):
                raise RuntimeError('unexpected data beyond the bounded OS window')
        if process.wait():
            raise RuntimeError('REC filesystem capture failed; base unchanged')
        unmounted()
        receipt = dict(state='complete', serial=a.serial, offset=base, bytes=done,
            purpose='current unmounted OS base for changed-band updates',
            previous_storage=storage, image_hashes=False, readback_comparison=False,
            created_utc=datetime.now(timezone.utc).isoformat())
        (out / 'capture.json').write_text(json.dumps(receipt, indent=2) + '\n')
        storage.update(image=str(bundle), capture_receipt=str(out / 'capture.json'))
        temporary = record.with_suffix('.new')
        temporary.write_text(json.dumps(storage, indent=2) + '\n')
        os.sync()
        temporary.replace(record)
        print('Current guest OS preserved; begin tracking before editing, then write changed bands only', flush=True)
    except BaseException:
        if process.poll() is None:
            process.kill()
            process.wait()
        raise


if __name__ == '__main__':
    main()
