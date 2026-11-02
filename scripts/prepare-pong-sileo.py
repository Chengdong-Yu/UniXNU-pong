#!/usr/bin/env python3
"""Stage Sileo/Procursus on writable Data for Pong 22H374; no device access.

stage downloads official packages into an ignored directory, extracts only
var/jb, signs Mach-O files with 4 KiB pages and builds the Pong root helper.
install copies the staged tree to an explicitly mounted local Data volume.
It never executes guest package scripts on macOS or mounts a phone partition.
"""
import argparse
import io
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import shutil
import stat
import struct
import subprocess
import tarfile
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
BOOTSTRAP='https://apt.procurs.us/bootstraps/1900/bootstrap-iphoneos-arm64.tar.zst'
SILEO='https://github.com/Sileo/Sileo/releases/download/2.5.1/org.coolstar.sileo_2.5.1_iphoneos-arm64.deb'

def extract(archive, destination):
    """Extract directories/files first, guest symlinks last; never traverse one."""
    links=[]
    for member in archive:
        name=PurePosixPath(member.name)
        if name.is_absolute() or '..' in name.parts:raise RuntimeError('archive path: '+member.name)
        parts=tuple(p for p in name.parts if p!='.')
        if not parts or parts in (('var',),('var','jb')):continue
        if parts[:2]!=('var','jb'):raise RuntimeError('non-rootless archive: '+member.name)
        path=destination.joinpath(*parts[2:])
        if any(p.is_symlink() for p in (path,*path.parents) if p!=destination.parent):
            raise RuntimeError('refusing archive traversal through symlink: '+member.name)
        if member.issym() or member.islnk():links.append((path,member));continue
        if member.isdir():path.mkdir(parents=True,exist_ok=True);os.chmod(path,member.mode&0o7777)
        elif member.isfile():
            path.parent.mkdir(parents=True,exist_ok=True)
            with archive.extractfile(member) as source,path.open('wb') as output:shutil.copyfileobj(source,output)
            os.chmod(path,member.mode&0o7777)
        else:raise RuntimeError('unsupported archive entry: '+member.name)
    for path,member in links:
        if any(p.is_symlink() for p in path.parents):raise RuntimeError('link parent: '+member.name)
        if member.issym():
            if os.path.lexists(path):path.unlink()
            path.symlink_to(member.linkname)
        else:
            target=PurePosixPath(member.linkname)
            if target.is_absolute() or '..' in target.parts or target.parts[:2]!=('var','jb'):
                raise RuntimeError('hardlink outside rootless tree: '+member.linkname)
            source=destination.joinpath(*target.parts[2:])
            if source.is_symlink() or not source.is_file():raise RuntimeError('unresolved hardlink')
            shutil.copyfile(source,path);os.chmod(path,member.mode&0o7777)

def entitlements(data):
    """Preserve original XML entitlements while generating new signatures."""
    start=0
    if data[:4] in (b'\xca\xfe\xba\xbe',b'\xca\xfe\xba\xbf'):
        wide=data[3]==0xbf;count=struct.unpack_from('>I',data,4)[0]
        for i in range(count):
            at=8+i*(32 if wide else 20)
            if struct.unpack_from('>I',data,at)[0]==0x100000c:
                start=struct.unpack_from('>Q' if wide else '>I',data,at+8)[0];break
    if data[start:start+4]!=b'\xcf\xfa\xed\xfe':return None
    n=struct.unpack_from('<I',data,start+16)[0];at=start+32
    for _ in range(n):
        command,size=struct.unpack_from('<II',data,at)
        if command==0x1d:
            offset,bytes_=struct.unpack_from('<II',data,at+8);base=start+offset
            if bytes_<12 or base+bytes_>len(data):raise RuntimeError('invalid signature extent')
            magic,length,count=struct.unpack_from('>III',data,base)
            if magic!=0xfade0cc0:return None
            for i in range(count):
                slot,relative=struct.unpack_from('>II',data,base+12+8*i)
                if slot==5:
                    tag,length=struct.unpack_from('>II',data,base+relative)
                    if tag==0xfade7171:return plistlib.loads(data[base+relative+8:base+relative+length])
        at+=size
    return None

