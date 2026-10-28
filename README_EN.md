# UniXNU-pong

[简体中文](README.md) | **English**

Format 2 device support for Nothing Phone (2), codename **Pong**, Qualcomm **SM8475**. Main targets **iPadOS 18.7.10 (22H374)** from iPad7,11 / J171AP, Darwin 24.6.0 / XNU 11417.140.69. The board remains Pong. The earlier iOS16 implementation remains in Git history.

- [UniXNU tools](https://github.com/UniXNU/UniXNU), [Pong UEFI](https://github.com/Chengdong-Yu/uefi-pong)
- Official [migration](https://unixnu.com/en/porting/migration) and [device repository](https://unixnu.com/en/porting/device-repository) guides
- [Format 2 integration](migration/format2/README.zh.md), [OS migration](migration/ipados18/README.zh.md), [porting record](porting.toml)

## Hardware and progress

| Area | Configuration and observations |
| --- | --- |
| CPU / memory | X2 + 3×A710 + 4×A510, PSCI/GICv3, 19.2 MHz timer, native 4 KiB pages; 12 GiB physical RAM, managed memory constrained by reservations |
| Display / input | 1080×2412, stride 4320, requested 120 Hz; desktop, touch and wallpaper observed; actual refresh and brightness remain under development |
| GPU / Metal | Adreno 730 with Pong firmware, Turnip and Metal integration used for desktop rendering; remaining paths need work |
| Storage | Native UFS/APFS boot, separate OS/SystemOS and writable 8 GiB UFS Data at `/private/var` |
| Wi-Fi | QCA6490/ath11k; PCIe0 link, BAR and DMA allocation observed; SMMU ordering repaired, scanning and connectivity unconfirmed |
| Audio | ADSP, AudioReach, two TFA9873 amplifiers and output HAL connected; CoreMedia provider selection repaired, actual PCM output unconfirmed |
| Buttons / power | Real PMIC events reach HID/SpringBoard; reported power/volume actions remain unresolved; full suspend/resume incomplete |

Host builds and service publication are recorded separately from device functionality. The latest [SMMU/audio repair](migration/ipados18/services73-smmu-audio.md) is built without a temporary device boot, as requested.

## Build

Requires full Xcode with the iPhoneOS SDK, Python 3.12, dtc, selected Apple open source dependencies, matching 22H374 restore/ABI inputs and firmware obtained from Pong. Source revisions are pinned in feeds, lock and migration records.

```sh
git clone https://github.com/UniXNU/UniXNU ../UniXNU-upstream
git -C ../UniXNU-upstream checkout 0fc0fc1f9ddd0fbd4787738afef38667df5e0a95
cp local.toml.example local.toml
# Set local build, Xcode and volume-layout paths in local.toml.
python3.12 scripts/migrate-pong-format2.py --unixnu ../UniXNU-upstream
python3.12 scripts/build-pong-format2.py --unixnu ../UniXNU-upstream --out ../pong-build --jobs 4
```

The entry point resolves ARM64, SM8475, OS and board layers, compiles the hardware DTS and invokes the Darwin24 builder. Outputs reside in `../pong-build/pong/ipados-18.7.10-22H374/pong_ipados18/`. See the OS migration guide for restore download, userland and userdata preparation; these require matching local binary inputs.

Image hashing, whole-image validation and device image readback are skipped by default. The protocol's SHNGROOT header checksum is retained.

## Building the kernel with GitHub Actions

If you would rather not prepare Xcode, the Apple open source dependencies and the private firmware locally, a GitHub-hosted macOS runner can build the kernel for you. The workflow is `.github/workflows/macos-build.yml` (**Pong macOS build**), runs on `macos-15-intel`, and takes roughly 25-30 minutes.

### Triggering a run

1. Open the repository's **Actions** tab and pick **Pong macOS build** on the left.
2. Choose **Run workflow**, select the branch to build, and confirm.
3. Wait for the job whose name starts with `CI kernel`. The `disk probe` job beside it says in its own name that it is not a kernel build; it only measures disk space.
4. Download `pong-macos-products` under **Artifacts** at the bottom of the run page.

A push to any branch also triggers the workflow (`on: push`). The concurrency group is `pong-macos-${{ github.ref }}` with `cancel-in-progress: true`, so a newer push to the same branch cancels the run that is still going: do not push to a branch while you want its run to finish.

### Artifacts

`pong-macos-products` (kept 14 days):

| File | Contents |
| --- | --- |
| `pong-xnu-initial.img` | Android boot v4 container; its ramdisk is a `PONGFAT1` pack holding `EFI/UniXNU/kernel.macho`, `kernel.macho.adt`, `DarwinBoot24.efi`, `unixnu.toml` and `pong-boot-manifest.json` |
| `mach.development.pong` | The XNU kernel built by this run |
| `DarwinBoot24.efi` | The UEFI loader driver built on the runner |
| `ci-manifest.json` | Paths, disk headroom, boot decision and product sizes for the run |
| `boot-decision.json` | Whether the `prelink` or the `initial` path was taken, plus the container validation result |

`kernel-ci-log` (kept 1 day) holds the kernel build logs; check it first when a run fails.

### Firmware: Path A and Path B

The workflow branches on whether the `PONG_FIRMWARE_TOKEN` secret exists:

- **With the token (Path B)**: `configs/pong_ipados18_defconfig`, with Wi-Fi, backlight and Adreno 730 enabled. The token must read the private `Chengdong-Yu/UniXNU-pong-firmware` repository; the workflow checks out `wifi/`, `audio/`, `adreno730/` and `power/` at the fixed commit `c1695a31`.
- **Without it (Path A)**: `configs/pong_ipados18_ci_defconfig`, with Wi-Fi, backlight and Adreno 730 off. It produces a kernel, but it is not the shipping configuration.

A fork therefore runs Path A and needs no secrets at all. For the full configuration, add a fine-grained token with `Contents: Read` on the firmware repository under **Settings → Secrets and variables → Actions**. Never commit the firmware: the workflow only reads it.

### What it does not do

- It never flashes a device; a run only produces files.
- It does not build the userdata image. That step needs a prepared userland (`result.json`, `apply-complete.json`, and the OS, SystemOS and Data images), and CI has none of those inputs, so `userdata-decision.json` records `skipped` rather than a failure.
- Image hashing and the single-core ADT audit are skipped by default (the prelink path passes `--skip-image-verification`); the container checks still run.
- Nothing is verified on hardware. CI validates the container and its packed contents only; it never boots anything.

## Source boundaries and license

Only source, configuration, patches and scripts are published. IPSWs, Apple private binaries, firmware, credentials and generated userdata remain local and ignored. Machine paths belong in local configuration.

`PongPlatform24` identifies the community A730/QCA6490 implementation, independently of the upstream A740/WCN7850 driver. The versioned SM8475 seed is kept under `migration/format2/soc-seed`.

Board code uses BSD-2-Clause-Patent. Imported components retain their licenses and SPDX identifiers; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
