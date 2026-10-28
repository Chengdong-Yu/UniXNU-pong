#!/usr/bin/env python3
"""Extract22H374 migration inputs without image hashes, CRC audits or booting.

Use --remote --only KernelCache to obtain the original kernel while the full
restore downloads; use --ipsw for the complete local extraction afterwards.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def restore_source(out, args, remote):
    """Resolve which restore a run reads.

    A local --ipsw names its own file, so it never needs download metadata. The
    remote path reads whichever checkpoint the download step published: the
    explicit download-source.json when one was written, else the progress file
    that download-ipados18-restore.py maintains, else the pinned restore that
    script already knows about. Previously only download-source.json was read,
    and nothing in this repository ever wrote it.
    """
    if not remote:
        return dict(url='file: ' + str(Path(args.ipsw).resolve()), size=Path(args.ipsw).stat().st_size)
    if args.url:
        if not args.size:
            raise SystemExit('--url also needs --size so ranges can be bounded')
        return dict(url=args.url, size=args.size)
    explicit = out / 'download-source.json'
    if explicit.is_file():
        declared = json.loads(explicit.read_text())
        return dict(url=declared['url'], size=declared.get('expected_download_bytes') or declared.get('bytes'))
    progress = out / 'download-progress.json'
    if progress.is_file():
        state = json.loads(progress.read_text())
        if state.get('state') != 'downloaded':
            raise SystemExit('the restore download has not finished; run --remote after it completes '
                             'or extract a component with download-ipados18-restore.py first')
        return dict(url=state['url'], size=state['bytes'])
    spec = importlib.util.spec_from_file_location('restore_download', ROOT / 'scripts/download-ipados18-restore.py')
    pinned = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pinned)
    return dict(url=pinned.URL, size=pinned.SIZE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--ipsw', type=Path)
    source.add_argument('--remote', action='store_true')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--only', action='append', default=[])
    parser.add_argument('--url', help='restore URL for --remote; defaults to the pinned Apple restore')
    parser.add_argument('--size', type=int, help='restore byte length for --remote')
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('restore_reader', ROOT / 'scripts/fetch-ios16-assets.py')
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    origin = restore_source(out, args, remote=args.remote)
    class CurlRangeFile(reader.RangeFile):
        def __init__(self):
            self.url = origin['url']
            self.size = origin['size']
            self.position = 0
            self.blocks = {}

        def _block(self, index):
            if index not in self.blocks:
                start = index * reader.BLOCK
                end = min(start + reader.BLOCK, self.size) - 1
                print(f'Fetching restore member bytes {start}..{end}', flush=True)
                command = ['curl', '--fail', '--location', '--silent', '--show-error',
                           '--connect-timeout', '15', '--max-time', '90', '--retry', '2',
                           '--range', f'{start}-{end}', '--max-filesize', str(end-start+1), self.url]
                data = subprocess.check_output(command)
                if len(data) != end-start+1:
                    raise RuntimeError('Short component transfer')
                if len(self.blocks) >= 8:
                    self.blocks.pop(next(iter(self.blocks)))
                self.blocks[index] = data
            return self.blocks[index]
    stream = args.ipsw.open('rb') if args.ipsw else CurlRangeFile()
    archive = reader.zipfile.ZipFile(stream)
    manifest = plistlib.loads(reader.member_bytes(archive, archive.getinfo('BuildManifest.plist')))
    if (manifest['ProductVersion'], manifest['ProductBuildVersion']) != ('18.7.10', '22H374'):
        raise SystemExit('Restore metadata names a different release')
    if not {'iPad7,11', 'iPad7,12'}.issubset(manifest['SupportedProductTypes']):
        raise SystemExit('Restore metadata does not describe the requested seventh-generation iPad')
    for name in ['BuildManifest.plist', 'Restore.plist']:
        (out / name).write_bytes(reader.member_bytes(archive, archive.getinfo(name)))
    boards = []
    members = {}
    for identity in manifest['BuildIdentities']:
        info = identity['Info']
        if info['RestoreBehavior'] != 'Erase' or info['DeviceClass'] not in ['j171ap', 'j172ap']:
            continue
        board = dict(device_class=info['DeviceClass'], chip_id=identity['ApChipID'],
                     board_id=identity['ApBoardID'], content_encoding=info.get('ContentEncoding'),
                     components={})
        for component, entry in identity['Manifest'].items():
            name = entry['Info'].get('Path')
            if not name:
                continue
            item = archive.getinfo(name)
            board['components'][component] = dict(path=name, bytes=item.file_size)
            if args.only and component not in args.only:
                continue
            member = members.setdefault(name, dict(path=name, bytes=item.file_size, components=[], boards=[]))
            if component not in member['components']:
                member['components'].append(component)
            if info['DeviceClass'] not in member['boards']:
                member['boards'].append(info['DeviceClass'])
        boards.append(board)
    metadata = dict(version='18.7.10', build='22H374', products=manifest['SupportedProductTypes'],
                    primary_product='iPad7,11', primary_device_class='j171ap', identities=boards,
                    source_url=origin['url'], image_verification='skipped by request')
    (out / 'components-metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    report_path = out / 'extraction-progress.json'
    report = dict(state='extracting', selected_components=args.only or 'all restore identity components',
                  image_verification='skipped by request', artifacts=[])
    for name, member in members.items():
        destination = out / 'components' / name
        if not destination.resolve().is_relative_to(out / 'components'):
            raise SystemExit('Unexpected restore member path')
        destination.parent.mkdir(parents=True, exist_ok=True)
        cached = destination.exists() and destination.stat().st_size == member['bytes']
        if not cached:
            print('Extracting', name, member['bytes'], 'bytes', flush=True)
            partial = destination.with_name(destination.name + '.partial')
            with partial.open('wb') as target:
                reader.raw_member(archive, archive.getinfo(name), target)
            os.replace(partial, destination)
        report['artifacts'].append(dict(member, cached=cached, local_path=str(destination)))
        report_path.write_text(json.dumps(report, indent=2) + '\n')
        print('Ready', name, flush=True)
    report['state'] = 'extracted'
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    print('Prepared', len(report['artifacts']), 'local restore components', flush=True)


if __name__ == '__main__':
    main()
