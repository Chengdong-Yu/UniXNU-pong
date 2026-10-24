"""Validate Pong loader inputs consumed by Darwin 22 and AppleImage4 5.0.0.

Missing optional identity/fuse/manifest inputs must remain missing. A software
loader cannot manufacture Apple hardware identity or prior-stage authorization.
"""
import hashlib
import json
import struct
import uuid
import xml.etree.ElementTree as ET


def parse_adt(blob):
    nodes = {}

    def read(at, parent, depth=0):
        if depth > 32 or at + 8 > len(blob):
            raise ValueError('Invalid ADT node extent/depth')
        count, children = struct.unpack_from('<II', blob, at)
        at += 8
        props = {}
        if count > 4096 or children > 4096:
            raise ValueError('Invalid ADT node counts')
        for _ in range(count):
            if at + 36 > len(blob):
                raise ValueError('Truncated ADT property')
            key, size = struct.unpack_from('<32sI', blob, at)
            key = key.rstrip(b'\0').decode('ascii')
            at += 36
            if not key or key in props or size > len(blob) - at:
                raise ValueError('Invalid/duplicate ADT property')
            props[key] = blob[at:at + size]
            at += (size + 3) & ~3
        name = props.get('name', b'')
        if not name.endswith(b'\0') or b'/' in name or b'\0' in name[:-1]:
            raise ValueError('Invalid ADT name')
        path = parent + '/' + name[:-1].decode('ascii') if depth else '/'
        if path in nodes:
            raise ValueError('Duplicate ADT path')
        nodes[path] = props
        for _ in range(children):
            at = read(at, path.rstrip('/'), depth + 1)
        return at

    if read(0, '') != len(blob):
        raise ValueError('Trailing/truncated ADT bytes')
    return nodes


