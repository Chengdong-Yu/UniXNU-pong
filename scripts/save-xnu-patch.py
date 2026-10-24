#!/usr/bin/env python3
"""Regenerate one incremental XNU patch from the working tree: save-xnu-patch.py NAME FILE...

The result is the difference between "pinned XNU + every patch that sorts before NAME" and the working tree, for the named
files only, written to patches/xnu/NAME. This Pong profile disables boot-progress
and offline GPU-addon instrumentation, so neither is part of the comparison baseline."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
XNU = ROOT / '.cache/xnu-work'
name, files = sys.argv[1], sys.argv[2:]
earlier = [p for p in sorted((ROOT / 'patches/xnu').glob('*.patch')) if p.name < name
           and p.name not in ('zzy-native-gpu-addon-mount.patch', 'zzz-boot-progress.patch')]


def git(tmp, *args, check=True):
    return subprocess.run(['git', '-c', 'user.email=a@b', '-c', 'user.name=n', *args], cwd=tmp, check=check,
                          capture_output=True, text=True).stdout


tmp = Path(tempfile.mkdtemp(dir=ROOT / '.cache'))
try:
    git(tmp, 'init', '-q')
    for f in files:
        (tmp / f).parent.mkdir(parents=True, exist_ok=True)
        original = subprocess.run(['git', 'show', 'HEAD:' + f], cwd=XNU, check=False, capture_output=True)
        if original.returncode == 0:
            (tmp / f).write_bytes(original.stdout)
    git(tmp, 'add', '-A')
    git(tmp, 'commit', '-q', '-m', 'base', '--allow-empty')
    for patch in earlier:
        for f in files:
            git(tmp, 'apply', '--include=' + f, str(patch), check=False)
    git(tmp, 'add', '-A')
    git(tmp, 'commit', '-q', '-m', 'earlier', '--allow-empty')
    for f in files:
        shutil.copy(XNU / f, tmp / f)
    git(tmp, 'add', '-N', *files)
    diff = git(tmp, 'diff', '--binary')
finally:
    shutil.rmtree(tmp)
(ROOT / 'patches/xnu' / name).write_text(diff)
print('wrote patches/xnu/%s (%d lines)' % (name, len(diff.splitlines())))
