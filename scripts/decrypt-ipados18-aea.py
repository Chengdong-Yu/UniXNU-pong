#!/usr/bin/env python3
"""Decrypt one profile-1 Apple Encrypted Archive using its public FCS key.

The AEA prologue is AEA1, a little-endian profile word, and the auth-data
length. Auth data is a sequence of entries: 4-byte little-endian entry length,
then a NUL-terminated key and the remaining bytes as the value. Profile 1
restore images carry com.apple.wkms.fcs-key-url (a public PEM) and
com.apple.wkms.fcs-response (enc-request and wrapped-key). The PEM unwraps
the wrapped key with HPKE (DHKEM P-256, HKDF-SHA256, AES-256-GCM). The raw
key is passed to /usr/bin/aea and is not printed.
"""
import argparse
import base64
import json
import subprocess
from pathlib import Path

from pyhpke import AEADId, CipherSuite, KDFId, KEMId, KEMKey


def auth_fields(path):
    data = path.read_bytes()
    if data[:4] != b'AEA1':
        raise SystemExit(f'{path} does not start with AEA1')
    profile = int.from_bytes(data[4:8], 'little') & 0xFFFFFF
    length = int.from_bytes(data[8:12], 'little')
    blob = data[12:12 + length]
    if len(blob) != length:
        raise SystemExit(f'{path} auth data is truncated')
    fields = {}
    index = 0
    while index + 4 <= len(blob):
        entry = int.from_bytes(blob[index:index + 4], 'little')
        if entry < 5 or index + entry > len(blob):
            raise SystemExit(f'{path} auth entry length is invalid')
        key, separator, value = blob[index + 4:index + entry].partition(b'\x00')
        if not separator:
            raise SystemExit(f'{path} auth entry has no key terminator')
        fields[key.decode()] = value
        index += entry
    return profile, fields


def unwrap(fields):
    response = json.loads(fields['com.apple.wkms.fcs-response'])
    url = fields['com.apple.wkms.fcs-key-url'].decode().strip('\x00')
    pem = subprocess.check_output(['curl', '--fail', '--location', '--silent', '--show-error', url])
    if b'PRIVATE KEY' not in pem:
        raise SystemExit('FCS key URL did not return a PEM private key')
    suite = CipherSuite.new(KEMId.DHKEM_P256_HKDF_SHA256, KDFId.HKDF_SHA256, AEADId.AES256_GCM)
    private = KEMKey.from_pem(pem)
    recipient = suite.create_recipient_context(base64.b64decode(response['enc-request']), private)
    key = recipient.open(base64.b64decode(response['wrapped-key']))
    if len(key) != 32:
        raise SystemExit(f'unwrapped key length is {len(key)}, expected 32')
    return key


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--aea', default='/usr/bin/aea')
    args = parser.parse_args()
    profile, fields = auth_fields(args.input)
    if profile != 1:
        raise SystemExit(f'{args.input} is AEA profile {profile}, not profile 1')
    key = unwrap(fields)
    print(f'unwrapped FCS key length {len(key)}', flush=True)
    encoded = 'base64:' + base64.b64encode(key).decode()
    try:
        subprocess.run([args.aea, 'decrypt', '-i', str(args.input), '-o', str(args.output),
                        '-key-value', encoded], check=True)
    except subprocess.CalledProcessError as error:
        raise SystemExit(f'aea decrypt failed with status {error.returncode}') from None
    print(f'decrypted {args.output} ({args.output.stat().st_size} bytes)', flush=True)


if __name__ == '__main__':
    main()
