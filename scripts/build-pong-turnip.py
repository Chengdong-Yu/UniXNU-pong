#!/usr/bin/env python3
"""Cross-build the Darwin Turnip library for Pong's iOS16.6.1 userland.

Mesa must already contain the UniXNU Darwin backend patches. This compiles
the library only: it does not install an ICD, publish a GPU, run a workload,
generate shader packs, hash images or contact the phone.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mesa', type=Path, required=True)
    p.add_argument('--abi-directory', type=Path, required=True)
    p.add_argument('--tools-directory', type=Path, required=True)
    p.add_argument('--venv', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--developer-dir', type=Path, default=Path(
        '/Volumes/PongDev/Applications/Xcode.app/Contents/Developer'))
    a = p.parse_args()
    backend = a.mesa / 'src/freedreno/vulkan/tu_knl_darwin.cc'
    if 'MIN2(tu_get_system_heap_size(device), info->va_size)' not in backend.read_text():
        subprocess.run(['git', 'apply', str(ROOT / 'patches/mesa/0001-pong-scoped-gpu-heap.patch')],
                       cwd=a.mesa, check=True)
    if 'darwin_reclaim_for_allocation' not in backend.read_text():
        subprocess.run(['git', 'apply', str(ROOT / 'patches/mesa/0002-pong-allocation-pressure-reclaim.patch')],
                       cwd=a.mesa, check=True)
    out = a.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DEVELOPER_DIR=str(a.developer_dir),
               IPHONEOS_DEPLOYMENT_TARGET='16.6.1')
    env['PATH'] = ':'.join([str(a.tools_directory), str(a.venv/'bin'),
        '/Users/desktop/Downloads/Minecraft-Zink-NVK/build-tools/bin',
        '/opt/local/bin', env['PATH']])
    # Do not inherit another Vulkan/IR3 build's diagnostic or compiler knobs.
    for key in list(env):
        if key.startswith(('IR3_', 'FD_MESA_', 'NIR_', 'MESA_SHADER_', 'TU_')):
            del env[key]

    def sdk_read(*args):
        return subprocess.check_output(['xcrun', '--sdk', 'iphoneos', *args],
            env=env, text=True).strip()

    sdk = sdk_read('--show-sdk-path')
    flags = ['-target', 'arm64-apple-ios16.6.1', '-isysroot', sdk,
             '-fstack-protector-strong']
    cross = out/'pong-ios16.cross'
    cross.write_text('[binaries]\n' + ''.join(
        name+' = '+repr(sdk_read('--find', tool))+'\n'
        for name, tool in [('c', 'clang'), ('cpp', 'clang++'), ('objc', 'clang'),
                           ('ar', 'ar'), ('strip', 'strip')]) +
        "pkg-config = '/usr/bin/false'\n"
        "[host_machine]\nsystem = 'darwin'\ncpu_family = 'aarch64'\n"
        "cpu = 'arm64'\nendian = 'little'\n"
        "[properties]\nneeds_exe_wrapper = true\n[built-in options]\n" +
        ''.join(key+' = '+repr(flags)+'\n' for key in
            ['c_args', 'cpp_args', 'objc_args', 'c_link_args', 'cpp_link_args',
             'objc_link_args']))
    env.update(CC=sdk_read('--find', 'clang'), CXX=sdk_read('--find', 'clang++'),
               CFLAGS='', CXXFLAGS='', CPPFLAGS='', LDFLAGS='')
    options = ['-Dgallium-drivers=', '-Dvulkan-drivers=freedreno',
        '-Dfreedreno-kmds=darwin', '-Dtu-darwin-abi-dir='+str(a.abi_directory.resolve()),
        '-Dplatforms=macos', '-Dglx=disabled', '-Degl=disabled', '-Dgbm=disabled',
        '-Dopengl=false', '-Dgles1=disabled', '-Dgles2=disabled', '-Dllvm=disabled',
        '-Dshared-glapi=disabled', '-Dxmlconfig=disabled', '-Dshader-cache=disabled',
        '-Dtools=', '-Dbuild-tests=false', '-Dvideo-codecs=', '-Dvulkan-layers=',
        '-Dlibunwind=disabled', '-Dlmsensors=disabled', '-Dvalgrind=disabled',
        '-Dexpat=disabled', '-Dzstd=disabled', '-Dbuildtype=release', '-Db_ndebug=true',
        # No raw offline variant pack is accepted or generated in this build.
        # Normal runtime IR3 compilation remains available; empty source ID
        # leaves the optional pack exporter inactive rather than relabelling it.
        '-Dtu-builtin-source-id=', '-Dtu-builtin-pack-header=']
    meson = str(a.venv/'bin/meson')
    command = ([meson, 'configure', str(out)] if (out/'build.ninja').exists()
        else [meson, 'setup', str(out), str(a.mesa.resolve()),
              '--wrap-mode=nodownload', '--cross-file', str(cross)]) + options
    with (out/'configure.log').open('w') as log:
        result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit('Turnip configure failed: '+str(out/'configure.log'))
    build = ['/opt/local/bin/ninja', '-C', str(out), '-j6']
    with (out/'build.log').open('w') as log:
        result = subprocess.run(build, env=env, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit('Turnip build failed: '+str(out/'build.log'))
    library = out/'src/freedreno/vulkan/libvulkan_freedreno.dylib'
    (out/'build-report.json').write_text(json.dumps(dict(library=str(library),
        deployment='iOS16.6.1', cpu_page_bytes=4096, chip_id='0x07030002',
        automatic_install=False, hardware_executed=False, commands=[command, build],
        image_verification='skipped by request'), indent=2)+'\n')
    print(library)


if __name__ == '__main__':
    main()
