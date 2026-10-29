#!/usr/bin/env python3
"""Prepare the 22H374 Pong userland on a macOS runner from an Apple IPSW URL.

The userdata layout needs a prepared userland: result.json, apply-complete.json
and the OS, SystemOS and Data images. Until now a CI run was handed that
directory as an archive built on a Mac that owns the restore. This script runs
the chain on the runner instead, so the only input is the IPSW download URL:

    download IPSW          scripts/download-ipados18-restore.py --url
    extract components     scripts/prepare-ipados18-components.py --ipsw
    AEA decrypt            scripts/decrypt-ipados18-aea.py --input --output
    decrypted UDIF -> raw  hdiutil convert -format UDTO
    source mounts          hdiutil attach -readonly
    SystemOS volume        hdiutil create (5 GiB APFS) + userland/build-systemos18.py
    Data volume            hdiutil create (8 GiB APFS) + hdiutil convert
    userland               userland/build-ipados18.py prepare / apply / finish

Which component feeds which step:

    OS                                    -> --os-image (the raw OS disk and its mount
                                             is --os-root)
    Cryptex1,SystemOS                     -> --system-cryptex (mounted) and the source
                                             build-systemos18.py copies from
    Cryptex1,AppOS                        -> --app-cryptex (mounted)
    Ap,SystemVolumeCanonicalMetadata,
    SystemVolume, Cryptex1,SystemVolume,
    Cryptex1,SystemTrustCache             -> extracted and hashed into the report only.
                                             No step in this repository reads them
                                             yet; they are kept so a later sealing
                                             check does not need a second download.

Every Apple byte stays under --work-dir, and the five files the userdata stage
needs are copied or converted into --out. No image is patched in place and
nothing here writes a phone.

apply and build-systemos18.py need root: they attach images with ownership on
and preserve the original uid/gid of Apple's files. Both run under `sudo -n`,
and a runner that cannot sudo is a hard failure rather than a skip, because a
userland built without them would look finished and boot nothing.

--out is then checked with scripts/stage-pong-userdata.py's own validators, the
same code the archive path used, so the five required files, the 22H374 build,
the 4 KiB geometry and the three distinct APFS volume UUIDs mean exactly what
they meant before.
"""
import argparse
import importlib.util
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GIB = 1 << 30

# Components the chain consumes, then the metadata components it only records.
USED_COMPONENTS = ('OS', 'Cryptex1,SystemOS', 'Cryptex1,AppOS')
RECORD_COMPONENTS = ('Ap,SystemVolumeCanonicalMetadata', 'SystemVolume',
                     'Cryptex1,SystemTrustCache', 'Cryptex1,SystemVolume')
COMPONENTS = USED_COMPONENTS + RECORD_COMPONENTS

# Geometry from migration/ipados18/README.zh.md: SystemOS grew from 3.69 GiB to
# 5 GiB, and Data is its own fresh 8 GiB disk rather than the iOS 16 one.
SYSTEMOS_MIB = 5242880
DATA_MIB = 8388608

# hdiutil detach reports "resource busy" while the last writes are still being
# flushed, so retry before giving up.
DETACH_ATTEMPTS = 5

# Free space this chain wants before it starts. The peak is at `prepare`, which
# holds the two raw sources and their two copies at the same time (GiB, from the
# real component sizes in migration/ipados18/baseline and the documented OS
# window of 6,845,104,128 bytes):
#
#   --work-dir   OS raw 6.4 + Data raw 8.0 + SystemOS source 1.5 + AppOS 0.02 + SystemOS cdr 5.0
#   --out        OS copy 6.4 + Data copy 8.0
#   ----------------------------------------------------------------------------------
#   about 37 GiB at that instant; finish then adds PackedRAMDisk (~1.5 GiB).
#
# 45 GiB leaves roughly 8 GiB for the checkout, the refer pins and whatever the
# kernel build left behind on the same volume. Each conversion also re-checks
# the number it actually needs, so this is a fast fail rather than a budget.
MIN_FREE_BYTES = 45 * GIB


def now():
    return datetime.now(timezone.utc).isoformat(timespec='seconds')


def redacted(*values):
    """Drop the query string from anything that looks like a URL.

    An IPSW URL is normally the public Apple one, but a pre-signed link carries
    its credential in the query string and this report is uploaded as an
    artifact, so the query never goes into it.
    """
    out = []
    for value in values:
        text = str(value)
        if '://' in text and '?' in text:
            head, _, _ = text.partition('?')
            text = head + '?<redacted>'
        out.append(text)
    return out


# A subprocess can print the URL it was given, so captured output is scrubbed
# with the same rule. Only the query string goes; the host and path stay, which
# is what makes a report readable.
URL_QUERY = re.compile(r'(https://[^\s\'"?]+)\?[^\s\'"]*')


def redact_text(text):
    return URL_QUERY.sub(r'\1?<redacted>', text)


def fail(message):
    raise SystemExit('userland from IPSW refused: ' + message)


