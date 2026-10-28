"""Logical sparsebundle I/O and write tracking. No hashes or image comparison."""
import json
from pathlib import Path
import plistlib


def describe(bundle):
    bundle = Path(bundle)
    if bundle.is_symlink() or not bundle.is_dir():
        raise RuntimeError('independent sparsebundle directory required')
    if (bundle / 'bands').is_symlink() or not (bundle / 'bands').is_dir():
        raise RuntimeError('real sparsebundle bands directory required')
    info = plistlib.loads((bundle / 'Info.plist').read_bytes())
    band, size = info.get('band-size'), info.get('size')
    if (info.get('diskimage-bundle-type') != 'com.apple.diskimage.sparsebundle' or
            not isinstance(band, int) or not isinstance(size, int) or
            band < 1048576 or band % 1048576 or size <= 0 or size % 4096):
        raise RuntimeError('unsupported sparsebundle geometry')
    return band, size


def snapshot(bundle):
    """Track filesystem writes via band mtimes/sizes, without reading image data."""
    result = {}
    for p in (Path(bundle) / 'bands').iterdir():
        if p.is_symlink() or not p.is_file():
            raise RuntimeError('unexpected band entry: ' + str(p))
        try:
            number = int(p.name, 16)
        except ValueError:
            raise RuntimeError('invalid sparsebundle band name: ' + p.name)
        if number < 0 or format(number, 'x') != p.name:
            raise RuntimeError('noncanonical sparsebundle band name')
        st = p.stat()
        result[p.name] = [st.st_mtime_ns, st.st_size]
    return result


def storage_for(raw_image):
    record = Path(raw_image).parent / 'os-storage.json'
    if not record.exists():
        return None
    state = json.loads(record.read_text())
    if state.get('format') != 'pong-os-sparsebundle-v1' or not state.get('ready'):
        raise RuntimeError('OS storage conversion is incomplete')
    band, size = describe(state['image'])
    if (state['logical_bytes'], state['band_bytes']) != (size, band):
        raise RuntimeError('OS storage geometry changed')
    state['record'] = str(record)
    return state


class LogicalImage:
    def __init__(self, bundle):
        self.path = Path(bundle)
        self.band_size, self.size = describe(self.path)
        self.position = 0

    def seek(self, offset):
        if not 0 <= offset <= self.size:
            raise ValueError('logical image seek out of bounds')
        self.position = offset

    def read(self, count):
        if count < 0:
            raise ValueError('bounded logical reads required')
        count = min(count, self.size - self.position)
        result = bytearray()
        while count:
            band, within = divmod(self.position, self.band_size)
            amount = min(count, self.band_size - within)
            p = self.path / 'bands' / format(band, 'x')
            if p.exists():
                if p.is_symlink() or p.stat().st_size > self.band_size:
                    raise RuntimeError('invalid sparsebundle band')
                with p.open('rb', buffering=0) as f:
                    f.seek(within)
                    data = f.read(amount)
            else:
                data = b''
            result.extend(data)
            result.extend(b'\0' * (amount - len(data)))
            count -= amount
            self.position += amount
        return bytes(result)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        pass
