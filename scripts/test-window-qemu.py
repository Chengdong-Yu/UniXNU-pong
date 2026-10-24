#!/usr/bin/env python3
"""Check the real post-ExitBootServices framebuffer for pass and fail controls."""
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'UniXNU/out/pong/ios16/window-probe'
spec = importlib.util.spec_from_file_location('qemu_helpers', ROOT / 'UniXNU/package/Scripts/qemu-run.py')
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)


def exercise(case):
    wrong_size = case == 'wrong-window'
    esp = OUT / ('qemu-' + case)
    if esp.exists():
        shutil.rmtree(esp)
    shutil.copytree(OUT / 'stage', esp)
    payload = esp / 'EFI/UniXNU'
    (esp / 'EFI/BOOT').mkdir()
    binaries = ROOT / 'Mu-Silicium/Build/pongPkg/RELEASE_CLANGPDB/AARCH64'
    shutil.copyfile(binaries / 'UniXNU.efi', esp / 'EFI/BOOT/BOOTAA64.EFI')
    shutil.copyfile(binaries / 'DarwinBoot.efi', payload / 'DarwinBoot.efi')
    config = payload / 'unixnu.toml'
    config.write_text(config.read_text().replace('[[entry]]', '[[driver]]\npath = "DarwinBoot.efi"\n[[entry]]'))
    if case == 'exception':
        config.write_text(config.read_text().replace('args = "pong.', 'args = "!pong.'))
    if wrong_size:
        subprocess.run(['python3', str(ROOT / 'UniXNU-pong/scripts/make-pong-adt.py'),
                        '--dram-size', '0x300000000', '--window-size', '0x20000000',
                        '--output', str(payload / 'window.macho.adt')], check=True)
    log = OUT / ('qemu-' + case + '.log')
    log.unlink(missing_ok=True)
    code, variables = helpers.find_firmware(None)
    with tempfile.TemporaryDirectory(prefix='pong-window-') as tmp:
        tmp = Path(tmp)
        cmd = ['qemu-system-aarch64', '-M', 'virt', '-cpu', 'cortex-a72', '-m', '1024', '-smp', '1',
               '-drive', f'if=pflash,format=raw,readonly=on,file={code}',
               '-drive', f'if=none,id=esp,format=raw,readonly=on,file=fat:{esp}',
               '-device', 'virtio-blk-pci,drive=esp', '-device', 'ramfb',
               '-device', 'qemu-xhci', '-device', 'usb-kbd', '-nic', 'none',
               '-display', 'none', '-no-reboot', '-serial', f'file:{log}',
               '-qmp', f'unix:{tmp / "qmp.sock"},server=on,wait=off']
        if variables:
            shutil.copyfile(variables, tmp / 'vars.fd')
            cmd += ['-drive', f'if=pflash,format=raw,file={tmp / "vars.fd"}']
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        qmp = None
        try:
            qmp = helpers.Qmp(str(tmp / 'qmp.sock'))
            deadline = time.monotonic() + 35
            entered = None
            while proc.poll() is None and time.monotonic() < deadline:
                output = log.read_text(errors='replace') if log.exists() else ''
                if 'DARWINBOOT: inputs validated; exiting boot services' in output:
                    entered = entered or time.monotonic()
                if entered and time.monotonic() - entered >= 6:
                    break
                time.sleep(0.1)
            if not entered:
                raise RuntimeError('No actual DarwinBoot handoff: ' + output[-1500:])
            ppm = OUT / ('qemu-' + case + '.ppm')
            result = qmp.command('screendump', filename=str(ppm))
            if 'error' in result:
                raise RuntimeError(str(result))
            raw = ppm.read_bytes()
            match = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', raw)
            if not match:
                raise RuntimeError('Unexpected QEMU PPM header')
            width = int(match[1])
            offset = match.end() + (65 * width + 1) * 3
            colour = raw[offset:offset + 3]
            expected = bytes.fromhex('aa33dd' if case == 'exception' else 'df2020' if wrong_size else '12d4a6')
            helpers.ppm_to_png(ppm, ppm.with_suffix('.png'))
            if colour != expected:
                raise RuntimeError(f'{case}: post-exit pixel {colour.hex()} != {expected.hex()}')
            return {'case': case, 'expected': 'EXCEPTION (injected BRK)' if case == 'exception' else 'FAIL (window size)' if wrong_size else 'PASS',
                    'post_exit_framebuffer_pixel': colour.hex(), 'passed': True}
        finally:
            if proc.poll() is None:
                if qmp:
                    qmp.command('quit')
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            if proc.returncode:
                print(proc.stdout.read().decode(errors='replace')[-2000:])


if __name__ == '__main__':
    results = [exercise('normal'), exercise('wrong-window'), exercise('exception')]
    (OUT / 'qemu-results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))
