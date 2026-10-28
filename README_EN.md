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
4. Wait for the job whose name starts with `CI kernel`. The `disk probe` job beside it says in its own name that it is not a kernel build; it only measures disk space.
5. Download `pong-macos-products` under **Artifacts** at the bottom of the run page.

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

CI can also produce the userdata layout and the whole image, but **the inputs can only come from you**. The workflow does not run `userland/build-ipados18.py` and does not download an Apple restore: that prepared userland exists only after prepare / apply / finish have run on a Mac that has the 22H374 restore, and this public repository has none of it. CI cannot invent it.

Pack one prepared directory into an archive somewhere https can reach and put its URL in `userland_url`. The archive may be `.zip`, `.tar.gz`, `.tgz` or `.tar` and, once unpacked (1-4 wrapper directories are fine), must hold these five files:

```
result.json                          # build must be 22H374
apply-complete.json
pong-ipados18-system.cdr             # OS, a 4 KiB-aligned raw APFS disk
pong-ipados18-systemos.cdr           # the output of build-systemos18.py
pong-ipados18-data.cdr               # Data
```

A `volume-layout.json` (see below) may sit beside them; it is optional.

`pong-ipados18-systemos.cdr` is not inside `prepared/`; locally it lives in `apple/ipados18-22H374/prepared/`. Put it in the same directory when packing. If `os-storage.json` says `ready: true` (the OS was switched to a tracked sparsebundle) the archive is refused: the layout script sizes the raw image and would disagree with what the installer streams.

The workflow reads each volume's UUID from its APFS volume header, writes `ci-volume-layout.json`, and hands those UUIDs to the kernel's `XNU_OWNER_*_VOLUME_UUID` values and to the boot manifest's root-match value. So **one archive belongs to one kernel build**: the UUIDs are compiled in, and a mismatch mounts nothing. When the headers cannot be read (an unusual container, say), put a `volume-layout.json` in the archive naming `os`, `systemos` and `data`:

```json
{
  "os": {"uuid": "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"},
  "systemos": {"uuid": "..."},
  "data": {"uuid": "..."}
}
```

Filling `userland_sha256` checks the whole archive first; a mismatch stops the run before a byte is unpacked. A run with the archive does three things: unpack and validate (`scripts/stage-pong-userdata.py`), write the SHNGROOT directory (`scripts/prepare-ipados18-ufs-layout.py`, which has to read all three images, about 19 GiB, and is the slow part), then assemble the whole image from that directory (`scripts/build-pong-userdata-image.py`).

`userdata_image` is on by default. The product is uploaded as four artifacts, `pong-userdata-part0` through `part3` (about 5 GiB each, kept 30 days). One artifact is capped at 10 GiB and the image is about 19.4 GiB, so it has to be split; concatenate the parts in order:

```sh
cat pong-userdata-part0 pong-userdata-part1 pong-userdata-part2 pong-userdata-part3 > pong-ipados18-userdata.img
```

The image stops at the end of the last window rather than filling the whole 454 GiB partition; everything the directory does not describe is zero. Flashing still goes through `scripts/install-ipados18-ufs.py`, one window at a time with the SHNGROOT directory committed last. This whole-disk image is the same bytes in another wrapper, for inspection, diffing and keeping a copy; CI never pushes it at a device.

Pack it locally with the repository's script. It checks the five files, the `result.json` build and the image alignment, folds in `pong-ipados18-systemos.cdr`, and prints the sha256:

```sh
python3 scripts/pack-pong-userland.py \
  --prepared ../private-inputs/22H374/prepared \
  --systemos out/ipados18/systemos/pong-ipados18-systemos.cdr \
  --output ../private-inputs/pong-userland.zip
```

Never commit the images. The workflow uses them only after the checks pass, and writes the archive's sha256, the three volume UUIDs it read and the unpacked paths into `ci-manifest.json`.

With no `userland_url` (every push-triggered run), the step only writes `userdata-decision.json` with `skipped` and exits 0. That is not a failure.

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

With a prepared userland there are also `pong-userdata-part0` through `part3` (kept 30 days): the whole userdata image cut into four ordered parts, which `cat` back into `pong-ipados18-userdata.img`. The run's `ci-manifest.json` records the layout, the image size and each part's size under `userdata`.

### A local build still extracts from the phone

A local build needs no archive: it reads the firmware out of the phone. Wi-Fi has a script for it, run in recovery:

```sh
python3 scripts/prepare-pong-wifi.py --serial <serial> --out ../private-inputs
```

It mounts `modem_a` read-only, pulls the whole `image/qca6490` directory into `<out>/qca6490`, and then checks that `amss.bin`, `amss20.bin`, `m3.bin`, `regdb.bin` and at least one `bdwlan*` are present. `adsp.mbn`, `tfa98xx.cnt` and the five Adreno 730 files come out of the same phone's modem and vendor partitions. The build reads them from `PONG_WIFI_FIRMWARE` (default `$PONG_INPUTS/pong-wifi-firmware`) for Wi-Fi, `PONG_AUDIO_INPUTS` (default `$PONG_INPUTS/pong-audio-inputs`, holding `tfa98xx.cnt`) for the backlight, and `PONG_ADRENO_FIRMWARE` (by default the guest's `out/ipados18/firmware/adreno730`). `scripts/pong_paths.py` holds every path rule.

The two inputs can be shared: once the files are in place locally, `scripts/pack-pong-firmware.py` turns them into the zip that Actions takes, so nothing has to be prepared twice.

### What it does not do

- It never flashes a device; a run only produces files.
- It does not invent the userdata inputs. The layout and image are built only when `userland_url` is supplied, because a prepared userland (`result.json`, `apply-complete.json`, and the OS, SystemOS and Data images) can only be produced on a machine that has the 22H374 restore. Without it, `userdata-decision.json` records `skipped` and exits 0; that is not a failure.
- It never pushes the userdata image at a device and never reads a device back. The whole-disk image is the same window bytes in one file.
- The prelink path skips image hashing and the ADT audit (`build-ipados18-prelinked-boot.py` passes `--skip-image-verification` to `build-ipados18-bootimg-local.py`), which leaves fields such as `image_sha256` as `null`; both paths still check the container structure.
- Nothing is verified on hardware. CI validates the container and its packed contents only; it never boots anything.

## Source boundaries and license

Only source, configuration, patches and scripts are published. IPSWs, Apple private binaries, firmware, credentials and generated userdata remain local and ignored. Machine paths belong in local configuration.

`PongPlatform24` identifies the community A730/QCA6490 implementation, independently of the upstream A740/WCN7850 driver. The versioned SM8475 seed is kept under `migration/format2/soc-seed`.

Board code uses BSD-2-Clause-Patent. Imported components retain their licenses and SPDX identifiers; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
