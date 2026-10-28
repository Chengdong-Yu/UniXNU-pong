#!/usr/bin/env python3
"""Pack locally extracted Pong firmware into the archive a CI run expects.

A local build reads the firmware straight out of the directories a phone
extraction produced. This script turns those directories into one zip with the
same layout, so the same inputs can be handed to a GitHub Actions run without
putting any firmware in Git.

Example, after extracting the phone's partitions:

    python3 scripts/pack-pong-firmware.py \
      --wifi ../private-inputs/pong-wifi-firmware \
      --audio ../private-inputs/pong-audio-inputs \
      --adreno730 out/ipados18/firmware/adreno730 \
      --adsp ../private-inputs/pong-power \
      --output ../private-inputs/pong-firmware.zip
"""
import argparse
import hashlib
import json
import zipfile
from pathlib import Path

WIFI_REQUIRED = ('amss.bin', 'amss20.bin', 'm3.bin', 'regdb.bin')
ADRENO_REQUIRED = ('gmu_gen70000.bin', 'a730_sqe.fw', 'a730_zap.mdt', 'a730_zap.b01', 'a730_zap.b02')


def check(base, names, note):
    missing = [name for name in names if not (base / name).is_file()]
    if missing:
        raise SystemExit('refusing to pack: ' + str(base) + ' is missing ' + ', '.join(missing) + ' (' + note + ')')
    empty = [name for name in names if (base / name).stat().st_size == 0]
    if empty:
        raise SystemExit('refusing to pack: empty file in ' + str(base) + ': ' + ', '.join(empty))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--wifi', type=Path, required=True, help='directory holding amss.bin, amss20.bin, m3.bin, regdb.bin and bdwlan*')
    parser.add_argument('--audio', type=Path, required=True, help='directory holding tfa98xx.cnt')
    parser.add_argument('--adreno730', type=Path, required=True, help='directory holding the Adreno 730 GMU/SQE/ZAP files')
    parser.add_argument('--adsp', type=Path, required=True, help='directory holding adsp.mbn')
    parser.add_argument('--output', type=Path, required=True, help='zip to write; the firmware itself stays local')
    args = parser.parse_args()
    wifi, audio = args.wifi.expanduser().resolve(), args.audio.expanduser().resolve()
    adreno, adsp = args.adreno730.expanduser().resolve(), args.adsp.expanduser().resolve()
    check(wifi, WIFI_REQUIRED, 'the phone modem image/qca6490 files')
    check(adreno, ADRENO_REQUIRED, 'the phone vendor GPU firmware')
    check(audio, ('tfa98xx.cnt',), 'the phone vendor amplifier container')
    check(adsp, ('adsp.mbn',), 'the phone modem ADSP firmware')
    if not any(child.name.startswith('bdwlan') for child in wifi.iterdir() if child.is_file()):
        raise SystemExit('refusing to pack: ' + str(wifi) + ' has no bdwlan* board data')
    output = args.output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    groups = {'wifi': wifi, 'audio': audio, 'adreno730': adreno, 'power': adsp}
    written = []
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for group, base in groups.items():
            for source in sorted(path for path in base.rglob('*') if path.is_file()):
                if group == 'power' and source.name != 'adsp.mbn':
                    continue
                if group == 'audio' and source.name != 'tfa98xx.cnt':
                    continue
                if group == 'adreno730' and source.name not in ADRENO_REQUIRED:
                    continue
                name = group + '/' + source.name
                archive.write(source, name)
                written.append({'path': name, 'bytes': source.stat().st_size})
        archive.writestr('manifest.json', json.dumps({
            'device': 'Nothing Phone (2) / Pong / SM8475',
            'layout': 'wifi/, audio/, adreno730/, power/',
            'files': written,
            'note': 'Extracted from this phone. Do not commit the archive or its contents.',
        }, indent=2) + '\n')
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    print(json.dumps({'archive': str(output), 'bytes': output.stat().st_size,
                      'sha256': digest, 'files': len(written)}, indent=2))


if __name__ == '__main__':
    main()
