#!/usr/bin/env python3
"""Extract the LZFSE payload from a 22H374 IM4P kernelcache and decompress it.

The KernelCache member is a plain IM4P (tag type krnl). Its payload is an
ASN.1 OCTET STRING whose bytes start with b'bvx2'. That is LZFSE, not LZSS
and not an encrypted AEA. The decompressed file is the fat Mach-O the UFS
binder and the prelinked boot expect.
"""
import argparse
import shutil
import subprocess
from pathlib import Path


def der_length(data, index):
    first = data[index]
    if first < 0x80:
        return first, index + 1
    count = first & 0x7F
    if count == 0 or index + 1 + count > len(data):
        raise SystemExit('IM4P DER length is truncated')
    return int.from_bytes(data[index + 1:index + 1 + count], 'big'), index + 1 + count


def lzfse_payload(data):
    if not data or data[0] != 0x30:
        raise SystemExit('kernelcache is not an IM4P SEQUENCE')
    length, index = der_length(data, 1)
    end = index + length
    if end > len(data):
        raise SystemExit('IM4P SEQUENCE extends past the file')
    while index < end:
        tag = data[index]
        size, body_at = der_length(data, index + 1)
        body = data[body_at:body_at + size]
        if tag == 0x04 and body.startswith(b'bvx2'):
            return body
        index = body_at + size
    raise SystemExit('IM4P has no OCTET STRING payload starting with bvx2')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--im4p', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--lzfse', default='')
    args = parser.parse_args()
    tool = args.lzfse or shutil.which('lzfse')
    if not tool:
        raise SystemExit('lzfse is not on PATH; brew install lzfse')
    payload = lzfse_payload(args.im4p.read_bytes())
    print(f'LZFSE payload {len(payload)} bytes, magic {payload[:4]!r}', flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + '.bvx2')
    temporary.write_bytes(payload)
    subprocess.run([tool, '-decode', '-i', str(temporary), '-o', str(args.output)], check=True)
    temporary.unlink()
    header = args.output.read_bytes()[:4]
    print(f'decompressed {args.output.stat().st_size} bytes, magic {header!r}', flush=True)
    if header not in (b'\xca\xfe\xba\xbe', b'\xfe\xed\xfa\xcf', b'\xcf\xfa\xed\xfe'):
        raise SystemExit(f'decompressed kernelcache is not a Mach-O: magic {header!r}')


if __name__ == '__main__':
    main()