def sign_tree(root,out):
    count=0
    for p in sorted(root.rglob('*')):
        if p.is_symlink() or not p.is_file():continue
        with p.open('rb') as f:magic=f.read(4)
        if magic not in (b'\xcf\xfa\xed\xfe',b'\xca\xfe\xba\xbe',b'\xca\xfe\xba\xbf'):continue
        e=entitlements(p.read_bytes());args=['codesign','--force','--sign','-','--pagesize','4096']
        if e:
            tmp=out/'sign-entitlements.plist';tmp.write_bytes(plistlib.dumps(e));args+=['--entitlements',str(tmp)]
        subprocess.run(args+[str(p)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        count+=1
    return count

def stage(a):
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    prefix=out/'data-overlay/jb'
    if prefix.exists():raise RuntimeError('choose a fresh output directory; existing stage retained')
    inputs=out/'inputs';inputs.mkdir(exist_ok=True)
    for url,name,existing in [(BOOTSTRAP,'bootstrap.tar.zst',a.bootstrap),(SILEO,'sileo.deb',a.sileo)]:
        target=inputs/name
        if existing:shutil.copyfile(existing,target)
        else:
            partial=target.with_suffix(target.suffix+'.partial');urllib.request.urlretrieve(url,partial);partial.rename(target)
    prefix.mkdir(parents=True)
    process=subprocess.Popen(['zstd','-dc',str(inputs/'bootstrap.tar.zst')],stdout=subprocess.PIPE)
    with tarfile.open(fileobj=process.stdout,mode='r|') as archive:extract(archive,prefix)
    if process.wait():raise RuntimeError('bootstrap decompression failed')
    # The bootstrap contains aliases like var/lib/dpkg. A second archive
    # writes Applications only, and must never traverse existing aliases.
    for section in ('data.tar.xz','control.tar.xz'):
        data=subprocess.check_output(['ar','-p',str(inputs/'sileo.deb'),section])
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            if section=='data.tar.xz':extract(archive,prefix)
            else:
                control=next(m for m in archive if m.name.lstrip('./')=='control')
                text=archive.extractfile(control).read().decode()
    # Keep dpkg's actual package state: Sileo is unpacked until the guest
    # finishes configuration. No postinst runs on this host.
    db=prefix/'Library/dpkg';(db/'info').mkdir(exist_ok=True)
    with (db/'status').open('a') as f:f.write('\n'+text.rstrip()+'\nStatus: install ok unpacked\n\n')
    (db/'info/org.coolstar.sileo.list').write_text('\n'.join('/var/jb/'+str(p.relative_to(prefix)) for p in (prefix/'Applications/Sileo.app').rglob('*'))+'\n')
    (db/'info/org.coolstar.sileo.postinst').write_text('#!/var/jb/bin/sh\nexit 0\n')
    os.chmod(db/'info/org.coolstar.sileo.postinst',0o755)
    # Rootless aliases in the archive are guest absolute paths. Do not use
    # Path.resolve or follow them while writing the host staging directory.
    for p in ('usr/bin/dash','usr/bin/apt','usr/bin/dpkg','usr/bin/uicache','Applications/Sileo.app/Sileo'):
        if not (prefix/p).is_file() or (prefix/p).is_symlink():raise RuntimeError('missing payload: '+p)
    shutil.copyfile(ROOT/'userland/sileo-firstboot.sh',prefix/'pong-firstboot.sh')
    os.chmod(prefix/'pong-firstboot.sh',0o755)
    for name in ('.procursus_strapped','.pong-sileo-staged'):(prefix/name).write_text('Pong 22H374 rootless\n')
    env=dict(os.environ,DEVELOPER_DIR=str(a.developer))
    sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],env=env,text=True).strip()
    helper=prefix/'Applications/Sileo.app/giveMeRoot'
    subprocess.run(['xcrun','clang','-target','arm64-apple-ios18.0','-isysroot',sdk,'-O2','-Wall','-Wextra',str(ROOT/'userland/sileo-root.c'),'-o',str(helper)],check=True,env=env)
    helper_ent=out/'root-helper-entitlements.plist'
    helper_ent.write_bytes(plistlib.dumps({'platform-application':True,'com.apple.private.security.no-container':True}))
    count=sign_tree(prefix,out)
    subprocess.run(['codesign','--force','--sign','-','--pagesize','4096','--entitlements',str(helper_ent),str(helper)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    os.chmod(helper,0o4755)
    app=prefix/'Applications/Sileo.app'
    # Seal the app bundle after replacing/signing its nested helper.
    app_ent=out/'app-entitlements.plist';app_ent.write_bytes(plistlib.dumps(entitlements((app/'Sileo').read_bytes()) or {}))
    subprocess.run(['codesign','--force','--sign','-','--pagesize','4096','--entitlements',str(app_ent),str(app)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    report={'version':'2.5.1','bootstrap':BOOTSTRAP,'sileo':SILEO,'data_path':'/private/var/jb','signed_macho_files':count,'signature_page_bytes':4096,'guest_scripts_executed':False,'runtime_qualified':False,'device_written':False}
    (out/'sileo-stage.json').write_text(json.dumps(report,indent=2)+'\n')
    print(out/'sileo-stage.json')

def install(a):
    root=a.data_root.resolve();stage_root=a.out.resolve()/'data-overlay/jb'
    if os.geteuid()!=0:raise RuntimeError('numeric root ownership needs system authentication')
    if not stage_root.is_dir() or not (a.out/'sileo-stage.json').is_file():raise RuntimeError('complete stage required')
    if not root.is_mount() or root==Path('/'):raise RuntimeError('explicit mounted local Data volume required')
    target=root/'jb'
    if os.path.lexists(target):raise RuntimeError('existing /var/jb retained; refusing replacement')
    shutil.copytree(stage_root,target,symlinks=True)
    for p in (target,*target.rglob('*')):
        os.chown(p,0,0,follow_symlinks=False)
        # chown clears setuid on macOS; restore original modes afterwards.
        if not p.is_symlink():os.chmod(p,(stage_root/p.relative_to(target)).stat().st_mode&0o7777)
    (a.out/'sileo-install.json').write_text(json.dumps({'data_root':str(root),'installed':True,'device_written':False,'runtime_qualified':False},indent=2)+'\n')
    print(target)

def embed(a):
    """Install in the prepared local image copies, retaining original images."""
    import importlib.util
    spec=importlib.util.spec_from_file_location('pong_userland18',ROOT/'userland/build-ipados18.py')
    builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
    prepared=a.prepared.resolve();runtime=a.runtime.resolve()
    plan=json.loads((prepared/'build-plan.json').read_text())
    if plan['version'].get('ProductBuildVersion')!='22H374':raise RuntimeError('22H374 required')
    info=json.loads((runtime/'helper.json').read_text())
    if info.get('runtime_path')!='/pong/init':raise RuntimeError('matching static init required')
    data=prepared/'pong-ipados18-data.cdr'
    if a.fresh_data:
        data=a.out/'PongSileoData18.sparseimage'
        if data.exists():raise RuntimeError('existing Sileo Data candidate retained')
        shutil.copyfile(plan['data_source'],data)
    if data.is_symlink() or data.samefile(plan['data_source']):raise RuntimeError('Data copy required')
    os_image=builder.os_copy(prepared)
    if os_image.samefile(plan['os_source']):raise RuntimeError('OS copy required')
    if os.geteuid()!=0:raise RuntimeError('image ownership needs system authentication')
    mount=a.out/'data-mount';device=builder.attach(data,mount)
    try:
        if a.fresh_data:
            builder.merge_tree(prepared/'data-root',mount)
            metadata=json.loads((prepared/'data-metadata.json').read_text())
            for rel,(mode,uid,gid) in metadata.items():
                target=mount/rel
                os.chown(target,uid,gid,follow_symlinks=False)
                if not stat.S_ISLNK(mode):os.chmod(target,mode&0o7777)
            for rel,mode,uid,gid in builder.EXTRA:
                target=mount/rel;target.mkdir(parents=True,exist_ok=True)
                os.chown(target,uid,gid);os.chmod(target,mode)
        class InstallArgs:pass
        args=InstallArgs();args.out=a.out;args.data_root=mount
        install(args)
    finally:builder.run(['hdiutil','detach',device])
    mount=a.out/'os-mount';device=builder.attach(os_image,mount)
    try:
        for rel in ('pong/init','sbin/launchd'):
            target=mount/rel
            if target.is_symlink() or not target.is_file():raise RuntimeError('unexpected init target')
            backup=a.out/(rel.replace('/','-')+'.previous')
            if not backup.exists():shutil.copyfile(target,backup)
            shutil.copyfile(runtime/'init',target);os.chown(target,0,0);os.chmod(target,0o755)
        for name in ('sileo-register','sileo-firstboot.sh'):
            target=mount/'pong'/name
            if target.is_symlink():raise RuntimeError('unexpected Sileo runtime symlink')
            shutil.copyfile(runtime/name,target);os.chown(target,0,0);os.chmod(target,0o755)
        builder.stage_sileo_job(mount)
        builder.stage_sileo_application(mount,a.out.resolve(),runtime)
    finally:builder.run(['hdiutil','detach',device])
    (a.out/'sileo-embed.json').write_text(json.dumps({'os_image':str(os_image),'data_image':str(data),
        'runtime':str(runtime),'local_images_updated':True,'device_written':False,'runtime_qualified':False},indent=2)+'\n')

def main():
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='mode',required=True)
    s=sub.add_parser('stage');s.add_argument('--out',type=Path,required=True)
    s.add_argument('--bootstrap',type=Path);s.add_argument('--sileo',type=Path)
    s.add_argument('--developer',type=Path,required=True)
    s=sub.add_parser('install');s.add_argument('--out',type=Path,required=True);s.add_argument('--data-root',type=Path,required=True)
    s=sub.add_parser('embed');s.add_argument('--out',type=Path,required=True)
    s.add_argument('--prepared',type=Path,required=True);s.add_argument('--runtime',type=Path,required=True)
    s.add_argument('--fresh-data',action='store_true',help='create a separate Sileo Data seed from the original template; retain current Data')
    a=p.parse_args()
    try:{'stage':stage,'install':install,'embed':embed}[a.mode](a)
    except subprocess.CalledProcessError as error:
        if error.stderr:print(error.stderr.decode(errors='replace') if isinstance(error.stderr,bytes) else error.stderr)
        raise
if __name__=='__main__':main()
