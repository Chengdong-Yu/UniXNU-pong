"""Host inputs for Pong builders; local paths never belong in public recipes."""
import os
from pathlib import Path
import tomllib
import pong_paths

ROOT = Path(__file__).resolve().parents[1]


def options():
    local = ROOT / 'local.toml'
    return tomllib.loads(local.read_text()).get('pong_build', {}) if local.exists() else {}


def input_path(name):
    variables = {
        'sheng_source': ('PONG_SHENG_ROOT', 'UNIXNU_SHENG_ROOT'),
        'wifi_shared': ('PONG_WIFI_SHARED',),
        'wifi_linux': ('PONG_WIFI_SOURCE',),
        'wifi_firmware': ('PONG_WIFI_FIRMWARE',),
        'audio_container': ('PONG_AUDIO_CONTAINER',),
        'storage_source': ('PONG_STORAGE_HEADERS',),
    }
    defaults = {
        'sheng_source': ROOT / 'refer/ipados18/UniXNU-sheng',
        'wifi_shared': ROOT / 'refer/ipados18/Ath12kWifi',
        'wifi_linux': ROOT / 'refer/ipados18/linux-7.0.14',
        'wifi_firmware': ROOT / 'out/ipados18/firmware/qca6490',
        'audio_container': ROOT / 'out/ipados18/firmware/audio/tfa98xx.cnt',
        'storage_source': ROOT / 'refer/ipados18/IOStorageFamily-323',
    }
    selected = next((os.environ[k] for k in variables[name] if os.environ.get(k)), None)
    path = Path(selected or options().get(name, defaults[name])).expanduser()
    return (path if path.is_absolute() else ROOT / path).resolve()


def build_env():
    env = dict(os.environ)
    if env.get('PONG_DEVELOPER_DIR'):
        env['DEVELOPER_DIR'] = str(Path(env['PONG_DEVELOPER_DIR']).expanduser())
    elif not env.get('DEVELOPER_DIR') and options().get('developer_dir'):
        path = Path(options()['developer_dir']).expanduser()
        env['DEVELOPER_DIR'] = str((path if path.is_absolute() else ROOT / path).resolve())
    elif not env.get('DEVELOPER_DIR'):
        env['DEVELOPER_DIR'] = str(pong_paths.developer_dir())
    return env
