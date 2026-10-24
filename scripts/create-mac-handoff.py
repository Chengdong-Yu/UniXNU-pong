#!/usr/bin/env python3
"""Package selected source snapshots, original Apple refs and verified artifacts."""
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]
DOCS = ROOT / 'Pong-Mac-Handoff'
PREFIX = 'Pong-Mac-Handoff/'
OUT = ROOT / 'artifacts/Pong-Mac-Handoff-20261020.zip'
entries = {}
directories = set()


def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args], stderr=subprocess.DEVNULL)


def add(path, arc, mode='644', link=None):
    if link is not None:
        entries[arc] = {'data': link.encode(), 'mode': '777', 'type': 'symlink', 'target': link}
    elif path.is_file():
        entries[arc] = {'source': path, 'mode': mode, 'type': 'file'}


def add_repo(repo, arc):
    for record in git(repo, 'ls-files', '--stage', '-z').split(b'\0'):
        if not record:
            continue
        header, name = record.split(b'\t', 1)
        mode = header.split()[0].decode()
        name = name.decode()
        path = repo / name
        if mode == '160000':
            if (path / '.git').exists():
                add_repo(path, arc + '/' + name)
        elif mode == '120000':
            if os.path.lexists(path):
                add(path, arc + '/' + name, link=git(repo, 'show', 'HEAD:' + name).decode())
        else:
            add(path, arc + '/' + name, '755' if mode == '100755' else '644')
    for raw in git(repo, 'ls-files', '--others', '--exclude-standard', '-z').split(b'\0'):
        if raw:
            name = raw.decode()
            if '__pycache__' not in name:
                add(repo / name, arc + '/' + name)


def add_tree(path, arc, metadata=False):
    if metadata:
        # Detached shallow fetches can have an empty refs/. Git still requires it.
        directories.update((arc + '/', arc + '/refs/', arc + '/objects/'))
    for base, dirs, files in os.walk(path):
        dirs[:] = [d for d in dirs if d not in ('__pycache__',) and (not metadata or d not in ('hooks', 'logs'))]
        for name in files:
            if name.endswith('.pyc') or (metadata and name.endswith('.lock')):
                continue
            source = Path(base) / name
            relative = source.relative_to(path).as_posix()
            add(source, arc + '/' + relative)


baselines = {}
for name in ['UniXNU', 'Mu-Silicium', 'Mu-Silicium/Mu_Basecore', 'Mu-Silicium/Common/Mu',
             'Mu-Silicium/Common/Mu_OEM_Sample', 'Mu-Silicium/Binaries', 'Mu-Silicium/Silicium-ACPI',
             'UniXNU-sheng', 'uefi-sheng-reference']:
    repo = ROOT / name
    baselines[name] = {'commit': git(repo, 'rev-parse', 'HEAD').decode().strip(),
                       'commit_time': git(repo, 'log', '-1', '--format=%cI').decode().strip()}
for name in ['UniXNU', 'Mu-Silicium', 'UniXNU-sheng', 'uefi-sheng-reference']:
    add_repo(ROOT / name, 'workspace/' + name)
add_tree(ROOT / 'UniXNU-pong', 'workspace/UniXNU-pong')

baselines['apple_references'] = []
for repo in sorted((ROOT / 'UniXNU/out/feeds').iterdir()):
    if repo.is_dir() and (repo / '.git').is_dir():
        arc = 'workspace/' + repo.relative_to(ROOT).as_posix()
        add_repo(repo, arc)
        add_tree(repo / '.git', arc + '/.git', metadata=True)
        baselines['apple_references'].append({'path': arc, 'commit': git(repo, 'rev-parse', 'HEAD').decode().strip()})
for name in ['boot-v1-uefi-only', 'boot-v3-mapped-probe', 'payload-probe']:
    add_tree(ROOT / 'UniXNU/out/pong' / name, 'workspace/UniXNU/out/pong/' + name)
for name in ['hardware-debug-tests.json', 'device-readonly.json', 'install-readiness.json']:
    add(ROOT / 'UniXNU/out/pong' / name, 'workspace/UniXNU/out/pong/' + name)
baselines['snapshot_note'] = 'Main project Git history omitted; working files include local Pong changes. Apple refs preserve original Git.'
baselines['user_verified_image_sha256'] = '755f737021918810760794ed25753b5012e11f03fd6e564bf371a95bae1c0221'
(DOCS / 'SOURCE_BASELINES.json').write_text(json.dumps(baselines, ensure_ascii=False, indent=2) + '\n')
for path in DOCS.iterdir():
    if path.is_file() and path.name != 'FILE_MANIFEST.json':
        add(path, path.name, '755' if path.suffix == '.py' else '644')

manifest = []
OUT.parent.mkdir(parents=True, exist_ok=True)
print(f'Packaging {len(entries)} files; compiled Linux tools and primary Git history excluded.', flush=True)
with zipfile.ZipFile(OUT, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=4, allowZip64=True) as archive:
    for arc in sorted(directories):
        info = zipfile.ZipInfo(PREFIX + arc, (2026, 10, 20, 0, 0, 0))
        info.create_system = 3
        info.external_attr = (stat.S_IFDIR | 0o755) << 16
        archive.writestr(info, b'')
    for arc, entry in sorted(entries.items()):
        data = entry.get('data')
        if data is None:
            data = entry['source'].read_bytes()
        info = zipfile.ZipInfo(PREFIX + arc, (2026, 10, 20, 0, 0, 0))
        info.create_system = 3
        info.external_attr = ((stat.S_IFLNK if entry['type'] == 'symlink' else stat.S_IFREG) | int(entry['mode'], 8)) << 16
        archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=4)
        item = {'path': arc, 'mode': entry['mode'], 'type': entry['type'], 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        if entry['type'] == 'symlink':
            item['target'] = entry['target']
        manifest.append(item)
    manifest_data = (json.dumps(manifest, ensure_ascii=False, indent=2) + '\n').encode()
    archive.writestr(PREFIX + 'FILE_MANIFEST.json', manifest_data)
(DOCS / 'FILE_MANIFEST.json').write_bytes(manifest_data)
with zipfile.ZipFile(OUT) as archive:
    bad = archive.testzip()
    if bad:
        raise RuntimeError('ZIP CRC failure: ' + bad)
digest = hashlib.sha256(OUT.read_bytes()).hexdigest()
OUT.with_suffix('.zip.sha256').write_text(digest + '  ' + OUT.name + '\n')
print(json.dumps({'archive': str(OUT), 'files': len(manifest), 'bytes': OUT.stat().st_size, 'sha256': digest}, indent=2))
