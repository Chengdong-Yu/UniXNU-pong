# UniXNU-pong

Device source for the Nothing Phone (2), codename **Pong**, Qualcomm **SM8475**, running the **iOS 16.6.1 / 20G81** userland. This repository follows the UniXNU device-repository layout: board config, kernel defconfig, device drivers, XNU patches, build scripts and the userdata builder.

The published kernel source is the **v98** source set. v99 is excluded because it panics on the device. The v98 build reached the iOS main interface; its Adreno 730 Metal path was not yet confirmed as accelerated.

- Firmware: [uefi-pong](https://github.com/Chengdong-Yu/uefi-pong)
- UniXNU core: [UniXNU](https://github.com/UniXNU/UniXNU)
- Device docs: [UniXNU documentation](https://unixnu.com)

## Build

Clone the UniXNU core and this device repository as siblings. All Apple restore inputs are fetched to the local UniXNU output directory when needed; IPSW archives, extracted Apple files and generated userdata images are excluded from this repository.

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone https://github.com/Chengdong-Yu/UniXNU-pong
cd UniXNU-pong
./scripts/build-v98-kernel.sh
```

To fetch the matching 20G81 kernel, device tree, trust caches, OS and SystemOS (Cryptex1) without downloading a whole IPSW:

```sh
python3 scripts/fetch-ios16-assets.py
```

The fetcher reads the IPSW BuildManifest by HTTP range and writes only the board-selected component files to `../UniXNU/out/ipsw/20G81`. Existing complete files of the expected length are reused without rereading them. No IPSW archive or Apple restore file is committed. Additional Apple-only build inputs such as the system-volume authentication blob are passed to build scripts from locally fetched/extracted files.

To build the Pong userdata root image from the fetched OS and SystemOS components:

```sh
python3 scripts/build-userdata.py
```

The userdata builder streams the input windows once and emits the v98 `SHNGROOT` metadata used by the Pong native UFS code. The format requires window/header digests; the script does not do a separate image verification or readback pass. Generated assets go under `out/`.

After installing the matching iOS 16.6.1 components and build tools, the upstream device workflow can also be run from the UniXNU checkout:

```sh
./unixnu defconfig pong_defconfig -D ../UniXNU-pong
./unixnu fetch
./unixnu build kernel
./unixnu build package
./unixnu build firmware --uefi ../uefi-pong
```

The board file selects iPad7,1 / J120 / 20G81 while the hardware-specific drivers and boot configuration use Pong SM8475 values. The repository contains no IPSW, Apple restore binaries, system-volume authentication blob or GPU firmware blobs. Restore inputs are obtained locally through the scripts; GPU firmware remains an explicit local input.

## Device support

| Feature | State | Notes |
| --- | --- | --- |
| iOS 16.6.1 boot | ⚠️ | v98 reaches the iOS main interface; v99 is excluded because it panics. |
| Display and touch | ✅ | Working in the v98 device build. |
| Physical buttons | ✅ | Pong key path retained in the v98 source. |
| UFS / APFS root | ⚠️ | Native UFS publishes the OS and SystemOS windows; runtime acceleration is not implied. |
| Writable Data | ⚠️ | The v98 userdata-root builder is included; generated image must be built locally. |
| Adreno 730 GPU / Metal | ⚠️ | v98 GPU startup was not confirmed as accelerated. |
| Wi-Fi, Bluetooth, camera, audio, sensors | ❌ | Not implemented in this source set. |

## License

Device-level files follow BSD-2-Clause-Patent. Imported components keep their original SPDX headers and licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
