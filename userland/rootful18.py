#!/usr/bin/env python3
"""Stage an official rootful bootstrap without executing any guest binaries.

Local inputs and generated Apple/Procursus files stay outside version control.
The guest performs the real APFS remount and package configuration after boot.
"""
import argparse
import importlib.util
import io
import json
import lzma
import os
from pathlib import Path, PurePosixPath
import plistlib
import shutil
import subprocess
import tarfile

PROJECT=Path(__file__).resolve().parents[1]
BOOTSTRAP='https://apt.procurs.us/bootstraps/iphoneos-arm64/1700/bootstrap-iphoneos-arm.tar.zst'
SILEO='https://github.com/Sileo/Sileo/releases/download/2.5.1/org.coolstar.sileo_2.5.1_iphoneos-arm.deb'
REPO='https://apt.procurs.us/dists/iphoneos-arm64/1700/'


def extract(archive, root):
    links=[]
    for member in archive:
        name=PurePosixPath(member.name)
        if name.is_absolute() or '..' in name.parts:raise RuntimeError('unsafe archive member')
        parts=tuple(p for p in name.parts if p!='.')
        if not parts:continue
        path=root.joinpath(*parts)
        if any(p.is_symlink() for p in (path,*path.parents) if p!=root.parent):
            raise RuntimeError('archive traverses link: '+str(name))
        if member.issym() or member.islnk():links.append((path,member));continue
        if member.isdir():path.mkdir(parents=True,exist_ok=True)
        elif member.isfile():
            path.parent.mkdir(parents=True,exist_ok=True)
            with archive.extractfile(member) as src,path.open('wb') as dst:shutil.copyfileobj(src,dst)
        else:raise RuntimeError('special file in bootstrap: '+str(name))
        os.chmod(path,member.mode&0o7777)
    for path,member in links:
        if any(p.is_symlink() for p in path.parents):raise RuntimeError('link parent')
        if member.issym():path.symlink_to(member.linkname)
        else:
            name=PurePosixPath(member.linkname)
            if name.is_absolute() or '..' in name.parts:raise RuntimeError('unsafe hardlink')
            src=root.joinpath(*name.parts)
            if src.is_symlink() or not src.is_file():raise RuntimeError('hardlink target')
            shutil.copyfile(src,path);os.chmod(path,member.mode&0o7777)


def module(path, name):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result)
    return result


def stage(a):
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    root=out/'root-overlay';root.mkdir()
    sign=module(PROJECT/'scripts/prepare-pong-sileo.py','sileo_sign')
    process=subprocess.Popen(['zstd','-dc',str(a.bootstrap)],stdout=subprocess.PIPE)
    with tarfile.open(fileobj=process.stdout,mode='r|') as archive:extract(archive,root)
    if process.wait():raise RuntimeError('bootstrap decompression failed')
    data=subprocess.check_output(['ar','-p',str(a.sileo),'data.tar.xz'])
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:extract(archive,root)
    control=subprocess.check_output(['ar','-p',str(a.sileo),'control.tar.xz'])
    with tarfile.open(fileobj=io.BytesIO(control)) as archive:
        text=archive.extractfile(next(m for m in archive if m.name.lstrip('./')=='control')).read().decode()
        if '\nArchitecture: iphoneos-arm\n' not in text:raise RuntimeError('rootful Sileo required')
        db=root/'Library/dpkg';(db/'info').mkdir(exist_ok=True)
        with (db/'status').open('a') as dst:dst.write('\n'+text.rstrip()+'\nStatus: install ok unpacked\n\n')
        for member in archive:
            name=member.name.lstrip('./')
            if name in ('preinst','postinst','prerm','postrm','conffiles','triggers') and member.isfile():
                target=db/'info'/('org.coolstar.sileo.'+name)
                target.write_bytes(archive.extractfile(member).read());os.chmod(target,member.mode&0o7777)
    app=root/'Applications/Sileo.app'
    (db/'info/org.coolstar.sileo.list').write_text('\n'.join('/'+str(p.relative_to(root)) for p in (app,*app.rglob('*')))+'\n')
    # Keep genuine architecture/suite and real indices, not an empty file
    # with the obsolete rootless name from the previous Data environment.
    apt=root/'private/etc/apt'
    (apt/'sources.list.d/procursus.sources').write_text('Types: deb\nURIs: https://apt.procurs.us/\nSuites: iphoneos-arm64/1700\nComponents: main\n')
    (apt/'apt.conf.d/99pong-indexes').write_text('Acquire::GzipIndexes "false";\nAcquire::Retries "2";\nAcquire::http::Timeout "20";\nAcquire::https::Timeout "20";\n')
    lists=root/'private/var/lib/apt/lists';lists.mkdir(parents=True,exist_ok=True)
    prefix='apt.procurs.us_dists_iphoneos-arm64_1700_'
    (lists/(prefix+'main_binary-iphoneos-arm_Packages')).write_bytes(lzma.decompress(a.packages.read_bytes()))
    shutil.copyfile(a.release,lists/(prefix+'Release'))
    shutil.copyfile(a.release_signature,lists/(prefix+'Release.gpg'))
    (root/'private/var/lib/apt/sileolists').mkdir(exist_ok=True)
    # Preserve the actual Sileo bundle entitlements before signing nested code.
    (out/'app-entitlements.plist').write_bytes(plistlib.dumps(sign.entitlements((app/'Sileo').read_bytes()) or {}))
    signed=sign.sign_tree(root,out)
    (root/'.procursus_strapped').write_text('Pong native Darwin24 rootful\n')
    (root/'.pong-rootful-staged').write_text('22H374 rootful-v1\n')
    # /private/var is a separate, existing UFS Data volume on the phone.
    # Seed only missing files there; never replace the user's Data image.
    def guest_owner(info):
        info.uid=info.gid=0;info.uname='root';info.gname='wheel'
        return info
    with tarfile.open(out/'rootful-data.tar','w') as archive:
        for item in sorted((root/'private/var').iterdir()):
            archive.add(item,arcname=item.name,recursive=True,filter=guest_owner)
    (out/'rootful-stage.json').write_text(json.dumps(dict(bootstrap=BOOTSTRAP,sileo=SILEO,
        repository=REPO,architecture='iphoneos-arm',signed_macho_files=signed,
        guest_scripts_executed=False,root_rw_observed=False,apt_install_observed=False),indent=2)+'\n')
    print(out/'rootful-stage.json')


