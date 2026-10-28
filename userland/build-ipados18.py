#!/usr/bin/env python3
"""Adapt HackiOS userland to the local, native-APFS UniXNU Pong 22H374 port.

prepare: build static init, curate real 22H374 jobs, extract a Data seed and clone images.
apply:   mount ONLY these image copies and apply the prepared changes (macOS admin).
finish:  produce PackedRAMDisk; image verification is opt-in.
helper:  compile init and the matching OS runtime without rebuilding images.
refresh_runtime: update only init and the UI adapter in the prepared OS copy.
refresh_cryptex: expose matching SystemOS files absent from the OS namespace.
rebuild_data: recreate the local Data copy from the original empty APFS template.
reset_data: replace the prepared Data copy with the original empty template and current seed.
No mode writes a phone, a physical disk or the original restore image.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import posixpath
import re
import shutil
import stat
import struct
import subprocess
import sys

VERIFY_IMAGES = False

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / 'tools/apfs'))
import apfs
sys.path.insert(0, str(PROJECT / 'tools'))
from sparsebundle import storage_for

CORE = '''com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system
com.apple.logd com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd
com.apple.fseventsd com.apple.containermanagerd com.apple.containermanagerd.system
com.apple.runningboardd com.apple.backboardd com.apple.mobile.usermanagerd
com.apple.mobilegestalt.xpc com.apple.lsd com.apple.powerd com.apple.analyticsd
com.apple.logd_helper com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd
com.apple.SpringBoard com.apple.configd com.apple.audiomxd com.apple.wifid'''.split()
# Restore the actual 22H374 service catalog. Earlier bring-up recipes removed
# hardware consumers, PerfPowerServices and prediction daemons, and then also
# dropped event-only jobs. The user now requests those software components
# back. Keep the source job's own feature/device gates and launch triggers;
# availability of a daemon must not claim that its board backend is ready.
# Setup Assistant's UI completion policy is handled by the existing runtime,
# not by deleting its launchd provider.
EXTRA = [
    ('keybags', 0o700, 0, 0), ('logs', 0o755, 0, 0),
    ('installd', 0o700, 33, 33), ('installd/Library', 0o700, 33, 33),
    ('installd/Library/MobileInstallation', 0o700, 33, 33),
    ('installd/Library/Caches', 0o700, 33, 33),
    ('db/uuidtext', 0o755, 272, 272), ('db/diagnostics/timesync', 0o755, 272, 272),
    ('containers', 0o755, 0, 0), ('containers/Data', 0o755, 0, 0),
    ('containers/Data/System', 0o755, 0, 0),
    ('containers/Shared', 0o755, 0, 0), ('containers/Bundle', 0o755, 0, 0),
    ('containers/Shared/SystemGroup', 0o755, 0, 0),
    ('mobile/Containers', 0o755, 501, 501), ('mobile/Containers/Data', 0o755, 501, 501),
    ('mobile/Containers/Shared', 0o755, 501, 501),
    ('mobile/Library/Caches', 0o755, 501, 501), ('mobile/Library/Preferences', 0o755, 501, 501),
    ('mobile/Library/Logs', 0o755, 501, 501), ('root/Library', 0o700, 0, 0),
    ('mobile/Library/Logs/CrashReporter', 0o755, 501, 501),
    ('mobile/Library/Pong', 0o700, 501, 501),
    ('mobile/Library/Pong/PosterBoard', 0o700, 501, 501),
    ('mobile/Library/Pong/LaunchServices', 0o700, 501, 501),
    ('mobile/Library/Pong/LaunchServices/Library', 0o700, 501, 501),
    ('mobile/Library/Pong/LaunchServices/Library/Caches', 0o700, 501, 501),
    ('mobile/Library/CoreDuet/Knowledge', 0o700, 501, 501),
    ('mobile/Library/IdentityServices/PDS', 0o700, 501, 501),
    ('root/Library/Caches', 0o700, 0, 0), ('root/Library/Preferences', 0o700, 0, 0),
    ('root/Library/Logs', 0o700, 0, 0), ('db/timezone', 0o755, 0, 0),
    ('cores', 0o1775, 0, 0),
]
TRIGGERS = ('RunAtLoad', 'KeepAlive', 'StartInterval', 'StartCalendarInterval',
            'WatchPaths', 'QueueDirectories', 'StartOnMount', 'LaunchEvents')


def run(cmd, **kw):
    return subprocess.run([str(x) for x in cmd], check=True, **kw)


def sha(path):
    if not VERIFY_IMAGES: return None
    return file_sha(path)


def file_sha(path):
    """Hash one file even when whole-image verification is off.

    sha() returns None in that mode. Comparing None with None made the
    launchd guards succeed without reading the file.
    """
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()


def native_init_signature(data):
    """Reject host-default 16K signatures before a 4K Pong kernel sees init."""
    if len(data) < 32 or struct.unpack_from('<I', data)[0] != 0xfeedfacf:
        raise RuntimeError('static init must be a thin 64-bit Mach-O')
    commands, command_bytes = struct.unpack_from('<II', data, 16)
    end = 32 + command_bytes
    if end > len(data): raise RuntimeError('truncated Mach-O load commands')
    offset, signature = 32, None
    for _ in range(commands):
        if offset + 8 > end: raise RuntimeError('truncated Mach-O load command')
        command, size = struct.unpack_from('<II', data, offset)
        if size < 8 or offset + size > end: raise RuntimeError('invalid Mach-O load command')
        if command == 0x1d:
            if size != 16: raise RuntimeError('invalid code signature command')
            start, length = struct.unpack_from('<II', data, offset + 8)
            if start + length > len(data): raise RuntimeError('truncated code signature')
            signature = data[start:start + length]
        offset += size
    if signature is None or len(signature) < 12:
        raise RuntimeError('static init has no embedded signature')
    magic, length, count = struct.unpack_from('>III', signature)
    if magic != 0xfade0cc0 or length > len(signature) or 12 + count * 8 > length:
        raise RuntimeError('invalid embedded signature')
    pages = []
    for i in range(count):
        _, start = struct.unpack_from('>II', signature, 12 + i * 8)
        if start + 8 > length: raise RuntimeError('invalid signature index')
        magic, size = struct.unpack_from('>II', signature, start)
        if size < 8 or start + size > length: raise RuntimeError('truncated signature blob')
        if magic == 0xfade0c02:
            if size < 44: raise RuntimeError('truncated CodeDirectory')
            shift = signature[start + 39]
            if shift != 12:
                raise RuntimeError(f'static init signature page shift {shift}; Pong requires 12 (4096 bytes)')
            pages.append(1 << shift)
    if not pages: raise RuntimeError('static init signature lacks a CodeDirectory')
    return {'code_directories': len(pages), 'page_bytes': 4096}


def packed_digest(path):
    """Reconstruct a DBRDISK1 digest without allocating its logical disk in RAM."""
    b = path.read_bytes()
    magic, version, page, size, count = struct.unpack_from('<8sIIQQ', b)
    if (magic, version, page) != (b'DBRDISK1', 1, 4096) or count > 65536:
        raise RuntimeError('invalid PackedRAMDisk header')
    cursor, previous = 32 + 16 * count, 0
    digest, zero = hashlib.sha256(), bytes(1024 * 1024)
    def zeros(n):
        while n:
            amount = min(n, len(zero)); digest.update(zero[:amount]); n -= amount
    for i in range(count):
        offset, length = struct.unpack_from('<QQ', b, 32 + 16 * i)
        if not length or ((offset | length) & 4095) or offset < previous or offset + length > size:
            raise RuntimeError('invalid PackedRAMDisk extent')
        zeros(offset - previous)
        digest.update(b[cursor:cursor + length])
        cursor += length; previous = offset + length
    zeros(size - previous)
    if cursor != len(b): raise RuntimeError('PackedRAMDisk size mismatch')
    return digest.hexdigest()


def dump(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')


def put_plist(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(plistlib.dumps(value, fmt=plistlib.FMT_BINARY))


def adapt_launchd18(source, destination, out):
    """Enable matching cache/persona initialization tasks in development restore.

    22H374 launchd allocates the cache's shared-memory destination first; its
    boot task fills that memory. Merely restoring launchd.plist leaves zeros
    when the restore policy skips the loader. Locate __config structurally,
    Restore skips usermanagerd --init as well, leaving a new writable Data
    without its native persona manifest. Retain both original task bodies.
    """
    data = bytearray(source.read_bytes())
    if struct.unpack_from('<II', data)[0:2] != (0xfeedfacf, 0x100000c):
        raise RuntimeError('22H374 arm64 launchd required')
    commands = struct.unpack_from('<I', data, 16)[0]
    cursor, section = 32, None
    for _ in range(commands):
        command, size = struct.unpack_from('<II', data, cursor)
        if size < 8 or cursor + size > len(data): raise RuntimeError('invalid launchd load command')
        if command == 0x19:
            count = struct.unpack_from('<I', data, cursor + 64)[0]
            for i in range(count):
                start = cursor + 72 + i * 80
                if data[start:start+16].rstrip(b'\0') == b'__config' and data[start+16:start+32].rstrip(b'\0') == b'__TEXT':
                    _, length, offset = struct.unpack_from('<QQI', data, start + 32)
                    section = offset, length
        cursor += size
    if section is None: raise RuntimeError('launchd __TEXT,__config missing')
    offset, length = section
    if offset + length > len(data): raise RuntimeError('launchd config outside file')
    config = plistlib.loads(data[offset:offset + length].rstrip(b'\0'))
    loader = config.get('Boot', {}).get('launchd_cache_loader', {})
    if loader.get('Program') != '/usr/libexec/launchd_cache_loader' or not loader.get('RequireSuccess'):
        raise RuntimeError('unexpected 22H374 cache-loader boot task')
    loader['PerformInRestore'] = True
    persona_init = config.get('Boot', {}).get('usermanagerd', {})
    if persona_init.get('ProgramArguments') != ['/usr/libexec/usermanagerd', '--init'] or not persona_init.get('AllowCrash'):
        raise RuntimeError('unexpected 22H374 usermanagerd initialization task')
    persona_init['PerformInRestore'] = True
    xml = re.sub(rb'>\s+<', b'><', plistlib.dumps(config, sort_keys=False))
    if len(xml) + 1 > length: raise RuntimeError('adapted launchd config exceeds original section')
    destination.parent.mkdir(parents=True, exist_ok=True)
    data[offset:offset + length] = xml + bytes(length - len(xml))
    destination.write_bytes(data)
    # Signing is required output generation, not image verification. Keep
    # the original identifier/entitlements and generate 4K signature pages.
    run(['codesign', '--force', '--sign', '-', '--pagesize', '4096',
         '--preserve-metadata=identifier,entitlements,flags,runtime', destination])
    os.chmod(destination, 0o755)
    dump(out / 'launchd-config-update.json', dict(build='22H374',
         section='__TEXT,__config', changed_tasks=['launchd_cache_loader', 'usermanagerd'],
         PerformInRestore=True, executable_instructions_changed=False,
         cache_policy='launchd_unsecure_cache=1 development boot argument',
         signature_page_bytes=4096, image_verification='skipped by request'))


def merge_tree(source, target):
    """Merge without following an existing destination symlink."""
    if source.is_symlink():
        if os.path.lexists(target): target.unlink()
        target.symlink_to(os.readlink(source))
    elif source.is_dir():
        if target.is_symlink(): raise RuntimeError('destination directory is a link: ' + str(target))
        target.mkdir(exist_ok=True)
        for child in source.iterdir(): merge_tree(child, target / child.name)
    else:
        if os.path.lexists(target): target.unlink()
        shutil.copy2(source, target)


def source_path(root, cryptex, path, app_cryptex=None):
    """Resolve iOS absolute symlinks inside the source volumes, never the host root."""
    for _ in range(40):
        path = posixpath.normpath('/' + path.lstrip('/'))
        prefix = '/System/Cryptexes/OS'
        if path == prefix or path.startswith(prefix + '/'):
            return cryptex / path[len(prefix):].lstrip('/')
        if path == '/System/Cryptexes/App' or path.startswith('/System/Cryptexes/App/'):
            return (app_cryptex / path[len('/System/Cryptexes/App'):].lstrip('/')) if app_cryptex else None
        parts = PurePosixPath(path).parts[1:]
        for i in range(len(parts)):
            p = root.joinpath(*parts[:i + 1])
            if p.is_symlink():
                target = os.readlink(p)
                parent = '/' + '/'.join(parts[:i])
                path = posixpath.join(target if target.startswith('/') else
                                      posixpath.join(parent, target), *parts[i + 1:])
                break
        else:
            return root.joinpath(*parts)
    raise RuntimeError('symlink cycle in source: ' + path)


def cryptex_link_plan(root, cryptex):
    """Merge the restore's split userland namespace without replacing OS files.

    Some frameworks have Info.plist on OS and an out-of-cache executable on
    SystemOS. Bridging only usr/lib leaves those daemons unloadable.
    """
    links = []
    trees = ('usr/lib', 'usr/libexec', 'usr/bin', 'usr/sbin',
             'System/Library/Frameworks', 'System/Library/PrivateFrameworks')
    for tree in trees:
        source = cryptex / tree
        if not source.is_dir(): continue
        for directory, dirs, files in os.walk(source, followlinks=False):
            directory = Path(directory)
            rel_dir = directory.relative_to(cryptex)
            # A supplied OS alias already owns this subtree. Never follow an
            # iOS absolute symlink into the host filesystem while building.
            if any((root / p).is_symlink() for p in (rel_dir, *rel_dir.parents) if str(p) != '.'):
                dirs[:] = []; continue
            symbolic_dirs = [name for name in dirs if (directory / name).is_symlink()]
            dirs[:] = [name for name in dirs if name not in symbolic_dirs]
            for name in files + symbolic_dirs:
                rel = (directory / name).relative_to(cryptex)
                if os.path.lexists(root / rel): continue
                links.append((rel.as_posix(), '/private/preboot/' + rel.as_posix()))
    return links


def install_cryptex_links(root, links, numeric_owners=False):
    for rel, destination in links:
        target = root / rel
        if os.path.lexists(target): continue
        missing = []
        p = target.parent
        while p != root and not p.exists():
            missing.append(p); p = p.parent
        for p in reversed(missing):
            p.mkdir()
            if numeric_owners: os.chown(p, 0, 0); os.chmod(p, 0o755)
        target.symlink_to(destination)
        if numeric_owners: os.chown(target, 0, 0, follow_symlinks=False)


def runtime_namespace(root):
    """Install aliases for the actual Darwin24 mount contract, not macOS paths.

    Operate on the OS copy's metadata only. Do not follow target-absolute
    symlinks into the host or replace an unexpected real file/directory.
    """
    aliases = {
        'var': 'private/var',
        'System/Cryptexes/OS': '/private/preboot',
        'System/Library/Caches/com.apple.dyld':
            '/private/preboot/System/Library/Caches/com.apple.dyld',
    }
    for rel in ('private', 'private/preboot', 'private/var', 'System',
                'System/Cryptexes', 'System/Library', 'System/Library/Caches'):
        directory = root / rel
        if not directory.is_symlink() and not directory.exists():
            directory.mkdir()
            os.chown(directory, 0, 0); os.chmod(directory, 0o755)
        if directory.is_symlink() or not directory.is_dir():
            raise RuntimeError('expected real OS mount/alias parent: /' + rel)
    changed = []
    for rel, destination in aliases.items():
        alias = root / rel
        if os.path.lexists(alias):
            if not alias.is_symlink():
                raise RuntimeError('preserving unexpected non-symlink at /' + rel)
            if os.readlink(alias) != destination:
                alias.unlink(); alias.symlink_to(destination); changed.append('/' + rel)
        else:
            alias.symlink_to(destination); changed.append('/' + rel)
        os.chown(alias, 0, 0, follow_symlinks=False)
    # Hide unsupported board-specific HALs at runtime without deleting them.
    # Keep /Library/.../VirtualAudio.plugin and all media codecs untouched.
    empty_hal = root / 'pong/audio-hal18'
    if empty_hal.is_symlink():
        raise RuntimeError('unexpected HAL view symlink')
    empty_hal.mkdir(exist_ok=True)
    if any(x.name not in ('PongAudio.plugin', 'PongAudio.driver') for x in empty_hal.iterdir()):
        raise RuntimeError('unexpected board HAL in Pong output-only view')
    os.chown(empty_hal, 0, 0); os.chmod(empty_hal, 0o755)
    return dict(build='22H374', expected_mounts={
        'SystemOS': {'path': '/private/preboot', 'readonly': True},
        'Data': {'path': '/private/var', 'readonly': False}},
        os_aliases={'/' + p: t for p, t in aliases.items()}, changed=changed,
        app_cryptex=('OS-resident matching AppOS' if (root / 'System/Cryptexes/App/usr/libexec/webbookmarksd').is_file()
                     else 'not supplied'), data_modified=False)


def stage_audio_policy(root):
    """Select 22H374's real in-process endpoint manager before launchd starts."""
    directory = root / 'pong/etc'
    directory.mkdir(parents=True, exist_ok=True)
    if directory.is_symlink():
        raise RuntimeError('unexpected audio policy directory symlink')
    target = directory / 'AirPlay-local-audio.plist'
    if target.is_symlink():
        raise RuntimeError('unexpected audio policy symlink')
    # This is port configuration, not a copy of Apple's private plist.
    put_plist(target, {'airplayd': {'Enabled': False}})
    if os.geteuid() == 0:
        os.chown(target, 0, 0)
    os.chmod(target, 0o644)


