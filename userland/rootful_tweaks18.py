#!/usr/bin/env python3
"""Stage the real arm64/rootful ElleKit runtime and Pong's native loader.

The official package remains a local input. No Apple files or guest data
are downloaded/copied here. A supplied mounted guest is required to install.
"""
import argparse
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

PROJECT=Path(__file__).resolve().parents[1]
UPSTREAM='https://ellekit.space/pool/ellekit_1.2_iphoneos-arm.deb'
VERSION='1.2+pong1'


def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module


def prepare(package,out):
    if out.exists():raise RuntimeError('use a new staging directory')
    out.mkdir(parents=True);root=out/'root-overlay';root.mkdir()
    base=load('rootful_extract',PROJECT/'userland/rootful18.py')
    raw=subprocess.check_output(['ar','-p',str(package),'data.tar.xz'])
    with tarfile.open(fileobj=io.BytesIO(raw)) as archive:base.extract(archive,root)
    raw=subprocess.check_output(['ar','-p',str(package),'control.tar.xz'])
    control=out/'control';control.mkdir()
    with tarfile.open(fileobj=io.BytesIO(raw)) as archive:base.extract(archive,control)
    text=(control/'control').read_text()
    if 'Package: ellekit\n' not in text or 'Version: 1.2\n' not in text or 'Architecture: iphoneos-arm\n' not in text:
        raise RuntimeError('expected official rootful ElleKit 1.2')
    text=text.replace('Version: 1.2\n','Version: '+VERSION+'\n')
    (control/'control').write_text(text)
    # Retain the stock maintainer script for attribution/recovery. Pong
    # already has a kernel exec adapter; remote suspension of PID 1 is not
    # needed and could overwrite the board runtime's own injection rules.
    docs=root/'usr/share/doc/ellekit';docs.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(control/'postinst',docs/'postinst.upstream')
    (control/'postinst').write_text('''#!/bin/sh
if [ -f /.pong-rootful-staged ]; then
    exec /pong/tweak-refresh
fi
exec /bin/bash /usr/share/doc/ellekit/postinst.upstream "$@"
''')
    (control/'postrm').write_text('''#!/bin/sh
if [ -f /.pong-rootful-staged ] && [ -x /pong/tweak-refresh ]; then
    /pong/tweak-refresh
fi
exit 0
''')
    for p in (control/'postinst',control/'postrm'):p.chmod(0o755)
    # Only the installed runtime and package-owned libraries are staged.
    # /etc/rc.d's upstream remote injector is omitted: this port's init
    # composes the real injector with its own UI rule before launchd starts.
    (root/'etc/rc.d/ellekit-loader').unlink(missing_ok=True)
    config=root/'private/etc/pong-tweak-processes';config.parent.mkdir(parents=True,exist_ok=True)
    config.write_text('# Explicit app executable names; append additional tweak targets here.\nSpringBoard Preferences MobilePhone Sileo\n')
    sign=load('tweak_sign',PROJECT/'scripts/prepare-pong-sileo.py')
    sign.sign_tree(root,out)
    return out



def compile_refresh(runtime,developer):
    runtime.mkdir(parents=True,exist_ok=True)
    builder=load('tweak_builder',PROJECT/'userland/build-ipados18.py')
    env=dict(os.environ,DEVELOPER_DIR=str(developer))
    target=runtime/'tweak-refresh'
    builder.run(['xcrun','--sdk','iphoneos','clang','-target','arm64-apple-ios18.0',
        '-Os','-Wall','-Wextra','-Werror','-Wl,-no_fixup_chains',
        PROJECT/'userland/tweak-refresh.c','-o',target],env=env)
    builder.run(['codesign','--force','--sign','-','--pagesize','4096',
        '--entitlements',PROJECT/'userland/init-entitlements.plist',target])
    return target