class Recorder:
    """One JSON report: every command, its real exit status, and byte counts."""

    def __init__(self):
        self.report = {
            'format': 'pong-userland-from-ipsw-v1',
            'build': '22H374',
            'started_utc': now(),
            'finished_utc': None,
            'platform': sys.platform,
            'python': sys.version.split()[0],
            'commands': [],
            'stages': [],
            'disk': {},
            'warnings': [],
            'image_verification': 'not performed: images are copied or converted and read '
                                  'through tools/apfs for container headers and volume UUIDs only',
        }

    # -- commands ---------------------------------------------------------
    def run(self, label, argv, *, capture=False, env=None, cwd=None, expect=0):
        argv = [str(item) for item in argv]
        started = time.monotonic()
        record = {'label': label, 'argv': redacted(*argv), 'cwd': str(cwd) if cwd else None,
                  'started_utc': now()}
        try:
            completed = subprocess.run(
                argv, cwd=cwd, env=env, check=False, text=True,
                stdout=subprocess.PIPE if capture else None,
                stderr=subprocess.STDOUT if capture else None)
        except OSError as error:
            record.update(exit='spawn-error', error=str(error),
                          seconds=round(time.monotonic() - started, 1))
            self.report['commands'].append(record)
            fail('could not run %s: %s' % (argv[0], error))
        record['exit'] = completed.returncode
        record['seconds'] = round(time.monotonic() - started, 1)
        if capture:
            text = completed.stdout or ''
            record['output_tail'] = redact_text(text[-4000:])
            record['output_bytes'] = len(text.encode())
        self.report['commands'].append(record)
        if expect is not None and completed.returncode != expect:
            tail = (record.get('output_tail') or '').strip().splitlines()[-12:]
            fail('%s exited %d%s' % (label, completed.returncode,
                                     (':\n  ' + '\n  '.join(tail)) if tail else ''))
        return completed

    def stage(self, name, **fields):
        entry = {'stage': name, 'at_utc': now(), **fields}
        self.report['stages'].append(entry)
        print('== %s: %s' % (name, json.dumps(fields, ensure_ascii=False)), flush=True)

    def warn(self, message):
        self.report['warnings'].append(message)
        print('WARNING: ' + message, flush=True)

    # -- disk -------------------------------------------------------------
    def df(self, label, path):
        completed = subprocess.run(['df', '-k', str(path)], capture_output=True, text=True)
        if completed.returncode != 0:
            fail('df -k %s failed: %s' % (path, completed.stderr.strip()))
        lines = completed.stdout.strip().splitlines()
        fields = lines[-1].split()
        available = int(fields[3]) * 1024
        used = int(fields[2]) * 1024
        self.report['disk'][label] = {'path': str(path), 'available_bytes': available,
                                      'used_bytes': used, 'df': lines}
        print('df %-28s %6.2f GiB free at %s' % (label, available / GIB, path), flush=True)
        return available

    def require(self, label, path, need_bytes, what):
        available = self.df(label, path)
        if available < need_bytes:
            fail('%s has %.2f GiB free but %s needs %.2f GiB; free space and try again'
                 % (path, available / GIB, what, need_bytes / GIB))
        return available


def disk_bytes(path):
    return path.stat().st_size if path.is_file() else 0


def du_bytes(path):
    total = 0
    for entry in Path(path).rglob('*'):
        try:
            if entry.is_file() and not entry.is_symlink():
                total += entry.stat().st_size
        except OSError:
            continue
    return total


def sudo_prefix(rec):
    """`sudo -n true` must succeed; a userland built without the root steps is a fake one.

    A GitHub-hosted macOS runner allows passwordless sudo. If this probe fails the
    run stops here rather than skipping apply or the SystemOS copy, because both
    write Apple's original uid/gid and a copy made without them is the exact
    failure the local recipe documents (shared caches owned by uid 99).
    """
    completed = rec.run('sudo probe', ['sudo', '-n', 'true'], capture=True, expect=None)
    if completed.returncode != 0:
        fail('`sudo -n true` failed (exit %d) on this host. apply and build-systemos18.py need '
             'root to preserve Apple file ownership, and this chain refuses to build a userland '
             'without them. Run on a runner that allows passwordless sudo.\n%s'
             % (completed.returncode, (completed.stdout or '').strip()[-800:]))
    return ['sudo', '-n', sys.executable]


def _attach_argv(image, *, readwrite, owners, mountpoint, prefix=(), bare=False):
    argv = list(prefix) + ['hdiutil', 'attach', '-noverify', '-nobrowse',
                           '-readwrite' if readwrite else '-readonly']
    if owners:
        argv += ['-owners', owners]
    if mountpoint:
        argv += ['-mountpoint', str(mountpoint)]
    if bare:
        # A bare APFS container, which is what the AppOS component is, has no
        # partition table for hdiutil to interpret. diskimage-class is the
        # documented way to say "this is a plain filesystem image".
        argv += ['-imagekey', 'diskimage-class=CRawDiskImage']
    argv += ['-plist', str(image)]
    return argv