def adapt_job18(job):
    """Adapt file-based 22H374 jobs to Pong's non-Exclaves boot environment."""
    changes = []
    if job.get('Label') == 'com.apple.watchdogd':
        # This phone currently has no native IOWatchdog provider. The stock
        # opt-in Mach service can launch it even with no matching device;
        # it traps and _PanicOnCrash then panics the development port.
        # Keep the binary/definition and native panic policy intact, but
        # restrict import to a real watchdog DT node until its driver exists.
        job['_LimitLoadToDeviceTree'] = ['watchdog', 'wdt']
        changes.append({'key': '_LimitLoadToDeviceTree',
                        'reason': 'requires real native watchdog node/provider; not implemented on Pong'})
    if job.get('Label') == 'com.apple.wifid':
        # The real QCA6490 service publishes through PongWifi, not Apple's
        # wlan/marconi-wifi DT nodes. Keep the native daemon and XPC ABI.
        if job.pop('_LimitLoadToDeviceTree', None) is not None:
            changes.append({'key': '_LimitLoadToDeviceTree',
                            'reason': 'Pong QCA6490 native transport replaces Apple WLAN node gating'})
    if job.get('Label') == 'com.apple.audiomxd':
        # This restore ships audiomxd instead of mediaserverd. Select its
        # real service definitions on this non-Exclaves port. Keep nested
        # AirPlay feature gates and the real provider executable intact.
        for key in ('MachServices', 'PublishesEvents'):
            value = job.get(key)
            if (isinstance(value, dict) and
                    value.get('#IfFeatureFlagEnabled') == 'CoreAudioServices/audiomxd' and
                    '#Then' in value):
                job[key] = value['#Then']
                changes.append({'key': key, 'reason': '22H374 audiomxd provider selected; no legacy mediaserverd job'})
    for key, reason in (
        ('_Conclave', 'Pong Darwin24 does not build CONFIG_EXCLAVES; this key rejects backboardd import'),
        ('HighPriorityIO', '22H374 file importer reports unknown key'),
        ('HopefullyExitsLast', '22H374 importer explicitly no longer respects this key'),
        ('NO_EnvironmentVariables', 'disabled source-template field, not a launchd environment dictionary'),
    ):
        if key in job:
            job.pop(key)
            changes.append({'key': key, 'reason': reason})
    return changes


