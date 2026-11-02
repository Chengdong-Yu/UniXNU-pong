"""Import native ringtone resources from the local, matching iPhone restore."""
from pathlib import Path
import os
import plistlib
import shutil
import subprocess


def stage_software_catalog(root, work):
    """Keep native tone IDs/paths, transcode their audio to software ALAC.

    AudioToolbox's internal tone players need not call interposable converter
    entry points. A real ALAC track in the existing M4R container removes the
    unimplemented Apple hardware AAC dependency without a substitute player.
    Original matching restore resources remain in the local work directory.
    """
    root, work = Path(root), Path(work)
    if os.geteuid():
        raise RuntimeError('native ringtone staging requires system ownership')
    directory = root / 'Library/Ringtones'
    tones = sorted(directory.rglob('*.m4r'))
    if not tones:
        raise RuntimeError('installed native ringtone catalog is missing')
    original = work / 'originals'
    encoded = work / 'software'
    original.mkdir(parents=True, exist_ok=True)
    encoded.mkdir(parents=True, exist_ok=True)
    records = []
    for tone in tones:
        if tone.is_symlink() or not tone.is_file():
            raise RuntimeError('unexpected ringtone resource alias: ' + str(tone))
        relative = tone.relative_to(directory)
        backup, output = original / relative, encoded / relative
        backup.parent.mkdir(parents=True, exist_ok=True)
        output.parent.mkdir(parents=True, exist_ok=True)
        if not backup.exists():
            shutil.copyfile(tone, backup)
        subprocess.run(['/usr/bin/afconvert', '-f', 'm4af', '-d', 'alac',
                        str(backup), str(output)], check=True)
        temporary = tone.with_name(tone.name + '.pong-software')
        shutil.copyfile(output, temporary)
        os.chown(temporary, 0, 0)
        os.chmod(temporary, 0o644)
        os.replace(temporary, tone)
        records.append(relative.as_posix())
    return dict(build='22H374', audio_format='Apple Lossless in native M4R',
                converted=records, native_catalog_paths_preserved=True,
                native_player_preserved=True, image_verification='not used')


def stage(root, donor):
    root, donor = Path(root), Path(donor)
    version = Path('System/Library/CoreServices/SystemVersion.plist')
    for base in (root, donor):
        info = plistlib.loads((base / version).read_bytes())
        if (info.get('ProductVersion'), info.get('ProductBuildVersion')) != ('18.7.10', '22H374'):
            raise RuntimeError('ringtone resources require matching 22H374 restores')
    if os.geteuid():
        raise RuntimeError('native resource staging requires system ownership')
    allowed = {'.m4r', '.caf', '.plist', '.strings', '.loctable', '.aupreset'}
    copied, retained = [], 0
    roots = ('Library/Ringtones', 'System/Library/Audio/UISounds',
             'System/Library/PrivateFrameworks/ToneLibrary.framework')
    for relative in roots:
        source = donor / relative
        if not source.is_dir():
            raise RuntimeError('missing native ringtone resource directory: ' + relative)
        for item in source.rglob('*'):
            if item.is_symlink() or not item.is_file() or item.suffix not in allowed:
                continue
            name = item.relative_to(source)
            if name.name == 'Info.plist' or any(p in name.parts for p in ('_CodeSignature', 'XPCServices')):
                continue
            target = root / relative / name
            if target.is_symlink():
                raise RuntimeError('unexpected ringtone resource alias: ' + str(target))
            # Preserve working iPad OS resources. Add the phone's missing
            # tones/localizations without replacing a framework or service.
            if target.is_file() and target.stat().st_size:
                retained += 1
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(item, target)
            os.chown(target, 0, 0)
            os.chmod(target, 0o644)
            copied.append((Path(relative) / name).as_posix())
    tones = list((root / 'Library/Ringtones').glob('*.m4r'))
    if not tones:
        raise RuntimeError('native ringtone directory is empty')
    return dict(build='22H374', copied=copied, existing_resources_retained=retained,
                available_ringtones=len(tones), native_code_replaced=False)
