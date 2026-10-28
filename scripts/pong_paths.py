#!/usr/bin/env python3
"""Resolve the Pong build inputs from flags, then the environment, then a local layout.

Every path that used to be a hardcoded volume name goes through one of these
resolvers, so the same scripts run on a workstation with mounted volumes and on
a clean CI runner that places everything under one directory. An explicit flag
always wins; otherwise the environment; otherwise the historical local default.
A resolver never returns a path that silently does not exist: call `require`
for inputs that must be present.
"""
import os
import shutil
import subprocess
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[1]
ROOT = DEVICE.parent


def _env(*names):
    for name in names:
        value = os.environ.get(name)
        if value:
            return Path(value).expanduser()
    return None


def input_root():
    """Root of the fetched Apple content and other large local inputs."""
    return _env('PONG_INPUTS', 'PONG_INPUT_ROOT') or Path('/Volumes/Untitled/Pong-iPadOS18')


def work_root():
    """Root of the local build scratch space."""
    return _env('PONG_WORK', 'PONG_WORK_ROOT') or _env('PONG_ROOT') or Path('/Volumes/PongDev')


def build_tree(explicit=None, name='Pong-iPadOS18-native'):
    if explicit:
        return Path(explicit).expanduser()
    return _env('PONG_BUILD_TREE') or work_root() / 'build' / name / 'tree'


