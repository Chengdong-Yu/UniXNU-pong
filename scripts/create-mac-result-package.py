#!/usr/bin/env python3
"""Package this round's sources, logs, kernels, images and observations."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--latest-boot', type=Path, required=True)
    args = parser.parse_args()
    out = args.output_dir.resolve()
    archive = out.with_suffix('.zip')
    if out.exists() or archive.exists():
        parser.error('Preserving existing outputs; choose a new directory name')
    latest = args.latest_boot.resolve()
    info = json.loads((latest.parent / 'build.json').read_text())
    native_report = json.loads((ROOT / 'UniXNU-pong/mac-build/ios16-xnu-pong/PONG_BUILD_RESULT.json').read_text())
    report_path = latest.parent / 'PONG_BUILD_RESULT.json'
    report = json.loads(report_path.read_text()) if report_path.exists() else native_report
    if report.get('base_kernel', report['kernel'])['sha256'] != native_report['kernel']['sha256']:
        raise ValueError('Latest candidate base does not match the exported native kernel')
    if digest(latest) != info['image_sha256'] or info['kernel_sha256'] != report['kernel']['sha256']:
        raise ValueError('Latest boot image does not match the exported kernel')
    for name, checksum in report['source_patch_sha256'].items():
        if digest(ROOT / 'UniXNU-pong/patches/xnu' / name) != checksum:
            raise ValueError('Exported kernel source patch differs: ' + name)
    out.mkdir(parents=True)
    ignore = shutil.ignore_patterns('.DS_Store', '__pycache__', '*.pyc', '.git')
    shutil.copytree(ROOT / 'UniXNU-pong', out / 'UniXNU-pong', ignore=ignore)
    shutil.copytree(ROOT / 'evidence', out / 'evidence', ignore=ignore)
    shutil.copytree(ROOT / 'apple', out / 'apple', ignore=ignore)
    if report.get('prelink'):
        prelinked = out / 'UniXNU-pong/mac-build/ios16-xnu-pong-prelinked'
        prelinked.mkdir()
        shutil.copyfile(latest.parent / 'kernel-corecrypto.macho', prelinked / 'kernel-corecrypto.macho')
        shutil.copyfile(report_path, prelinked / 'PONG_BUILD_RESULT.json')
        shutil.copyfile(latest.parent / 'prelink.json', prelinked / 'prelink.json')
    shutil.copyfile(ROOT / 'FILE_MANIFEST.json', out / 'evidence/INPUT_FILE_MANIFEST.json')
    for name in ('MAC_RESULT_NEXT.md', 'AGENT_NEXT.md', 'IOS16_MODULES_NEXT.md'):
        shutil.copyfile(ROOT / name, out / name)
    images = out / 'images'
    images.mkdir()
    for directory in sorted((ROOT / 'build').iterdir()):
        if not directory.is_dir():
            continue
        candidates = list(directory.glob('pong-*.img'))
        for image in candidates:
            shutil.copyfile(image, images / (directory.name + '.img'))
            if (directory / 'build.json').exists():
                shutil.copyfile(directory / 'build.json', images / (directory.name + '.json'))
        symbol_kernel = directory / 'mach.development.pong.unstripped'
        if symbol_kernel.exists():
            symbols = out / 'symbols' / directory.name
            symbols.mkdir(parents=True)
            shutil.copyfile(symbol_kernel, symbols / symbol_kernel.name)
            for name in ('PONG_BUILD_RESULT.json', 'pong-early-trace-stages.json', 'prelink.json'):
                if (directory / name).exists():
                    shutil.copyfile(directory / name, symbols / name)
            if (directory / 'xnu-patches').exists():
                shutil.copytree(directory / 'xnu-patches', symbols / 'xnu-patches')
    latest_name = latest.parent.name + '.img'
    info['packaged_path'] = 'images/' + latest_name
    if report.get('prelink'):
        info['prelink']['report'] = 'symbols/' + latest.parent.name + '/prelink.json'
    attempts = []
    for record in sorted((ROOT / 'evidence').glob('*hardware.json')):
        value = json.loads(record.read_text())
        if value.get('image_sha256') == info['image_sha256']:
            attempts.append({'evidence': 'evidence/' + record.name,
                **{key: value[key] for key in ('returncode', 'screen_result',
                   'xnu_entry_confirmed', 'xnu_boot_confirmed', 'full_os_boot_confirmed',
                   'last_stage', 'stage_hex', 'stage_decimal', 'exception', 'diagnosis') if key in value}})
    info['hardware_attempts'] = attempts
    info['hardware_tested'] = bool(attempts)
    info['xnu_boot_confirmed'] = any(record.get('xnu_boot_confirmed') is True for record in attempts)
    info['full_os_boot_confirmed'] = any(record.get('full_os_boot_confirmed') is True for record in attempts)
    (out / 'LATEST_BOOT.json').write_text(json.dumps(info, indent=2) + '\n')
    symbols = latest.parent / 'mach.development.pong.unstripped'
    if symbols.exists():
        shutil.copyfile(symbols, out / 'UniXNU-pong/mac-build/ios16-xnu-pong/mach.development.pong.unstripped')
    (out / 'README_FIRST.md').write_text(
        '# Pong iOS 16 Mac 结果包\n\n'
        '先读 AGENT_NEXT.md 与 MAC_RESULT_NEXT.md。最新候选镜像和哈希见 LATEST_BOOT.json。\n\n'
        '本包包含本轮设备源码、补丁、内核、完整构建日志、诊断镜像和真机记录。'
        '各次符号内核位于 symbols/，Apple 20G81 小组件位于 apple/。'
        '完整 Apple 源树/Xcode 仍在 /Volumes/PongDev。\n\n'
        '运行 python3 verify_bundle.py 校验文件。真机是否通过应以 evidence 中的屏幕记录为准；'
        'hardware_tested 只表示有启动尝试，完整启动状态另看 xnu_boot_confirmed。\n')
    shutil.copyfile(ROOT / 'verify_bundle.py', out / 'verify_bundle.py')
    entries = [{'path': str(p.relative_to(out)), 'bytes': p.stat().st_size, 'sha256': digest(p)}
               for p in sorted(out.rglob('*')) if p.is_file()]
    (out / 'FILE_MANIFEST.json').write_text(json.dumps(entries, indent=2) + '\n')
    with zipfile.ZipFile(archive, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for path in sorted(out.rglob('*')):
            if path.is_file():
                bundle.write(path, str(Path(out.name) / path.relative_to(out)))
    with zipfile.ZipFile(archive) as bundle:
        if bundle.testzip() is not None:
            raise ValueError('Archive integrity failed')
    checksum = archive.with_suffix('.zip.sha256')
    checksum.write_text(digest(archive) + '  ' + archive.name + '\n')
    print(archive)
    print(checksum.read_text().strip())


if __name__ == '__main__':
    main()
