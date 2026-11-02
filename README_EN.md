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
| Display / input | 1080×2412, stride 4320, requested 120 Hz; desktop, touch, wallpaper and brightness control have device confirmation; actual refresh remains under investigation |
| GPU / Metal | Adreno 730 with Pong firmware, Turnip and Metal desktop rendering; upstream scheduling, cache and Metal updates integrated, with remaining slow-frame and resume issues |
| Storage | Native UFS/APFS boot, separate OS/SystemOS and writable Data at `/private/var`; the device Data volume is 128 GB, while the hosted seed remains 8 GiB |
| Wi-Fi | QCA6490/ath11k with Pong firmware; PCIe/SMMU/DMA repairs and native system integration, with scanning and association confirmed |
| Audio | ADSP, AudioReach, two TFA9873 amplifiers and native IOAudio2; ordinary playback and volume confirmed, but audio recovery after lock remains unresolved |
| Buttons / power | Real PMIC events reach HID/SpringBoard and native power/volume actions; full suspend/resume remains incomplete |

Host builds and service publication are recorded separately from device functionality. Historical source and device observations remain in the migration records.

Current source includes the accumulated Wi-Fi, brightness, HID, audio, battery, native USB/Finder, keyboard, rootful userland and GPU/Metal fixes. Wi-Fi association, brightness and native audio volume have device confirmation; audio recovery after lock and remaining GPU frame latency are still under investigation. The installed Data volume has been expanded to 128 GB; the hosted userdata recipe retains its original 8 GiB seed. arm64e/PAC preparation does not switch the running 22H374 arm64 system. See [runtime integration and preserved packaging scripts](migration/ipados18/runtime-fixes-and-build-overlay.md).

## Driver repositories

Every board driver under `drivers/` is a submodule of its own Chengdong-Yu repository, the same layout [UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng) uses. Path names are unchanged so the build scripts and the drivers' relative `#include`s keep working.

