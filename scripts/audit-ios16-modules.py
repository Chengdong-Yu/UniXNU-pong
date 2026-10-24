#!/usr/bin/env python3
"""Inventory genuine 20G81 module closures and static bindings; never boot them."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

DEVICE = Path(__file__).resolve().parents[1]
ROOT = DEVICE.parent
sys.path.insert(0, str(DEVICE / 'kits/NativePrelink'))
from native_prelink import macho, backed_offset, unserialize_xml, plan
from emit_prelink import COALESCED, instruction_inventory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path, default=ROOT / 'build/ios16-corecrypto-audit/kernelcache.release.ipad7.decompressed')
    parser.add_argument('--output', type=Path, default=ROOT / 'evidence/ios16-module-static-audit.json')
    args = parser.parse_args()
    original = args.original.read_bytes()
    if hashlib.sha256(original).hexdigest() != 'd260defcc2eaac258fd3a2234454dda46791dae45e97608c5416f9ba8fd366e9':
        raise ValueError('Expected pinned 20G81 ordinary ARM64 image')
    kernel = DEVICE / 'mac-build/ios16-xnu-pong/mach.development.pong'
    native = kernel.read_bytes()
    image, native_image = macho(original), macho(native)
    info = next(s for s in image['segments'] if s['name'] == '__PRELINK_INFO')
    at = backed_offset(image['segments'], info['va'], info['filesize'], len(original))
    entries = {e['CFBundleIdentifier']: e for e in unserialize_xml(original[at:at + info['filesize']])['_PrelinkInfoDictionary']}
    old_base = min(s['va'] for s in image['segments'] if s['name'] in COALESCED and s['size'])
    native_end = max(s['va'] + s['size'] for s in native_image['segments'] if s['size'])
    slide = (native_end + 0x100000 - old_base + 4095) & ~4095
    report = {'native_sha256': hashlib.sha256(native).hexdigest(),
        'original_sha256': hashlib.sha256(original).hexdigest(),
        'runtime_qualified': False, 'images_modified': False, 'modules': []}
    identifiers = ['com.apple.kec.corecrypto', 'com.apple.kec.Libm', 'com.apple.kec.pthread',
        'com.apple.iokit.IOStorageFamily', 'com.apple.kext.CoreTrust', 'com.apple.security.AppleImage4',
        'com.apple.driver.AppleMobileFileIntegrity', 'com.apple.security.sandbox',
        'com.apple.filesystems.apfs', 'com.apple.driver.AppleSEPKeyStore']
    for identifier in identifiers:
        entry = entries[identifier]
        selected, interfaces, pending = set(), set(), [identifier]
        while pending:
            name = pending.pop()
            if name in selected or name in interfaces:
                continue
            item = entries[name]
            if item.get('OSKernelResource') is True:
                interfaces.add(name)
            else:
                selected.add(name)
                pending.extend(item.get('OSBundleLibraries') or {})
        value = {'identifier': identifier, 'version': entry.get('CFBundleVersion'),
            'security_extension': bool(entry.get('AppleSecurityExtension')),
            'closure': sorted(selected), 'kernel_interfaces': sorted(interfaces),
            'runtime_qualified': False,
            'hardware_related_dependencies': sorted(name for name in selected if any(token in name
                for token in ['AppleARMPlatform', 'ApplePMGR', 'AppleT8011', 'SEP', 'AppleA7IOP', 'IOSlaveProcessor']))}
        try:
            result = plan(native, original, [identifier], slide)
            value['pointer_counts'] = result['pointer_counts']
            modules = {name: macho(original, backed_offset(image['segments'],
                entries[name]['_PrelinkExecutableLoadAddr'], 32, len(original))) for name in result['components']}
            value['instruction_inventory'] = instruction_inventory(original, image['segments'], modules)
            value['static_status'] = 'pointer plan and PC-relative range inventory completed; ABI remains unqualified'
        except (ValueError, KeyError, StopIteration) as error:
            value['static_status'] = 'blocked by static validation'
            value['error'] = str(error)
        report['modules'].append(value)
        print(identifier, value['static_status'], value.get('error', ''), flush=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
