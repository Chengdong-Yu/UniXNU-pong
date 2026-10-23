#!/usr/bin/env python3
"""Adapt HackiOS userland to the local, native-APFS UniXNU Pong 20G81 port.

prepare: build static init, curate real 20G81 jobs, extract a Data seed and clone images.
apply:   mount ONLY these image copies and apply the prepared changes (macOS admin).
finish:  produce PackedRAMDisk without a separate image-verification pass.
helper:  compile a RAM-Data init update without rebuilding the OS image.
No mode writes a phone, a physical disk or the original restore image.
"""
import argparse
import ast
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import posixpath
import shutil
import stat
import struct
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[1]
USERLAND = PROJECT / 'userland'
sys.path.insert(0, str(PROJECT / 'tools/apfs'))
import apfs

CORE = '''com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system
com.apple.logd com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd
com.apple.fseventsd com.apple.containermanagerd com.apple.containermanagerd.system
com.apple.runningboardd com.apple.backboardd com.apple.mobile.usermanagerd
com.apple.mobilegestalt.xpc com.apple.lsd com.apple.powerd com.apple.analyticsd
com.apple.logd_helper com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd
com.apple.SpringBoard com.apple.configd'''.split()
# Restore the Pong-specific exclusions now that writable Data/KeyBag work.
# Keep the reference's on-demand policy, so restoring a provider does not
# start every service at boot. These software services have real 20G81 jobs;
# a missing dependency is not evidence that its clients should be disabled.
BOOT_SKIP = {
    # v80 REC: Chrono's eager extension discovery repeatedly launches widgets
    # which never check in, then kills them all. Keep the other restored
    # providers; widget timelines are optional before the first home screen.
    'com.apple.chronod': 'widget extension discovery repeatedly times out before the first UI',
}
RESTORED_SOFTWARE = {
    'mobileassetd', 'bird', 'homed', 'routined', 'duetexpertd',
    'com.apple.dmd', 'syncdefaultsd', 'photoanalysisd', 'ReportCrash',
}
EXTRA = [
    ('db/uuidtext', 0o755, 272, 272), ('db/diagnostics/timesync', 0o755, 272, 272),
    ('containers', 0o755, 0, 0), ('containers/Data', 0o755, 0, 0),
    ('containers/Shared', 0o755, 0, 0), ('containers/Bundle', 0o755, 0, 0),
    ('mobile/Containers', 0o755, 501, 501), ('mobile/Containers/Data', 0o755, 501, 501),
    ('mobile/Containers/Shared', 0o755, 501, 501),
    ('mobile/Library/Caches', 0o755, 501, 501), ('mobile/Library/Preferences', 0o755, 501, 501),
    ('mobile/Library/Logs', 0o755, 501, 501), ('root/Library', 0o700, 0, 0),
    ('mobile/Library/Logs/CrashReporter', 0o755, 501, 501),
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


def dump(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')


def put_plist(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(plistlib.dumps(value, fmt=plistlib.FMT_BINARY))


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


def source_path(root, cryptex, path):
    """Resolve iOS absolute symlinks inside the source volumes, never the host root."""
    for _ in range(40):
        path = posixpath.normpath('/' + path.lstrip('/'))
        prefix = '/System/Cryptexes/OS'
        if path == prefix or path.startswith(prefix + '/'):
            return cryptex / path[len(prefix):].lstrip('/')
        if path == '/System/Cryptexes/App' or path.startswith('/System/Cryptexes/App/'):
            return None  # no matching 20G81 App cryptex in this port yet
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


def jobs(root, cryptex, overlay, out):
    # Read the project's policy as data, without executing its command-line script.
    tree = ast.parse((PROJECT / 'userland/mkjobs.py').read_text())
    deny = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
                and any(isinstance(t, ast.Name) and t.id == 'DENY' for t in n.targets))
    deny = tuple(word for word in deny if word not in RESTORED_SOFTWARE)
    dest = overlay / 'pong/LaunchDaemons'
    dest.mkdir(parents=True, exist_ok=True)
    for old in dest.glob('*.plist'): old.unlink()
    records, selected = [], set()
    for folder in ('LaunchDaemons', 'LaunchAngels'):
        for f in sorted((root / 'System/Library' / folder).glob('*.plist')):
            job = plistlib.loads(f.read_bytes())
            label = job.get('Label', f.stem)
            program = job.get('Program') or (job.get('ProgramArguments') or [''])[0]
            mapped = source_path(root, cryptex, program) if program else None
            core = label in CORE
            reason = None
            if not mapped or not mapped.is_file(): reason = 'program absent from 20G81 OS/SystemCryptex'
            elif job.get('_LimitLoadToVariant') == 'IsDarwinOS': reason = 'DarwinOS variant; this source is iOS'
            elif not core and label in BOOT_SKIP: reason = 'Pong boot: ' + BOOT_SKIP[label]
            elif not core and any(s in label for s in deny): reason = 'HackiOS unsupported-service policy'
            elif not core and not job.get('MachServices'): reason = 'non-core job without MachServices'
            record = dict(source=f.relative_to(root).as_posix(), label=label, program=program,
                          action='excluded' if reason else ('boot' if core else 'on-demand'), reason=reason)
            records.append(record)
            if reason: continue
            selected.add(label)
            if core:
                job.pop('_LimitLoadToDeviceTree', None)
            else:
                # Preserve ContextStore/Biome's original publisher declarations
                # for future prepared images. v74's observed SIGTRAP instead
                # came from MobileKeyBag's missing AppleKeyStore service;
                # disabling their event handlers is not a fix for that crash.
                # they remain on-demand because RunAtLoad/KeepAlive are absent.
                if label not in ('com.apple.contextstored', 'com.apple.biomed'):
                    for k in TRIGGERS: job.pop(k, None)
            job['StandardErrorPath'] = job['StandardOutPath'] = '/dev/console'
            # Some stock descriptors carry this misspelling. launchd logs it
            # as an unknown key and otherwise loses the intended settings.
            legacy_env = job.pop('EnvironmentalVariables', None)
            if isinstance(legacy_env, dict):
                job['EnvironmentVariables'] = {**legacy_env, **job.get('EnvironmentVariables', {})}
            # A few genuine 20G81 descriptors encode an integer as a string.
            # launchd rejects that field, including chronod's retry interval.
            interval = job.get('ThrottleInterval')
            if isinstance(interval, str) and interval.isdecimal():
                job['ThrottleInterval'] = int(interval)
            if label == 'com.apple.backboardd':
                # Install before libtrace initializes; also survives xpcproxy.
                job.setdefault('EnvironmentVariables', {}).update({
                    'OS_ACTIVITY_DT_MODE': 'YES',
                    # HackiOS ca_vbl route: drive the display clock with this
                    # backend's real timed refresh notifications. Disable the
                    # Apple display-idle optimization unsupported by scanout.
                    'CA_FORCE_TIMER': '1', 'CA_DISABLE_DISPLAY_IDLE': '1',
                })
            put_plist(dest / f.name, job)
    dump(out / 'jobs-report.json', {'jobs': records, 'missing_core_labels': sorted(set(CORE) - selected)})
    return {kind: sum(r['action'] == kind for r in records)
            for kind in ('boot', 'on-demand', 'excluded')}


def settings_resources(root, destination):
    """Use original 20G81 localizations while omitting unported baseband panes.

    This is a resource bundle for the Settings process only. In particular,
    preserve DoNotDisturb: 20G81 replaces that entry with Focus dynamically.
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


def data_seed(image, root, overlay, out):
    data = out / 'data-root'
    data.mkdir()
    c = apfs.Container(str(image))
    v = apfs.pick_volume(c, None)
    metadata = {}
    oid = v.lookup('/private/var')[0]

    def extract(oid, rel):
        if rel.split('/')[0] in ('staged_system_apps', 'MobileAsset'): return
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
            # macOS transparently reads all decmpfs compression types shipped by 20G81.
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
            metadata.setdefault(p, [stat.S_IFDIR | 0o755, uid, gid])
        metadata.setdefault(rel, [stat.S_IFDIR | mode, uid, gid])
    for name in ('com.apple.purplebuddy.plist', 'com.apple.migration.plist'):
        rel = 'mobile/Library/Preferences/' + name
        value = plistlib.loads((PROJECT / 'userland/var' / rel).read_bytes())
        if name == 'com.apple.migration.plist':
            value['LastSystemVersion'] = '20G81'
            value['DMLastMigrationResults']['buildVersion'] = '20G81'
        put_plist(data / rel, value)
        metadata[rel] = [stat.S_IFREG | 0o644, 501, 501]
    for owner, uid, gid in [('mobile',501,501),('root',0,0)]:
        rel = owner + '/Library/Preferences/.GlobalPreferences.plist'
        path = data / rel
        value = plistlib.loads(path.read_bytes()) if path.exists() else {}
        value.update(AppleLanguages=['zh-Hans-CN'], AppleLocale='zh_CN')
        path.parent.mkdir(parents=True, exist_ok=True)
        put_plist(path, value)
        metadata[rel] = [stat.S_IFREG | 0o644, uid, gid]
    settings = data / 'pong/PreferencesUI.framework'
    removed = settings_resources(root, settings)
    dump(out / 'settings-resources.json', removed)
    for p in [data/'pong', settings, *settings.iterdir()]:
        rel = str(p.relative_to(data))
        kind = stat.S_IFLNK if p.is_symlink() else stat.S_IFDIR if p.is_dir() else stat.S_IFREG
        metadata[rel] = [kind | (0o755 if kind != stat.S_IFREG else 0o644), 0, 0]
    link = 'db/timezone/zoneinfo'
    # /usr/share/zoneinfo points into /var on 20G81. Repeating that target
    # here makes a cycle before timed starts; HackiOS uses the shipped data.
    if (data / link).is_symlink() and os.readlink(data / link) == '/usr/share/zoneinfo':
        (data / link).unlink()
    if not os.path.lexists(data / link): (data / link).symlink_to('/usr/share/zoneinfo.default')
    metadata[link] = [stat.S_IFLNK | 0o755, 0, 0]
    for rel in ('db/diagnostics/.migrated', 'db/diagnostics/timesync/.migrated', 'db/uuidtext/.migrated'):
        (data / rel).touch(); metadata[rel] = [stat.S_IFREG | 0o644, 272, 272]
    (data / '.pong-data-20G81').write_text('Pong development Data seed for 16.6.1 / 20G81\n')
    metadata['.pong-data-20G81'] = [stat.S_IFREG | 0o644, 0, 0]
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
                template='20G81 native APFS inode owners/modes; staged apps remain on OS')


def compile_init(out, developer, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DEVELOPER_DIR=str(developer))
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], env=env, text=True).strip()
    cmd = ['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64-apple-ios16.6.1',
           '-isysroot', sdk, '-Os', '-static', '-nostdlib', '-ffreestanding',
           '-fno-stack-protector', '-fno-stack-check', '-fno-builtin', '-Wall', '-Wextra', '-Werror',
           '-I' + str(USERLAND / 'libmini'), '-Wl,-e,_start',
           '-Wl,-pagezero_size,0x100000000', '-Wl,-no_fixup_chains',
           str(USERLAND / 'libmini/start.S'), str(USERLAND / 'libmini/mini.c'),
           str(USERLAND / 'init.c'), '-o', str(destination)]
    run(cmd, env=env)
    # codesign defaults to 16K for arm64, independently of the kernel ABI.
    run(['codesign', '--force', '--sign', '-', '--pagesize', '4096',
         '--entitlements', USERLAND / 'init-entitlements.plist', destination])
    init_signature = native_init_signature(destination.read_bytes())
    os.chmod(destination, 0o755)
    (out / 'compile-command.json').write_text(json.dumps(cmd, indent=2) + '\n')
    return init_signature


def compile_ui(out, developer):
    """Build the 20G81 adapter; no prebuilt 20F66 addresses/libraries."""
    env = dict(os.environ, DEVELOPER_DIR=str(developer))
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], env=env, text=True).strip()
    destination = out / 'libpong-ui.dylib'
    cmd = ['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64-apple-ios16.6.1',
           '-isysroot', sdk, '-Os', '-fblocks', '-dynamiclib', '-Wall', '-Wextra', '-Werror',
           '-Wl,-no_fixup_chains', '-install_name', '/private/var/pong/libpong-ui.dylib',
           '-framework', 'CoreFoundation', '-framework', 'CoreGraphics', '-lobjc',
           str(USERLAND / 'ui_compat.c'), str(USERLAND / 'icons_cpu.c'),
           str(USERLAND / 'touch.c'),
           '-o', str(destination)]
    run(cmd, env=env)
    run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', destination])
    (out / 'ui-compile-command.json').write_text(json.dumps(cmd, indent=2) + '\n')
    return destination


def helper(a):
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    signature = compile_init(out, a.developer, out / 'init')
    ui_library = compile_ui(out, a.developer)
    dump(out / 'helper.json', dict(helper=str(out / 'init'), runtime_path='/private/var/pong/init',
        signature=signature, required_kernel=['pong_init_path',
        'native writable Data mount', 'DEVELOPMENT PONG arm64 4K'],
        ui_library=str(ui_library),
        ui_library_runtime='/private/var/pong/libpong-ui.dylib'))
    print(out / 'init')


def prepare(a):
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    root, cryptex = a.os_root.resolve(), a.system_cryptex.resolve()
    version = plistlib.loads((root / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if (version['ProductVersion'], version['ProductBuildVersion']) != ('16.6.1', '20G81'):
        raise RuntimeError('this adapter requires the actual 16.6.1 / 20G81 root')
    cv = plistlib.loads((cryptex / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if (cv['ProductVersion'], cv['ProductBuildVersion']) != ('16.6.1', '20G81'):
        raise RuntimeError('SystemCryptex must also be 16.6.1 / 20G81; refusing mixed builds')
    overlay = out / 'overlay'
    (overlay / 'pong/etc').mkdir(parents=True, exist_ok=True)
    (overlay / 'pong/LICENSE-HackiOS').write_bytes((USERLAND / 'LICENSE-HackiOS').read_bytes())
    (overlay / 'sbin').mkdir(exist_ok=True)
    init_signature = compile_init(out, a.developer, overlay / 'sbin/launchd')
    counts = jobs(root, cryptex, overlay, out)
    seed = data_seed(a.os_image, root, overlay, out)
    for p in sorted((cryptex / 'usr/lib').rglob('*')):
        rel = p.relative_to(cryptex)
        if (p.is_file() or p.is_symlink()) and not os.path.lexists(root / rel):
            dest = overlay / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.symlink_to('/System/Cryptexes/OS/' + rel.as_posix())
    cache = cryptex / 'System/Library/Caches/com.apple.dyld'
    cache_files = sorted(p for p in cache.glob('dyld_shared_cache_arm64*') if p.is_file())
    if len(cache_files) < 2: raise RuntimeError('matching split arm64 shared cache missing')
    state = dict(format='pong-hackios-userland-20G81-v1', version=version, cryptex_version=cv,
                 os_source=str(a.os_image.resolve()), os_root=str(root), system_cryptex=str(cryptex),
                 data_source=str(a.data_image.resolve()),
                 jobs=counts, data=seed, static_init_signature=init_signature,
                 disabled=['20F66 fixed-address injection', 'Redwood sysctls',
                 'HackiOS ramfs / custom APFS ABI', 'hardware firmware and GPU libraries'],
                 system_cryptex_runtime='/private/preboot', app_cryptex='not supplied',
                 kernel_prerequisites=['native APFS 20G81', 'SystemOS mounted at /private/preboot',
                 'skipIgnition dyld_flags=0x8', 'a writable Data device or pre-mounted /private/var'],
                 phone_written=False)
    dump(out / 'build-plan.json', state)
    for source, name in ((a.os_image, 'pong-ios1661-system.cdr'), (a.data_image, 'pong-ios1661-data.cdr')):
        dest = out / name
        if not dest.exists(): run(['/bin/cp', '-c', source, dest])
        if dest.samefile(source): raise RuntimeError('destination must be an independent image')
    print(json.dumps({'prepared': str(out), 'jobs': counts, 'data': seed}, indent=2))


def attach(image, mountpoint):
    mountpoint.mkdir(exist_ok=True)
    result = run(['hdiutil', 'attach', '-readwrite', '-nobrowse', '-owners', 'on',
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
    if plan['format'] != 'pong-hackios-userland-20G81-v1': raise RuntimeError('wrong build plan')
    if (out / 'apply-complete.json').exists(): raise RuntimeError('images already modified; use a new output directory')
    for name, source in (('pong-ios1661-system.cdr', plan['os_source']),
                         ('pong-ios1661-data.cdr', plan['data_source'])):
        target = out / name
        if not target.is_file() or target.is_symlink() or target.samefile(source):
            raise RuntimeError('refusing non-copy target: ' + str(target))
    root = out / 'system-mount'
    device = attach(out / 'pong-ios1661-system.cdr', root)
    try:
        stock = root / 'pong/stock-config'
        stock.mkdir(parents=True)
        for rel in ('System/Library/LaunchDaemons', 'System/Library/NanoLaunchDaemons',
                    'System/Library/LaunchAngels', 'System/Library/xpc/launchd.plist',
                    'System/Library/xpc/launchd.plist.sig'):
            p = root / rel
            if os.path.lexists(p): shutil.move(str(p), str(stock / p.name))
        (root / 'System/Library/LaunchDaemons').symlink_to('/pong/LaunchDaemons')
        (root / 'sbin/launchd').rename(root / 'sbin/launchd.ios')
        merge_tree(out / 'overlay', root)
        for rel in ('pong', 'sbin/launchd'):
            base = root / rel
            paths = [base] + list(base.rglob('*')) if base.is_dir() else [base]
            for p in paths:
                os.chown(p, 0, 0, follow_symlinks=False)
        link = root / 'System/Cryptexes/OS'
        if not link.is_symlink(): raise RuntimeError('unexpected Cryptex OS layout')
        original_link = os.readlink(link)
        link.unlink(); link.symlink_to('/private/preboot')
        os.chown(link, 0, 0, follow_symlinks=False)
        cache_link = root / 'System/Library/Caches/com.apple.dyld'
        if os.path.lexists(cache_link): raise RuntimeError('unexpected root cache already present')
        cache_link.symlink_to('/private/preboot/System/Library/Caches/com.apple.dyld')
        os.chown(cache_link, 0, 0, follow_symlinks=False)
        # Restore normally moves these applications out of the Data template.
        apps = root / 'private/var/staged_system_apps'
        moved = []
        for p in sorted(apps.glob('*.app')):
            dest = root / 'Applications' / p.name
            if os.path.lexists(dest): raise RuntimeError('app already exists: ' + p.name)
            p.rename(dest); moved.append(p.name)
    finally:
        run(['hdiutil', 'detach', device])
    data = out / 'data-mount'
    device = attach(out / 'pong-ios1661-data.cdr', data)
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
                                      'relocated_20G81_apps': moved, 'phone_written': False})
    print('Applied userland changes to independent OS and Data image copies.')


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


def finish(a):
    out = a.out.resolve(); plan = json.loads((out / 'build-plan.json').read_text())
    if not (out / 'apply-complete.json').exists(): raise RuntimeError('apply has not completed')
    packed = out / 'pong-ios1661-data.packed'
    pack_data(out / 'pong-ios1661-data.cdr', packed)
    result = {'phone_written': False, 'device_boot_verified': False,
              'static_init_signature': plan.get('static_init_signature'),
              'files': {name: {'bytes': (out / name).stat().st_size} for name in
                  ('pong-ios1661-system.cdr', 'pong-ios1661-data.cdr', 'pong-ios1661-data.packed')}}
    dump(out / 'result.json', result)
    print(json.dumps(result, indent=2))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='mode', required=True)
    s = sub.add_parser('prepare'); s.add_argument('--out', type=Path, required=True)
    for name in ('os-root', 'os-image', 'system-cryptex', 'data-image', 'developer'):
        s.add_argument('--' + name, type=Path, required=True)
    for name in ('apply', 'finish'):
        s = sub.add_parser(name); s.add_argument('--out', type=Path, required=True)
    s = sub.add_parser('helper'); s.add_argument('--out', type=Path, required=True)
    s.add_argument('--developer', type=Path, required=True)
    a = p.parse_args()
    globals()[a.mode](a)


if __name__ == '__main__': main()
