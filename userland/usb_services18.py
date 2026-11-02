"""Adapt the supplied 22H374 native USB service to Pong's hardware."""
import os
import plistlib
import struct
import subprocess
from pathlib import Path


def build_adapter(out, developer):
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DEVELOPER_DIR=str(developer))
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], env=env, text=True).strip()
    target = out / 'libpong-usb18.dylib'
    subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios18.0', '-isysroot', sdk,
                    '-Os', '-Wall', '-Wextra', '-Werror', '-dynamiclib',
                    '-Wl,-install_name,/pong/libpong-usb18.dylib',
                    str(Path(__file__).with_name('usb_compat18.c')), '-framework', 'CoreFoundation',
                    '-framework', 'IOKit', '-lMobileGestalt', '-o', str(target)], env=env, check=True)
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', str(target)], check=True)
    return target


def stage_adapter(root, runtime, soc_serial, usb_serial, platform_uuid):
    """Use the connected Pong's actual serial, never invent an Apple ECID."""
    import shutil
    import uuid
    soc_serial = int(soc_serial)
    if not 0 < soc_serial <= 0xffffffff or not usb_serial or len(usb_serial) > 32:
        raise RuntimeError('actual Pong SoC and USB serial required')
    software_uid = uuid.UUID(platform_uuid).hex + f'{soc_serial:08x}'
    target = root / 'pong/libpong-usb18.dylib'
    config = root / 'pong/usb-board-identity.plist'
    if target.is_symlink() or config.is_symlink():
        raise RuntimeError('unexpected USB runtime alias')
    shutil.copyfile(runtime / target.name, target)
    prior = plistlib.loads(config.read_bytes()) if config.is_file() else {}
    values = dict(UniqueChipID=soc_serial, UniqueDeviceID=software_uid,
        SerialNumber=usb_serial, ChipID=530, ModelNumber='MYMC3', RegionInfo='LL/A',
        ModelName='iPhone 16 Pro', MarketingName='iPhone 16 Pro', RegulatoryModelNumber='A3083',
        HardwareModel='D93AP', IdentityOrigin='Pong Qualcomm SoC serial; requested iPhone software presentation')
    # User-requested software component identifiers. These are explicitly
    # synthetic, persistent port metadata, never Apple factory/calibration
    # records or claims that a camera/Bluetooth controller is implemented.
    import secrets
    alphabet = '0123456789ABCDEFGHJKLMNPQRSTUVWXYZ'
    def software_id(key, length):
        old = prior.get(key)
        return old if isinstance(old, str) and old.startswith('PONG') and len(old) == length else \
            'PONG' + ''.join(secrets.choice(alphabet) for _ in range(length - 4))
    values['MLBSerialNumber'] = software_id('MLBSerialNumber', 17)
    def component_id(key, length):
        old = prior.get(key)
        if (prior.get('ComponentIdentifierVersion') == 2 and isinstance(old, str)
                and len(old) == length and old.isascii() and old.isdecimal()):
            return old
        return secrets.choice('123456789') + ''.join(secrets.choice('0123456789') for _ in range(length - 1))
    for key, length in (('BatterySerialNumber', 22),
                        ('FrontCameraModuleSerialNumString', 17),
                        ('BackCameraModuleSerialNumString', 17)):
        values[key] = component_id(key, length)
    values['FrontFacingCameraModuleSerialNumber'] = values['FrontCameraModuleSerialNumString'].encode('ascii')
    values['RearFacingCameraModuleSerialNumber'] = values['BackCameraModuleSerialNumString'].encode('ascii')
    panel = component_id('SoftwarePanelSerialNumber', 21)
    values.update(SoftwarePanelSerialNumber=panel, PanelSerialNumber=panel.encode('ascii'),
                  RawPanelSerialNumber=panel.encode('ascii'),
                  ComponentIdentifierVersion=2,
                  ComponentIdentifierOrigin='Synthetic persistent component identifiers for the Pong port; not Apple factory/calibration values')
    config.write_bytes(plistlib.dumps(values, fmt=plistlib.FMT_BINARY))
    for path, mode in ((target, 0o755), (config, 0o644)):
        os.chown(path, 0, 0)
        os.chmod(path, mode)
    return dict(serial_origin='actual connected Pong', software_uid=software_uid)


