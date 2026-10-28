#!/usr/bin/env python3
"""Unpack and validate a Pong firmware archive for the CI kernel build.

The archive holds the same tree a local build uses: the QCA6490 modem files,
the TFA9873 amplifier container, the Adreno 730 GMU/SQE/ZAP firmware and
adsp.mbn. A machine that owns the phone produces that tree from the device; a
CI run receives it as a zip or tar.gz so no firmware has to live in Git.

The archive may name the Wi-Fi directory wifi/ or qca6490/, and may be wrapped
in one or more extra top-level directories, as a zip made from a folder or a
tarball made with tar czf adds. Nothing is hashed or read back beyond the name,
size and digest checks below.
"""
import argparse
import hashlib
import json
import shutil
import sys
import tarfile
import zipfile
from pathlib import Path

GROUPS = {
    'wifi': dict(names=('amss.bin', 'amss20.bin', 'm3.bin', 'regdb.bin'),
                 note='the phone modem image/qca6490 files; bdwlan* board data is required too'),
    'audio': dict(names=('tfa98xx.cnt',), note='the TFA9873 container from the phone vendor partition'),
    'adreno730': dict(names=('gmu_gen70000.bin', 'a730_sqe.fw', 'a730_zap.mdt', 'a730_zap.b01', 'a730_zap.b02'),
                      note='the GPU GMU, SQE and split ZAP firmware from the phone vendor partition'),
    'power': dict(names=('adsp.mbn',), note='the ADSP firmware from the phone modem partition'),
}
WIFI_ALIASES = ('wifi', 'qca6490')


def fail(message):
    raise SystemExit('firmware archive refused: ' + message)


def check_member(name):
    """Reject absolute paths, parent traversal and drive letters from an archive."""
    if not name or name.startswith(('/', '\\')):
        fail('archive member is absolute: ' + name)
    if len(name) > 2 and name[1] == ':':
        fail('archive member has a drive letter: ' + name)
    parts = [part for part in name.replace('\\', '/').split('/') if part not in ('', '.')]
    if any(part == '..' for part in parts):
        fail('archive member escapes the archive root: ' + name)
    return '/'.join(parts)


def digest_of(path):
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def extract_zip(path, dest):
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            if info.is_dir():
                continue
            if (info.external_attr >> 16) & 0o170000 == 0o120000:
                fail('archive contains a symbolic link: ' + info.filename)
            relative = check_member(info.filename)
            if not relative:
                continue
            target = dest / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as source, target.open('wb') as sink:
                shutil.copyfileobj(source, sink)


def extract_tar(path, dest):
    with tarfile.open(path) as archive:
        for info in archive.getmembers():
            if info.isdir():
                continue
            if info.islnk() or info.issym():
                fail('archive contains a link: ' + info.name)
            if not info.isreg():
                fail('archive contains a non-regular file: ' + info.name)
            relative = check_member(info.name)
            if not relative:
                continue
            target = dest / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            source = archive.extractfile(info)
            if source is None:
                fail('archive member is unreadable: ' + info.name)
            with source, target.open('wb') as sink:
                shutil.copyfileobj(source, sink)


def flatten_single_tops(dest):
    """Descend while dest holds exactly one directory and no firmware group yet."""
    stripped = []
    for _ in range(4):
        if any((dest / name).is_dir() for name in list(GROUPS) + list(WIFI_ALIASES)):
            break
        entries = [entry for entry in dest.iterdir() if not entry.name.startswith('.')]
        if len(entries) != 1 or not entries[0].is_dir():
            break
        stripped.append(entries[0].name)
        inner = dest / '__flatten__'
        entries[0].rename(inner)
        for child in inner.iterdir():
            child.rename(dest / child.name)
        inner.rmdir()
    return '/'.join(stripped) or None


def wifi_dir(dest):
    for name in WIFI_ALIASES:
        if (dest / name).is_dir():
            return dest / name
    return None


def validate(dest):
    problems = []
    found = {}
    for group, spec in GROUPS.items():
        base = wifi_dir(dest) if group == 'wifi' else dest / group
        if base is None:
            problems.append('missing the Wi-Fi directory: expected wifi/ or qca6490/ holding ' +
                            ', '.join(spec['names']) + ' (' + spec['note'] + ')')
            continue
        for name in spec['names']:
            path = base / name
            if not path.is_file():
                problems.append('missing ' + group + '/' + name + ' (' + spec['note'] + ')')
            elif path.stat().st_size == 0:
                problems.append('empty ' + group + '/' + name)
        found[group] = base
    base = found.get('wifi')
    if base is not None and not any(child.name.startswith('bdwlan') for child in base.iterdir() if child.is_file()):
        problems.append('missing Wi-Fi board data: no bdwlan* file under ' + base.name +
                        '/, so the ath11k board firmware has no input')
    if problems:
        fail('\n  ' + '\n  '.join(problems))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True, help='zip, tar.gz, tgz or tar holding the firmware tree')
    parser.add_argument('--dest', type=Path, required=True, help='directory to unpack into; replaced if it exists')
    parser.add_argument('--json-out', type=Path, help='write the staging report here')
    parser.add_argument('--expect-sha256', help='refuse the archive unless it has this digest')
    parser.add_argument('--max-mib', type=int, default=512, help='refuse an archive larger than this (default 512)')
    args = parser.parse_args()
    archive = args.archive.expanduser().resolve()
    if not archive.is_file():
        fail('archive not found: ' + str(archive))
    size = archive.stat().st_size
    if size > args.max_mib << 20:
        fail('archive is %d bytes, above the %d MiB limit' % (size, args.max_mib))
    digest = digest_of(archive)
    if args.expect_sha256 and digest != args.expect_sha256.strip().lower():
        fail('archive sha256 is ' + digest + ', expected ' + args.expect_sha256.strip().lower())
    lower = archive.name.lower()
    if lower.endswith('.zip'):
        kind = 'zip'
    elif lower.endswith(('.tar.gz', '.tgz', '.tar')):
        kind = 'tar'
    else:
        fail('expected a .zip, .tar.gz, .tgz or .tar file, got ' + archive.name)
    dest = args.dest.expanduser().resolve()
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    (extract_zip if kind == 'zip' else extract_tar)(archive, dest)
    wrapper = flatten_single_tops(dest)
    found = validate(dest)
    files = sorted(str(path.relative_to(dest)) for path in dest.rglob('*') if path.is_file())
    report = {
        'archive': archive.name,
        'archive_bytes': size,
        'archive_sha256': digest,
        'archive_format': kind,
        'wrapper_directories': wrapper,
        'files': len(files),
        'bytes': sum((dest / name).stat().st_size for name in files),
        'paths': {group: str(base.resolve()) for group, base in found.items()},
        'image_verification': 'not performed: names, sizes and the optional digest only',
    }
    report['paths']['adsp'] = str((dest / 'power' / 'adsp.mbn').resolve())
    if args.json_out:
        out = args.json_out.expanduser().resolve()
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