def install(root,stage,runtime):
    root=root.resolve()
    if os.geteuid() or root==Path('/') or not root.is_mount() or not (root/'.pong-rootful-staged').is_file():
        raise RuntimeError('mounted local rootful guest and root ownership required')
    overlay=stage/'root-overlay';db=root/'Library/dpkg'
    status=db/'status';text=status.read_text()
    conflicts={'com.ex.substitute','org.coolstar.libhooker','science.xnu.substitute','mobilesubstrate','com.saurik.substrate.safemode'}
    paragraphs=[b.strip() for b in text.split('\n\n') if b.strip()]
    for block in paragraphs:
        name=next((l[9:] for l in block.splitlines() if l.startswith('Package: ')),None)
        if name in conflicts and 'Status: install ok installed' in block:raise RuntimeError('preserving installed injection framework: '+name)
    # Reuse rootful18's guest-path mapping. Absolute guest symlinks must
    # never resolve into the host's /usr, /Library or /private trees.
    def guest(path):
        relative=Path(path);target=root
        for component in relative.parts:
            candidate=target/component
            if candidate.is_symlink():
                link=os.readlink(candidate)
                target=root/link.lstrip('/') if link.startswith('/') else candidate.parent/link
                target=Path(os.path.normpath(target))
            else:target=candidate
            if not target.is_relative_to(root):raise RuntimeError('guest path escaped local mount')
        return target
    installed=[]
    for source in sorted(overlay.rglob('*'),key=lambda p:len(p.parts)):
        rel=source.relative_to(overlay);parent=guest(rel.parent)
        parent.mkdir(parents=True,exist_ok=True);target=parent/rel.name
        if source.is_dir() and not source.is_symlink():
            target=guest(rel) # /etc is the guest's /private/etc alias
            if not target.exists():target.mkdir()
            if target.is_symlink() or not target.is_dir():raise RuntimeError('conflicting directory: '+str(rel))
        else:
            if str(rel)=='private/etc/pong-tweak-processes' and target.exists():continue
            if target.is_dir() and not target.is_symlink():raise RuntimeError('conflicting package path: '+str(rel))
            target.unlink(missing_ok=True)
            if source.is_symlink():target.symlink_to(os.readlink(source))
            else:shutil.copyfile(source,target)
        os.chown(target,0,0,follow_symlinks=False)
        if not target.is_symlink():os.chmod(target,source.stat().st_mode&0o777)
        installed.append('/'+str(rel))
    target=root/'pong/tweak-refresh';shutil.copyfile(runtime/'tweak-refresh',target);os.chown(target,0,0);os.chmod(target,0o755)
    control=stage/'control';info=db/'info';info.mkdir(exist_ok=True)
    for name in ('postinst','postrm'):
        target=info/('ellekit.'+name);shutil.copyfile(control/name,target);os.chown(target,0,0);os.chmod(target,0o755)
    listing=info/'ellekit.list';listing.write_text('\n'.join(installed)+'\n');os.chown(listing,0,0);os.chmod(listing,0o644)
    (info/'ellekit.md5sums').unlink(missing_ok=True) # original signatures changed to native 4K; metadata is optional
    paragraphs=[b for b in paragraphs if not b.startswith('Package: ellekit\n')]
    paragraphs.append((control/'control').read_text().strip()+'\nStatus: install ok unpacked')
    status.write_text('\n\n'.join(paragraphs)+'\n\n');os.chown(status,0,0);os.chmod(status,0o644)
    return dict(package='ellekit',version=VERSION,architecture='iphoneos-arm',
        source=UPSTREAM,loader='kernel exec rule; explicit app targets',automatic_reboot=False)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--package',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--runtime',type=Path);p.add_argument('--developer',type=Path)
    a=p.parse_args()
    if bool(a.runtime)!=bool(a.developer):p.error('--runtime and --developer must be supplied together')
    prepare(a.package,a.out)
    if a.runtime:compile_refresh(a.runtime,a.developer)