def jobs(root, cryptex, overlay, out, app_cryptex=None):
    # Darwin 24 only permits system event publishers/host special ports from
    # its native service cache. File-only import makes even stock notifyd an
    # untrusted job, rejects com.apple.notifyd.matching, then notifyd traps.
    cache = plistlib.loads((root / 'System/Library/xpc/launchd.plist').read_bytes())
    if cache.get('VersionNumber') != 7 or not isinstance(cache.get('LaunchDaemons'), dict):
        raise RuntimeError('matching 22H374 Version 7 service cache required')
    cached_jobs = {}
    # Unlike 20G81, 22H374 rejects event publishers from ordinary file jobs.
    # Its cache lookup uses the resolved directory: the old /System -> /pong
    # symlink makes /System cache keys miss and silently falls back to files.
    dest = overlay / 'System/Library/LaunchDaemons'
    legacy = overlay / 'pong/LaunchDaemons'
    if legacy.is_dir(): shutil.rmtree(legacy)
    dest.mkdir(parents=True, exist_ok=True)
    for old in dest.glob('*.plist'): old.unlink()
    records, resources, selected = [], [], set()
    for folder in ('LaunchDaemons', 'LaunchAngels'):
        for f in sorted((root / 'System/Library' / folder).glob('*.plist')):
            job = plistlib.loads(f.read_bytes().lstrip())
            # RunningBoard reads these board memory policies directly from
            # LaunchDaemons. They are configuration resources, not jobs, and
            # must survive directory replacement without entering the cache.
            if (folder == 'LaunchDaemons' and
                    f.name.startswith('com.apple.jetsamproperties.') and
                    isinstance(job.get('Version4'), dict) and
                    not any(k in job for k in ('Label', 'Program', 'ProgramArguments'))):
                shutil.copyfile(f, dest / f.name)
                resources.append(f.relative_to(root).as_posix())
                continue
            label = job.get('Label', f.stem)
            program = job.get('Program') or (job.get('ProgramArguments') or [''])[0]
            mapped = source_path(root, cryptex, program, app_cryptex) if program else None
            core = label in CORE
            reason = None
            if not mapped or not mapped.is_file(): reason = 'program absent from supplied 22H374 OS/SystemOS/AppOS'
            elif job.get('_LimitLoadToVariant') == 'IsDarwinOS': reason = 'DarwinOS variant; this source is iOS'
            record = dict(source=f.relative_to(root).as_posix(), label=label, program=program,
                          action='excluded' if reason else ('boot' if core or job.get('RunAtLoad') or job.get('KeepAlive') is True else 'on-demand'), reason=reason)
            records.append(record)
            if reason: continue
            record['ipados18_changes'] = adapt_job18(job)
            selected.add(label)
            if core:
                job.pop('_LimitLoadToDeviceTree', None)
            # Preserve native RunAtLoad, KeepAlive, timers, path triggers and
            # LaunchEvents. Event-only providers have no MachServices and are
            # still genuine services; filtering them loses dependency chains.
            job['StandardErrorPath'] = job['StandardOutPath'] = '/dev/console'
            # Some stock descriptors carry this misspelling. launchd logs it
            # as an unknown key and otherwise loses the intended settings.
            legacy_env = job.pop('EnvironmentalVariables', None)
            if isinstance(legacy_env, dict):
                job['EnvironmentVariables'] = {**legacy_env, **job.get('EnvironmentVariables', {})}
            # A few genuine 22H374 descriptors encode an integer as a string.
            # launchd rejects that field, including chronod's retry interval.
            interval = job.get('ThrottleInterval')
            if isinstance(interval, str) and interval.isdecimal():
                job['ThrottleInterval'] = int(interval)
            if label == 'com.apple.runningboardd':
                # services32's two snapshots show both UI main threads waiting
                # for this provider's handshake. Retain its native behavior and
                # preserve the actual server error on Data, independent of pstore.
                job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
                job['StandardErrorPath'] = '/private/var/logs/PongService-runningboardd.log'
            if label == 'com.apple.PosterBoard':
                # Preserve the real database/migration error. The previous
                # generic scene assertion discards its underlying cause.
                job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
            if label == 'com.apple.backboardd':
                # Install before libtrace initializes; also survives xpcproxy.
                job.setdefault('EnvironmentVariables', {}).update({
                    'OS_ACTIVITY_DT_MODE': 'YES',
                    # Drive the display clock with this
                    # backend's real timed refresh notifications. Disable the
                    # Apple display-idle optimization unsupported by scanout.
                    'CA_FORCE_TIMER': '1', 'CA_DISABLE_DISPLAY_IDLE': '1',
                })
            if label == 'com.apple.lsd':
                # 22H374's original _LSDefaults reads this before requesting
                # its MCM container and appends Library/Caches itself. Keep
                # the real server, database/schema and XPC endpoints; use a
                # real mobile-owned directory on this development port.
                job.setdefault('EnvironmentVariables', {})['LS_DATABASE_DIR'] = \
                    '/private/var/mobile/Library/Pong/LaunchServices'
            put_plist(dest / f.name, job)
            # All selected definitions use this real canonical directory,
            # including launch angels. No /pong symlink changes cache keys.
            cached_jobs['/System/Library/LaunchDaemons/' + f.name] = job
    cache['LaunchDaemons'] = cached_jobs
    # Retain the matching 18.x extension/bundle/tree schema and feature gates.
    # The restore policy reads this local cache directly; no stale stock
    # signature is paired with this adapted, development-only cache.
    put_plist(overlay / 'System/Library/xpc/launchd.plist', cache)
    dump(out / 'jobs-report.json', {'jobs': records, 'resources': resources,
         'missing_core_labels': sorted(set(CORE) - selected),
         'service_cache': {'version': 7, 'jobs': len(cached_jobs), 'source_build': '22H374',
                           'path': '/System/Library/xpc/launchd.plist'}})
    return {kind: sum(r['action'] == kind for r in records)
            for kind in ('boot', 'on-demand', 'excluded')}


def settings_resources(root, destination):
    """Use original 22H374 localizations while omitting unported baseband panes.

    This is a resource bundle for the Settings process only. In particular,
    preserve DoNotDisturb: 22H374 replaces that entry with Focus dynamically.
    A missing bundle in the restore is not generally a reason to delete it.
    """
    stock = root / 'System/Library/PrivateFrameworks/PreferencesUI.framework'
    destination.mkdir(parents=True, exist_ok=True)
    omit = {'AIRPLANE_MODE', 'MOBILE_DATA_SETTINGS_ID', 'INTERNET_TETHERING',
            'Phone', 'EMERGENCY_SOS'}
    # This iPad restore has no Health/Compass app panes; the requested iPhone
    # identity otherwise admits their !ipad / magnetometer entries.
    for ident, bundle in [('HEALTH','HealthSettings'), ('COMPASS','CompassSettings')]:
        if not (root / 'System/Library/PreferenceBundles' / (bundle+'.bundle')).exists():
            omit.add(ident)
    removed = {}
    for item in stock.iterdir():
        target = destination / item.name
        if item.name in ('Settings.plist', 'Settings-EDU.plist'):
            value = plistlib.loads(item.read_bytes())
            removed[item.name] = [x.get('id') for x in value['items'] if x.get('id') in omit]
            value['items'] = [x for x in value['items'] if x.get('id') not in omit]
            put_plist(target, value)
        elif not os.path.lexists(target):
            target.symlink_to('/System/Library/PrivateFrameworks/PreferencesUI.framework/'+item.name)
    return removed


def wallpaper_default_configuration(root):
    """Resolve installed phone defaults using catalog metadata, not image reads."""
    base = root / 'Library/Wallpaper'
    config = base / 'DefaultWallpapers~iphone.plist'
    if not config.is_file(): return {'configuration': 'missing'}
    defaults = plistlib.loads(config.read_bytes())
    selections = {}
    for role, selection in defaults.items():
        if not isinstance(selection, dict): continue
        collection_id = selection.get('collectionIdentifier')
        wallpaper_id = selection.get('wallpaperIdentifier')
        matches = []
        for collection in (base / 'Collections').glob('*.wallpaperCollection'):
            if '~ipad' in collection.name: continue
            manifest = collection / 'WallpaperCollection.plist'
            if not manifest.is_file(): continue
            catalog = plistlib.loads(manifest.read_bytes())
            if catalog.get('identifier') != collection_id: continue
            for package in (collection / 'Wallpapers').glob('*.wallpaper'):
                if package.is_symlink() or '~ipad' in package.name: continue
                entry = package / 'Wallpaper.plist'
                if not entry.is_file(): continue
                value = plistlib.loads(entry.read_bytes())
                if value.get('identifier') != wallpaper_id: continue
                assets = []
                def references(node):
                    if isinstance(node, dict):
                        for key, item in node.items():
                            if 'FileName' in key and isinstance(item, str):
                                target = package / item
                                relative = target.relative_to(base).as_posix()
                                assets.append({'path': '/Library/Wallpaper/' + relative,
                                    'exists': target.exists(),
                                    'mode': oct(target.stat().st_mode & 0o777) if target.exists() else None})
                            else: references(item)
                    elif isinstance(node, list):
                        for item in node: references(item)
                references(value.get('assets', {}))
                matches.append({'package': '/Library/Wallpaper/' + package.relative_to(base).as_posix(),
                    'logicalScreenClass': value.get('logicalScreenClass'), 'assets': assets})
        selections[role] = {'collectionIdentifier': collection_id,
            'wallpaperIdentifier': wallpaper_id, 'installed_phone_variants': matches}
    return {'configuration': '/Library/Wallpaper/DefaultWallpapers~iphone.plist',
            'selections': selections, 'scope': 'catalog and referenced path metadata only'}


def phone_wallpaper_resources(source_root, destination_root):
    """Restore phone catalog paths used before services42.

    Keep native product filtering enabled: only the phone wrapper is eligible
    on this phone. The iPad restore remains available for the original files.
    Preserve collection IDs and paths referenced by existing Data posters.
    """
    source = source_root / 'Library/Wallpaper'
    target = destination_root / 'Library/Wallpaper'
    target.mkdir(parents=True, exist_ok=True)
    copied, wrappers, variants = [], [], []
    for item in source.rglob('*'):
        relative = item.relative_to(source)
        if '~iphone' in relative.as_posix(): continue
        dest = target / relative
        if os.path.lexists(dest): continue
        if item.is_dir(): dest.mkdir(parents=True, exist_ok=True)
        elif item.is_file():
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(item, dest); copied.append(relative.as_posix())
    for original in (source / 'Collections').glob('*~ipad.wallpaperCollection'):
        alias = target / 'Collections' / original.name.replace('~ipad', '~iphone')
        if alias.is_symlink(): raise RuntimeError('unexpected wallpaper catalog symlink')
        if not alias.exists():
            shutil.copytree(original, alias); wrappers.append(alias.name)
        for manifest in list((alias / 'Wallpapers').rglob('Wallpaper.plist')):
            if manifest.parent.is_symlink():continue
            value=plistlib.loads(manifest.read_bytes())
            # Renaming only the outer collection left every actual variant
            # tagged ~ipad in its directory name. WallpaperKit filters these
            # before reading the plist. Pong is 1080x2412 pixels / scale 3.
            screen_class='360w-804h@3x~iphone'
            package=manifest.parent
            source_name=original/'Wallpapers'/package.name/'Wallpaper.plist'
            source_value=plistlib.loads(source_name.read_bytes()) if source_name.is_file() else {}
            source_class=source_value.get('logicalScreenClass','')
            if '~ipad' in source_class:
                destination=package.with_name(package.name.replace(source_class,screen_class))
                if destination.exists():raise RuntimeError('duplicate Pong wallpaper variant')
                package.rename(destination)
                # Keep old serialized poster URLs usable without a second
                # copied variant. Native phone scanning skips the ~ipad name.
                package.symlink_to(destination.name,target_is_directory=True)
                manifest=destination/'Wallpaper.plist'
            variants.append(str(manifest.parent.relative_to(target)))
            value['logicalScreenClass']=screen_class
            put_plist(manifest,value)
    for name in ('DefaultWallpapers', 'Wallpapers'):
        original = source / (name + '~ipad.plist')
        if not original.is_file(): continue
        value = plistlib.loads(original.read_bytes())
        if name == 'Wallpapers':
            value = [item for item in value if not item.get('path') or
                     (source / item['path']).exists()]
        put_plist(target / (name + '~iphone.plist'), value)
        # WallpaperKit 22H374 names these using GSDeviceName, then falls
        # back to the unsuffixed manifest. A phone identity adapted from an
        # iPad restore must not fall through to an absent Collections.plist.
        put_plist(target / (name + '.plist'), value)
    collection_list=source/'Collections/Collections~ipad.plist'
    available_collections=set()
    for manifest in (target/'Collections').glob('*.wallpaperCollection/WallpaperCollection.plist'):
        entry=plistlib.loads(manifest.read_bytes())
        if entry.get('identifier'):available_collections.add(entry['identifier'])
    if collection_list.is_file():
        value=plistlib.loads(collection_list.read_bytes())
        # Restore catalogs also name MobileAsset collections that are absent
        # from this IPSW. Match iOS16's existing-resource policy: preserve the
        # actual packages and IDs, without offering missing downloadable ones.
        value['order']=[key for key in value.get('order',[]) if key in available_collections]
        put_plist(target/'Collections/Collections~iphone.plist',value)
        put_plist(target/'Collections/Collections.plist',value)
    # All catalog/animation files must be readable by the real mobile
    # render extensions; no container or world-writable resource shortcuts.
    for item in (target,*target.rglob('*')):
        if item.is_symlink():continue
        os.chmod(item,0o755 if item.is_dir() else 0o644)
        if os.geteuid()==0:os.chown(item,0,0)
    return dict(restored_files=copied, restored_phone_catalogs=wrappers, phone_variants=variants,
                panel_pixels=[1080,2412], logical_points=[360,804], scale=3,
                source_build='22H374', source_artwork_preserved=True,
                collection_ids=sorted(available_collections),
                default_configuration=wallpaper_default_configuration(destination_root),
                selection='native product filter; installed catalog IDs and existing paths retained')