def developer_dir(explicit=None):
    """Xcode developer directory: flag, then env, then the active toolchain."""
    if explicit:
        return Path(explicit).expanduser()
    found = _env('PONG_DEVELOPER_DIR', 'DEVELOPER_DIR')
    if found:
        return found
    try:
        selected = subprocess.check_output(['xcode-select', '-p'], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        selected = ''
    if selected:
        return Path(selected)
    return Path('/Applications/Xcode.app/Contents/Developer')


def volume_layout(explicit=None):
    if explicit:
        return Path(explicit).expanduser()
    return _env('PONG_VOLUME_LAYOUT') or work_root() / 'build/Pong-iPadOS18-inputs/volume-layout.json'


def prepared_userland(explicit=None):
    if explicit:
        return Path(explicit).expanduser()
    return _env('PONG_PREPARED_USERLAND') or input_root() / '22H374/prepared'


def wifi_source(explicit=None):
    if explicit:
        return Path(explicit).expanduser()
    return _env('PONG_WIFI_SOURCE') or input_root() / 'wifi-source-7.0.14'


def wifi_build(explicit=None):
    if explicit:
        return Path(explicit).expanduser()
    return _env('PONG_WIFI_BUILD') or input_root() / 'wifi-ath11k-build'


def wifi_firmware(explicit=None):
    if explicit:
        return Path(explicit).expanduser()
    return _env('PONG_WIFI_FIRMWARE') or input_root() / 'pong-wifi-firmware'


def audio_inputs(explicit=None):
    if explicit:
        return Path(explicit).expanduser()
    return _env('PONG_AUDIO_INPUTS') or input_root() / 'pong-audio-inputs'


def sheng_root(explicit=None):
    """Shared UniXNU-sheng sources; falls back to the sibling checkout."""
    if explicit:
        return Path(explicit).expanduser()
    found = _env('PONG_SHENG_ROOT', 'UNIXNU_SHENG_ROOT')
    if found:
        return found
    for candidate in (ROOT / 'UniXNU-sheng', work_root() / 'upstream/UniXNU-sheng'):
        if candidate.is_dir():
            return candidate
    return work_root() / 'upstream/UniXNU-sheng'


def adb(explicit=None):
    """adb binary: flag, then the Android SDK layout, then PATH."""
    if explicit:
        return Path(explicit).expanduser()
    found = _env('PONG_ADB')
    if found:
        return found
    for base in (os.environ.get('ANDROID_SDK_ROOT'), os.environ.get('ANDROID_HOME')):
        if base and (Path(base) / 'platform-tools/adb').is_file():
            return Path(base) / 'platform-tools/adb'
    located = shutil.which('adb')
    if located:
        return Path(located)
    return work_root() / 'tools/platform-tools/adb'


def require(path, what, how):
    """Fail with the missing input and the resolver that supplies it."""
    if not Path(path).exists():
        raise SystemExit(f'missing {what}: {path}\nprovide it with {how}')
    return Path(path)


def handoff_root(explicit=None):
    """A handoff bundle of prebuilt local images."""
    if explicit:
        return Path(explicit).expanduser()
    return _env('PONG_HANDOFF') or work_root() / 'handoffs/Pong-iOS16-Mac-Next-20261020'


def boot_template(explicit=None):
    """Boot image template that the local boot packagers stage into."""
    if explicit:
        return Path(explicit).expanduser()
    return _env('PONG_BOOT_TEMPLATE') or handoff_root() / 'images/pong-window-vectors.img'


def mu_basecore(explicit=None):
    """EDK2 Mu_Basecore checkout used by the DarwinBoot loaders."""
    if explicit:
        return Path(explicit).expanduser()
    return _env('PONG_EDK2') or work_root() / 'source/Mu_Basecore'


def unixnu_package(explicit=None):
    """UniXNU package tree that the DarwinBoot loaders build against."""
    if explicit:
        return Path(explicit).expanduser()
    found = _env('PONG_UNIXNU_PACKAGE')
    if found:
        return found
    local = ROOT / 'package'
    return local if local.is_dir() else work_root() / 'upstream/UniXNU/package'


def llvm_toolchain(explicit=None):
    """LLVM bin directory for the EDK2 loaders; empty means use the host one."""
    if explicit:
        return Path(explicit).expanduser()
    found = _env('PONG_LLVM')
    if found:
        return found
    ports = Path('/opt/local/libexec/llvm-18/bin')
    return ports if ports.is_dir() else None


def extra_path():
    """Optional extra PATH entries, colon separated; absent by default."""
    return os.environ.get('PONG_EXTRA_PATH', '')


def storage_headers(explicit=None):
    """Apple IOStorageFamily checkout that supplies the storage ABI headers."""
    if explicit:
        return Path(explicit).expanduser()
    return _env('PONG_STORAGE_HEADERS') or DEVICE / 'refer/ipados18/IOStorageFamily-323'


def xnu_work(tree):
    """Patched XNU working copy inside a prepared build tree."""
    return Path(tree) / '.cache/xnu-work'


def macports():
    """Whether the MacPorts prefix is usable on this host."""
    return Path('/opt/local/bin').is_dir()


def tool(name, env_name=None, fallbacks=()):
    """Locate a build tool by env override, then PATH, then known local installs.

    Returns the resolved path as a string so it can be used directly in a
    command list. Raises with the names tried when nothing is found.
    """
    if env_name:
        configured = os.environ.get(env_name)
        if configured:
            return configured
    located = shutil.which(name)
    if located:
        return located
    for candidate in fallbacks:
        path = Path(candidate)
        if path.is_file() and os.access(path, os.X_OK):
            return str(path)
    tried = ', '.join([name, *fallbacks])
    raise SystemExit(f'missing build tool {name}: install it or set {env_name or "PATH"} (tried {tried})')


def path_with(*entries):
    """Join PATH entries, dropping empties and keeping the inherited PATH last."""
    parts = [str(item) for item in entries if item]
    inherited = os.environ.get('PATH', '')
    if inherited:
        parts.append(inherited)
    return os.pathsep.join(parts)


def optional(path):
    """Render an optional path for reporting without turning None into '.'."""
    if path is None:
        return 'unset'
    return f'{path} ' + ('present' if Path(path).exists() else 'absent')


def describe():
    """Print the resolved layout; used by the workflow to record what it built against."""
    entries = {
        'input_root': input_root(), 'work_root': work_root(),
        'handoff_root': handoff_root(), 'boot_template': boot_template(),
        'mu_basecore': mu_basecore(), 'unixnu_package': unixnu_package(),
        'llvm_toolchain': llvm_toolchain(), 'storage_headers': storage_headers(),
        'build_tree': build_tree(), 'developer_dir': developer_dir(),
        'volume_layout': volume_layout(), 'prepared_userland': prepared_userland(),
        'wifi_source': wifi_source(), 'wifi_build': wifi_build(),
        'wifi_firmware': wifi_firmware(), 'audio_inputs': audio_inputs(),
        'sheng_root': sheng_root(), 'adb': adb(),
    }
    for name, path in entries.items():
        print(f'{name}: {optional(path)}')


if __name__ == '__main__':
    describe()