def audit_adt(blob, *, require_manifest=True, boot_manifest=None):
    nodes = parse_adt(blob)
    chosen = nodes['/chosen']
    manifest = nodes['/chosen/manifest-properties']

    def width(props, key, size, required=True):
        if key not in props:
            if required:
                raise ValueError('Missing loader property: ' + key)
            return
        if len(props[key]) != size:
            raise ValueError(f'{key}: expected {size} bytes')

    width(chosen, 'mix-n-match-prevention-status', 4)
    width(chosen, 'debug-enabled', 4)
    width(chosen, 'ephemeral-storage', 4, False)
    width(chosen, 'amfi-only-platform-code', 4, False)
    width(chosen, 'system-volume-auth-blob', 208, False)
    amfi_only_platform_code = False
    if 'amfi-only-platform-code' in chosen:
        if struct.unpack('<I', chosen['amfi-only-platform-code'])[0] != 1:
            raise ValueError('amfi-only-platform-code: expected Sheng strict-policy uint32 value 1')
        amfi_only_platform_code = True
    debug = struct.unpack('<I', chosen['debug-enabled'])[0]
    if debug not in (0, 1):
        raise ValueError('debug-enabled: expected explicit uint32 boolean')
    if chosen['mix-n-match-prevention-status'] != bytes(4):
        raise ValueError('Development loader has no prior Apple manifest chain')
    ephemeral = struct.unpack('<I', chosen['ephemeral-storage'])[0] if 'ephemeral-storage' in chosen else 0
    if ephemeral not in (0, 1):
        raise ValueError('ephemeral-storage: expected explicit uint32 boolean')
    memory_map = nodes['/chosen/memory-map']
    packed_ramdisk = 'PackedRAMDisk' in memory_map
    if packed_ramdisk != bool(ephemeral) or (packed_ramdisk and len(memory_map['PackedRAMDisk']) != 16):
        raise ValueError('PackedRAMDisk range and ephemeral-storage marker must appear together')
    if 'RAMDisk' in memory_map:
        raise ValueError('Pong scratch Data uses the PackedRAMDisk contract only')
    if chosen.get('crypto-hash-method') != b'sha2-384\0':
        raise ValueError('Expected SHA-384 loader digest policy')
    width(manifest, 'BNCH', 48)
    if manifest['BNCH'] == bytes(48):
        raise ValueError('Boot nonce digest must not be a zero placeholder')
    # The same runtime getter copies into fixed-size buffers. Check optional
    # properties too, so a future board addition cannot introduce EILSEQ.
    for key in ('certificate-production-status', 'certificate-security-mode',
                'effective-production-status-ap', 'effective-security-mode-ap',
                'internal-use-only-unit', 'research-enabled',
                'engineering-use-only-unit', 'factory-prerelease-global-trust',
                'chip-epoch', 'board-id', 'chip-id', 'security-domain', 'esdm-fuses'):
        width(chosen, key, 4, False)
    width(chosen, 'unique-chip-id', 8, False)
    # The chip initializer permits ENOENT under AMNM, but nonce initialization
    # at 0xfffffff00b870d4c..d68 does not. Audit every consumer, not just the first.
    width(chosen, 'boot-manifest-hash', 48, require_manifest)
    if chosen.get('boot-manifest-hash') == bytes(48):
        raise ValueError('Boot manifest digest must not be a zero placeholder')
    if boot_manifest is not None:
        if chosen.get('boot-manifest-hash') != hashlib.sha384(boot_manifest).digest():
            raise ValueError('DT boot manifest hash does not match the staged manifest')
        description = json.loads(boot_manifest)
        if (description.get('format') != 'pong-development-boot-manifest-v1' or
                description.get('apple_personalization') is not False):
            raise ValueError('Unexpected development boot manifest policy')
        if bool(debug) != description.get('development_debug', False):
            raise ValueError('DT debugger gate disagrees with boot manifest')
        if amfi_only_platform_code != description.get('amfi_only_platform_code', False):
            raise ValueError('DT AMFI policy disagrees with boot manifest')
        expected_auth_blob = description.get('system_volume_auth_blob_sha256')
        actual_auth_blob = chosen.get('system-volume-auth-blob')
        if (expected_auth_blob is None) != (actual_auth_blob is None):
            raise ValueError('DT system-volume-auth-blob disagrees with boot manifest')
        if actual_auth_blob is not None and hashlib.sha256(actual_auth_blob).hexdigest() != expected_auth_blob:
            raise ValueError('DT system-volume-auth-blob hash differs from boot manifest')
        dev_args = {'pong_allow_unauthenticated_root=1', 'cs_enforcement_disable=1',
                    'cs_library_val_enable=0'}
        if dev_args.intersection(description.get('boot_args', '').split()) and not debug:
            raise ValueError('Development security options require /chosen/debug-enabled=1')
    if 'love' in manifest:
        value = manifest['love']
        if not 1 <= len(value) <= 64 or not value.endswith(b'\0'):
            raise ValueError('love: expected terminated string, at most 64 bytes')
    if 'apfs-preboot-uuid' in chosen:
        value = chosen['apfs-preboot-uuid']
        if len(value) != 37 or value[-1:] != b'\0':
            raise ValueError('apfs-preboot-uuid: expected UUID C string')
        uuid.UUID(value[:-1].decode('ascii'))
    for key in ('dram-base', 'dram-size', 'darwinboot-memory-size'):
        width(chosen, key, 8, key != 'darwinboot-memory-size')
    width(chosen, 'random-seed', 64)
    if 'root-matching' in chosen:
        value = chosen['root-matching']
        if not value.endswith(b'\0'):
            raise ValueError('root-matching must be terminated XML')
        try:
            xml = ET.fromstring(value[:-1])
        except ET.ParseError as error:
            raise ValueError('root-matching: malformed XML') from error
        values = list(xml)
        if (xml.tag != 'dict' or len(values) != 4 or
                values[0].text != 'IOProviderClass' or values[1].text != 'IOMedia' or
                values[2].text != 'IOPropertyMatch' or
                [v.tag for v in values] != ['key', 'string', 'key', 'dict']):
            raise ValueError('Unexpected root matching dictionary')
        match = list(values[3])
        if (len(match) != 2 or match[0].text != 'UUID' or
                [v.tag for v in match] != ['key', 'string']):
            raise ValueError('Root must select the installed APFS volume UUID')
        uuid.UUID(match[1].text)
    cpu_paths = [path for path in nodes
                 if path.rsplit('/', 1)[0] == '/cpus' and
                 path.rsplit('/', 1)[1].startswith('cpu') and
                 path.rsplit('/', 1)[1] != 'cpu-map']
    if cpu_paths != ['/cpus/cpu0']:
        raise ValueError('Pong single-core ADT must expose only /cpus/cpu0')
    cpu_phys_ids = set()
    boot_cpus = 0
    for path in cpu_paths:
        cpu = nodes[path]
        width(cpu, 'reg', 4)
        width(cpu, 'timebase-frequency', 4)
        if struct.unpack('<I', cpu['timebase-frequency'])[0] != 19200000:
            raise ValueError('Pong CNTV frequency must come from the SM8475 DT')
        phys_id = struct.unpack('<I', cpu['reg'])[0]
        if phys_id in cpu_phys_ids:
            raise ValueError('Duplicate Pong MPIDR affinity in /cpus')
        cpu_phys_ids.add(phys_id)
        if cpu.get('state') == b'running\0':
            boot_cpus += 1
    if boot_cpus != 1 or struct.unpack('<I', nodes['/cpus/cpu0']['reg'])[0] != 0:
        raise ValueError('Pong ADT must mark CPU0 as its unique running boot CPU')
    return {'status': 'PASS', 'nodes': len(nodes),
            'adt_sha256': hashlib.sha256(blob).hexdigest(),
            'nonce_digest_bytes': 48, 'nonce_scope': 'development image',
            'prior_manifest_required': require_manifest,
            'manifest_content_verified': boot_manifest is not None,
            'development_debug': bool(debug),
            'amfi_only_platform_code': amfi_only_platform_code,
            'packed_data_ramdisk': packed_ramdisk,
            'system_volume_auth_blob_bytes': len(chosen['system-volume-auth-blob']) if 'system-volume-auth-blob' in chosen else 0,
            'cpu_count': 1,
            'optional_identity_inputs': sorted(set(chosen) & {
                'unique-chip-id', 'chip-id', 'board-id', 'boot-manifest-hash'}),
            'checks': ['ADT structure', 'Image4 mandatory inputs',
                       'optional getter buffer widths', 'root match XML/UUID',
                       'AMFI policy cell', '20G81 SystemVolume auth blob',
                       'DRAM cell widths', 'Pong timer cell', 'ephemeral Data RAMDisk range']}
