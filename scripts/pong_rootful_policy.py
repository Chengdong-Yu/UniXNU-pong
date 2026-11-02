"""22H374's single APFS root-upgrade policy call, backed by native VFS checks.

This adapter leaves the real mount flags, filesystem authentication, snapshots,
and all other APFS permission checks intact. It never edits the restore input.
"""
import struct
from native_prelink import backed_offset, exports, macho


def apply(candidate, native, original, slide, components):
    if 'com.apple.filesystems.apfs' not in components:
        raise ValueError('Rootful policy requires the matching APFS component')
    helper = [address for address, names in exports(native, macho(native)).items()
              if '_native_pong_apfs_upgrade_flags' in names]
    if len(helper) != 1:
        raise ValueError('Native rootful policy entry missing or ambiguous')
    # apfs_mount_upgrade_checks in the supplied ordinary ARM64 22H374 cache.
    # Its ROOTFS-only gate follows this vfs_flags call. The helper returns
    # the ordinary flags unless an explicit, root-owned Pong OS update passes.
    site = 0xfffffff0066f58a0
    parent = macho(original)['segments']
    old_at = backed_offset(parent, site - 8, 20, len(original))
    words = struct.unpack_from('<5I', original, old_at)
    if (words[0] != 0xf941be60 or words[1] & 0xff000000 != 0xb4000000 or
            words[2] & 0xfc000000 != 0x94000000 or words[3] != 0x37700060 or
            words[4] != 0x52800000):
        raise ValueError('Unsupported APFS root-upgrade call ABI; input retained')
    pc = site + slide
    delta = helper[0] - pc
    if delta & 3 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError('Native rootful policy outside direct ARM64 call range')
    output = bytearray(candidate)
    at = backed_offset(macho(output)['segments'], pc, 4, len(output))
    struct.pack_into('<I', output, at, 0x94000000 | ((delta >> 2) & 0x3ffffff))
    return bytes(output), dict(build='22H374', original_call=hex(site),
        linked_call=hex(pc), native_policy=hex(helper[0]),
        activation='pong_rootful=1, uid 0, bound native UFS OS root only',
        mount_flags_modified=False, restore_input_modified=False)