def attach(rec, image, *, readwrite=False, owners=None, mountpoint=None, label=None,
           prefix=()):
    """Attach one image and return (whole disk device, [mount points])."""
    label = label or ('attach ' + Path(image).name)
    options = dict(readwrite=readwrite, owners=owners, mountpoint=mountpoint, prefix=prefix)
    completed = rec.run(label, _attach_argv(image, **options), capture=True, expect=None)
    if completed.returncode != 0 and image_kind(image) == 'apfs':
        rec.warn('hdiutil did not attach %s as an ordinary image; retrying it as a bare '
                 'filesystem image' % Path(image).name)
        completed = rec.run(label + ' (bare filesystem image)',
                            _attach_argv(image, bare=True, **options),
                            capture=True, expect=None)
    if completed.returncode != 0:
        raise SystemExit('userland from IPSW refused: %s could not be attached (hdiutil exit %d)'
                         % (Path(image).name, completed.returncode))
    try:
        doc = plistlib.loads(completed.stdout.encode())
    except Exception as error:
        fail('hdiutil attach did not print a property list for %s: %s' % (image, error))
    entities = doc.get('system-entities') or []
    whole = next((item['dev-entry'] for item in entities if item.get('dev-entry')), None)
    if not whole:
        fail('hdiutil attach returned no device for ' + str(image))
    mounts = [Path(item['mount-point']) for item in entities if item.get('mount-point')]
    print('attached %s as %s at %s' % (Path(image).name, whole,
                                       ', '.join(str(m) for m in mounts) or 'no mount'),
          flush=True)
    return whole, mounts


class Attached:
    """Every mount this run made, so a failure still releases all of them.

    A runner is a throwaway VM, but a volume left attached makes the *next*
    hdiutil call in the same step ambiguous ("resource busy", two mounts of the
    same name), which turns one real failure into a confusing second one.
    """

    def __init__(self, rec):
        self.rec = rec
        self.devices = []

    def add(self, device, label):
        self.devices.append((device, label))
        return device

    def release_all(self):
        while self.devices:
            device, label = self.devices.pop()
            try:
                detach(self.rec, device, label)
            except SystemExit as error:
                self.rec.warn('detaching %s failed: %s' % (device, error))


def detach(rec, device, label='detach'):
    for attempt in range(1, DETACH_ATTEMPTS + 1):
        completed = subprocess.run(['hdiutil', 'detach', str(device)],
                                   capture_output=True, text=True)
        rec.report['commands'].append({
            'label': '%s %s (attempt %d)' % (label, device, attempt),
            'argv': ['hdiutil', 'detach', str(device)], 'exit': completed.returncode,
            'output_tail': (completed.stdout + completed.stderr)[-2000:]})
        if completed.returncode == 0:
            print('detached ' + str(device), flush=True)
            return
        time.sleep(2 * attempt)
    rec.run(label + ' (forced)', ['hdiutil', 'detach', '-force', str(device)], capture=True)


def mount_holding(mounts, probe, what):
    """The one mount under which `probe` exists. A disk image may attach several."""
    matches = [path for path in mounts if (path / probe).is_file()]
    if not matches:
        fail('%s mounted at %s holds no %s'
             % (what, ', '.join(str(m) for m in mounts) or 'nowhere', probe))
    if len(matches) > 1:
        fail('%s mounted more than one volume holding %s: %s'
             % (what, probe, ', '.join(str(m) for m in matches)))
    return matches[0]


def system_mount(mounts, what):
    """The mount that carries System/Library/CoreServices/SystemVersion.plist."""
    return mount_holding(mounts, 'System/Library/CoreServices/SystemVersion.plist', what)


def image_kind(path):
    """What kind of disk image this file is, from its own bytes.

    The restore does not use one container for everything: the AEA OS decrypts
    to a UDIF (a `koly` trailer), the AppOS component is a bare APFS container
    (an `NXSB` superblock at offset 32), and a converted window comes back as a
    GPT disk (`EFI PART` at LBA 1). Deciding this by reading the file is the
    only way to avoid converting something that is already raw.
    """
    path = Path(path)
    # A sparseimage is a bundle directory, so this test comes before any open.
    if path.is_dir():
        return 'sparseimage'
    with path.open('rb') as handle:
        head = handle.read(4096)
        size = handle.seek(0, os.SEEK_END)
        handle.seek(max(0, size - 512))
        trailer = handle.read(512)
    if trailer[:4] == b'koly':
        return 'udif'
    if len(head) >= 4096 and head[32:36] == b'NXSB':
        return 'apfs'
    if len(head) >= 1024 and head[512:520] == b'EFI PART':
        return 'gpt'
    return 'unknown'