def seed_firmware_version(root):
    """Describe the installed guest OS to dpkg without probing absent hardware.

    firmware is the bootstrap's version-only virtual package, not a driver.
    Derive it from the mounted guest's real SystemVersion.plist. Leave every
    CPU/board/MobileGestalt capability to the native firmware tool.
    """
    if os.geteuid()!=0 or root==Path('/') or not root.is_mount():
        raise RuntimeError('mounted local guest and root ownership required')
    version_path=root/'System/Library/CoreServices/SystemVersion.plist'
    status=root/'Library/dpkg/status'
    listing=root/'Library/dpkg/info/firmware.list'
    if any(p.is_symlink() or not p.resolve().is_relative_to(root.resolve())
           for p in (version_path,status,listing)):
        raise RuntimeError('unexpected rootful metadata symlink')
    version=plistlib.loads(version_path.read_bytes())
    release=version['ProductVersion']
    if version.get('ProductBuildVersion')!='22H374' or not all(c.isdigit() or c=='.' for c in release):
        raise RuntimeError('expected actual 22H374 guest version')
    paragraphs=[p.strip() for p in status.read_text().split('\n\n') if p.strip()]
    existing=[p for p in paragraphs if p.splitlines()[0]=='Package: firmware']
    if existing:
        # Preserve an actual guest-generated record if it already agrees.
        if len(existing)!=1 or 'Version: '+release not in existing[0].splitlines():
            raise RuntimeError('conflicting installed firmware version record')
        if any(line.startswith('Maintainer:') for line in existing[0].splitlines()):
            return dict(version=release,source='existing guest package record')
        if 'Description: Installed guest OS version from SystemVersion.plist' not in existing[0].splitlines():
            raise RuntimeError('preserving a foreign incomplete firmware package record')
        paragraphs.remove(existing[0])
    paragraphs.append('\n'.join(['Package: firmware','Name: iOS Firmware',
        'Version: '+release,'Architecture: all','Essential: yes',
        'Priority: required','Section: System','Installed-Size: 0',
        'Maintainer: UniXNU Pong system integration','Status: install ok installed',
        'Description: Installed guest OS version from SystemVersion.plist',
        'Tag: role::cydia']))
    status.write_text('\n\n'.join(paragraphs)+'\n\n')
    os.chown(status,0,0);os.chmod(status,0o644)
    if not listing.exists():listing.write_text('/.\n')
    os.chown(listing,0,0);os.chmod(listing,0o644)
    return dict(version=release,build=version['ProductBuildVersion'],
        source='installed guest SystemVersion.plist',hardware_capabilities_added=False)