def data_seed(image, root, overlay, out):
    data = out / 'data-root'
    data.mkdir()
    c = apfs.Container(str(image))
    v = apfs.pick_volume(c, None)
    metadata = {}
    oid = v.lookup('/private/var')[0]

    def extract(oid, rel):
        if rel.split('/')[0] == 'staged_system_apps': return
        # Restore moves apps onto OS separately. Preinstalled MobileAssets
        # are genuine OS resources, however, and must survive the Data mount.
        # Do not import unrelated download/cache state from the restore.
        if (rel.startswith('MobileAsset/') and
                rel != 'MobileAsset/PreinstalledAssetsV2' and
                not rel.startswith('MobileAsset/PreinstalledAssetsV2/')): return
        ino = v.inode(oid)
        dest = data / rel
        mode = ino.mode & 0o7777
        metadata[rel] = [ino.mode, ino.owner, ino.group]
        if ino.is_dir:
            dest.mkdir(parents=True, exist_ok=True)
            for name, child, _ in v.dir_entries(oid): extract(child, posixpath.join(rel, name))
        elif ino.is_link:
            dest.symlink_to(os.fsdecode(ino.symlink_target_bytes()))
        elif ino.is_reg:
            # macOS transparently reads all decmpfs compression types shipped by 22H374.
            shutil.copyfile(root / 'private/var' / rel, dest)
        else:
            metadata.pop(rel)
            return
        if not ino.is_link: os.chmod(dest, mode)

    extract(oid, '')
    c.close()
    for rel, mode, uid, gid in EXTRA:
        # Ensure every parent exists; do not replace template owner/mode values.
        parts = PurePosixPath(rel).parts
        for i in range(1, len(parts) + 1):
            p = '/'.join(parts[:i]); (data / p).mkdir(exist_ok=True)
            # The target is also visited by this loop. Do not seed it as
            # 0755 before trying to set its requested (e.g. keybags 0700)
            # mode with setdefault. Preserve metadata from the restore.
            metadata.setdefault(p, [stat.S_IFDIR | (mode if i == len(parts) else 0o755), uid, gid])
    for name in ('com.apple.purplebuddy.plist', 'com.apple.purplebuddy.notbackedup.plist',
                 'com.apple.springboard.plist', 'com.apple.migration.plist'):
        rel = 'mobile/Library/Preferences/' + name
        source = plistlib.loads((PROJECT / 'userland/var' / rel).read_bytes())
        target = data / rel
        # Merge the established iOS 16 setup bypass into the matching 22H374
        # preference domain. Preserve unrelated restore preferences.
        value = plistlib.loads(target.read_bytes()) if target.exists() else {}
        value.update(source)
        if name == 'com.apple.migration.plist':
            value['LastSystemVersion'] = '22H374'
            value['DMLastMigrationResults']['buildVersion'] = '22H374'
        if name == 'com.apple.purplebuddy.plist':
            value.pop('SetupState', None)
            value.update(SetupDone=True, SetupFinishedAllSteps=True, SetupVersion=11,
                         ForceNoBuddy=True, daemonizeNeedsToRun=False)
        put_plist(target, value)
        metadata[rel] = [stat.S_IFREG | 0o600, 501, 501]
    for owner, uid, gid in [('mobile',501,501),('root',0,0)]:
        rel = owner + '/Library/Preferences/.GlobalPreferences.plist'
        path = data / rel
        value = plistlib.loads(path.read_bytes()) if path.exists() else {}
        value.update(AppleLanguages=['zh-Hans-CN'], AppleLocale='zh_CN')
        path.parent.mkdir(parents=True, exist_ok=True)
        put_plist(path, value)
        metadata[rel] = [stat.S_IFREG | 0o644, uid, gid]
    settings = data / 'pong/PreferencesUI.framework'
    settings.mkdir(parents=True,exist_ok=True)
    removed = {}
    dump(out / 'settings-resources.json', removed)
    for p in [data/'pong', settings, *settings.iterdir()]:
        rel = str(p.relative_to(data))
        kind = stat.S_IFLNK if p.is_symlink() else stat.S_IFDIR if p.is_dir() else stat.S_IFREG
        metadata[rel] = [kind | (0o755 if kind != stat.S_IFREG else 0o644), 0, 0]
    link = 'db/timezone/zoneinfo'
    # /usr/share/zoneinfo points into /var on 22H374. Repeating that target
    # here makes a cycle before timed starts; HackiOS uses the shipped data.
    if (data / link).is_symlink() and os.readlink(data / link) == '/usr/share/zoneinfo':
        (data / link).unlink()
    if not os.path.lexists(data / link): (data / link).symlink_to('/usr/share/zoneinfo.default')
    metadata[link] = [stat.S_IFLNK | 0o755, 0, 0]
    for rel in ('db/diagnostics/.migrated', 'db/diagnostics/timesync/.migrated', 'db/uuidtext/.migrated'):
        (data / rel).touch(); metadata[rel] = [stat.S_IFREG | 0o644, 272, 272]
    (data / '.pong-data-22H374').write_text('Pong development Data seed for 18.7.10 / 22H374\n')
    metadata['.pong-data-22H374'] = [stat.S_IFREG | 0o644, 0, 0]
    for rel, (mode, _, _) in metadata.items():
        if not stat.S_ISLNK(mode): os.chmod(data / rel, mode & 0o7777)
    dump(out / 'data-metadata.json', metadata)
    # Init only creates absent entries, preserving an existing Data volume's contents.
    seed = overlay / 'pong/var-seed'
    shutil.copytree(data, seed, symlinks=True)
    lines = []
    for rel, (mode, uid, gid) in sorted(metadata.items()):
        path = '/private/var' + ('/' + rel if rel else '')
        if stat.S_ISDIR(mode) and rel:
            lines.append(f'd\t{mode & 0o7777:o}\t{uid}\t{gid}\t{path}')
    for rel, (mode, uid, gid) in sorted(metadata.items()):
        path = '/private/var/' + rel
        if stat.S_ISLNK(mode): lines.append(f'l\t{os.readlink(data / rel)}\t{path}')
        elif stat.S_ISREG(mode):
            lines.append(f'f\t{mode & 0o7777:o}\t{uid}\t{gid}\t{path}\t/pong/var-seed/{rel}')
    skeleton = '\n'.join(lines) + '\n'
    if len(skeleton.encode()) >= 65536: raise RuntimeError('var.skel exceeds init buffer')
    (overlay / 'pong/etc/var.skel').write_text(skeleton)
    return dict(entries=len(metadata), skeleton_bytes=len(skeleton.encode()),
                template='22H374 native APFS inode owners/modes; staged apps remain on OS')


def compile_init(out, developer, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DEVELOPER_DIR=str(developer))
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], env=env, text=True).strip()
    cmd = ['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64-apple-ios18.0',
           '-isysroot', sdk, '-Os', '-static', '-nostdlib', '-ffreestanding',
           '-fno-stack-protector', '-fno-stack-check', '-fno-builtin', '-Wall', '-Wextra', '-Werror',
           '-I' + str(PROJECT / 'userland/libmini'), '-DPONG_SYSTEM_VERSION="18.7.10"', '-DPONG_SYSTEM_BUILD="22H374"',
           '-DPONG_NATIVE_APFS18=1',
           '-DPONG_UI_LIBRARY_PATH="/pong/libpong-ui18.dylib"',
           '-DPONG_UI_EXTRA_PROCESSES=" usermanagerd lsd installd pkd runningboardd PosterBoard"', '-Wl,-e,_start',
           '-Wl,-pagezero_size,0x100000000', '-Wl,-no_fixup_chains',
           str(PROJECT / 'userland/libmini/start.S'), str(PROJECT / 'userland/libmini/mini.c'),
           str(PROJECT / 'userland/init.c'), '-o', str(destination)]
    run(cmd, env=env)
    # codesign defaults to 16K for arm64, independently of the kernel ABI.
    run(['codesign', '--force', '--sign', '-', '--pagesize', '4096',
         '--entitlements', PROJECT / 'userland/init-entitlements.plist', destination])
    if VERIFY_IMAGES:
        run(['codesign', '--verify', '--strict', destination])
        init_signature = native_init_signature(destination.read_bytes())
    else:
        init_signature = {'page_bytes': 4096, 'verification': 'skipped by request'}
    os.chmod(destination, 0o755)
    (out / 'compile-command.json').write_text(json.dumps(cmd, indent=2) + '\n')
    return init_signature


def compile_ui(out, developer):
    """Build the 22H374 adapter; no prebuilt 20F66 addresses/libraries."""
    env = dict(os.environ, DEVELOPER_DIR=str(developer))
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], env=env, text=True).strip()
    destination = out / 'libpong-ui.dylib'
    cmd = ['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64-apple-ios18.0',
           '-isysroot', sdk, '-Os', '-fblocks', '-dynamiclib', '-Wall', '-Wextra', '-Werror',
           '-Wl,-no_fixup_chains', '-install_name', '/pong/libpong-ui18.dylib',
           '-framework', 'CoreFoundation', '-framework', 'CoreGraphics', '-lobjc',
           str(PROJECT / 'userland/ui_compat18.c'),
           str(PROJECT / 'userland/settings_compat18.c'),
           str(PROJECT / 'userland/boot_snapshot18.c'),
           str(PROJECT / 'userland/touch.c'),
           '-o', str(destination)]
    run(cmd, env=env)
    run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', destination])
    (out / 'ui-compile-command.json').write_text(json.dumps(cmd, indent=2) + '\n')
    return destination


