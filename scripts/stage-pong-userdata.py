#!/usr/bin/env python3
"""Unpack and validate a prepared 22H374 userland archive for the CI userdata build.

The userdata layout is built from a prepared userland: the two image copies that
`userland/build-ipados18.py prepare/apply/finish` leaves behind, the SystemOS copy
that `userland/build-systemos18.py` produces, and the completion records those
steps write. A machine that owns the 22H374 restore produces that directory; a CI
run receives it as a zip or tar.gz, so no Apple image has to live in Git.

The archive may be wrapped in one or more extra top-level directories, as a zip
made from a folder or a tarball made with `tar czf` adds.

What this checks: the five required names, that both records parse, that
result.json names build 22H374, that every image is a 4 KiB-aligned native raw
disk carrying an APFS container (bare, or inside a GPT), and that the three
volume UUIDs are readable and distinct. The images are not hashed here;
prepare-ipados18-ufs-layout.py has to stream every one of them to record the
SHA-256 each SHNGROOT entry carries, so doing it twice would only double the
slowest part of the run.
"""
import argparse
import hashlib
import json
import shutil
import struct
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APFS_TOOLS = ROOT / 'tools/apfs'

REQUIRED = ('result.json', 'apply-complete.json',
            'pong-ipados18-system.cdr', 'pong-ipados18-systemos.cdr',
            'pong-ipados18-data.cdr')
# Image name -> (volume-layout key, APFS role to prefer when the image holds
# more than one volume, where the image comes from). 0x1 is the sealed System
# role, 0x40 the writable Data role.
IMAGES = {
    'pong-ipados18-system.cdr': dict(layout_key='os', role=0x1,
        note='the OS copy build-ipados18.py writes beside result.json'),
    'pong-ipados18-systemos.cdr': dict(layout_key='systemos', role=None,
        note='the image userland/build-systemos18.py produces'),
    'pong-ipados18-data.cdr': dict(layout_key='data', role=0x40,
        note='the Data copy build-ipados18.py writes beside result.json'),
}
EXPECTED_BUILD = '22H374'


class Unreadable(Exception):
    """An image whose APFS volumes could not be read, as opposed to a wrong archive."""


def fail(message):
    raise SystemExit('userland archive refused: ' + message)


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
                shutil.copyfileobj(source, sink, 8 << 20)


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
                shutil.copyfileobj(source, sink, 8 << 20)


def flatten_single_tops(dest):
    """Descend while dest holds exactly one directory and no required file yet."""
    stripped = []
    for _ in range(4):
        if any((dest / name).is_file() for name in REQUIRED):
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


def read_record(path, what):
    try:
        value = json.loads(path.read_text())
    except (UnicodeDecodeError, ValueError) as error:
        fail(what + ' is not readable JSON: ' + str(error))
    if not isinstance(value, dict):
        fail(what + ' is not a JSON object')
    return value


def header_kind(path):
    """APFS container, or a GPT that holds one. Anything else is refused."""
    with path.open('rb') as handle:
        head = handle.read(4096)
    if len(head) >= 4096 and head[32:36] == b'NXSB':
        block_size = struct.unpack_from('<I', head, 36)[0]
        if block_size != 4096:
            fail('%s is an APFS container with %d-byte blocks; every Pong UFS window is '
                 '4096-byte blocked' % (path.name, block_size))
        return 'apfs'
    if len(head) >= 1024 and head[512:520] == b'EFI PART':
        return 'gpt'
    fail('%s carries neither an APFS container superblock at offset 32 nor a GPT header at '
         'LBA 1 (first 8 bytes: %s)' % (path.name, head[:8].hex() or 'empty'))


def volume_identity(path, label, prefer_role):
    """(uuid, volume name, role) of the volume an image is expected to hold."""
    sys.path.insert(0, str(APFS_TOOLS))
    import apfs
    try:
        container = apfs.Container(str(path))
        volumes = container.volumes()
    except Exception as error:
        raise Unreadable('%s could not be read as APFS: %s' % (path.name, error))
    try:
        if not volumes:
            fail(path.name + ' holds no APFS volume')
        chosen = None
        if len(volumes) == 1:
            chosen = volumes[0]
        elif prefer_role is not None:
            matching = [volume for volume in volumes if volume.role == prefer_role]
            if len(matching) == 1:
                chosen = matching[0]
        if chosen is None:
            described = ', '.join('%s role 0x%x' % (volume.volname, volume.role)
                                  for volume in volumes)
            fail('%s holds %d APFS volumes (%s) and none of them is the %s volume; supply an '
                 'image that holds only that volume' % (path.name, len(volumes), described, label))
        return apfs._uuid(chosen.vol_uuid).upper(), chosen.volname, chosen.role
    finally:
        container.close()


