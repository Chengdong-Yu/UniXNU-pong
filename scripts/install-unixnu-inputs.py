#!/usr/bin/env python3
"""Install this repository's OS profile and component entries into a UniXNU checkout.

UniXNU resolves board.os_profile as a directory under its own profiles/os/ and every
[kernel] archives component against its own components.toml. This board's 22H374 profile
and its PongPlatform24 kit exist only here, so a run installs them into the checkout
before resolving (the same arrangement the NX769J port uses for its iOS 16 profile).
The step is idempotent: an identical file is left alone, a differing one stops the run.
"""
import argparse
import json
import shutil
import subprocess
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def head(directory):
    try:
        return subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def install_profile(unixnu, profile):
    source = ROOT / 'os-profiles' / profile
    if not source.is_dir():
        raise SystemExit(f'{source} missing: board.os_profile names a profile this repository must carry')
    target = unixnu / 'profiles/os' / profile
    for path in sorted(source.rglob('*')):
        if not path.is_file():
            continue
        destination = target / path.relative_to(source)
        if destination.exists() and destination.read_bytes() != path.read_bytes():
            raise SystemExit(f'{destination} differs from {path}; keep the profile in one place')
        if not destination.exists():
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, destination)
    return target


def install_components(unixnu):
    catalog = unixnu / 'components.toml'
    if not catalog.is_file():
        raise SystemExit(f'{catalog} missing: {unixnu} is not a UniXNU checkout')
    wanted = tomllib.loads((ROOT / 'migration/format2/components.toml').read_text())['component']
    existing = {entry['id']: entry for entry in tomllib.loads(catalog.read_text()).get('component', [])}
    revision = head(ROOT)
    added, updated = [], []
    text = catalog.read_text()
    for entry in wanted:
        entry = dict(entry)
        if revision:
            # The kit ships in this repository, so the recipe's pin is the checkout it came from.
            entry['revision'] = revision
        if entry['id'] not in existing:
            text += '\n[[component]]\n' + ''.join(f'{key} = {json.dumps(value)}\n' for key, value in entry.items())
            added.append(entry['id'])
        elif {k: v for k, v in existing[entry['id']].items() if k != 'revision'} != {k: v for k, v in entry.items() if k != 'revision'}:
            raise SystemExit(f'components.toml already carries a different {entry["id"]} recipe; review it first')
        else:
            updated.append(entry['id'])
    if added:
        catalog.write_text(text)
    return added, updated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unixnu', required=True, type=Path)
    args = parser.parse_args()
    unixnu = args.unixnu.resolve()
    board = tomllib.loads((ROOT / 'board.toml').read_text())
    profile = board['board']['os_profile']
    target = install_profile(unixnu, profile)
    added, present = install_components(unixnu)
    print(json.dumps({'profile': profile, 'installed_at': str(target),
                      'components_added': added, 'components_present': present}, indent=2))


if __name__ == '__main__':
    main()
