#!/usr/bin/env python3
"""22H374 policy for native services using the non-Apple Metal runtime."""
import os
import plistlib
import stat


def expand_framework_shaders(root, primelist):
    """Expand actual compressed assets on the host, before idle priming.

    No synthetic shaders or Apple GPU binaries are substituted for AIR.
    Frameworks outside the installed list and replay/debug assets stay alone.
    """
    changes = []
    for entry in primelist.read_text().splitlines():
        if not entry.startswith(('/System/Library/Frameworks/', '/System/Library/PrivateFrameworks/')):
            continue
        if 'MetalTools' in entry or 'MTLReplayer' in entry:
            continue
        path = root / entry.lstrip('/')
        if not path.is_file() or not path.resolve().is_relative_to(root.resolve()):
            continue
        info = path.stat()
        if not info.st_flags & stat.UF_COMPRESSED:
            continue
        space = os.statvfs(root)
        if info.st_size + (128 << 20) > space.f_bavail * space.f_frsize:
            changes.append(dict(path=entry, skipped='host OS filesystem free space'))
            continue
        temporary = path.with_name(path.name + '.pong-uncompressed')
        if temporary.exists():
            raise RuntimeError('unexpected shader staging temporary file')
        # macOS reads the native decmpfs payload and writes ordinary bytes.
        # Keep identity/permissions/time; compression xattrs are not copied.
        data = path.read_bytes()
        with temporary.open('xb') as output:
            output.write(data)
        os.chown(temporary, info.st_uid, info.st_gid)
        os.chmod(temporary, stat.S_IMODE(info.st_mode))
        os.utime(temporary, ns=(info.st_atime_ns, info.st_mtime_ns))
        os.replace(temporary, path)
        changes.append(dict(path=entry, expanded_bytes=len(data)))
    return changes


def configure(root):
    cache_path = root / 'System/Library/xpc/launchd.plist'
    cache = plistlib.loads(cache_path.read_bytes())
    changes = []
    for path, job in cache['LaunchDaemons'].items():
        if job.get('Label') != 'com.apple.cameracaptured':
            continue
        executable = job.get('Program') or next(iter(job.get('ProgramArguments', [])), '')
        if not executable.endswith('/cameracaptured'):
            raise RuntimeError('unexpected camera service executable')
        # services155: this real process is jetsammed at 27MiB every 5s
        # while creating its actual Vulkan device. Preserve the native job,
        # IPC and executable; give this one Metal client a bounded allowance.
        previous = dict(job.get('JetsamProperties', {}))
        properties = job.setdefault('JetsamProperties', {})
        for key in ('MemoryLimit', 'ActiveHardMemoryLimit', 'InactiveHardMemoryLimit'):
            value = properties.get(key)
            if value is None:
                properties[key] = 192
            elif isinstance(value, int) and 0 < value < 192:
                properties[key] = 192
        # Keep the genuine native retry policy bounded even if another
        # process manager reapplies its own memory class after launch.
        job['ThrottleInterval'] = max(30, job.get('ThrottleInterval', 0))
        canonical = root / path.lstrip('/')
        if not canonical.is_file() or canonical.is_symlink():
            raise RuntimeError('native camera plist missing or aliased')
        canonical.write_bytes(plistlib.dumps(job, fmt=plistlib.FMT_BINARY))
        changes.append(dict(label=job['Label'], before=previous, after=dict(properties)))
    cache_path.write_bytes(plistlib.dumps(cache, fmt=plistlib.FMT_BINARY))
    return changes
