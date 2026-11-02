#!/usr/bin/env python3
"""Fetch pinned public board prerequisites. No firmware, images or device I/O."""
import argparse
from pathlib import Path
import subprocess
import tomllib
from pong_native_inputs import ROOT, input_path


def main():
    sources = tomllib.loads((ROOT / 'configs/pong-native-sources.toml').read_text())['source']
    local = ROOT / 'local.toml'
    overrides = tomllib.loads(local.read_text()).get('source', []) if local.exists() else []
    for source in tomllib.loads((ROOT / 'feeds.toml').read_text())['source']:
        if source['kind'] != 'refer':
            continue
        override = next((s for s in overrides if s.get('kind') == 'refer' and s.get('name') == source['name']), {})
        path = Path(override['local']).expanduser() if override.get('local') else ROOT / 'refer/ipados18' / source['ref']
        if not path.is_absolute(): path = ROOT / path
        sources.append(dict(input=source['name'], git=source['git'], commit=source['commit'],
                            path=path.resolve(), required=[]))
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--only', choices=[s['input'] for s in sources], action='append')
    a = p.parse_args()
    for s in sources:
        if a.only and s['input'] not in a.only:
            continue
        path = s['path'] if 'path' in s else input_path(s['input'])
        if not path.exists():
            path.mkdir(parents=True)
        if not (path / '.git').exists():
            if any(path.iterdir()):
                raise RuntimeError(f'Existing non-Git input retained: {path}; select a new local.toml path')
            subprocess.run(['git', 'init', '-q', str(path)], check=True)
            subprocess.run(['git', '-C', str(path), 'remote', 'add', 'origin', s['git']], check=True)
        head = subprocess.run(['git', '-C', str(path), 'rev-parse', '--verify', 'HEAD'],
                              capture_output=True, text=True)
        if head.returncode:
            # Resume an interrupted first fetch without touching an existing
            # source checkout or unrelated files.
            origin = subprocess.check_output(['git', '-C', str(path), 'remote', 'get-url', 'origin'], text=True).strip()
            if origin != s['git'] or any(p.name != '.git' for p in path.iterdir()):
                raise RuntimeError(f'Incomplete local checkout retained: {path}')
            subprocess.run(['git', '-C', str(path), 'fetch', '--depth', '1', 'origin', s['commit']], check=True)
            subprocess.run(['git', '-C', str(path), 'checkout', '--detach', s['commit']], check=True)
        actual = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != s['commit']:
            raise RuntimeError(f'Existing source revision retained: {path}; expected {s["commit"]}')
        for name in s['required']:
            if not (path / name).is_file():
                raise RuntimeError(f'Missing public prerequisite: {path / name}')
        print(s['input'], actual, path)


if __name__ == '__main__':
    main()
