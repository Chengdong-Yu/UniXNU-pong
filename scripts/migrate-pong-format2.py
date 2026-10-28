#!/usr/bin/env python3
"""Install the versioned SM8475 and 22H374 inputs in a main UniXNU checkout."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parents[1]


def install(unixnu, soc):
    overlay = ROOT / 'migration/format2'
    state = json.loads((overlay / 'migration-state.json').read_text())
    if not soc.exists():
        shutil.copytree(overlay / 'soc-seed', soc)
        identity = json.loads((overlay / 'soc-commit.json').read_text())
        env = dict(os.environ, GIT_AUTHOR_NAME=identity['author_name'],
                   GIT_AUTHOR_EMAIL=identity['author_email'], GIT_AUTHOR_DATE=identity['author_date'],
                   GIT_COMMITTER_NAME=identity['committer_name'],
                   GIT_COMMITTER_EMAIL=identity['committer_email'], GIT_COMMITTER_DATE=identity['committer_date'])
        subprocess.run(['git', 'init', '-q'], cwd=soc, check=True)
        subprocess.run(['git', 'add', '.'], cwd=soc, check=True)
        subprocess.run(['git', '-c', 'commit.gpgsign=false', 'commit', '-qm', identity['message']], cwd=soc, env=env, check=True)
    actual = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=soc, text=True).strip()
    if actual != state['soc_revision']:
        raise RuntimeError('SM8475 source differs from the selected source pin')
    if not (unixnu / 'scripts/unixnu/profiles.py').is_file():
        raise RuntimeError('A main UniXNU checkout with format2 support is required')
    for source in (overlay / 'profiles').rglob('*'):
        if source.is_file():
            target = unixnu / source.relative_to(overlay)
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists() and target.read_bytes() != source.read_bytes():
                raise RuntimeError(f'Existing OS profile edits retained: {target}')
            shutil.copyfile(source, target)
    catalog = unixnu / 'components.toml'
    components = tomllib.loads(catalog.read_text()).get('component', [])
    custom = tomllib.loads((overlay / 'components.toml').read_text())['component'][0]
    existing = next((x for x in components if x['id'] == custom['id']), None)
    if existing and existing != custom:
        # A reviewed source pin advances with the published device source.
        # Permit only that change; preserve any independent recipe edits.
        if ({k:v for k,v in existing.items() if k != 'revision'} !=
                {k:v for k,v in custom.items() if k != 'revision'}):
            raise RuntimeError('Existing Pong component recipe differs; review it before replacing')
        blocks = catalog.read_text().split('[[component]]')
        for i in range(1, len(blocks)):
            entry = tomllib.loads('[[component]]' + blocks[i])['component'][0]
            if entry['id'] == custom['id']:
                blocks[i] = (overlay / 'components.toml').read_text().split('[[component]]', 1)[1]
                break
        catalog.write_text('[[component]]'.join(blocks))
    if existing is None:
        catalog.write_text(catalog.read_text() + (overlay / 'components.toml').read_text())
    # Machine-specific paths are kept in the ignored local.toml.
    local = ROOT / 'local.toml'
    doc = tomllib.loads(local.read_text()) if local.exists() else {}
    if set(doc)-{'source','local','pong_build'}:
        raise RuntimeError('Unrecognized local.toml tables retained; adapt them before writing overrides')
    entries = { (x['kind'], x['name']): x for x in doc.get('source', []) }
    paths = [('soc', 'sm8475', soc), ('kit', 'PongPlatform24', ROOT)]
    paths += [('refer', x['name'], ROOT / 'refer/ipados18' / x['ref'])
              for x in tomllib.loads((ROOT / 'feeds.toml').read_text())['source'] if x['kind'] == 'refer']
    for kind, name, path in paths:
        entries[(kind, name)] = dict(kind=kind, name=name, local=str(path))
    text = '# Local source overrides; do not commit absolute paths.\n'
    for x in entries.values():
        text += '\n[[source]]\n' + ''.join(k+' = '+json.dumps(v)+'\n' for k,v in x.items())
    if doc.get('local'):
        text += '\n[local]\n'+''.join(k+' = '+json.dumps(v)+'\n' for k,v in doc['local'].items())
    text += '\n[pong_build]\n'
    for k,v in doc.get('pong_build', {}).items():
        text += k+' = '+json.dumps(v)+'\n'
    local.write_text(text)
    return state


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--unixnu', required=True, type=Path)
    p.add_argument('--soc', type=Path, default=ROOT.parent / 'UniXNU-sm8475-common')
    a = p.parse_args()
    print(json.dumps(install(a.unixnu.resolve(), a.soc.resolve()), indent=2))


if __name__ == '__main__':
    main()
