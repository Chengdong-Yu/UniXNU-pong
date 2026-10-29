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
2. Choose **Run workflow** and select the branch to build.
3. For the shipping configuration, put the download URL of your firmware archive in `firmware_url` (next section). Leave it empty to build the CI configuration only.
4. To build the userdata image as well, put the download URL of Apple's iPadOS 18.7.10 (22H374) restore in `ipsw_url` (next section). Leave it empty to skip it and download nothing.
5. Wait for the job whose name starts with `CI kernel`. The `disk probe` job beside it says in its own name that it is not a kernel build; it only measures disk space.
6. Download `pong-macos-products` under **Artifacts** at the bottom of the run page, and, when a userdata image was built, `pong-userdata-part0` through `part3`.

A push to any branch also triggers the workflow (`on: push`), but a push event carries no inputs, so it always builds the CI configuration without Wi-Fi, backlight or the GPU. Start the workflow manually for the full one.

The concurrency group is `pong-macos-${{ github.ref }}` with `cancel-in-progress: true`, so a newer push to the same branch cancels the run that is still going: do not push to a branch while you want its run to finish.

### The firmware archive

Wi-Fi, the backlight and the Adreno 730 need firmware that only exists on this phone, and this public repository carries none of it. Pack that firmware into one archive, put it somewhere CI can download from (object storage, a private release, anything that yields an https URL), and give the URL as `firmware_url`.

The archive can be `.zip`, `.tar.gz`, `.tgz` or `.tar`, and after unpacking it must hold at least these four groups. One extra wrapper directory is fine, as when a zip is made from a folder:

```
wifi/                 # qca6490/ is accepted as the same directory
  amss.bin
  amss20.bin
  m3.bin
  regdb.bin
  bdwlan.e01          # at least one bdwlan* board data file
  ...
audio/
  tfa98xx.cnt
adreno730/
  gmu_gen70000.bin
  a730_sqe.fw
  a730_zap.mdt
  a730_zap.b01
  a730_zap.b02
power/
  adsp.mbn
```

Put the whole `wifi/` directory in: the build embeds every file in it into the kernel's firmware pack, including files such as `Data.msc`. `adreno730/` and `audio/` use only the names listed above, and other files there are ignored.

When `firmware_sha256` is set, the archive's digest is checked first and a mismatch fails the run before anything is unpacked. The URL must be https.

An archive in a public location, or a pre-signed object-storage link, needs nothing else configured. When the archive lives on a private GitHub release, the workflow fetches it with an `Authorization: Bearer` header, so add a secret `PONG_FIRMWARE_ARCHIVE_TOKEN` with `Contents: Read` on that repository and give `firmware_url` in API form, such as
`https://api.github.com/repos/<owner>/<repo>/releases/assets/<asset id>`; curl follows the redirect to the signed storage URL.

To produce the archive locally, use the repository script, which builds the zip above and prints its sha256:

```sh
python3 scripts/pack-pong-firmware.py \
  --wifi ../private-inputs/pong-wifi-firmware \
  --audio ../private-inputs/pong-audio-inputs \
  --adreno730 out/ipados18/firmware/adreno730 \
  --adsp ../private-inputs/pong-power \
  --output ../private-inputs/pong-firmware.zip
```

Never commit the firmware. The workflow uses it only after the checks pass, and writes the archive's sha256 and the unpacked paths into `ci-manifest.json`.

### The userdata image

CI can produce the userdata layout and the whole image from **one input**: the download URL of Apple's iPadOS 18.7.10 / 22H374 restore, given as `ipsw_url`. Decryption, mounting and assembly all happen on the runner, so nothing has to be prepared on another machine first.

The URL must be https and must end in `.ipsw`. Filling `ipsw_sha256` checks the whole IPSW after the download; a mismatch stops the run before a byte is unpacked.

On the runner the chain is (`scripts/prepare-pong-userland-from-ipsw.py` drives it, and every step is a script that already exists in this repository):

1. Download the IPSW (`scripts/download-ipados18-restore.py`: bounded parallel ranges, resumable), check its length and, if asked, its sha256.
2. Extract the components with `scripts/prepare-ipados18-components.py --ipsw`, which refuses anything that is not 18.7.10 / 22H374 on an iPad7,11 or 7,12.
3. Decrypt the OS and SystemOS `.dmg.aea` archives with `scripts/decrypt-ipados18-aea.py` (AEA profile 1: the public FCS key unwraps the archive key, which `/usr/bin/aea` then uses).
4. Expand the decrypted OS UDIF to a raw disk with `hdiutil convert -format UDTO`, mount it as `--os-root`, and hand the same raw disk as `--os-image`.
5. Create a fresh 5 GiB ordinary APFS volume and copy the original SystemOS files into it with `userland/build-systemos18.py`, producing `pong-ipados18-systemos.cdr`.
6. Create a fresh empty 8 GiB APFS volume and convert it to a raw disk as the Data starting point; the iOS 16 Data is not reused.
7. Run `userland/build-ipados18.py prepare` / `apply` / `finish`, which produce `result.json`, `apply-complete.json` and the OS and Data images.
8. Check the five files with `scripts/stage-pong-userdata.py`'s own validators: all five present, `result.json` build 22H374, every image 4 KiB aligned with an APFS container (or a GPT holding one) at the front, and three readable, distinct volume UUIDs.

It then writes `ci-userland-path.txt` and `ci-volume-layout.json` in exactly the shapes the later steps already read, so the layout and image stages need no change: the SHNGROOT directory (`scripts/prepare-ipados18-ufs-layout.py`, which reads all three images, about 19 GiB, and is the slow part), then the whole image (`scripts/build-pong-userdata-image.py`).

