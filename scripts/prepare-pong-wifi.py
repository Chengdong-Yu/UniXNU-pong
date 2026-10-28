#!/usr/bin/env python3
"""Capture Pong's actual Wi-Fi inputs from REC, without image hashes/readback.

Firmware/calibration stays in --out (ignored local inputs). A board profile can
be written separately with --profile. Never select Sheng's radio or a random
MAC when the actual endpoint or calibration record is missing.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import pong_paths


def fdt_nodes(data):
    if len(data) < 40:
        raise ValueError('Truncated hardware DT')
    magic, total, start, strings, _, version, _, _, string_size, size = struct.unpack_from('>10I', data)
    if magic != 0xd00dfeed or version < 17 or total > len(data) or start + size > total or strings + string_size > total:
        raise ValueError('Unsupported hardware DT')
    names = data[strings:strings + string_size]
    at, end, stack, nodes = start, start + size, [], {}
    while at + 4 <= end:
        token, = struct.unpack_from('>I', data, at); at += 4
        if token == 1:
            stop = data.index(0, at, end)
            stack.append(data[at:stop].decode('ascii')); at = (stop + 4) & ~3
            nodes['/' + '/'.join(stack[1:])] = {}
        elif token == 2:
            if not stack: raise ValueError('Invalid DT nesting')
            stack.pop()
        elif token == 3:
            length, name = struct.unpack_from('>II', data, at); at += 8
            if not stack or at + length > end or name >= len(names): raise ValueError('Invalid DT property')
            key = names[name:names.index(0, name)].decode('ascii')
            nodes['/' + '/'.join(stack[1:])][key] = data[at:at + length]
            at = (at + length + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            return nodes
        else:
            raise ValueError('Unknown hardware DT token')
    raise ValueError('Hardware DT has no end marker')


def cells(p, name):
    b = p[name]
    if len(b) % 4: raise ValueError('Unaligned DT cell ' + name)
    return list(struct.unpack('>' + 'I' * (len(b) // 4), b))


def text(p, name):
    return p[name].rstrip(b'\0').decode('ascii')


def describe(data, pci):
    nodes = fdt_nodes(data)
    refs = {cells(p, 'phandle')[0]: path for path, p in nodes.items() if 'phandle' in p}
    wlan_path = next(k for k, p in nodes.items() if p.get('compatible') == b'qcom,cnss-qca6490\0')
    wlan = nodes[wlan_path]
    rc = cells(wlan, 'qcom,wlan-rc-num')[0]
    host_path = next(k for k, p in nodes.items() if p.get('compatible') == b'qcom,pci-msm\0' and cells(p, 'cell-index') == [rc])
    host = nodes[host_path]
    domain_path = refs[cells(host, 'gdsc-vdd-supply')[0]]
    domain = nodes[domain_path]
    collapse = cells(domain, 'qcom,collapse-vote')
    collapse_node = nodes[refs[collapse[0]]]
    power_domain = dict(control=cells(domain, 'reg')[0],
                        collapse_vote=cells(collapse_node, 'reg')[0],
                        collapse_bit=collapse[1])
    if pci['vendor'] != 0x17cb or pci['device'] != 0x1103:
        raise ValueError('This profile requires the actual QCA6490 PCI endpoint 17cb:1103')
    region_cells = cells(host, 'reg')
    region_names = text(host, 'reg-names').split('\0')
    if len(region_cells) != len(region_names) * 2:
        raise ValueError('Unexpected Pong PCIe resource cell layout')
    regions = {name: {'base': region_cells[i * 2], 'bytes': region_cells[i * 2 + 1]}
               for i, name in enumerate(region_names)}
    iommu_path = next(k for k in nodes if k.startswith(host_path + '/') and 'qcom,iommu-dma-addr-pool' in nodes[k])
    iommu = nodes[iommu_path]
    msi_map = cells(host, 'msi-map')
    active_msi = nodes[refs[msi_map[1]]]
    phy = cells(host, 'qcom,phy-sequence')
    if len(phy) % 3: raise ValueError('Malformed Pong PHY sequence')
    rails = []
    for name, raw in wlan.items():
        if not name.endswith('-supply'): continue
        rail_path = refs[struct.unpack('>I', raw)[0]]
        rail = nodes[rail_path]
        parent = nodes[rail_path.rsplit('/', 1)[0]]
        rails.append({'supply': name[:-7], 'resource': text(parent, 'qcom,resource-name'),
                      'regulator': text(rail, 'regulator-name'),
                      'request': cells(wlan, 'qcom,' + name[:-7] + '-config')})
    analogue = []
    for supply in ('vreg-1p8', 'vreg-0p9'):
        rail_path = refs[cells(host, supply + '-supply')[0]]
        parent = nodes[rail_path.rsplit('/', 1)[0]]
        analogue.append(dict(supply=supply, resource=text(parent, 'qcom,resource-name'),
                             microvolts=cells(host, 'qcom,' + supply + '-voltage-level')[0]))
    old_msi = next((p for p in nodes.values() if p.get('compatible') == b'qcom,pci-msi\0' and
                    p.get('reg') == struct.pack('>II', 0x17110040, 0)), None)
    gpio = {name: cells(wlan, name)[1] for name in
            ('wlan-en-gpio', 'qcom,bt-en-gpio', 'qcom,sw-ctrl-gpio', 'qcom,xo-clk-gpio')}
    gpio.update(perst=cells(host, 'perst-gpio')[1], wake=cells(host, 'wake-gpio')[1])
    return dict(format='pong-qca6490-board-v1', board='pong', soc='SM8475', pci=pci,
        radio='QCA6490', driver_family='ath11k', transport='PCIe/MHI/QRTR',
        source=dict(wlan=wlan_path, pcie=host_path, input='REC /sys/firmware/fdt and PCI sysfs'),
        pcie=dict(index=rc, regions=regions, gpio=gpio, analogue_rails=analogue,
                  power_domain=power_domain,
                  clock_names=text(host, 'clock-names').split('\0'), clock_specifiers=cells(host, 'clocks'),
                  reset_specifiers=cells(host, 'resets'), sid_map=cells(host, 'iommu-map'),
                  phy_sequence=[phy[i:i + 3] for i in range(0, len(phy), 3)],
                  phy_status_offset=cells(host, 'qcom,phy-status-offset')[0],
                  phy_status_bit=cells(host, 'qcom,phy-status-bit')[0],
                  phy_power_down_offset=cells(host, 'qcom,phy-power-down-offset')[0],
                  max_clock_frequency_hz=cells(host, 'max-clock-frequency-hz'),
                  aux_clock_mhz=cells(host, 'qcom,aux-clk-freq')[0],
                  endpoint_latency_ms=cells(host, 'qcom,ep-latency')[0],
                  perst_delay_us=cells(host, 'qcom,perst-delay-us-min')[0]
                    if 'qcom,perst-delay-us-min' in host else 1000),
        dma=dict(iova_pool=cells(iommu, 'qcom,iommu-dma-addr-pool'),
                 msi_bytes=cells(iommu, 'qcom,iommu-msi-size')[0],
                 cache_policy=text(iommu, 'qcom,iommu-pagetable')),
        msi=dict(map=msi_map, controller=refs[msi_map[1]], compatible=text(active_msi, 'compatible'),
                 reg=cells(active_msi, 'reg'),
                 legacy_spi_node_status=text(old_msi, 'status') if old_msi else 'absent'),
        rails=rails, nv_mac_required='use-nv-mac' in wlan,
        firmware=dict(device_path='/mnt/pong-wifi-firmware/image/qca6490',
                      runtime_path='/pong/firmware/qca6490',
                      required=['amss.bin', 'amss20.bin', 'm3.bin', 'regdb.bin'],
                      board_data='select from actual QMI chip/board ID; no default variant'),
        readiness=dict(board_inputs=True, ath11k_backend=False, darwin24_apple80211=False,
                       firmware_boot_observed=False, scan_observed=False, link_observed=False))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--adb', type=Path, default=pong_paths.adb())
    p.add_argument('--port', type=int, default=5038)
    p.add_argument('--serial', required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--profile', type=Path)
    p.add_argument('--dtb', type=Path, help='use an already captured hardware DT')
    p.add_argument('--firmware-dir', type=Path, help='use already extracted local QCA6490 firmware')
    a = p.parse_args(); out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    adb = [str(a.adb), '-P', str(a.port), '-s', a.serial]
    def shell(cmd):
        return subprocess.check_output(adb + ['shell', cmd], text=True).strip()
    if subprocess.check_output(adb + ['get-state'], text=True).strip() != 'recovery':
        raise RuntimeError('REC required')
    dtb = a.dtb.read_bytes() if a.dtb else subprocess.check_output(adb + ['exec-out', 'cat', '/sys/firmware/fdt'])
    pci = {}
    device = '/sys/bus/pci/devices/0000:01:00.0/'
    for key in ('vendor', 'device', 'subsystem_vendor', 'subsystem_device', 'revision'):
        pci[key] = int(shell('cat ' + device + key), 16)
    resource = shell('cat ' + device + 'resource').splitlines()[0].split()
    pci['bar0'] = dict(base=int(resource[0], 16), bytes=int(resource[1], 16) - int(resource[0], 16) + 1)
    profile = describe(dtb, pci)
    (out / 'pong-live.dtb').write_bytes(dtb)
    firmware = a.firmware_dir.resolve() if a.firmware_dir else out / 'qca6490'
    owned_mount = False
    try:
        if not a.firmware_dir:
            mountpoint = '/mnt/pong-wifi-firmware'
            if shell('mountpoint -q ' + mountpoint + '; echo $?') != '0':
                shell('mkdir -p ' + mountpoint + ' && mount -t vfat -o ro /dev/block/by-name/modem_a ' + mountpoint)
                owned_mount = True
            subprocess.run(adb + ['pull', profile['firmware']['device_path'] + '/.', str(firmware)], check=True)
        for name in profile['firmware']['required']:
            if not (firmware / name).is_file(): raise RuntimeError('Missing actual device firmware: ' + name)
        inventory = [dict(name=f.name, bytes=f.stat().st_size) for f in sorted(firmware.iterdir()) if f.is_file()]
        if not any(f['name'].startswith('bdwlan.') for f in inventory): raise RuntimeError('Missing board data')
        (out / 'local-inputs.json').write_text(json.dumps(dict(firmware_directory=str(firmware), files=inventory,
            profile=profile, image_verification='skipped by request', calibration_modified=False), indent=2) + '\n')
        destination = a.profile or out / 'board.json'
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(profile, indent=2) + '\n')
        print(destination)
    finally:
        if owned_mount: shell('umount /mnt/pong-wifi-firmware')


if __name__ == '__main__': main()
