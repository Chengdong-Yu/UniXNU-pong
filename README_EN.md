# UniXNU-pong

[简体中文](README.md) | **English**

UniXNU device support for Nothing Phone (2), codename **Pong**, Qualcomm **SM8475**. This repository contains the device configuration, XNU patches, board drivers, userland adaptation, and build scripts for **iOS 16.6.1 (20G81)**.

The current source reflects the device state that reaches the iOS home screen. Display and touch work; Adreno 730 hardware acceleration and Metal integration are not yet confirmed.

- Firmware: [uefi-pong](https://github.com/Chengdong-Yu/uefi-pong)
- UniXNU core: [UniXNU](https://github.com/UniXNU/UniXNU)
- Documentation: [UniXNU documentation](https://unixnu.com)

## Build

Clone the UniXNU core, this device repository, and the UEFI repository as sibling directories. Cloning this repository requires `--recurse-submodules` so the NativeMetal submodule is included:

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone --recurse-submodules https://github.com/Chengdong-Yu/UniXNU-pong
git clone --recurse-submodules https://github.com/Chengdong-Yu/uefi-pong
```

Run the following commands from the UniXNU core directory:

```sh
./unixnu defconfig pong_defconfig -D ../UniXNU-pong
./unixnu fetch
python3 ../UniXNU-pong/scripts/fetch-ios16-assets.py
./unixnu build kernel
./unixnu build package
./unixnu build firmware --uefi ../uefi-pong
```

The asset fetcher reads the IPSW BuildManifest according to the iOS 16.6.1 / 20G81 settings in `board.toml`, then uses HTTP Range requests to extract only the kernel, device tree, trust caches, OS, and SystemOS (Cryptex1) components needed for the build. Each member is checked against the ZIP CRC-32 in the IPSW central directory as it is written. It does not download a complete IPSW archive. Extracted components are saved under UniXNU's `out/ipsw/20G81` directory.

After fetching the components, build the Pong userdata root image separately:

```sh
python3 ../UniXNU-pong/scripts/build-userdata.py
```

The image is written locally under `UniXNU-pong/out/`. The builder generates the window and header digest fields required by the SHNGROOT format during a single streaming copy. It does not perform an extra image verification or readback pass. Apple restore files, generated userdata images, the system-volume authentication data, and GPU firmware are not included in this repository. Required system components are extracted from locally fetched restore inputs; GPU firmware is supplied as a local input.

The generated JSON includes the report-only field `write_target` (`/dev/block/sda13`). It names the Pong userdata partition for reference and must not be flashed blindly.

The restore identity selects iPad7,1 / J120 / 20G81 components, while the board drivers and boot configuration target Nothing Phone (2), Pong / SM8475 hardware.

## Device support

| Feature | State | Notes |
| --- | --- | --- |
| iOS 16.6.1 boot | ✅ | Reaches the iOS home screen |
| Display | ✅ | System UI is visible |
| Touch | ✅ | Pong touch path works |
| Physical buttons | ✅ | Pong power and volume key paths are included |
| UFS / APFS | ⚠️ | Native UFS and APFS boot paths are connected; some mount capabilities remain in progress |
| Writable Data | ⚠️ | A local userdata root image builder is included; the image is generated locally |
| Adreno 730 / Metal | ⚠️ | Driver and Metal adaptation sources are included; hardware acceleration is not confirmed |
| Wi-Fi, Bluetooth, camera, audio, sensors | ❌ | Device support is not yet implemented |

✅ available, ⚠️ partial or unconfirmed, ❌ unavailable.

## License

Device-level code is licensed under BSD-2-Clause-Patent. Imported components retain their SPDX identifiers and original licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