The workflow reads each volume's UUID from its APFS volume header, writes `ci-volume-layout.json`, and hands those UUIDs to the kernel's `XNU_OWNER_*_VOLUME_UUID` values and to the boot manifest's root-match value. So **one run's kernel and userland belong together**: the UUIDs are compiled in, and a mismatch mounts nothing.

This step needs macOS and passwordless `sudo`: `apply` and `build-systemos18.py` must run as root to write Apple's original uid/gid onto the copies. A failing `sudo -n true` stops the run instead of skipping a root step, because the resulting userland would look complete and then fail on the device over shared-cache ownership.

`ci-userland-from-ipsw.json` records the input, every command, each stage's bytes and timings; `ci-manifest.json` points at it under `userland_from_ipsw`. A query string on the IPSW URL (a pre-signed link) is replaced with `?<redacted>` in that report.

`userdata_image` is on by default. The product is uploaded as four artifacts, `pong-userdata-part0` through `part3` (about 5 GiB each, kept 30 days). One artifact is capped at 10 GiB and the image is about 19.4 GiB, so it has to be split; concatenate the parts in order:

```sh
cat pong-userdata-part0 pong-userdata-part1 pong-userdata-part2 pong-userdata-part3 > pong-ipados18-userdata.img
```

The image stops at the end of the last window rather than filling the whole 454 GiB partition; everything the directory does not describe is zero. Flashing still goes through `scripts/install-ipados18-ufs.py`, one window at a time with the SHNGROOT directory committed last. This whole-disk image is the same bytes in another wrapper, for inspection, diffing and keeping a copy; CI never pushes it at a device.

If a prepared userland already exists on your Mac you can still pack it for local keeping with `scripts/pack-pong-userland.py`: it checks the five files, the `result.json` build and the image alignment, folds in `pong-ipados18-systemos.cdr`, and prints the sha256. That path is now for local archiving and comparison only; CI no longer reads it, though `stage-pong-userdata.py`'s validators still apply to it and to the last step of the runner chain.

```sh
python3 scripts/pack-pong-userland.py \
  --prepared ../private-inputs/22H374/prepared \
  --systemos out/ipados18/systemos/pong-ipados18-systemos.cdr \
  --output ../private-inputs/pong-userland.zip
```

Never commit the images or an IPSW. The workflow uses them only after every step succeeds, and writes the IPSW URL, the extracted component sizes, the three volume UUIDs it read and the resulting paths into `ci-userland-from-ipsw.json`.

With no `ipsw_url` (every push-triggered run), the step only writes `userdata-decision.json` with `skipped` and exits 0. That is not a failure.

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

With `ipsw_url` filled there are also `pong-userdata-part0` through `part3` (kept 30 days): the whole userdata image cut into four ordered parts, which `cat` back into `pong-ipados18-userdata.img`. The run's `ci-manifest.json` records the layout, the image size and each part's size under `userdata`, and how the userland was built from the IPSW under `userland_from_ipsw`.

### A local build still extracts from the phone

A local build needs no archive: it reads the firmware out of the phone. Wi-Fi has a script for it, run in recovery:

```sh
python3 scripts/prepare-pong-wifi.py --serial <serial> --out ../private-inputs
```

It mounts `modem_a` read-only, pulls the whole `image/qca6490` directory into `<out>/qca6490`, and then checks that `amss.bin`, `amss20.bin`, `m3.bin`, `regdb.bin` and at least one `bdwlan*` are present. `adsp.mbn`, `tfa98xx.cnt` and the five Adreno 730 files come out of the same phone's modem and vendor partitions. The build reads them from `PONG_WIFI_FIRMWARE` (default `$PONG_INPUTS/pong-wifi-firmware`) for Wi-Fi, `PONG_AUDIO_INPUTS` (default `$PONG_INPUTS/pong-audio-inputs`, holding `tfa98xx.cnt`) for the backlight, and `PONG_ADRENO_FIRMWARE` (by default the guest's `out/ipados18/firmware/adreno730`). `scripts/pong_paths.py` holds every path rule.

The two inputs can be shared: once the files are in place locally, `scripts/pack-pong-firmware.py` turns them into the zip that Actions takes, so nothing has to be prepared twice.

### What it does not do

- It never flashes a device; a run only produces files.
- It does not invent the userdata inputs. The layout and image are built only when `ipsw_url` is supplied, and the userland itself is built on the runner from that IPSW. Without it, `userdata-decision.json` records `skipped` and exits 0; that is not a failure.
- It never degrades silently. A failure anywhere in the chain (download, component build check, AEA decryption, `sudo`, `apply`, the five-file check) fails that step; it never leaves a userland that merely looks complete.
- It never pushes the userdata image at a device and never reads a device back. The whole-disk image is the same window bytes in one file.
- The prelink path skips image hashing and the ADT audit (`build-ipados18-prelinked-boot.py` passes `--skip-image-verification` to `build-ipados18-bootimg-local.py`), which leaves fields such as `image_sha256` as `null`; both paths still check the container structure.
- Nothing is verified on hardware. CI validates the container and its packed contents only; it never boots anything.

## Source boundaries and license

Only source, configuration, patches and scripts are published. IPSWs, Apple private binaries, firmware, credentials and generated userdata remain local and ignored. Machine paths belong in local configuration.

`PongPlatform24` identifies the community A730/QCA6490 implementation, independently of the upstream A740/WCN7850 driver. The versioned SM8475 seed is kept under `migration/format2/soc-seed`.

Board code uses BSD-2-Clause-Patent. Imported components retain their licenses and SPDX identifiers; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