def materialize_raw(rec, source, destination, label):
    """Put a native raw disk image at `destination` and return its byte length.

    A UDIF is expanded with `hdiutil convert -format UDTO`, the same conversion
    the local recipe documents. Anything already raw is moved rather than copied:
    the OS disk is about 6.4 GiB and duplicating it would need that much more
    room for no gain, and the source is not needed afterwards.
    """
    source = Path(source)
    if not source.exists():
        fail('%s does not exist, so it cannot become %s' % (source, destination))
    kind = image_kind(source)
    rec.stage('image-kind', image=source.name, kind=kind, bytes=disk_bytes(source))
    if destination.exists():
        fail('refusing to overwrite ' + str(destination))
    if kind in ('apfs', 'gpt'):
        os.replace(source, destination)
    elif kind in ('udif', 'sparseimage'):
        rec.run(label, ['hdiutil', 'convert', str(source), '-format', 'UDTO',
                        '-o', str(destination)])
        # hdiutil appends .cdr when the requested name does not already end in it.
        produced = destination if destination.exists() else destination.with_name(
            destination.name + '.cdr')
        if not produced.exists():
            fail('hdiutil convert wrote neither %s nor %s'
                 % (destination, destination.with_name(destination.name + '.cdr')))
        if produced != destination:
            produced.rename(destination)
    else:
        fail('%s is not a UDIF, a GPT disk or an APFS container, so there is no existing '
             'route in this repository to turn it into a raw window image' % source)
    size = destination.stat().st_size
    if size == 0 or size % 4096:
        fail('%s is %d bytes; a raw window input must be 4 KiB aligned' % (destination, size))
    return size


def create_apfs_image(rec, path, mib, volume_name, label):
    rec.run(label, ['hdiutil', 'create', '-size', '%dk' % mib, '-type', 'SPARSE',
                    '-layout', 'NONE', '-fs', 'APFS', '-volname', volume_name, str(path)])
    if not path.is_dir():
        fail('hdiutil create did not produce the sparseimage ' + str(path))


