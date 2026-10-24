#!/usr/bin/env python3
"""Audit a produced ADT, including rejection of known bad loader contracts."""
import argparse
import copy
import hashlib
import importlib.util
import json
import tomllib
from pathlib import Path

from pong_adt_contract import audit_adt, parse_adt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('adt', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--boot-manifest', type=Path, required=True)
    args = parser.parse_args()
    blob = args.adt.read_bytes()
    boot_manifest = args.boot_manifest.read_bytes()
    report = audit_adt(blob, boot_manifest=boot_manifest)
    description = json.loads(boot_manifest)
    for key, name in [('kernel_sha384', 'kernel.macho'),
                      ('loader_sha384', 'DarwinBoot22.efi')]:
        actual = hashlib.sha384((args.adt.parent / name).read_bytes()).hexdigest()
        if description[key] != actual:
            raise ValueError('Manifest payload mismatch: ' + name)
    config = tomllib.loads((args.adt.parent / 'unixnu.toml').read_text())
    if config['entry'][0]['args'] != description['boot_args']:
        raise ValueError('Manifest boot arguments do not match loader configuration')
    report['payload_bindings_verified'] = ['kernel', 'DarwinBoot22', 'boot arguments']
    spec = importlib.util.spec_from_file_location('maker', Path(__file__).with_name('make-pong-adt.py'))
    maker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(maker)
    original = parse_adt(blob)

    def encode(nodes, path='/'):
        children = [p for p in nodes if p != '/' and
                    p.rsplit('/', 1)[0] == path.rstrip('/')]
        return maker.node(nodes[path]['name'][:-1].decode(),
                          [(k, v) for k, v in nodes[path].items() if k != 'name'],
                          [encode(nodes, p) for p in children])

    chosen, manifest = '/chosen', '/chosen/manifest-properties'
    cases = [
        ('missing BNCH', manifest, 'BNCH', None),
        ('oversized BNCH', manifest, 'BNCH', b'x' * 49),
        ('zero placeholder BNCH', manifest, 'BNCH', bytes(48)),
        ('missing AMNM policy', chosen, 'mix-n-match-prevention-status', None),
        ('missing PE debugger gate', chosen, 'debug-enabled', None),
        ('oversized PE debugger gate', chosen, 'debug-enabled', b'x' * 5),
        ('missing nonce-subsystem CHMH', chosen, 'boot-manifest-hash', None),
        ('zero manifest placeholder', chosen, 'boot-manifest-hash', bytes(48)),
        ('string instead of uint32', chosen, 'mix-n-match-prevention-status', b'false\0'),
        ('oversized optional identity', chosen, 'unique-chip-id', b'x' * 9),
        ('oversized prior manifest hash', chosen, 'boot-manifest-hash', b'x' * 49),
        ('unterminated OS version', manifest, 'love', b'x' * 64),
        ('invalid root XML', chosen, 'root-matching', b'<dict>\0'),
        ('incorrect timer cell', '/cpus/cpu0', 'timebase-frequency', bytes(4)),
    ]
    results = []
    for name, path, key, value in cases:
        nodes = copy.deepcopy(original)
        if value is None:
            nodes[path].pop(key, None)
        else:
            nodes[path][key] = value
        try:
            audit_adt(encode(nodes), boot_manifest=boot_manifest)
        except (ValueError, KeyError) as error:
            results.append({'case': name, 'rejected': True, 'reason': str(error)})
        else:
            raise ValueError('Failed to reject bad input: ' + name)
    if encode(original) != blob:
        raise ValueError('ADT audit round trip changed bytes')
    try:
        audit_adt(blob, boot_manifest=boot_manifest + b' ')
    except ValueError as error:
        results.append({'case': 'manifest content changed', 'rejected': True, 'reason': str(error)})
    else:
        raise ValueError('Failed to reject a manifest content mismatch')
    # A self-consistent manifest with development arguments is still invalid
    # when the PE debugger gate is off. Check the cross-input prerequisite.
    import struct
    nodes = copy.deepcopy(original)
    nodes[chosen]['debug-enabled'] = struct.pack('<I', 0)
    off_manifest = json.loads(boot_manifest)
    off_manifest['development_debug'] = False
    off_manifest = json.dumps(off_manifest).encode()
    nodes[chosen]['boot-manifest-hash'] = hashlib.sha384(off_manifest).digest()
    if any(arg in description['boot_args'].split() for arg in (
            'pong_allow_unauthenticated_root=1', 'cs_enforcement_disable=1', 'cs_library_val_enable=0')):
        try:
            audit_adt(encode(nodes), boot_manifest=off_manifest)
        except ValueError as error:
            results.append({'case': 'development options without PE debugger gate',
                            'rejected': True, 'reason': str(error)})
        else:
            raise ValueError('Failed to reject ineffective development options')
    report['negative_cases'] = results
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'ADT PASS; {len(results)} invalid-input cases rejected')


if __name__ == '__main__':
    main()