def compile_audio(out, developer):
    """Build the Pong output-only AudioServerPlugIn against public SDK headers."""
    env=dict(os.environ,DEVELOPER_DIR=str(developer))
    sdk=Path(subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],env=env,text=True).strip())
    mac=Path(subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],env=env,text=True).strip())
    # AudioServerPlugIn is shipped in 22H374's CoreAudio. Its public C ABI
    # headers are distributed in the macOS SDK; use only those two headers,
    # keeping every library, platform target and remaining header on iPhoneOS.
    includes=out/'audio-sdk-headers/CoreAudio';includes.mkdir(parents=True,exist_ok=True)
    for name in ('AudioServerPlugIn.h','AudioHardwareBase.h'):
        shutil.copyfile(mac/'System/Library/Frameworks/CoreAudio.framework/Headers'/name,includes/name)
    # 22H374 HALS_PlugInManager::LoadPlugIns enumerates .driver bundles.
    # .plugin is the legacy AHP namespace (e.g. VirtualAudio), whose factory
    # and interface UUIDs differ from AudioServerPlugIn's public C ABI.
    bundle=out/'PongAudio.driver';bundle.mkdir(exist_ok=True)
    target=bundle/'PongAudio'
    cmd=['xcrun','clang','-target','arm64-apple-ios18.0','-isysroot',str(sdk),
        '-Os','-fblocks','-bundle','-fvisibility=hidden','-Wall','-Wextra','-Werror',
        '-Wl,-no_fixup_chains',
        '-I'+str(includes.parent),'-I'+str(PROJECT/'drivers/PongPower'),
        '-framework','CoreFoundation','-framework','IOKit',
        str(PROJECT/'userland/PongAudioPlugIn.c'),'-o',str(target)]
    run(cmd,env=env)
    factory='341D03C1-9CF6-4163-A3E9-FA3AC191C764'
    put_plist(bundle/'Info.plist',dict(CFBundleDevelopmentRegion='en',CFBundleExecutable='PongAudio',
        CFBundleIdentifier='org.unixnu.pong.audio',CFBundleInfoDictionaryVersion='6.0',
        CFBundleName='PongAudio',CFBundlePackageType='BNDL',CFBundleShortVersionString='1.0',
        CFBundleVersion='1',CFBundleSupportedPlatforms=['iPhoneOS'],MinimumOSVersion='18.0',
        CFPlugInFactories={factory:'PongAudio_Create'},
        CFPlugInTypes={'443ABAB8-E7B3-491A-B985-BEB9187030DB':[factory]},
        AudioServerPlugIn_IOKitUserClients=['PongAudioUserClient']))
    run(['codesign','--force','--sign','-','--pagesize','4096',bundle])
    dump(out/'audio-compile-command.json',cmd)
    return bundle


def compile_sleep(out, developer):
    """22H374 local Sleep configuration service; uses shipped XPC interfaces."""
    env = dict(os.environ, DEVELOPER_DIR=str(developer))
    destination = out / 'sleepserver'
    cmd = ['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64-apple-ios18.0',
           '-Os', '-fobjc-arc', '-fblocks', '-Wall', '-Wextra', '-Werror',
           '-Wl,-no_fixup_chains', '-framework', 'Foundation',
           PROJECT / 'userland/sleepserver.m', '-o', destination]
    run(cmd, env=env)
    run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', destination])
    (out / 'sleep-compile-command.json').write_text(json.dumps([str(x) for x in cmd], indent=2) + '\n')
    put_plist(out / 'com.apple.sleepd.plist', dict(Label='com.apple.sleepd',
        ProgramArguments=['/private/var/pong/sleepserver'], UserName='mobile',
        MachServices={'com.apple.sleepd.sleepserver': True}, RunAtLoad=True,
        ThrottleInterval=30, StandardOutPath='/dev/console', StandardErrorPath='/dev/console',
        POSIXSpawnType='Interactive'))
    return destination


def helper(a):
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    signature = compile_init(out, a.developer, out / 'init')
    ui_library = compile_ui(out, a.developer)
    sleep_service = compile_sleep(out, a.developer)
    audio_plugin = compile_audio(out, a.developer)
    preference_seed = out / 'var-seed/mobile/Library/Preferences'
    preference_seed.mkdir(parents=True, exist_ok=True)
    for name in ('com.apple.purplebuddy.plist', 'com.apple.purplebuddy.notbackedup.plist',
                 'com.apple.springboard.plist'):
        source = PROJECT / 'userland/var/mobile/Library/Preferences' / name
        shutil.copyfile(source, preference_seed / name)
    dump(out / 'helper.json', dict(helper=str(out / 'init'), runtime_path='/pong/init',
        signature=signature, required_kernel=['pong_init_path',
        'native writable Data mount', 'DEVELOPMENT PONG arm64 4K'],
        ui_library=str(ui_library),
        ui_library_runtime='/pong/libpong-ui18.dylib',
        preference_seed=str(preference_seed),
        audio_plugin=str(audio_plugin), audio_scope='physical stereo output; no microphone',
        sleep_service=str(sleep_service), sleep_scope='local configuration; no alarm/Focus/Health/Watch backend',
        image_verification='skipped' if not VERIFY_IMAGES else 'requested'))
    print(out / 'init')


