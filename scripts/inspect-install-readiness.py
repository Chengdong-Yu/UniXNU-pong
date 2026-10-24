#!/usr/bin/env python3
"""Read-only Pong inventory for the XNU/iPadOS port; never boots or flashes."""
import json
from pathlib import Path
import re
import struct
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parents[2]


def fdt_nodes(path):
    """Read node properties from the locally checked-out flattened device tree."""
    data = path.read_bytes()
    magic, total, offset, strings_at, _, _, _, _, strings_size, struct_size = struct.unpack_from('>10I', data)
    if magic != 0xd00dfeed or total > len(data):
        raise ValueError('Invalid FDT header')
    strings = data[strings_at:strings_at + strings_size]
    limit = offset + struct_size
    nodes, stack = {}, []
    while offset < limit:
        token = struct.unpack_from('>I', data, offset)[0]
        offset += 4
        if token == 1:
            end = data.index(0, offset, limit)
            stack.append(data[offset:end].decode())
            offset = (end + 4) & ~3
            nodes['/' + '/'.join(x for x in stack if x)] = {}
        elif token == 2:
            stack.pop()
        elif token == 3:
            size, name_at = struct.unpack_from('>II', data, offset)
            offset += 8
            name_end = strings.index(0, name_at)
            name = strings[name_at:name_end].decode()
            nodes['/' + '/'.join(x for x in stack if x)][name] = data[offset:offset + size]
            offset = (offset + size + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            break
        else:
            raise ValueError(f'Invalid FDT token {token}')
    return nodes


def describe(properties):
    result = {}
    for name in ('compatible', 'device_type', 'model', 'method', 'enable-method'):
        if name in properties:
            result[name] = properties[name].rstrip(b'\0').decode(errors='replace').split('\0')
    for name in ('reg', 'redistributor-stride', 'iommus', 'timebase-frequency', 'clock-frequency'):
        data = properties.get(name)
        if data and len(data) % 4 == 0:
            result[name + '_u32_cells'] = [hex(x[0]) for x in struct.iter_unpack('>I', data)]
    return result


def main():
    uefi = ROOT / 'Mu-Silicium'
    nodes = fdt_nodes(uefi / 'Resources/DTBs/pong.dtb')
    selected = {}
    for path, properties in nodes.items():
        compatible = properties.get('compatible', b'')
        leaf = path.rsplit('/', 1)[-1]
        if (leaf.startswith(('cpu@', 'memory@', 'ufshc@', 'ufsphy@'))
                or b'arm,gic-v3' in compatible or leaf == 'psci'):
            selected[path] = describe(properties)
    board = tomllib.loads((ROOT / 'UniXNU-pong/board.toml').read_text())
    boot_dir = ROOT / 'UniXNU/out/pong/boot-v3-mapped-probe'
    if not (boot_dir / 'boot.json').is_file():
        boot_dir = ROOT / 'UniXNU/out/pong/boot'
    boot = json.loads((boot_dir / 'boot.json').read_text())
    sec = (uefi / 'Silicon/Silicium/SiliciumPkg/Sec/Sec.c').read_text()
    entry = (uefi / 'Silicon/Silicium/SiliciumPkg/Sec/AArch64/ModuleEntryPoint.S').read_text()
    dsc = (uefi / 'Platforms/Nothing/pongPkg/pong.dsc').read_text()
    fastboot = Path('C:/Users/desktop/AppData/Local/Android/Sdk/platform-tools/fastboot.exe')
    variables = {}
    if fastboot.is_file():
        for name in ('product', 'current-slot', 'unlocked', 'partition-size:userdata',
                     'partition-type:userdata', 'partition-size:boot_b', 'max-download-size'):
            try:
                process = subprocess.run([str(fastboot), '-s', 'dd72e429', 'getvar', name],
                                         capture_output=True, text=True, timeout=10)
                output = process.stdout + process.stderr
                match = re.search(re.escape(name) + r':\s*(\S+)', output)
                variables[name] = match.group(1) if match else 'unavailable'
            except subprocess.TimeoutExpired:
                variables[name] = 'unavailable (timeout)'
                break
    report = {
        'scope': 'read-only source/boot-image inventory and selected fastboot getvar commands',
        'user_reported_uefi_menu_working': True,
        'ipad_os_hardware_tested': False,
        'firmware_ramdisk_bytes': boot['ramdisk_bytes'],
        'inspected_boot_image': str(boot_dir.relative_to(ROOT)),
        'missing_board_sections': [key for key in ('ipsw', 'kernel', 'payload', 'userdata') if key not in board],
        'expected_pong_artifacts_present': {
            'prelinked_kernel': (ROOT / 'UniXNU/out/pong/kernel/kernel.macho').is_file(),
            'fat_payload': (ROOT / 'UniXNU/out/pong/payload/payload.img').is_file(),
            'system_userdata_image': (ROOT / 'UniXNU/out/pong/userdata/userdata-root.img').is_file(),
        },
        'sec_entry_argument_preserved': 'gSecEntryArg0' in entry,
        'entry_argument_hob_published': 'gSiliciumEntryArgHobGuid' in sec,
        'sheng_boot_memory_driver_in_pong_dsc': 'ShengBootMemoryDxe.inf' in dsc,
        'pong_initrd_driver_in_dsc': 'PongBootMemoryDxe.inf' in dsc,
        'fastboot': variables,
        'local_dtb_path': 'Mu-Silicium/Resources/DTBs/pong.dtb',
        'local_dtb_is_not_a_live_ram_or_partition_measurement': True,
        'local_dtb_nodes': selected,
        'write_operations_performed': False,
    }
    size = variables.get('partition-size:userdata', '')
    if re.fullmatch(r'0x[0-9a-fA-F]+', size):
        report['userdata_bytes'] = int(size, 16)
        report['userdata_GiB'] = round(int(size, 16) / 2**30, 3)
    out = ROOT / 'UniXNU/out/pong/install-readiness.json'
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({key: value for key, value in report.items() if key != 'local_dtb_nodes'},
                     ensure_ascii=False, indent=2))
    print(f'Local FDT: {len(selected)} relevant nodes recorded in {out}')


if __name__ == '__main__':
    main()