def load_stage_module():
    """The userdata contract lives in stage-pong-userdata.py; use it, do not copy it."""
    spec = importlib.util.spec_from_file_location(
        'stage_pong_userdata', ROOT / 'scripts/stage-pong-userdata.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def probe_volume(rec, stage, path, layout_key, role):
    """Run stage-pong-userdata.py's own header and volume rules on a fresh image.

    The final verification is still authoritative. Doing it here as well means a
    conversion that produced something other than a raw APFS disk fails within
    minutes of being written, instead of after prepare/apply/finish and before
    the layout stage spends its time hashing 19 GiB.
    """
    try:
        kind = stage.header_kind(path)
        uuid, volume_name, found_role = stage.volume_identity(path, layout_key, role)
    except stage.Unreadable as error:
        fail(str(error))
    except SystemExit as error:
        fail('%s: %s' % (Path(path).name, error.code if isinstance(error.code, str) else error))
    rec.stage('probe', image=Path(path).name, kind=kind, layout_key=layout_key,
              volume_uuid=uuid, volume_name=volume_name, volume_role=found_role)
    return dict(kind=kind, volume_uuid=uuid, volume_name=volume_name, volume_role=found_role)


def expect_file(path, what, minimum=1):
    """A step that reported success must actually have produced its file.

    Every producer in this chain writes its own output; a zero exit code with no
    file means the step is not what this chain assumes, and the next read would
    die on a bare FileNotFoundError far from the cause.
    """
    path = Path(path)
    if not path.is_file():
        fail('%s reported success but %s does not exist' % (what, path))
    size = path.stat().st_size
    if size < minimum:
        fail('%s produced %d bytes at %s; at least %d expected' % (what, size, path, minimum))
    return size


def component_paths(work):
    """Map component name -> extracted file from the extractor's own report."""
    progress = work / '22H374/extraction-progress.json'
    if not progress.is_file():
        fail('prepare-ipados18-components.py wrote no extraction-progress.json under ' + str(work))
    state = json.loads(progress.read_text())
    if state.get('state') != 'extracted':
        fail('component extraction did not finish: state is %r' % state.get('state'))
    mapping = {}
    for artifact in state['artifacts']:
        for name in artifact['components']:
            mapping.setdefault(name, Path(artifact['local_path']))
    return mapping


def need_component(mapping, name):
    path = mapping.get(name)
    if path is None:
        fail('the restore has no %s component; this is not the 22H374 iPad restore' % name)
    if not path.is_file() or path.stat().st_size == 0:
        fail('%s was extracted as %s but is missing or empty' % (name, path))
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ipsw-url', required=True,
                        help='https URL of the iPad 18.7.10 / 22H374 restore (the IPSW itself)')
    parser.add_argument('--ipsw-sha256', default='',
                        help='optional sha256 of the IPSW; a mismatch stops before extraction')
    parser.add_argument('--work-dir', type=Path, required=True,
                        help='somewhere to download and expand; emptied of intermediates as the '
                             'run proceeds. Must not be inside --out')
    parser.add_argument('--out', type=Path, required=True,
                        help='prepared userland directory to create; must be absent or empty')
    parser.add_argument('--developer', type=Path,
                        help='Xcode developer directory; default is xcode-select -p')
    parser.add_argument('--json-out', type=Path, help='write the run report here')
    parser.add_argument('--volume-layout-out', type=Path,
                        help='write the volume-layout.json these images imply here as well')
    parser.add_argument('--jobs', type=int, default=8, help='parallel ranges for the IPSW download')
    parser.add_argument('--chunk-mib', type=int, default=64, help='IPSW download chunk size')
    parser.add_argument('--keep-work', action='store_true',
                        help='keep the downloaded and decrypted intermediates for a post mortem')
    args = parser.parse_args()

    rec = Recorder()
    report = rec.report
    started = time.monotonic()

    if sys.platform != 'darwin':
        fail('this chain needs macOS: hdiutil, diskutil, codesign and /usr/bin/aea only exist there '
             '(running on %s)' % sys.platform)

    developer = args.developer or Path(subprocess.check_output(
        ['xcode-select', '-p'], text=True).strip())
    # Test the thing the build actually needs, not a guessed file layout.
    # build-ipados18.py compiles with `xcrun --sdk iphoneos` under DEVELOPER_DIR,
    # and on the macOS runner image `$DEVELOPER_DIR/usr/bin/xcrun` does not
    # exist while /usr/bin/xcrun resolves the same Xcode correctly. Asking
    # xcrun for the iPhoneOS SDK is both the real precondition and the check
    # that the selected Xcode is a full one with an iOS SDK rather than
    # CommandLineTools.
    sdk_env = dict(os.environ, DEVELOPER_DIR=str(developer))
    try:
        probe = subprocess.run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'],
                               capture_output=True, text=True, env=sdk_env)
    except OSError as error:
        fail('could not run xcrun: %s. This needs a macOS host with Xcode.' % error)
    sdk_path = probe.stdout.strip()
    if probe.returncode != 0 or not sdk_path or not Path(sdk_path).is_dir():
        fail('`xcrun --sdk iphoneos --show-sdk-path` failed with DEVELOPER_DIR=%s (exit %d): %s\n'
             'The userland build needs a full Xcode with the iPhoneOS SDK; if only '
             'CommandLineTools are selected, run `sudo xcode-select -s /Applications/Xcode.app`.'
             % (developer, probe.returncode,
                (probe.stderr or probe.stdout).strip()[-600:] or 'no output'))

    work = args.work_dir.expanduser().resolve()
    out = args.out.expanduser().resolve()
    if out.exists() and any(out.iterdir()):
        fail('%s already holds files; use a new output directory' % out)
    if work == out or work.is_relative_to(out) or out.is_relative_to(work):
        fail('--work-dir and --out must be separate trees: %s and %s' % (work, out))
    work.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    report['inputs'] = {'ipsw_url': redacted(args.ipsw_url)[0],
                        'ipsw_url_has_query': '?' in args.ipsw_url,
                        'ipsw_sha256': args.ipsw_sha256.strip().lower() or None,
                        'work_dir': str(work), 'out': str(out), 'developer': str(developer),
                        'iphoneos_sdk': sdk_path}

    rec.df('start', work)
    rec.require('start:required', work, MIN_FREE_BYTES, 'the userland chain')

    root = sudo_prefix(rec)
    stage = load_stage_module()
    attached = Attached(rec)

    # -- 1. download ------------------------------------------------------
    #
    # The IPSW is downloaded whole rather than read through HTTP ranges: the
    # URL is the input, so the run should prove it can fetch the file the user
    # named, and --ipsw-sha256 can only cover a file that exists locally. The
    # extractor then reads that file, so its own ZIP directory check applies.
    rec.stage('download', url=redacted(args.ipsw_url)[0])
    downloader = ROOT / 'scripts/download-ipados18-restore.py'
    download_argv = [sys.executable, str(downloader), '--output-dir', str(work / 'ipsw'),
                     '--jobs', str(args.jobs), '--chunk-mib', str(args.chunk_mib),
                     '--url', args.ipsw_url]
    if args.ipsw_sha256.strip():
        download_argv += ['--sha256', args.ipsw_sha256.strip().lower()]
    rec.run('download IPSW', download_argv)
    state = json.loads((work / 'ipsw/download-progress.json').read_text())
    ipsw = work / 'ipsw' / state['image']
    if not ipsw.is_file() or ipsw.stat().st_size != state['bytes']:
        fail('the downloader finished without a complete %s' % ipsw)
    rec.stage('downloaded', bytes=ipsw.stat().st_size, image=ipsw.name,
              sha256=state.get('sha256'))
    report['inputs']['ipsw_bytes'] = ipsw.stat().st_size
    report['inputs']['ipsw_file'] = str(ipsw)

    # -- 2. extract the components the chain needs ------------------------
    rec.stage('extract', components=list(COMPONENTS))
    extract_argv = [sys.executable, str(ROOT / 'scripts/prepare-ipados18-components.py'),
                    '--ipsw', str(ipsw), '--output-dir', str(work / '22H374')]
    for name in COMPONENTS:
        extract_argv += ['--only', name]
    rec.run('extract components', extract_argv)
    mapping = component_paths(work)
    os_aea = need_component(mapping, 'OS')
    systemos_aea = need_component(mapping, 'Cryptex1,SystemOS')
    appos_dmg = need_component(mapping, 'Cryptex1,AppOS')
    recorded = {}
    for name in RECORD_COMPONENTS:
        path = mapping.get(name)
        recorded[name] = {'path': str(path) if path else None,
                          'bytes': disk_bytes(path) if path else None,
                          'consumed_by': None}
    report['components'] = {
        'OS': {'path': str(os_aea), 'bytes': os_aea.stat().st_size},
        'Cryptex1,SystemOS': {'path': str(systemos_aea), 'bytes': systemos_aea.stat().st_size},
        'Cryptex1,AppOS': {'path': str(appos_dmg), 'bytes': appos_dmg.stat().st_size},
        'recorded_only': recorded,
    }
    rec.stage('extracted', os_bytes=os_aea.stat().st_size,
              systemos_bytes=systemos_aea.stat().st_size,
              appos_bytes=appos_dmg.stat().st_size,
              recorded_only=list(recorded))

    # The IPSW has done its job. It is the largest single file in the run and
    # the components above are all that is read from it.
    if not args.keep_work:
        for part in (work / 'ipsw').iterdir():
            if part.is_file():
                part.unlink()
        shutil.rmtree(work / 'ipsw', ignore_errors=True)
    rec.df('after-extract', work)

    # -- 3. AEA decrypt ---------------------------------------------------
    #
    # decrypt-ipados18-aea.py unwraps the profile-1 archive key with pyhpke and
    # hands the raw key to /usr/bin/aea. The key is never an argument here and
    # never reaches this report.
    rec.stage('decrypt')
    os_dmg = work / 'os.dmg'
    systemos_dmg = work / 'systemos.dmg'
    rec.require('before-os-decrypt', work, os_aea.stat().st_size + 6 * GIB, 'the OS decryption')
    rec.run('decrypt OS', [sys.executable, str(ROOT / 'scripts/decrypt-ipados18-aea.py'),
                           '--input', str(os_aea), '--output', str(os_dmg)])
    rec.require('before-systemos-decrypt', work, systemos_aea.stat().st_size + 2 * GIB,
                'the SystemOS decryption')
    rec.run('decrypt SystemOS', [sys.executable, str(ROOT / 'scripts/decrypt-ipados18-aea.py'),
                                 '--input', str(systemos_aea), '--output', str(systemos_dmg)])
    # Two distinct 4 KiB-blocked images; a smaller one is a truncated decrypt
    # that still exited 0 and would fail much later inside hdiutil.
    os_dmg_bytes = expect_file(os_dmg, 'the OS decryption', 1 << 30)
    systemos_dmg_bytes = expect_file(systemos_dmg, 'the SystemOS decryption', 1 << 20)
    rec.stage('decrypted', os_dmg_bytes=os_dmg_bytes, systemos_dmg_bytes=systemos_dmg_bytes)
    if not args.keep_work:
        os_aea.unlink(missing_ok=True)
        systemos_aea.unlink(missing_ok=True)

    # -- 4. OS: UDIF -> raw disk, then mount it as --os-root --------------
    #
    # build-ipados18.py prepare copies --os-image verbatim into
    # pong-ipados18-system.cdr, so the input has to be the raw disk already: a
    # UDIF file would be copied as a UDIF file and the userdata stage would
    # refuse it (it looks for an APFS container or a GPT at the front).
    rec.stage('os-image')
    os_raw = work / 'os-raw.cdr'
    rec.require('before-os-convert', work, os_dmg.stat().st_size + 6 * GIB, 'the OS conversion')
    os_raw_bytes = materialize_raw(rec, os_dmg, os_raw, 'convert OS to raw')
    probe_volume(rec, stage, os_raw, 'os', 0x1)
    if not args.keep_work:
        os_dmg.unlink(missing_ok=True)
    os_device, os_mounts = attach(rec, os_raw, label='attach OS (read only)')
    attached.add(os_device, 'detach OS')
    os_root = system_mount(os_mounts, 'the OS image')
    version = plistlib.loads(
        (os_root / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if (version.get('ProductVersion'), version.get('ProductBuildVersion')) != ('18.7.10', '22H374'):
        fail('the OS image is %s/%s, not 18.7.10/22H374'
             % (version.get('ProductVersion'), version.get('ProductBuildVersion')))
    if not (os_root / 'sbin/launchd').is_file():
        fail('%s has no sbin/launchd; that is not a bootable 22H374 root' % os_root)
    # data_seed() reads the Data template out of this image's /private/var and
    # copies file contents from this mount, so an absent one is fatal while an
    # empty one only means the seed is the port's own directory policy.
    private_var = os_root / 'private/var'
    if not private_var.is_dir():
        fail('%s has no private/var; build-ipados18.py seeds Data from it' % os_root)
    private_var_entries = sum(1 for _ in private_var.iterdir())
    # A restore's OS image carries /private/var only as the mount point for the
    # Data volume it will later mount, so an empty one is the expected shape and
    # means the Data seed is the port's directory policy and preference plists.
    # It is recorded because it decides what the Data window holds.
    rec.stage('data-template', private_var=str(private_var),
              entries=private_var_entries,
              meaning='restore template for the Data seed; 0 means the seed is the port '
                      'directory policy and preferences only')
    rec.stage('os-mounted', device=os_device, mount=str(os_root), raw_bytes=os_raw_bytes,
              product_version=version.get('ProductVersion'),
              product_build=version.get('ProductBuildVersion'),
              private_var_entries=private_var_entries)

    # -- 5. SystemOS: decrypt already done; build the ordinary APFS copy ---
    rec.stage('systemos')
    # Ownership has to be on the source *before* build-systemos18.py reads it:
    # that script refuses a source volume reporting GlobalPermissionsEnabled
    # false and would try to enable it on what is a read-only mount. It is a
    # mount option and needs root on macOS, hence the sudo prefix here while the
    # script itself stays under sudo later.
    systemos_device, systemos_mounts = attach(rec, systemos_dmg, owners='on',
                                              label='attach SystemOS (read only, owners on)',
                                              prefix=root[:-1])
    attached.add(systemos_device, 'detach SystemOS')
    systemos_source = system_mount(systemos_mounts, 'the SystemOS image')
    cryptex_version = plistlib.loads(
        (systemos_source / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if cryptex_version.get('ProductBuildVersion') != '22H374':
        fail('the SystemOS image is build %r, not 22H374'
             % cryptex_version.get('ProductBuildVersion'))
    cache = systemos_source / 'System/Library/Caches/com.apple.dyld'
    cache_files = sorted(p for p in cache.glob('dyld_shared_cache_arm64*') if p.is_file())
    if len(cache_files) < 2:
        fail('%s holds %d split arm64 shared caches; build-ipados18.py needs at least two'
             % (cache, len(cache_files)))

    systemos_sparse = work / 'PongSystemOS18.sparseimage'
    create_apfs_image(rec, systemos_sparse, SYSTEMOS_MIB, 'Pong-iPadOS18-SystemOS',
                      'create SystemOS volume')
    systemos_cdr = out / 'pong-ipados18-systemos.cdr'
    # Root for the same reason the local recipe is run with sudo: ditto has to
    # write Apple's original uid/gid onto the copy, and hdiutil needs ownership
    # enabled on both sides for that to survive.
    rec.run('build SystemOS volume',
            root + [str(ROOT / 'userland/build-systemos18.py'),
                    '--source', str(systemos_source),
                    '--image', str(systemos_sparse),
                    '--mountpoint', str(work / 'systemos-build-mount'),
                    '--output', str(systemos_cdr)])
    systemos_bytes = expect_file(systemos_cdr, 'build-systemos18.py', 4096)
    if systemos_bytes % 4096:
        fail('build-systemos18.py produced %d bytes at %s; a window image must be 4 KiB aligned'
             % (systemos_bytes, systemos_cdr))
    probe_volume(rec, stage, systemos_cdr, 'systemos', None)
    if not args.keep_work:
        shutil.rmtree(systemos_sparse, ignore_errors=True)
    rec.stage('systemos-built', bytes=systemos_bytes, cache_pieces=len(cache_files))

    # -- 6. AppOS: mount it for --app-cryptex ------------------------------
    appos_device, appos_mounts = attach(rec, appos_dmg, label='attach AppOS (read only)')
    attached.add(appos_device, 'detach AppOS')
    # build-ipados18.py's install_app_cryptex() names these three files as the
    # proof that a complete 22H374 AppOS was supplied, so use the same test to
    # pick the mount rather than assuming a SystemVersion.plist.
    appos_root = mount_holding(appos_mounts, 'usr/libexec/webbookmarksd', 'the AppOS image')
    for required in ('usr/libexec/AuthenticationServicesAgent',
                     'System/Library/CoreServices/SafariSupport.bundle/SafariBookmarksSyncAgent'):
        if not (appos_root / required).is_file():
            fail('%s holds no %s; that is not a complete 22H374 AppOS' % (appos_root, required))

    # -- 7. Data: a fresh 8 GiB APFS disk, as the local recipe makes ------
    #
    # The iOS 16 writable Data is not reused. prepare copies --data-image into
    # pong-ipados18-data.cdr, so this is also converted to a raw disk first.
    rec.stage('data-image')
    data_sparse = work / 'PongData18.sparseimage'
    create_apfs_image(rec, data_sparse, DATA_MIB, 'Pong-iPadOS18-Data', 'create Data volume')
    data_raw = work / 'data-template.cdr'
    rec.require('before-data-convert', work, DATA_MIB * 1024 + 2 * GIB, 'the Data conversion')
    data_raw_bytes = materialize_raw(rec, data_sparse, data_raw, 'convert Data to raw')
    if data_raw_bytes != DATA_MIB * 1024:
        fail('the Data image is %d bytes, not the %d a %d KiB APFS volume gives'
             % (data_raw_bytes, DATA_MIB * 1024, DATA_MIB))
    probe_volume(rec, stage, data_raw, 'data', 0x40)
    if not args.keep_work:
        shutil.rmtree(data_sparse, ignore_errors=True)
    rec.stage('data-created', bytes=data_raw_bytes)

    # -- 8. prepare -------------------------------------------------------
    #
    # prepare copies both raw images into --out and builds the overlay; it must
    # run as the normal runner user because it compiles with xcrun/codesign.
    rec.stage('prepare')
    rec.require('before-prepare', work, os_raw_bytes + data_raw_bytes + 4 * GIB,
                'prepare (two image copies)')
    rec.run('prepare userland', [sys.executable, str(ROOT / 'userland/build-ipados18.py'),
                                 'prepare', '--out', str(out),
                                 '--os-root', str(os_root),
                                 '--os-image', str(os_raw),
                                 '--system-cryptex', str(systemos_source),
                                 '--data-image', str(data_raw),
                                 '--developer', str(developer),
                                 '--app-cryptex', str(appos_root)])
    rec.stage('prepared',
              system_cdr_bytes=expect_file(out / 'pong-ipados18-system.cdr',
                                           'prepare (the OS copy)', 1 << 30),
              data_cdr_bytes=expect_file(out / 'pong-ipados18-data.cdr',
                                         'prepare (the Data copy)', 1 << 20))
    # Nothing after prepare reads the three source volumes: apply and finish
    # work only on the copies under --out. Releasing them here is what keeps the
    # peak at the two copies plus their sources.
    attached.release_all()
    if not args.keep_work:
        systemos_dmg.unlink(missing_ok=True)
        appos_dmg.unlink(missing_ok=True)
    rec.df('after-prepare', work)

    # -- 9. apply (root) --------------------------------------------------
    #
    # apply refuses a target that is the source file, so the two raw images
    # have to outlive this step; they are removed immediately afterwards.
    rec.stage('apply')
    rec.run('apply userland', root + [str(ROOT / 'userland/build-ipados18.py'),
                                      'apply', '--out', str(out)])
    expect_file(out / 'apply-complete.json', 'apply', 2)

    # -- 10. finish -------------------------------------------------------
    rec.stage('finish')
    rec.run('finish userland', [sys.executable, str(ROOT / 'userland/build-ipados18.py'),
                                'finish', '--out', str(out)])
    expect_file(out / 'result.json', 'finish', 2)

    # -- 11. drop the intermediates ---------------------------------------
    #
    # apply refused to treat --out/pong-ipados18-{system,data}.cdr as its own
    # source, which needs those two raw images to have existed while it ran;
    # they are finished with now.
    attached.release_all()
    if not args.keep_work:
        for path in (os_raw, data_raw):
            path.unlink(missing_ok=True)
        shutil.rmtree(work / '22H374', ignore_errors=True)
    rec.df('after-cleanup', work)

    # -- 12. check the output with the userdata stage's own rules ---------
    rec.stage('verify')
    result, sizes = stage.validate_tree(out)
    images, unreadable = {}, []
    for name, spec in stage.IMAGES.items():
        entry = {'bytes': sizes[name], 'kind': stage.header_kind(out / name),
                 'layout_key': spec['layout_key']}
        try:
            uuid, volume_name, role = stage.volume_identity(out / name, spec['layout_key'],
                                                            spec['role'])
        except stage.Unreadable as error:
            unreadable.append(str(error))
            uuid = volume_name = role = None
        entry.update(volume_uuid=uuid, volume_name=volume_name, volume_role=role)
        images[name] = entry
    if unreadable:
        # The archive path could accept a hand-written volume-layout.json here.
        # This chain produced the images itself, so an unreadable header is a
        # broken build, not a packaging choice.
        fail('could not read the volume UUIDs out of the images it just wrote:\n  '
             + '\n  '.join(unreadable))
    uuids = {spec['layout_key']: images[name]['volume_uuid'] for name, spec in stage.IMAGES.items()}
    if len(set(uuids.values())) != len(uuids):
        fail('the three images carry the same volume UUID: ' + json.dumps(uuids))
    layout = dict(ci_placeholder=False,
                  note='Volume UUIDs for the 22H374 images this run produced, uppercase as '
                       'diskutil and IOMedia print them. Source: read from the APFS volume '
                       'headers.',
                  **{key: {'uuid': value} for key, value in uuids.items()})
    (out / 'volume-layout.json').write_text(json.dumps(layout, indent=2) + '\n')
    if args.volume_layout_out:
        destination = args.volume_layout_out.expanduser().resolve()
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(layout, indent=2) + '\n')
    report.update({
        'userland': str(out),
        'build': result.get('build'),
        'images': images,
        'volume_uuid_source': 'read from the APFS volume headers',
        'volume_layout': layout,
        'userland_bytes': du_bytes(out),
        'seconds': round(time.monotonic() - started, 1),
        'finished_utc': now(),
    })
    rec.stage('verified', build=result.get('build'), images=sizes,
              userland_bytes=report['userland_bytes'])

    if args.json_out:
        destination = args.json_out.expanduser().resolve()
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in
                      ('build', 'userland', 'userland_bytes', 'volume_layout', 'seconds',
                       'warnings')}, indent=2), flush=True)
    print('userland ready at ' + str(out), flush=True)


if __name__ == '__main__':
    main()