def configure_native_binary(root, backup):
    """Omit optional Apple DRM hardware bring-up, retaining native USB trust.

    The actual 22H374 daemon stalls between -UseRaptorCerts and the return
    from FairPlayGetHWInfo. Its normal missing-CESM path continues startup.
    Match instructions within executable Mach-O segments, never file=VA-base.
    This supplies no DRM context/certificate and changes no pairing checks.
    """
    version = plistlib.loads((root / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if version.get('ProductBuildVersion') != '22H374':
        raise RuntimeError('USB adaptation requires native 22H374')
    path = root / 'usr/libexec/lockdownd'
    if path.is_symlink():
        raise RuntimeError('native lockdownd must be a regular file')
    data = bytearray(path.read_bytes())
    if struct.unpack_from('<II', data) != (0xfeedfacf, 0x100000c):
        raise RuntimeError('native arm64 Mach-O required')
    segments, at = [], 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, at)
        if size < 8 or at + size > len(data):
            raise RuntimeError('invalid native load command')
        if command == 0x19:
            va, length, offset, file_size = struct.unpack_from('<QQQQ', data, at + 24)
            initial_protection = struct.unpack_from('<I', data, at + 60)[0]
            if initial_protection & 4:
                segments.append((va, min(length, file_size), offset))
        at += size
    def offset_for(va, size):
        for start, length, offset in segments:
            if start <= va and va + size <= start + length:
                return offset + va - start
        raise RuntimeError('USB adaptation site is not executable file data')
    edits = ((0x1000103f0, bytes.fromhex('7d000094'), bytes.fromhex('1f2003d5')),
             (0x1000103f8, bytes.fromhex('60d7ff97'), bytes.fromhex('000080d2')))
    # Include the intervening argument setup and the native NULL branch.
    if data[offset_for(0x1000103f4, 4):offset_for(0x1000103f4, 4)+4] != bytes.fromhex('e00315aa') or \
       data[offset_for(0x1000103fc, 4):offset_for(0x1000103fc, 4)+4] != bytes.fromhex('600200b4'):
        raise RuntimeError('unexpected 22H374 CESM call sequence')
    for va, original, replacement in edits:
        offset = offset_for(va, 4)
        if data[offset:offset+4] not in (original, replacement):
            raise RuntimeError(f'unexpected native USB instruction at {va:#x}')
    if all(data[offset_for(va, 4):offset_for(va, 4)+4] == replacement for va, _, replacement in edits):
        return {'build': '22H374', 'optional_cesm': 'already omitted'}
    if any(data[offset_for(va, 4):offset_for(va, 4)+4] != original for va, original, _ in edits):
        raise RuntimeError('incomplete native USB adaptation')
    backup.parent.mkdir(parents=True, exist_ok=True)
    if backup.exists():
        raise RuntimeError('refusing to replace original native USB backup')
    backup.write_bytes(data)
    for va, _, replacement in edits:
        offset = offset_for(va, 4)
        data[offset:offset+4] = replacement
    path.write_bytes(data)
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096',
                    '--preserve-metadata=identifier,entitlements,flags,runtime', str(path)], check=True)
    os.chown(path, 0, 0)
    os.chmod(path, 0o755)
    return {'build': '22H374', 'optional_cesm': 'unavailable hardware omitted',
            'native_pairing_and_tls': 'retained', 'backup': str(backup)}


def configure_lockdown(root):
    relative = 'System/Library/LaunchDaemons/com.apple.mobile.lockdown.plist'
    path = root / relative
    cache_path = root / 'System/Library/xpc/launchd.plist'
    if path.is_symlink() or cache_path.is_symlink():
        raise RuntimeError('real native USB service configuration required')
    job = plistlib.loads(path.read_bytes())
    if (job.get('Label') != 'com.apple.mobile.lockdown' or
            job.get('ProgramArguments') != ['/usr/libexec/lockdownd'] or
            not (root / 'usr/libexec/lockdownd').is_file()):
        raise RuntimeError('matching 22H374 native lockdownd required')
    # Pong imports the restore's service set through the mobile domain.
    # An omitted UserName would run this privileged pairing daemon as501.
    # Keep its own launchd sockets, event handlers, trust policy and binary.
    job.update(UserName='root', GroupName='wheel', RunAtLoad=True,
               StandardOutPath='/dev/console', StandardErrorPath='/dev/console')
    job.pop('ServiceIPC', None)  # obsolete on the supplied launchd
    cache = plistlib.loads(cache_path.read_bytes())
    cache['LaunchDaemons']['/' + relative] = job
    for target, value in ((path, job), (cache_path, cache)):
        target.write_bytes(plistlib.dumps(value, fmt=plistlib.FMT_BINARY))
        os.chown(target, 0, 0)
        os.chmod(target, 0o644)
    return {'label': job['Label'], 'uid': 0, 'sockets': list(job.get('Sockets', {}))}
