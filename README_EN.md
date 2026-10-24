# UniXNU-pong

[简体中文](README.md) | **English**

Device support for UniXNU on the Nothing Phone (2) (Pong, Qualcomm SM8475), targeting iOS 16.6.1 (20G81). This repository contains the Pong device tree, XNU patches, UFS/APFS, display and touch drivers, Metal/Adreno integration, and build scripts.

## Current status

- XNU has handed off to iOS through DarwinBoot on Pong; a previous build reached the iOS home screen.
- Native UFS boot, APFS volume reads, and a writable Data window have device-side implementations. The Data image is built locally.
- Pong display, touch, and physical keys have device-specific support.
- Device logs show Adreno 730 command submission, Metal device discovery, and display scanout. The wallpaper picker still has a precompressed-image compatibility issue; the visual result of the latest decode fallback is awaiting device confirmation.
- Consult local REC logs for the result of each boot and current issues. Device logs, photos, and build artifacts are not part of this repository.

## Target hardware and system

| Item | Configuration |
| --- | --- |
| Device | Nothing Phone (2) |
| Codename / SoC | Pong / SM8475 |
| GPU | Qualcomm Adreno 730 |
| iOS restore | iPadOS 16.6.1 / 20G81 components |
| XNU / Darwin | 8796.141.3 / 22.6.0 |
| Page size | 4 KiB |
| Data | APFS on a UFS window |

## Build

Clone UniXNU, this device repository, and UEFI as sibling directories. Recursively clone NativeMetal submodules:

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone --recurse-submodules https://github.com/Chengdong-Yu/UniXNU-pong
git clone --recurse-submodules https://github.com/Chengdong-Yu/uefi-pong
```

Run defconfig, dependency fetching, and the device firmware build from the UniXNU core directory. Restore inputs for iOS 16 are read by the scripts specified in `board.toml`. Build outputs and Apple private components remain in local ignored directories and are not committed.

The Darwin/macOS kernel build entry point is `scripts/build-ios16-pong-macos.py`; the prelinked boot image entry point is `scripts/build-pong-prelinked-boot.py`. See each script's `--help` and `board.toml` for command arguments, Xcode paths, and output locations. The optional `scripts/build-userdata.py` packs locally prepared OS and SystemOS files into SHNGROOT windows without a separate checksum pass or output readback.

The device tree uses the `drivers/DarwinBoot22` package path. Restore preparation extracts components declared in its manifest without downloading or committing a complete IPSW. Image builds skip additional full-image digest and readback passes.

## Layout

- `drivers/`: Pong UFS, APFS, touch, keys, CPU, display, Adreno, and Metal integration for SM8475.
- `patches/xnu/`: XNU patches for iOS 16.6.1 / Darwin 22.
- `configs/`: Pong kernel configuration.
- `scripts/`: restore-component, kernel, boot image, and userdata build scripts.
- `docs/`: development documents such as the local log collection plan.

## Local-only inputs

Apple IPSWs, system volumes and Cryptexes, trust caches, private firmware, userdata images, kernel images, boot logs, and device photos are local inputs or outputs. Prepare required components with the scripts; do not commit these files.

## License

Device-level code follows the repository license and per-file SPDX notices. Imported component license details are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