| Path | Repository | Hardware |
| --- | --- | --- |
| `drivers/ArmCpu` | [pong-arm-cpu](https://github.com/Chengdong-Yu/pong-arm-cpu) | SM8475 GICv3 + ARM generic timer, PSCI secondaries; Pong variant of upstream [ArmCpu](https://github.com/UniXNU/ArmCpu) |
| `drivers/ArmDTPlatform` | [pong-arm-dt-platform](https://github.com/Chengdong-Yu/pong-arm-dt-platform) | Apple-format DT platform expert (`/chosen`/`/cpus`/`/arm-io`); Pong variant of upstream [ArmDTPlatform](https://github.com/UniXNU/ArmDTPlatform) |
| `drivers/NativeRamBlock` | [pong-ram-block](https://github.com/Chengdong-Yu/pong-ram-block) | RAM-backed Data volume (sparse seed + persistent overlay); Pong variant of upstream [RamBlock](https://github.com/UniXNU/RamBlock) |
| `drivers/DarwinBoot24` | [pong-darwin-boot24](https://github.com/Chengdong-Yu/pong-darwin-boot24) | Darwin 24 UEFI XNU loader + PONGFAT1; Darwin 24 variant of upstream [DarwinBoot](https://github.com/UniXNU/DarwinBoot) |
| `drivers/AdrenoMetal` | [pong-adreno-metal](https://github.com/Chengdong-Yu/pong-adreno-metal) | Adreno Metal frontend pinned at `c6d29ca`; upstream [AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) |
| `drivers/NativeUfs` | [pong-ufs](https://github.com/Chengdong-Yu/pong-ufs) | SM8475 UFS host (`0x01d84000`), SID `0xe0`, SPI 265; SHNGROOT v1 three-window layout |
| `drivers/PongAdreno` | [pong-adreno](https://github.com/Chengdong-Yu/pong-adreno) | Adreno 730 GMU/CP/HFI/ZAP/SMMU/scanout (Cape GPUCC) |
| `drivers/PongMetal` | [pong-metal](https://github.com/Chengdong-Yu/pong-metal) | A730 Metal frontend for 22H374 with GOP scanout |
| `drivers/PongPower` | [pong-power](https://github.com/Chengdong-Yu/pong-power) | ADSP PAS1, dual TFA9873, WCD938x, NT37705 DCS backlight, battery |
| `drivers/PongUsb` | [pong-usb](https://github.com/Chengdong-Yu/pong-usb) | SM8475 DWC3/eUSB2 + NXP repeater, 32 KiB DMA / SID0 |
| `drivers/PongWifi` | [pong-wifi](https://github.com/Chengdong-Yu/pong-wifi) | QCA6490 over PCIe0 + MHI + ath11k, SID `0x1c01` |
| `drivers/PongBootCapture` | [pong-boot-capture](https://github.com/Chengdong-Yu/pong-boot-capture) | Bounded diagnostic restart via PMK8350 SID0 SDAM7100 |

The shared header `drivers/PongProbe.h` stays in this repository. Clone with `--recurse-submodules`, or run `git submodule update --init --recursive` afterwards.

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

## Packaging with GitHub Actions

When there is no local Xcode, Apple open-source dependency tree or private firmware to hand, let a GitHub-hosted macOS runner do the packaging. The workflow is `.github/workflows/package.yml` (name **package**), it runs on `macos-15-intel` and drives `scripts/package.sh` the way UniXNU's packaging document (`docs/porting/packaging.md`) describes, plus the userdata image that document does not cover.

### Two jobs

| job | what it does | artifacts |
| --- | --- | --- |
| **userdata image** | builds the prepared userland, the SHNGROOT directory and the whole userdata image from the IPSW at `ipsw_url` | `pong-userdata` (30 days), `volume-layout` (1 day) |
| **boot image, payload and kernel** | `scripts/package.sh --os …` runs deps / preflight / patches / build / pack / verify, then adds the two things the spec does not describe but the local flow does | `pong-package` (14 days) |

The jobs are split by disk, not by CPU: the userdata chain peaks near 37 GiB, the packaging job's XNU objects and EDK II output near 25 GiB, and a hosted runner has 14 GiB of RAM and 80–180 GiB of disk. The packaging job takes only a 2 KB `ci-volume-layout.json` from the other, so a failed or skipped `userdata` never blocks it.

### Running it by hand

1. Open the repository's **Actions** tab and pick **package** on the left.
2. Press **Run workflow** and choose the branch.
3. The OS profile is fixed to `ipados-18.7.10-22H374` (the only choice).
4. For a userdata image as well, put the restore download URL for Apple's iPadOS 18.7.10 (22H374) into `ipsw_url`. Leaving it empty skips that job, and the packaging job then compiles the kernel against uuid5 placeholder UUIDs, so that run's output cannot be booted on hardware.
5. To use the stock 22H374 kernelcache as a prelink base, tick `prelink`. It is off by default and the run uses the natively built kernel.
6. When the run finishes, download `pong-package` from **Artifacts** at the bottom of the run page, plus `pong-userdata` when that job ran.

The `package` job needs a repository secret named `UNIXNU_UNIQUE_CHIP_ID`: the device's 8-digit hexadecimal platform seed. Without it the payload step refuses to run, because the seed is compiled into the image and differs per chip.

The concurrency group is `package-${{ github.ref }}` with `cancel-in-progress: true`, so starting another run on the same branch cancels the one still going. Only `workflow_dispatch` triggers the workflow; pushing does not start a run.

### What the packaging job does

`scripts/package.sh` is byte-identical to UniXNU's `templates/package.sh`. It follows `board.toml [package] unixnu` to the UniXNU checkout and runs `unixnu release`. Output lands in `UniXNU/out/pong/<os>/<config>/`: `boot/boot.img`, `payload/payload.img`, `kernel/kernel.macho`, `package.manifest.json`.

Three things differ from the repository defaults on the runner, none of them incidental:

- **defconfig**: `release` always uses the `[package.defconfig]` mapping and there is no command-line override, so the runner repoints that mapping to `pong_ipados18_ci_defconfig`. The shipping configuration's Wi-Fi, backlight and Adreno stages need private firmware that the public repository does not carry, so CI cannot build it. The repository keeps the shipping mapping.
- **Build tree**: the shared kernel step's tree already has real `drivers/`, `kits/` and `patches/` directories, so the device kernel builder uses a separate tree and copies `mach.development.pong` back to the shared tree's `[kernel.prelink] kernel` path.
- **Volume UUIDs**: the kernel bakes the OS, SystemOS and Data UUIDs in as constants. A run that produced userdata uses that layout; otherwise fixed-namespace uuid5 placeholders keep the object tree reusable, at the cost of an output that only proves it compiles.

### Matching the local artifacts

`docs/porting/packaging.md` stops at `boot/boot.img`, while the local flow does two more things. CI performs them with `scripts/finalize-pong-adt.py` under the same parameters, so a CI image has the same shape as a local one:

- **ADT from the local generator**: the shared `devicetree.generate()` writes only root, chosen and the nodes configured under `[adt]`. The `pong,ufs-*` properties, touch and button resources and PMIC metadata that the kernel reads on `/arm-io` are not among them. This step calls `scripts/make-pong-adt-ipados18.py` with the local parameters: `--dram-size 0x300000000` (12 GiB), `--window-size 0x100000000` (4 GiB), `--cpus 1`, `--chip-id 0x8010`, `--development-debug`, `--skip-image-verification`, and stores the SHA-384 of `pong-boot-manifest.json` in `/chosen/boot-manifest-hash`. The result replaces `EFI/UniXNU/Payloads/kernel.macho.adt` inside `payload.img`, and a copy of `pong-boot-manifest.json` sits beside it.
- **Boot image in the flashable shape**: the firmware (`PongBootMemoryDxe`) only accepts an initrd carrying the `PONGFAT1` envelope, while `release` puts `payload.img` into the ramdisk slot unwrapped. This step rewrites `boot/pong-xnu-initial.img` in the local container's shape: Android boot v4 header, empty kernel slot (this machine boots firmware from its own partition), ramdisk is the envelope plus `payload.img`, and 4096 zero bytes of empty signature area at the end. `boot/boot.img` remains the spec artifact that `release` wrote.

Inside that `PONGFAT1` envelope `payload.img` must be 96 MiB, because `PongBootMemoryDxe` rejects an initrd over 128 MiB and the local `--fat-mib` is 96 as well. That is why `board.toml [payload] size_mib` is 96.

### The userdata image

There is one input: the download URL of Apple's iPadOS 18.7.10 / 22H374 restore (IPSW), in `ipsw_url`. Unpacking, decryption, mounting and assembly all happen on the runner.

The link must be https and end in `.ipsw`. A non-empty `ipsw_sha256` is checked against the whole IPSW after download, and a mismatch fails before a single byte is unpacked.

On the runner (`scripts/prepare-pong-userland-from-ipsw.py` chains the steps):

1. Download the IPSW (`scripts/download-ipados18-restore.py`, chunked and resumable), check its size and the sha256 when given.
2. Take the components with `scripts/prepare-ipados18-components.py --ipsw`, after confirming it really is 18.7.10 / 22H374 for an iPad7,11 or 7,12.
3. Decrypt the OS and SystemOS `.dmg.aea` files (`scripts/decrypt-ipados18-aea.py`, AEA profile 1).
4. Expand OS into a raw disk and mount it as `--os-root`, with the same raw disk as `--os-image`.
5. Create a fresh 5 GiB plain APFS volume, copy the stock SystemOS files with `userland/build-systemos18.py`, and emit `pong-ipados18-systemos.cdr`.
6. Create a fresh empty 8 GiB APFS volume, convert it to a raw disk, and use it as the Data starting point.
7. Run `userland/build-ipados18.py prepare` / `apply` / `finish`.
8. Check the five files, the build in `result.json`, image alignment and the three volume UUIDs with the validators in `scripts/stage-pong-userdata.py`.

Then it writes `ci-volume-layout.json`, generates the SHNGROOT directory (`scripts/prepare-ipados18-ufs-layout.py`, which reads about 19 GiB and is the slow part), and finally assembles the whole image from that directory (`scripts/build-pong-userdata-image.py`).

The three UUIDs are read from the APFS volume headers of the image, baked into the kernel's `XNU_OWNER_*_VOLUME_UUID` and written into the ADT's root-match value. **The kernel from a run and that run's userland belong together**: the UUIDs are kernel constants, so a different userland means rebuilding the kernel.

This step needs macOS with passwordless `sudo`: `apply` and `build-systemos18.py` must be root to write the stock files' uid/gid correctly. A failing `sudo -n true` fails the step rather than skipping a root step and pretending it worked.

The image is uploaded as four parts of `pong-userdata`, named `<name>.part00` … `.part03` (two-digit numbering, at most 8 GiB each for a ~19.4 GiB image, kept 30 days). Join them in order:

```sh
cat pong-ipados18-userdata.img.part00 pong-ipados18-userdata.img.part01 \
    pong-ipados18-userdata.img.part02 pong-ipados18-userdata.img.part03 > pong-ipados18-userdata.img
```

The image stops at the end of the last window rather than spanning the whole 454 GiB partition. Writing it to a phone is still `scripts/install-ipados18-ufs.py`, one window at a time with a final commit of the SHNGROOT directory; CI never pushes to a device.

Without `ipsw_url` the userdata job reports `staged=false` and stops. That is not a failure.

### Artifacts

`pong-package` (14 days):

| file | contents |
| --- | --- |
| `pong-xnu-initial.img` | the boot image in the local shape: Android boot v4, empty kernel slot, ramdisk is the `PONGFAT1` envelope plus `payload.img`, empty signature area; `pong-boot.json` records the parameters and the ADT and manifest digests |
| `boot.img` / `payload.img` | the spec artifacts `release` wrote; the ramdisk in `boot.img` is the unwrapped `payload.img` |
| `kernel/kernel.macho` | the kernel this run built (or prelinked) |
| `package.manifest.json` | the `release` manifest, with digests updated for the rewritten files |
| `pong-boot-manifest.json` | the same unsigned development boot description the local image carries |
| `ci-volume-layout.json` / `local.toml` | the volume layout this run used and the board-level local inputs written on the runner |

`pong-userdata` (30 days) is `.part00` … `.part03` plus `userdata-image.json` and `ci-volume-layout.json`. `volume-layout` (1 day) only passes the layout between the two jobs.

### What it does not do

- It does not flash a device or touch hardware; a run only produces files.
- It has no degraded mode: any failing step (download, component check, AEA decryption, `sudo`, `apply`, the five checks, packaging, the ADT and container checks) fails that step.
- The ADT and boot image are generated with the local parameters, and `--skip-image-verification` matches the local flow, so no hash audit beyond `image_sha256` happens; the container structure is still checked.
- No artifact has been on real hardware. CI validates container structure and packaged contents, not booting.
- The prelink slide is not yet expressed for this board, so the `prelink` path uses the local fixed base; that path has not been verified on a runner.

## Source boundaries and license

Only source, configuration, patches and scripts are published. IPSWs, Apple private binaries, firmware, credentials and generated userdata remain local and ignored. Machine paths belong in local configuration.

`PongPlatform24` identifies the community A730/QCA6490 implementation, independently of the upstream A740/WCN7850 driver. The versioned SM8475 seed is kept under `migration/format2/soc-seed`.

Board code uses BSD-2-Clause-Patent. Imported components retain their licenses and SPDX identifiers; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