def prepare(a):
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    root, cryptex = a.os_root.resolve(), a.system_cryptex.resolve()
    version = plistlib.loads((root / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if (version['ProductVersion'], version['ProductBuildVersion']) != ('18.7.10', '22H374'):
        raise RuntimeError('this adapter requires the actual 18.7.10 / 22H374 root')
    cv = plistlib.loads((cryptex / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if (cv['ProductVersion'], cv['ProductBuildVersion']) != ('18.7.10', '22H374'):
        raise RuntimeError('SystemCryptex must also be 18.7.10 / 22H374; refusing mixed builds')
    overlay = out / 'overlay'
    (overlay / 'pong/etc').mkdir(parents=True, exist_ok=True)
    stage_audio_policy(overlay)
    (overlay / 'pong/LICENSE-HackiOS').write_bytes((PROJECT / 'userland/LICENSE-HackiOS').read_bytes())
    (overlay / 'sbin').mkdir(exist_ok=True)
    init_signature = compile_init(out, a.developer, overlay / 'sbin/launchd')
    counts = jobs(root, cryptex, overlay, out, a.app_cryptex)
    if a.app_cryptex:
        install_app_cryptex(a.app_cryptex, overlay, numeric_owners=False)
    dump(out / 'phone-artwork.json', phone_wallpaper_resources(root, overlay))
    seed = data_seed(a.os_image, root, overlay, out)
    ui=compile_ui(out,a.developer)
    shutil.copyfile(overlay/'sbin/launchd',overlay/'pong/init')
    shutil.copyfile(ui,overlay/'pong/libpong-ui18.dylib')
    data=out/'data-root/pong';data.mkdir(exist_ok=True)
    shutil.copyfile(ui,data/'libpong-ui.dylib')
    shutil.copyfile(overlay/'sbin/launchd',data/'init')
    metadata=json.loads((out/'data-metadata.json').read_text())
    for name in ('libpong-ui.dylib','init'):metadata['pong/'+name]=[stat.S_IFREG|0o755,0,0]
    dump(out/'data-metadata.json',metadata)
    links = cryptex_link_plan(root, cryptex)
    install_cryptex_links(overlay, links)
    dump(out / 'cryptex-links.json', dict(build='22H374', links=links,
         count=len(links), existing_OS_files_preserved=True))
    required = ['sbin/launchd', 'usr/lib/dyld', 'System/Library/CoreServices/SystemVersion.plist']
    hashes = {p: file_sha(root / p) for p in required}
    cache = cryptex / 'System/Library/Caches/com.apple.dyld'
    cache_files = sorted(p for p in cache.glob('dyld_shared_cache_arm64*') if p.is_file())
    if len(cache_files) < 2: raise RuntimeError('matching split arm64 shared cache missing')
    # Record all matching cache pieces; do not copy or replace any with 20F66 files.
    cache_info = {p.name: {'size': p.stat().st_size, 'sha256': sha(p)} for p in cache_files}
    state = dict(format='pong-hackios-userland-22H374-v1', version=version, cryptex_version=cv,
                 os_source=str(a.os_image.resolve()), os_root=str(root), system_cryptex=str(cryptex),
                 data_source=str(a.data_image.resolve()), apple_sha256=hashes, cache=cache_info,
                 jobs=counts, data=seed, static_init_signature=init_signature,
                 disabled=['20F66 fixed-address injection', 'Redwood sysctls',
                 'HackiOS ramfs / custom APFS ABI', 'hardware firmware and GPU libraries'],
                 system_cryptex_runtime='/private/preboot',
                 app_cryptex=str(a.app_cryptex.resolve()) if a.app_cryptex else None,
                 kernel_prerequisites=['native APFS 22H374', 'SystemOS mounted at /private/preboot',
                 'skipIgnition dyld_flags=0x8', 'a writable Data device or pre-mounted /private/var'],
                 phone_written=False)
    dump(out / 'build-plan.json', state)
    for source, name in ((a.os_image, 'pong-ipados18-system.cdr'), (a.data_image, 'pong-ipados18-data.cdr')):
        dest = out / name
        if not dest.exists():
            if source.suffix == '.sparseimage':
                run(['hdiutil', 'convert', source, '-format', 'UDRW', '-o', dest])
                appended=dest.with_name(dest.name+'.dmg')
                if not dest.exists() and appended.exists():appended.rename(dest)
            else: shutil.copyfile(source, dest)
        if dest.samefile(source): raise RuntimeError('destination must be an independent image')
    print(json.dumps({'prepared': str(out), 'jobs': counts, 'data': seed}, indent=2))


def os_copy(out, updating=True):
    """Use the tracked writable OS while retaining the last raw base."""
    raw = out / 'pong-ipados18-system.cdr'
    tracked = storage_for(raw)
    if tracked and updating and not tracked.get('pending'):
        raise RuntimeError('begin a tracked OS update before editing the installed generation')
    return Path(tracked['image']) if tracked else raw


def attach(image, mountpoint):
    mountpoint.mkdir(exist_ok=True)
    result = run(['hdiutil', 'attach', '-readwrite', '-nobrowse', '-noverify', '-owners', 'on',
                  '-mountpoint', mountpoint, '-plist', image], capture_output=True)
    data = plistlib.loads(result.stdout)
    devices = data['system-entities']
    whole = next(d['dev-entry'] for d in devices if d.get('dev-entry'))
    mounted = next(d for d in devices if d.get('mount-point') == str(mountpoint))
    if not mounted.get('dev-entry'): raise RuntimeError('image mount was not returned')
    return whole


def apply(a):
    if os.geteuid() != 0: raise RuntimeError('apply requires macOS administrator authorization')
    out = a.out.resolve(); plan = json.loads((out / 'build-plan.json').read_text())
    if plan['format'] != 'pong-hackios-userland-22H374-v1': raise RuntimeError('wrong build plan')
    if (out / 'apply-complete.json').exists(): raise RuntimeError('images already modified; use a new output directory')
    for name, source in (('pong-ipados18-system.cdr', plan['os_source']),
                         ('pong-ipados18-data.cdr', plan['data_source'])):
        target = out / name
        if not target.is_file() or target.is_symlink() or target.samefile(source):
            raise RuntimeError('refusing non-copy target: ' + str(target))
    root = out / 'system-mount'
    device = attach(out / 'pong-ipados18-system.cdr', root)
    try:
        if file_sha(root / 'sbin/launchd') != plan['apple_sha256']['sbin/launchd']:
            raise RuntimeError('launchd does not match source 22H374')
        stock = root / 'pong/stock-config'
        stock.mkdir(parents=True)
        for rel in ('System/Library/LaunchDaemons', 'System/Library/NanoLaunchDaemons',
                    'System/Library/LaunchAngels', 'System/Library/xpc/launchd.plist',
                    'System/Library/xpc/launchd.plist.sig'):
            p = root / rel
            if os.path.lexists(p): shutil.move(str(p), str(stock / p.name))
        (root / 'System/Library/LaunchDaemons').mkdir()
        (root / 'sbin/launchd').rename(root / 'sbin/launchd.ios')
        shutil.copyfile(root / 'sbin/launchd.ios', stock / 'launchd.original')
        adapt_launchd18(stock / 'launchd.original', root / 'sbin/launchd.ios', out)
        merge_tree(out / 'overlay', root)
        # The generated overlay may live on a host filesystem without Unix
        # owners/modes. Canonical system jobs must have target-side metadata.
        job_dir = root / 'System/Library/LaunchDaemons'
        os.chown(job_dir, 0, 0); os.chmod(job_dir, 0o755)
        for p in list(job_dir.glob('*.plist')) + [root / 'System/Library/xpc/launchd.plist']:
            os.chown(p, 0, 0); os.chmod(p, 0o644)
        for rel in ('pong', 'sbin/launchd'):
            base = root / rel
            paths = [base] + list(base.rglob('*')) if base.is_dir() else [base]
            for p in paths:
                os.chown(p, 0, 0, follow_symlinks=False)
        for rel in ('pong/init', 'pong/libpong-ui18.dylib', 'sbin/launchd'):
            os.chmod(root / rel, 0o755)
        link = root / 'System/Cryptexes/OS'
        original_link = os.readlink(link) if link.is_symlink() else None
        dump(out / 'runtime-namespace.json', runtime_namespace(root))
        # Restore normally moves these applications out of the Data template.
        apps = root / 'private/var/staged_system_apps'
        moved = []
        for p in sorted(apps.glob('*.app')):
            dest = root / 'Applications' / p.name
            if os.path.lexists(dest): raise RuntimeError('app already exists: ' + p.name)
            p.rename(dest); moved.append(p.name)
        if file_sha(stock / 'launchd.original') != plan['apple_sha256']['sbin/launchd']:
            raise RuntimeError('original launchd changed')
    finally:
        run(['hdiutil', 'detach', device])
    data = out / 'data-mount'
    device = attach(out / 'pong-ipados18-data.cdr', data)
    try:
        merge_tree(out / 'data-root', data)
        metadata = json.loads((out / 'data-metadata.json').read_text())
        for rel, (mode, uid, gid) in metadata.items():
            p = data / rel
            os.chown(p, uid, gid, follow_symlinks=False)
            if not stat.S_ISLNK(mode): os.chmod(p, mode & 0o7777)
    finally:
        run(['hdiutil', 'detach', device])
    dump(out / 'apply-complete.json', {'original_cryptex_link': original_link,
                                      'relocated_22H374_apps': moved, 'phone_written': False})
    print('Applied userland changes to independent OS and Data image copies.')


def refresh_cryptex(a):
    """Repair split-framework links on the prepared OS copy, retaining Data."""
    if os.geteuid() != 0: raise RuntimeError('refresh_cryptex requires system authentication')
    out = a.out.resolve()
    version = plistlib.loads((a.system_cryptex / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if version.get('ProductBuildVersion') != '22H374': raise RuntimeError('matching 22H374 SystemOS required')
    plan = json.loads((out / 'build-plan.json').read_text())
    image = os_copy(out)
    if image.is_symlink() or image.samefile(plan['os_source']): raise RuntimeError('prepared OS copy required')
    root = out / 'system-mount'; device = attach(image, root)
    try:
        version = plistlib.loads((root / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
        if version.get('ProductBuildVersion') != '22H374': raise RuntimeError('matching 22H374 OS required')
        namespace = runtime_namespace(root)
        links = cryptex_link_plan(root, a.system_cryptex)
        install_cryptex_links(root, links, numeric_owners=True)
    finally:
        run(['hdiutil', 'detach', device])
    dump(out / 'cryptex-links-update.json', dict(build='22H374', count=len(links),
        links=links, existing_OS_files_preserved=True, data_modified=False,
        image_verification='skipped by request'))
    dump(out / 'runtime-namespace.json', namespace)
    print(f'Installed {len(links)} missing matching SystemOS file links in OS')


def rebuild_data(a):
    """Recreate a local Data seed; never read or modify the phone here."""
    if os.geteuid() != 0: raise RuntimeError('rebuild_data requires system authentication')
    out = a.out.resolve()
    plan = json.loads((out / 'build-plan.json').read_text())
    if plan.get('format') != 'pong-hackios-userland-22H374-v1':
        raise RuntimeError('matching 22H374 prepared userland required')
    source = Path(plan['data_source']).resolve()
    target = out / 'pong-ipados18-data.cdr'
    if target.is_symlink() or source == target or (target.exists() and target.samefile(source)):
        raise RuntimeError('independent Data copy required')
    candidate = out / 'pong-ipados18-data-rebuild.cdr'
    if candidate.exists(): raise RuntimeError('unfinished Data candidate already exists')
    # UDTO emits the actual disk bytes, including GPT and the APFS container;
    # the sparseimage storage file itself is not a device-ready disk image.
    run(['hdiutil', 'convert', source, '-format', 'UDTO', '-o', candidate])
    expected = target.stat().st_size if target.exists() else 8 * 1024**3
    if candidate.stat().st_size != expected:
        raise RuntimeError('Data disk geometry changed; retaining both local files')
    data = out / 'data-mount'
    device = attach(candidate, data)
    try:
        merge_tree(out / 'data-root', data)
        metadata = json.loads((out / 'data-metadata.json').read_text())
        for rel, (mode, uid, gid) in metadata.items():
            p = data / rel
            os.chown(p, uid, gid, follow_symlinks=False)
            if not stat.S_ISLNK(mode): os.chmod(p, mode & 0o7777)
        # Port-owned seed directories use the current explicit policy even if
        # this prepared tree predates it. This affects only the fresh copy.
        for rel, mode, uid, gid in EXTRA:
            p = data / rel
            p.mkdir(parents=True, exist_ok=True)
            os.chown(p, uid, gid); os.chmod(p, mode)
    finally:
        run(['hdiutil', 'detach', device])
    os.replace(candidate, target)
    dump(out / 'data-rebuild.json', dict(build='22H374', source=str(source),
        target=str(target), bytes=target.stat().st_size,
        seed='original APFS template plus prepared 22H374 Data files',
        image_verification='skipped by request', phone_written=False))
    print('Rebuilt local 22H374 APFS Data seed; phone unchanged.')


def reset_data(a):
    """Explicit reset of the port-owned local copy, before a phone Data install.

    Recreate from the original host-created empty APFS template. Do not rely
    on mounting or repairing the old copy. The phone remains unchanged until
    install-ipados18-ufs.py --windows Data. Releasing the replaceable local
    copy first avoids requiring another 8 GiB of temporary host storage.
    """
    if os.geteuid() != 0: raise RuntimeError('reset_data requires system authentication')
    out = a.out.resolve()
    plan = json.loads((out / 'build-plan.json').read_text())
    if plan['version'].get('ProductBuildVersion') != '22H374':
        raise RuntimeError('matching 22H374 prepared Data required')
    target = out / 'pong-ipados18-data.cdr'
    original = Path(plan['data_source']).resolve()
    if target.is_symlink() or target.resolve() == original or (target.exists() and target.samefile(original)):
        raise RuntimeError('only the port-owned Data copy may be cleared')
    size = target.stat().st_size if target.exists() else 8 * 1024**3
    if size != 8 * 1024**3 or not original.is_file():
        raise RuntimeError('expected current 8 GiB Data and original local template')
    seed = out / 'data-root'
    metadata = json.loads((out / 'data-metadata.json').read_text())
    if not (seed / 'mobile').is_dir() or not (seed / 'db').is_dir() or not metadata:
        raise RuntimeError('prepared Data seed is absent')
    if os.path.ismount(out / 'data-mount'): raise RuntimeError('detach local Data before resetting it')
    target.unlink(missing_ok=True)
    rebuild_data(a)
    dump(out / 'data-reset.json', dict(build='22H374', target=str(target), bytes=size,
        operation='explicit local Data reset from original empty APFS template',
        seed='prepared 22H374 files and current directory ownership policy',
        image_verification='skipped by request', phone_written=False))
    print('Reset local Data contents; phone is unchanged until --windows Data install.')


def pack_data(data_path, output):
    # DBRDISK1 is the existing DarwinBoot/NativeRamBlock sparse input format.
    extents = []
    with data_path.open('rb') as f:
        for index in range(data_path.stat().st_size // 4096):
            b = f.read(4096)
            if any(b):
                if extents and extents[-1][0] + len(extents[-1][1]) == index * 4096:
                    extents[-1][1].extend(b)
                else:
                    extents.append((index * 4096, bytearray(b)))
    if len(extents) > 65536: raise RuntimeError('too many DBRDISK1 extents')
    packed = output
    with packed.open('wb') as f:
        f.write(struct.pack('<8sIIQQ', b'DBRDISK1', 1, 4096, data_path.stat().st_size, len(extents)))
        for offset, b in extents: f.write(struct.pack('<QQ', offset, len(b)))
        for _, b in extents: f.write(b)
    return packed


def install_app_cryptex(source, root, numeric_owners=True):
    """Expose the matching AppOS files at their guest cryptex namespace.

    AppOS is a separate restore component. Its files were already downloaded
    but the earlier recipe treated every App executable as missing. Keep the
    original directory layout and symlinks; never resolve guest links on macOS.
    """
    source = source.resolve()
    if not all((source / path).is_file() for path in (
            'usr/libexec/webbookmarksd', 'usr/libexec/AuthenticationServicesAgent',
            'System/Library/CoreServices/SafariSupport.bundle/SafariBookmarksSyncAgent')):
        raise RuntimeError('matching complete 22H374 AppOS mount required')
    target = root / 'System/Cryptexes/App'
    if target.is_symlink():
        # The restore alias points into a separately mounted App cryptex.
        # Pong mounts SystemOS at /private/preboot; AppOS is OS-resident in
        # this port, so replace only this known dangling restore mount alias.
        if os.readlink(target) not in ('../../private/preboot/Cryptexes/App',
                                      '/private/preboot/Cryptexes/App'):
            raise RuntimeError('unexpected App cryptex alias')
        target.unlink()
    target.mkdir(parents=True, exist_ok=True)
    for child in source.iterdir():
        if child.name.startswith('.'): continue
        merge_tree(child, target / child.name)
    if numeric_owners:
        for directory, dirs, files in os.walk(target, followlinks=False):
            for item in [Path(directory), *(Path(directory) / p for p in dirs + files)]:
                os.chown(item, 0, 0, follow_symlinks=False)


def refresh_jobs(a):
    """Update only the prepared 22H374 OS job definitions; retain device Data."""
    if os.geteuid() != 0: raise RuntimeError('refresh_jobs requires system authentication')
    out = a.out.resolve()
    plan = json.loads((out / 'build-plan.json').read_text())
    previous_app = plan.get('app_cryptex')
    if a.app_cryptex is None and previous_app and previous_app != 'not supplied':
        a.app_cryptex = Path(previous_app)
    version = plistlib.loads((a.os_root / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if version.get('ProductBuildVersion') != '22H374': raise RuntimeError('22H374 source jobs required')
    counts = jobs(a.os_root, a.system_cryptex, out / 'overlay', out, a.app_cryptex)
    adapt_launchd18(a.os_root / 'sbin/launchd', out / 'launchd.ios', out)
    image = os_copy(out)
    if image.is_symlink() or image.samefile(plan['os_source']): raise RuntimeError('prepared OS copy required')
    root = out / 'system-mount'; device = attach(image, root)
    try:
        if a.app_cryptex:
            install_app_cryptex(a.app_cryptex, root)
        target = root / 'System/Library/LaunchDaemons'
        if target.is_symlink():
            if os.readlink(target) != '/pong/LaunchDaemons': raise RuntimeError('unexpected job directory alias')
            target.unlink()
        target.mkdir(exist_ok=True)
        if not target.is_dir(): raise RuntimeError('unexpected job directory')
        for old in target.glob('*.plist'): old.unlink()
        for source in (out / 'overlay/System/Library/LaunchDaemons').glob('*.plist'):
            destination = target / source.name
            shutil.copyfile(source, destination)
            os.chown(destination, 0, 0); os.chmod(destination, 0o644)
        os.chown(target, 0, 0); os.chmod(target, 0o755)
        cache_target = root / 'System/Library/xpc/launchd.plist'
        cache_target.parent.mkdir(parents=True, exist_ok=True)
        if cache_target.is_symlink(): raise RuntimeError('unexpected service cache symlink')
        shutil.copyfile(out / 'overlay/System/Library/xpc/launchd.plist', cache_target)
        os.chown(cache_target, 0, 0); os.chmod(cache_target, 0o644)
        original = root / 'pong/stock-config/launchd.original'
        if not original.exists():
            shutil.copyfile(a.os_root / 'sbin/launchd', original)
            os.chown(original, 0, 0); os.chmod(original, 0o755)
        executable = root / 'sbin/launchd.ios'
        if executable.is_symlink(): raise RuntimeError('unexpected launchd executable symlink')
        shutil.copyfile(out / 'launchd.ios', executable)
        os.chown(executable, 0, 0); os.chmod(executable, 0o755)
    finally:
        run(['hdiutil', 'detach', device])
    dump(out / 'jobs-update.json', dict(build='22H374', counts=counts, data_modified=False,
         app_cryptex=str(a.app_cryptex) if a.app_cryptex else None,
         operation='Pong non-Exclaves jobs and native Version 7 service cache', image_verification='skipped by request'))
    if a.app_cryptex:
        plan.update(app_cryptex=str(a.app_cryptex.resolve()), app_cryptex_runtime='/System/Cryptexes/App')
        dump(out / 'build-plan.json', plan)
    print(json.dumps(counts, indent=2))


def stage_preinstalled_assets(plan, destination_root):
    """Keep matching restore assets available after writable Data hides /var.

    macOS mounts with ownership disabled (501:20 here). Read numeric metadata
    from APFS, and use the mount only for transparently decompressed contents.
    Init's O_EXCL seed preserves every existing on-device asset/cache file.
    """
    base = 'MobileAsset/PreinstalledAssetsV2'
    source_root = Path(plan['os_root']) / 'private/var'
    seed = destination_root / 'pong/var-seed'
    container = apfs.Container(plan['os_source'])
    volume = apfs.pick_volume(container, None)
    records, total = [], 0

    def stage(rel, descend=True):
        nonlocal total
        hit = volume.lookup('/private/var/' + rel)
        if not hit: raise RuntimeError('22H374 preinstalled asset path absent: ' + rel)
        inode = volume.inode(hit[0]); mode = inode.mode & 0o7777
        source, target = source_root / rel, seed / rel
        guest = '/private/var/' + rel
        if inode.is_dir:
            target.mkdir(parents=True, exist_ok=True)
            records.append(f'd\t{mode:o}\t{inode.owner}\t{inode.group}\t{guest}')
            if descend:
                for name, _, _ in volume.dir_entries(hit[0]):
                    if any(x in name for x in ('/', '\t', '\n')): raise RuntimeError('invalid asset path component')
                    stage(rel + '/' + name)
        elif inode.is_reg:
            total += inode.size
            if total > 32 * 1024**2: raise RuntimeError('unexpected size of this build preinstalled assets')
            shutil.copyfile(source, target)
            os.chown(target, 0, 0); os.chmod(target, 0o644)
            records.append(f'f\t{mode:o}\t{inode.owner}\t{inode.group}\t{guest}\t/pong/var-seed/{rel}')
        else:
            raise RuntimeError('unexpected non-file asset entry: ' + rel)
        if len(records) > 4096: raise RuntimeError('unexpected preinstalled asset entry count')

    try:
        stage('MobileAsset', descend=False)
        stage(base)
    finally:
        container.close()
    # Parent directory entries precede every file, as in var.skel.
    records = [r for r in records if r.startswith('d\t')] + [r for r in records if r.startswith('f\t')]
    skeleton = '\n'.join(records) + '\n'
    if len(skeleton.encode()) >= 65536: raise RuntimeError('preinstalled asset seed exceeds init buffer')
    manifest = destination_root / 'pong/etc/preinstalled-assets.skel'
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(skeleton); os.chown(manifest, 0, 0); os.chmod(manifest, 0o644)
    return dict(build='22H374', entries=len(records), bytes=total,
                destination='/private/var/' + base, overwrite_existing=False,
                metadata_source='original APFS inode owners and modes',
                image_verification='skipped by request')


def refresh_ui(a):
    """Update only the Darwin24 UI adapter; preserve jobs, artwork and Data."""
    if os.geteuid() != 0: raise RuntimeError('refresh_ui requires system authentication')
    out, runtime = a.out.resolve(), a.runtime.resolve()
    plan = json.loads((out / 'build-plan.json').read_text())
    info = json.loads((runtime / 'helper.json').read_text())
    if plan['version'].get('ProductBuildVersion') != '22H374' or info['ui_library_runtime'] != '/pong/libpong-ui18.dylib':
        raise RuntimeError('matching 22H374 UI runtime required')
    image = os_copy(out)
    if image.is_symlink() or image.samefile(plan['os_source']): raise RuntimeError('prepared OS copy required')
    device = attach(image, out / 'system-mount')
    try:
        target = out / 'system-mount/pong/libpong-ui18.dylib'
        if target.is_symlink() or not target.is_file(): raise RuntimeError('existing OS UI library required')
        shutil.copyfile(target, runtime / 'libpong-ui18.previous.dylib')
        shutil.copyfile(runtime / 'libpong-ui.dylib', target)
        os.chown(target, 0, 0); os.chmod(target, 0o755)
        if a.refresh_init:
            for rel in ('pong/init', 'sbin/launchd'):
                target = out / 'system-mount' / rel
                if target.is_symlink() or not target.is_file():
                    raise RuntimeError('expected existing static init: ' + rel)
                shutil.copyfile(runtime / 'init', target)
                os.chown(target, 0, 0); os.chmod(target, 0o755)
            stage_audio_policy(out / 'system-mount')
        if a.enable_audio_hal:
            plugin=runtime/'PongAudio.driver'
            if not plugin.is_dir():raise RuntimeError('matching Pong audio plugin required')
            hal=out/'system-mount/pong/audio-hal18'
            if hal.is_symlink() or not hal.is_dir():raise RuntimeError('existing HAL view required')
            for name in ('PongAudio.plugin', 'PongAudio.driver'):
                previous=hal/name
                if previous.is_symlink():raise RuntimeError('unexpected audio plugin symlink')
                if previous.exists():shutil.rmtree(previous)
            dest=hal/'PongAudio.driver'
            shutil.copytree(plugin,dest)
            for item in (dest,*dest.rglob('*')):
                os.chown(item,0,0);os.chmod(item,0o755 if item.is_dir() or item.name=='PongAudio' else 0o644)
    finally:
        run(['hdiutil', 'detach', device])
    dump(runtime / 'os-ui-update.json', dict(build='22H374', os_image=str(image),
         ui_library='/pong/libpong-ui18.dylib', data_modified=False,
         audio_hal_exposed=a.enable_audio_hal, audio_hal_format='AudioServerPlugIn .driver / MH_BUNDLE',
         hal_factory_requires_physical_ready=False,
         hal_device_requires_physical_ready=True,
         init_updated=a.refresh_init, jobs_modified=False, artwork_modified=False,
         image_verification='skipped by request'))


def refresh_runtime(a):
    """Install an OS-resident runtime; never replace the phone's current Data."""
    if os.geteuid() != 0: raise RuntimeError('refresh_runtime requires system authentication')
    out, runtime = a.out.resolve(), a.runtime.resolve()
    plan = json.loads((out / 'build-plan.json').read_text())
    if plan['version'].get('ProductBuildVersion') != '22H374': raise RuntimeError('22H374 prepared OS required')
    info = json.loads((runtime / 'helper.json').read_text())
    if (info['runtime_path'], info['ui_library_runtime']) != ('/pong/init', '/pong/libpong-ui18.dylib'):
        raise RuntimeError('Build the matching OS runtime first')
    image = os_copy(out)
    if image.is_symlink() or image.samefile(plan['os_source']): raise RuntimeError('prepared OS copy required')
    device = attach(image, out / 'system-mount')
    try:
        root = out / 'system-mount'
        for name, source in [('init', runtime / 'init'), ('libpong-ui18.dylib', runtime / 'libpong-ui.dylib')]:
            target = root / 'pong' / name
            if target.is_symlink(): raise RuntimeError('unexpected runtime symlink: ' + str(target))
            shutil.copyfile(source, target)
            os.chown(target, 0, 0); os.chmod(target, 0o755)
        stage_audio_policy(root)
        # The modern AudioServerPlugIn must not enter the legacy .plugin
        # loader. Remove that old bundle when updating either runtime mode.
        plugin=runtime/'PongAudio.driver'
        if not plugin.is_dir():raise RuntimeError('matching Pong audio plugin required')
        hal=root/'pong/audio-hal18';hal.mkdir(exist_ok=True)
        if hal.is_symlink():raise RuntimeError('unexpected audio HAL symlink')
        for name in ('PongAudio.plugin', 'PongAudio.driver'):
            previous=hal/name
            if previous.is_symlink():raise RuntimeError('unexpected Pong audio plugin symlink')
            if previous.exists():shutil.rmtree(previous)
        staged=root/'pong/staged-audio18';staged.mkdir(exist_ok=True)
        if staged.is_symlink():raise RuntimeError('unexpected staged audio symlink')
        dest=(hal if a.enable_audio_hal else staged)/'PongAudio.driver'
        if dest.is_symlink():raise RuntimeError('unexpected Pong audio plugin symlink')
        if dest.exists():shutil.rmtree(dest)
        shutil.copytree(plugin,dest)
        for item in (dest,*dest.rglob('*')):
            os.chown(item,0,0);os.chmod(item,0o755 if item.is_dir() or item.name=='PongAudio' else 0o644)
        if a.power_firmware:
            firmware = a.power_firmware.resolve() / 'adsp.mbn'
            if not firmware.is_file(): raise RuntimeError('Actual Pong adsp.mbn required')
            dest = root / 'pong/firmware'; dest.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(firmware, dest / 'adsp.mbn')
            os.chown(dest / 'adsp.mbn', 0, 0); os.chmod(dest / 'adsp.mbn', 0o644)
        dump(runtime / 'phone-artwork.json', phone_wallpaper_resources(Path(plan['os_root']), root))
        dump(runtime / 'preinstalled-assets.json', stage_preinstalled_assets(plan, root))
        seed_target = root / 'pong/var-seed/mobile/Library/Preferences'
        seed_target.mkdir(parents=True, exist_ok=True)
        for name in ('com.apple.purplebuddy.plist', 'com.apple.purplebuddy.notbackedup.plist',
                     'com.apple.springboard.plist'):
            source = runtime / 'var-seed/mobile/Library/Preferences' / name
            target = seed_target / name
            shutil.copyfile(source, target)
            os.chown(target, 0, 0); os.chmod(target, 0o644)
        # Retain the normal /sbin/launchd fallback with the same UI path.
        shutil.copyfile(runtime / 'init', root / 'sbin/launchd')
        os.chown(root / 'sbin/launchd', 0, 0); os.chmod(root / 'sbin/launchd', 0o755)
    finally:
        run(['hdiutil', 'detach', device])
    dump(runtime / 'os-runtime-update.json', dict(build='22H374', os_image=str(image),
         helper='/pong/init', ui_library='/pong/libpong-ui18.dylib', data_modified=False,
         audio_hal_exposed=a.enable_audio_hal, audio_hal_format='AudioServerPlugIn .driver / MH_BUNDLE',
         hal_factory_requires_physical_ready=False, hal_device_requires_physical_ready=True,
         image_verification='skipped by request'))


def refresh_metal(a):
    """Stage the matching native plugin in the real OS extension directory."""
    if os.geteuid() != 0: raise RuntimeError('refresh_metal requires system authentication')
    out, runtime = a.out.resolve(), a.runtime.resolve()
    report = json.loads((runtime / 'build-report.json').read_text())
    if report.get('guest') != '22H374': raise RuntimeError('Build a 22H374 Metal plugin first')
    bundle = runtime / 'PongAdrenoMetal.bundle'
    info = plistlib.loads((bundle / 'Info.plist').read_bytes())
    if info.get('CFBundleIdentifier') != 'dev.unixnu.pong.Metal': raise RuntimeError('Unexpected Metal bundle')
    device = attach(os_copy(out), out / 'system-mount')
    try:
        extensions = out / 'system-mount/System/Library/Extensions'
        if extensions.is_symlink(): raise RuntimeError('Native18 requires a real Extensions directory')
        extensions.mkdir(parents=True, exist_ok=True)
        target = extensions / bundle.name
        if target.is_symlink(): raise RuntimeError('Unexpected plugin alias')
        if target.exists():
            old = plistlib.loads((target / 'Info.plist').read_bytes())
            if old.get('CFBundleIdentifier') != info['CFBundleIdentifier']: raise RuntimeError('Preserving a foreign bundle')
            shutil.rmtree(target)
        shutil.copytree(bundle, target)
        for name in ('PongAdrenoMetal.publish', 'PongAdrenoMetal.clients'):
            shutil.copyfile(runtime / name, extensions / name)
            os.chown(extensions / name, 0, 0); os.chmod(extensions / name, 0o644)
        for path in [target, *target.rglob('*')]:
            if path.is_symlink(): raise RuntimeError('Plugin must contain ordinary owned files')
            os.chown(path, 0, 0)
            os.chmod(path, 0o755 if path.is_dir() or path.name != 'Info.plist' else 0o644)
    finally:
        run(['hdiutil', 'detach', device])
    dump(runtime / 'os-metal-update.json', dict(build='22H374',
         path='/System/Library/Extensions/PongAdrenoMetal.bundle', data_modified=False,
         publication='real CP/fence and runtime ABI prerequisites; stock discovery',
         hardware_acceleration_confirmed=False, image_verification='skipped by request'))


def finish(a):
    out = a.out.resolve(); plan = json.loads((out / 'build-plan.json').read_text())
    if not (out / 'apply-complete.json').exists(): raise RuntimeError('apply has not completed')
    if not VERIFY_IMAGES:
        packed = out / 'pong-ipados18-data.packed'
        pack_data(out / 'pong-ipados18-data.cdr', packed)
        tracked = storage_for(out / 'pong-ipados18-system.cdr')
        files = {name: {'bytes': (out / name).stat().st_size} for name in
                 ('pong-ipados18-system.cdr', 'pong-ipados18-data.cdr', 'pong-ipados18-data.packed')}
        if tracked:
            files['pong-ipados18-system.cdr'] = dict(path=tracked['image'],
                bytes=tracked['logical_bytes'], storage='sparsebundle; raw cdr retained as earlier base')
        result=dict(build='22H374', data_backend='ufs', image_verification='skipped by request',
                    device_boot_verified=False, static_init_signature=plan.get('static_init_signature'),
                    phone_written=False, files=files)
        dump(out/'result.json',result)
        print(json.dumps(result,indent=2))
        return
    c = apfs.Container(str(out / 'pong-ipados18-system.cdr')); v = apfs.pick_volume(c, None)
    def content(path):
        found = v.lookup(path)
        if not found: raise RuntimeError('missing image path: ' + path)
        return v.inode(found[0]).contents()
    for source, expected in plan['apple_sha256'].items():
        path = '/pong/stock-config/launchd.original' if source == 'sbin/launchd' else '/' + source
        if hashlib.sha256(content(path)).hexdigest() != expected:
            raise RuntimeError('Apple original readback mismatch: ' + source)
    if content('/System/Cryptexes/OS') != b'/private/preboot': raise RuntimeError('wrong cryptex link')
    if content('/System/Library/Caches/com.apple.dyld') != b'/private/preboot/System/Library/Caches/com.apple.dyld':
        raise RuntimeError('wrong canonical shared cache link')
    if content('/sbin/launchd') != (out / 'overlay/sbin/launchd').read_bytes():
        raise RuntimeError('static init readback mismatch')
    init_signature = native_init_signature(content('/sbin/launchd'))
    if content('/System/Library/xpc/launchd.plist') != (out / 'overlay/System/Library/xpc/launchd.plist').read_bytes():
        raise RuntimeError('adapted Version 7 service cache mismatch')
    for p in (out / 'overlay/System/Library/LaunchDaemons').glob('*.plist'):
        inode = v.inode(v.lookup('/System/Library/LaunchDaemons/' + p.name)[0])
        if inode.owner != 0 or inode.group != 0 or inode.contents() != p.read_bytes():
            raise RuntimeError('launchd job ownership/content mismatch: ' + p.name)
    system_uuid = apfs._uuid(v.vol_uuid); c.close()
    data_path = out / 'pong-ipados18-data.cdr'
    c = apfs.Container(str(data_path)); v = apfs.pick_volume(c, None)
    data_uuid = apfs._uuid(v.vol_uuid)
    for path, (mode, uid, gid) in json.loads((out / 'data-metadata.json').read_text()).items():
        found = v.lookup('/' + path)
        if not found: raise RuntimeError('missing Data seed entry: ' + path)
        inode = v.inode(found[0])
        if (inode.mode, inode.owner, inode.group) != (mode, uid, gid):
            raise RuntimeError('Data seed metadata mismatch: ' + path)
    ino = v.inode(v.lookup('/mobile/Library/Preferences/com.apple.migration.plist')[0])
    migration = plistlib.loads(ino.contents())
    if migration['LastSystemVersion'] != '22H374' or ino.owner != 501:
        raise RuntimeError('Data version/ownership mismatch')
    c.close()
    packed = out / 'pong-ipados18-data.packed'
    pack_data(data_path, packed)
    result = {'os_volume_uuid': system_uuid, 'data_volume_uuid': data_uuid,
              'original_launchd_preserved': True, 'original_dyld_and_SystemVersion_preserved': True,
              'static_init_readback': True,
              'static_init_signature': init_signature,
              '22H374_Data_preferences': True, 'phone_written': False,
              'Data_numeric_owners_and_modes_preserved': True,
              'launchd_plists_root_wheel': len(list((out / 'overlay/System/Library/LaunchDaemons').glob('*.plist'))),
              'Data_metadata_entries': len(json.loads((out / 'data-metadata.json').read_text())),
              'native_APFS_readback': 'passed',
              'device_boot_verified': False, 'files': {}}
    for name in ('pong-ipados18-system.cdr', 'pong-ipados18-data.cdr', 'pong-ipados18-data.packed'):
        p = out / name
        result['files'][name] = {'bytes': p.stat().st_size, 'sha256': sha(p)}
    packed_sha = packed_digest(packed)
    if packed_sha != result['files']['pong-ipados18-data.cdr']['sha256']:
        raise RuntimeError('PackedRAMDisk reconstruction does not match Data')
    result['packed_Data_roundtrip_sha256'] = packed_sha
    dump(out / 'result.json', result)
    print(json.dumps(result, indent=2))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='mode', required=True)
    s = sub.add_parser('prepare'); s.add_argument('--out', type=Path, required=True)
    for name in ('os-root', 'os-image', 'system-cryptex', 'data-image', 'developer'):
        s.add_argument('--' + name, type=Path, required=True)
    s.add_argument('--app-cryptex', type=Path)
    for name in ('apply', 'finish', 'rebuild_data', 'reset_data'):
        s = sub.add_parser(name); s.add_argument('--out', type=Path, required=True)
    s = sub.add_parser('helper'); s.add_argument('--out', type=Path, required=True)
    s.add_argument('--developer', type=Path, required=True)
    s = sub.add_parser('refresh_jobs'); s.add_argument('--out', type=Path, required=True)
    s.add_argument('--os-root', type=Path, required=True)
    s.add_argument('--system-cryptex', type=Path, required=True)
    s.add_argument('--app-cryptex', type=Path)
    s = sub.add_parser('refresh_runtime'); s.add_argument('--out', type=Path, required=True)
    s.add_argument('--enable-audio-hal', action='store_true',
                   help='Expose experimental PCM HAL; default keeps it outside the boot HAL view')
    s.add_argument('--power-firmware', type=Path, help='Local REC-extracted Pong signed firmware directory')
    s.add_argument('--runtime', type=Path, required=True)
    s = sub.add_parser('refresh_ui'); s.add_argument('--out', type=Path, required=True)
    s.add_argument('--runtime', type=Path, required=True)
    s.add_argument('--refresh-init', action='store_true',
                   help='Also stage the matching static init and native audio-provider policy')
    s.add_argument('--enable-audio-hal', action='store_true', help='Install the physical-readiness-gated Pong output HAL')
    s = sub.add_parser('refresh_metal'); s.add_argument('--out', type=Path, required=True)
    s.add_argument('--runtime', type=Path, required=True)
    s = sub.add_parser('refresh_cryptex'); s.add_argument('--out', type=Path, required=True)
    s.add_argument('--system-cryptex', type=Path, required=True)
    for mode in sub.choices.values():
        mode.add_argument('--verify-images', action='store_true',
                          help='explicitly request image hashes and readback (off by default)')
    a = p.parse_args()
    global VERIFY_IMAGES
    VERIFY_IMAGES = a.verify_images
    globals()[a.mode](a)


if __name__ == '__main__': main()