def install(root, stage, runtime):
    """Merge into a mounted local OS; never follow guest symlinks to macOS."""
    if os.geteuid()!=0 or not root.is_mount() or root==Path('/'):
        raise RuntimeError('explicit mounted OS and numeric root ownership required')
    builder=module(PROJECT/'userland/build-ipados18.py','rootful_builder')
    payload=stage/'root-overlay';report=dict(replaced=[],preserved=[])
    app=root/'Applications/Sileo.app'
    if app.is_symlink():raise RuntimeError('unexpected Sileo application link')
    if app.exists():
        backup=root/'pong/rootful-stock/Applications/Sileo.app'
        if not backup.exists():
            backup.parent.mkdir(parents=True,exist_ok=True);shutil.copytree(app,backup,symlinks=True)
        shutil.rmtree(app)
    protected={'bin/launchctl','usr/bin/launchctl','sbin/launchd','sbin/mount',
        'sbin/mount_apfs','usr/lib/dyld','private/etc/localtime','private/etc/resolv.conf'}
    paths=sorted(payload.rglob('*'),key=lambda p:(len(p.parts),str(p)))
    links=[]
    for source in paths:
        relative=source.relative_to(payload);name=relative.as_posix()
        if name=='private/var' or name.startswith('private/var/') or name=='prep_bootstrap.sh':continue
        # Resolve only destination PARENTS using the guest resolver. Leaves
        # are replaced themselves, never a symlink's host-absolute target.
        parent=builder.source_path(root,root, '/'+relative.parent.as_posix())
        if parent is None or not parent.is_relative_to(root):raise RuntimeError('unmapped rootful parent')
        parent.mkdir(parents=True,exist_ok=True);target=parent/source.name
        exists=os.path.lexists(target)
        if exists and (name in protected or name.startswith('usr/lib/system/') or
                       (name.startswith('usr/lib/') and not source.is_dir())):
            report['preserved'].append(name);continue
        if source.is_dir() and not source.is_symlink():
            if target.is_symlink() or (exists and not target.is_dir()):raise RuntimeError('directory collision: '+name)
            target.mkdir(exist_ok=True);os.chown(target,0,0);continue
        if source.is_symlink():links.append((source,target));continue
        if exists:
            backup=root/'pong/rootful-stock'/relative
            if not os.path.lexists(backup):
                backup.parent.mkdir(parents=True,exist_ok=True)
                if target.is_symlink():backup.symlink_to(os.readlink(target))
                else:shutil.copy2(target,backup)
            target.unlink();report['replaced'].append(name)
        shutil.copy2(source,target);os.chown(target,0,0);os.chmod(target,source.stat().st_mode&0o7777)
    for source,target in links:
        if os.path.lexists(target):target.unlink()
        target.symlink_to(os.readlink(source));os.chown(target,0,0,follow_symlinks=False)
    shutil.copyfile(runtime/'sileo-root',app/'giveMeRoot')
    os.chown(app/'giveMeRoot',0,0);os.chmod(app/'giveMeRoot',0o4755)
    builder.run(['codesign','--force','--sign','-','--pagesize','4096','--entitlements',stage/'app-entitlements.plist',app])
    for source in (stage/'rootful-data.tar',PROJECT/'userland/sileo-rootful-firstboot.sh'):
        target=root/'pong'/source.name;shutil.copyfile(source,target);os.chown(target,0,0);os.chmod(target,0o644)
    label='dev.unixnu.pong.sileo-firstboot';path='/System/Library/LaunchDaemons/'+label+'.plist'
    job=dict(Label=label,ProgramArguments=['/usr/bin/dash','/pong/sileo-rootful-firstboot.sh'],
        UserName='root',RunAtLoad=True,LaunchOnlyOnce=True,ProcessType='Background',
        StandardOutPath='/private/var/logs/pong-sileo-rootful.log',
        StandardErrorPath='/private/var/logs/pong-sileo-rootful.log',
        EnvironmentVariables=dict(PATH='/usr/bin:/bin:/usr/sbin:/sbin'))
    builder.put_plist(root/path.lstrip('/'),job)
    cache_path=root/'System/Library/xpc/launchd.plist';cache=plistlib.loads(cache_path.read_bytes())
    cache['LaunchDaemons'][path]=job;builder.put_plist(cache_path,cache)
    for item in (root/path.lstrip('/'),cache_path):os.chown(item,0,0);os.chmod(item,0o644)
    report['firmware_version']=seed_firmware_version(root)
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for flag in ('out','bootstrap','sileo','packages','release','release-signature'):
        p.add_argument('--'+flag,type=Path,required=True)
    stage(p.parse_args())