def validate_tree(dest):
    missing = []
    for name in REQUIRED:
        path = dest / name
        if not path.is_file():
            missing.append('missing ' + name + (
                '' if name not in IMAGES else ' (' + IMAGES[name]['note'] + ')'))
        elif path.stat().st_size == 0:
            missing.append('empty ' + name)
    if missing:
        fail('\n  ' + '\n  '.join(missing))
    result = read_record(dest / 'result.json', 'result.json')
    read_record(dest / 'apply-complete.json', 'apply-complete.json')
    if result.get('build') != EXPECTED_BUILD:
        fail('result.json names build %r; this workflow builds the %s userdata layout'
             % (result.get('build'), EXPECTED_BUILD))
    # A tracked OS generation installs from the sparsebundle named in
    # os-storage.json, while the layout records the raw image's size, so the two
    # would disagree and the device would reject the geometry. Ask for the plain
    # raw copy instead of guessing.
    storage = dest / 'os-storage.json'
    if storage.is_file() and read_record(storage, 'os-storage.json').get('ready'):
        fail('os-storage.json records a tracked sparsebundle OS generation. '
             'prepare-ipados18-ufs-layout.py sizes the raw image, so a tracked generation '
             'would disagree with what the installer streams. Supply the plain '
             'pong-ipados18-system.cdr, or finish the tracked generation first.')
    sizes = {}
    for name in IMAGES:
        path = dest / name
        size = path.stat().st_size
        if size < 4096 or size % 4096:
            fail('%s is %d bytes; every window input must be a native raw disk of at least '
                 '4096 bytes in 4096-byte blocks' % (name, size))
        sizes[name] = size
    return result, sizes


def supplied_layout(dest):
    """An optional volume-layout.json in the archive, validated for shape."""
    path = dest / 'volume-layout.json'
    if not path.is_file():
        return None
    doc = read_record(path, 'volume-layout.json')
    values = {}
    for key in ('os', 'systemos', 'data'):
        entry = doc.get(key)
        uuid = entry.get('uuid') if isinstance(entry, dict) else entry
        if not isinstance(uuid, str) or len(uuid) != 36:
            fail('volume-layout.json has no usable %r uuid; every entry needs the 36-character '
                 'UUID as diskutil prints it' % key)
        values[key] = uuid.upper()
    if len(set(values.values())) != len(values):
        fail('volume-layout.json repeats one UUID across volumes: ' + json.dumps(values))
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--archive', type=Path, required=True,
                        help='zip, tar.gz, tgz or tar holding the prepared userland')
    parser.add_argument('--dest', type=Path, required=True,
                        help='directory to unpack into; replaced if it exists')
    parser.add_argument('--json-out', type=Path, help='write the staging report here')
    parser.add_argument('--volume-layout-out', type=Path,
                        help='write the volume-layout.json these images imply')
    parser.add_argument('--expect-sha256', help='refuse the archive unless it has this digest')
    parser.add_argument('--max-mib', type=int, default=25600,
                        help='refuse an archive larger than this (default 25600)')
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
    # The three images land next to the two records, so the run needs the archive
    # plus the unpacked tree. Refuse here rather than die on ENOSPC at 90%.
    probe = dest.parent if dest.parent.exists() else Path('/')
    free = shutil.disk_usage(probe).free
    needed = 2 * size + (1 << 30)
    if free < needed:
        fail('%s has %d bytes free; unpacking a %d-byte archive needs about %d'
             % (probe, free, size, needed))
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    (extract_zip if kind == 'zip' else extract_tar)(archive, dest)
    wrapper = flatten_single_tops(dest)
    result, sizes = validate_tree(dest)
    declared = supplied_layout(dest)
    images, unreadable = {}, []
    for name, spec in IMAGES.items():
        entry = dict(bytes=sizes[name], kind=header_kind(dest / name),
                     layout_key=spec['layout_key'])
        try:
            uuid, volume_name, role = volume_identity(dest / name, spec['layout_key'], spec['role'])
        except Unreadable as error:
            unreadable.append(str(error))
            uuid = volume_name = role = None
        entry.update(volume_uuid=uuid, volume_name=volume_name, volume_role=role)
        images[name] = entry
    if declared is not None:
        for name, spec in IMAGES.items():
            found = images[name]['volume_uuid']
            if found is not None and found != declared[spec['layout_key']]:
                fail('%s holds volume %s but volume-layout.json says %s is %s'
                     % (name, found, spec['layout_key'], declared[spec['layout_key']]))
        uuids, source = declared, 'the volume-layout.json inside the archive'
    elif unreadable:
        fail('could not read the volume UUIDs out of the images:\n  ' +
             '\n  '.join(unreadable) +
             '\n  add a volume-layout.json to the archive with os, systemos and data UUIDs, '
             'as the local build keeps in volume-layout.json')
    else:
        uuids = {spec['layout_key']: images[name]['volume_uuid'] for name, spec in IMAGES.items()}
        source = 'read from the APFS volume headers'
    if len(set(uuids.values())) != len(uuids):
        fail('the three images carry the same volume UUID: ' + json.dumps(uuids))
    layout = dict(ci_placeholder=False,
                  note='Volume UUIDs for the supplied 22H374 images, uppercase as diskutil '
                       'and IOMedia print them. Source: ' + source + '.',
                  **{key: {'uuid': uuid} for key, uuid in uuids.items()})
    if args.volume_layout_out:
        out = args.volume_layout_out.expanduser().resolve()
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(layout, indent=2) + '\n')
    files = [path for path in dest.rglob('*') if path.is_file()]
    report = {
        'archive': archive.name,
        'archive_bytes': size,
        'archive_sha256': digest,
        'archive_format': kind,
        'wrapper_directories': wrapper,
        'files': len(files),
        'bytes': sum(path.stat().st_size for path in files),
        'build': result.get('build'),
        'userland': str(dest),
        'images': images,
        'volume_uuid_source': source,
        'volume_layout': layout,
        'image_verification': 'not performed: names, sizes, container headers and volume '
                              'UUIDs only',
    }
    if args.json_out:
        out = args.json_out.expanduser().resolve()
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
