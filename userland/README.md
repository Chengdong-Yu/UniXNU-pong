# Pong / UniXNU — iOS 16.6.1 userland adaptation

## v99 local Sleep configuration service

The20G81 iPad restore contains Sleep.framework but omits sleepd's job and
SleepDaemon.framework. The matching20G81 iPhone sleepd imports that missing
framework and cannot simply be copied into this restore.

`helper` also builds `sleepserver` and `com.apple.sleepd.plist`. Stage the binary
in `/private/var/pong` and the job in the existing Data `pong/boot-jobs` view.
The mobile-user service uses the shipped `HKSPSleepServerInterface` and client
interface, plus the exact20G81 callback ABI and model classes. It retains the
Sleep entitlement check, stores disabled-schedule/basic settings/event-record
configuration atomically under mobile's `Library/Application Support/PongSleep`,
maintains real revision anchors and notifies connected clients to refresh.
The initial state is Off/Disabled; no configured alarm or tracking is claimed.

This is a limited configuration provider. Alarm execution, Focus control,
Health collection and Watch sync are not implemented; operations requiring
them return explicit errors. It does not publish the unsupported alloy IDS
service, redirect Sleep requests to healthd, or install16.5 private binaries.
The v99 device run must establish whether the listener removes the observed
missing-service retry storm. Native GPU startup/scanout are separate work.

This directory adapts the **userland** project. It does not build or replace the
HackiOS kernel. Inputs are the local **16.6.1 / 20G81 J120** OS and matching
SystemCryptex, plus Pong's existing native APFS scratch Data seed.

## Differences from the upstream 16.5 recipe

| Upstream recipe | Pong adaptation |
|---|---|
| iPhone10,3 / 10,6, 16.5 / 20F66 | J120 arm64, 16.6.1 / 20G81, checked before building |
| 16.5 prebuilt executables and fixed-address injections | Recompile static init and the v73 UI adapter; preserve Apple 20G81 executables/cache; use runtime Objective-C metadata and a 20G81-specific QuartzCore slot |
| `/redwood` helper image on `/dev/md1` | Embed helpers at `/pong` on a copy of the real OS image |
| HackiOS ramfs, APFS string mount arguments, bindfs, Redwood sysctls | Native Apple APFS mount payload used by the existing Pong kernel; no custom filesystem or Redwood syscall dependency |
| Merge the cryptex into the selected root | Reuse the existing SystemOS mount at `/private/preboot`; repair canonical Cryptex and shared cache links |
| 20F66 Data skeleton and migration preferences | Extract directory modes and numeric owners from 20G81 APFS inodes; use 20G81 preferences |
| Entire stock launchd database | Apply `mkjobs.py`'s core/on-demand service strategy to the actual 20G81 plists, with stdout and stderr on `/dev/console` |
| Redwood firmware and graphics/media injection | Omit Redwood hardware resources; v73 connects the native software framebuffer and a limited 20G81 UI adapter |

The static init opens console descriptors before handing PID 1 to the **unchanged
20G81 launchd**, preserved as `/sbin/launchd.ios`. It seeds **absent** Data entries without replacing existing user data. Optional
maxvnodes/sysctl tuning is omitted in v63 onward. The kernel owner mount hook
sets the 64-bit skipIgnition flag before init. No test suite runs during boot.

The static helper is explicitly signed with **4096-byte CodeDirectory pages**,
matching Pong's 4K kernel and the stock 20G81 launchd. The host's default 16K arm64 signature otherwise prevents PID 1 from executing.
Image hashes, readback comparisons and audits are disabled by default.
`--verify-images` enables them only when explicitly requested.

## v73 SpringBoard adapter

`helper --out OUTPUT --developer XCODE_DEVELOPER` now builds both `init` and
`libpong-ui.dylib`, signed with 4 KiB CodeDirectory pages. Copy both into `pong/`
on the cloned writable RAM Data volume before packing it. The helper configures
`kern.exec_inject` before executing the original launchd, targeting SpringBoard,
backboardd and the two container-manager processes. Simply compiling the kernel
exec hook without this configuration leaves the adapter disconnected.

The adapter skips Siri orb shader prewarming when Metal reports no device,
selects software-readable JPEG/PNG snapshots, disables the optional vibrancy
effect, and uses real UID container paths when personas are unavailable on this
development port. It caches an actual failed accelerated-renderer creation so
CoreAnimation can retain its software renderer. Objective-C method argument and
return types are checked at runtime; incompatible methods remain unchanged.
The QuartzCore renderer vtable slot is specific to the supplied 20G81 arm64
cache and is replaced only when its current pointer and image match.

These adaptations do not supply Pong GPU, touch or audio hardware drivers.
The v75 kernel adds HackiOS's software AppleKeyStore, including upstream
wrap/unwrap, to resolve the missing service behind the v74 MobileKeyBag
registration trap in SpringBoard, contextstored and biomed. Package with
`pong_software_keybag=1` and `pong_software_ui=1`. This is a no-passcode software
bag, not a SEP; wrapping is the upstream reversible development format.
The speculative ContextStore event-handler interposer has been removed.
Runtime success must be established from the next boot.

## Build on this Mac

### v76 optional startup services

The helper can compose a Data-resident `pong/boot-jobs` directory over the
existing `/pong/LaunchDaemons`, and an empty `pong/empty-hal` over the system
HAL plug-in directories. It is signed with `com.apple.private.bindfs-allow`;
the kernel's normal bindfs entitlement check is retained. This follows upstream
init's composition and removes the observed Bluetooth HAL retry storm while
retaining mediaserverd's VideoToolbox service. `BOOT_SKIP` in build.py records
the six optional widget, Screen Time, remote-management/activity jobs omitted
for initial bring-up; core jobs are protected from this list.

The current UI library also adapts HackiOS's development SpringBoard setup gate
through runtime ivar/type checks, and disables optional camera-filter prewarming
without Metal in mediaserverd. It contains no GPU driver or 20F66 object offset.
Actual graphical startup remains to be established from runtime evidence.

```sh
/usr/bin/python3 pong/build.py prepare \
  --out /Volumes/PongDev/build/Pong-iOS16.6.1-HackiOS-userland-v1 \
  --os-root /Volumes/SydneyG20G81.J120J121J207J208OS \
  --os-image /Volumes/PongDev/handoffs/Pong-iOS16-Mac-Next-20261020/apple/20G81/096-57366-083.disk.cdr \
  --system-cryptex /Volumes/SydneyG20G81.J120J121J207J208SystemCryptex \
  --data-image /Volumes/PongDev/handoffs/Pong-iOS16-Mac-Next-20261020/UniXNU-pong/build/apfs-scratch-data-v25/scratch-data.raw.dmg \
  --developer /Volumes/PongDev/Applications/Xcode.app/Contents/Developer
```

Use a fresh output directory for another build. `prepare` compiles and stages the
change, records shared-cache filenames/sizes, and clones both source images.
`apply --out OUTPUT` mounts **only those clones** and requires macOS administrator
authorization. `finish --out OUTPUT` writes `result.json` and converts Data to the existing
DarwinBoot `DBRDISK1` format. It performs no image verification by default.

Outputs:

- `pong-ios1661-system.cdr`: native GPT/APFS OS image with the original volume UUID.
- `pong-ios1661-data.cdr`: native 512 MiB APFS Data image with Pong's existing UUID.
- `pong-ios1661-data.packed`: sparse Data image for DarwinBoot / NativeRamBlock.
- `build-plan.json`, `jobs-report.json`, `data-metadata.json`, `result.json`:
  version inputs, exact job decisions, numeric ownership and construction results.

The stock LaunchDaemons, NanoLaunchDaemons, LaunchAngels and XPC launchd database
are retained under `/pong/stock-config`. Staged 20G81 applications are moved into
`/Applications`, as an installed system expects; no 16.5 applications are added.
The Data migration preference is a **development clean-seed setting**, adapted
from upstream; it is not evidence that Apple's restore/migration process ran.

## Kernel and installation requirements

Use the current **DEVELOPMENT** UniXNU build: stock RELEASE XNU rejects static
arm64 executables in `bsd/kern/mach_loader.c`. The current UniXNU must publish the native OS root and mount matching SystemOS at
`/private/preboot` before PID 1, with `dyld_flags=0x8` (skipIgnition). These paths
match the existing Pong owner mount configuration. The AMFI development policy
must accept the locally signed static helper; keep the user's existing
`amfi_get_out_of_my_way=1` configuration.

Full iOS startup also needs **writable Data**. The v58 kernel has NativeRamBlock and owner Data mounting enabled. Its recovery
log confirms `/private/var` mounted read-write on the 512 MiB native APFS Data seed.
Other configurations must enable both features; supplying an OS image alone does
not enable Data. Either mount a real selected Data volume
before init, or enable NativeRamBlock in the existing UniXNU build and supply the
packed seed. Init can mount an explicitly selected native APFS BSD device using
`pong_data_device=/dev/diskXsY`. It never guesses or formats a disk. Without Data,
it prints the missing prerequisite and continues to launchd for diagnostics.

The App cryptex is not in the current inputs. App-cryptex-only jobs are recorded
as excluded; never substitute the 16.5 App cryptex. Matching 20G81 AppOS can be added
as a separate, version-matched follow-up. Native graphics/IOSurface support is
also required before SpringBoard can draw a usable interface. These image changes
alone do not establish that iOS or its UI boots.

Neither the builder nor `apply` invokes ADB/Fastboot, writes phone partitions, or
changes the original restore images. See `../LICENSE` for upstream's MIT license.


## v59 helper updates and kernel comparison

See [KERNEL-COMPATIBILITY.md](KERNEL-COMPATIBILITY.md) for the comparison with the
local HackiOS kernel and the v58 recovery evidence.

The helper can now live at `/private/var/pong/init` in the packed Data seed,
selected by `pong_init_path=/private/var/pong/init`. It then executes the original
`/sbin/launchd.ios` as PID 1. This avoids rewriting the OS/userdata image for small
init updates, matching upstream's separate helper-image approach.

```sh
/opt/local/bin/python3.12 pong/build.py helper \
  --out /Volumes/PongDev/build/Pong-iOS16-v59-hackios-abi/userland \
  --developer /Volumes/PongDev/Applications/Xcode.app/Contents/Developer
```

The v59 private `kern.pong_boot_log` endpoint was retired in v63. Init now
records milestones on `/dev/console` after opening it, before/after Data setup
and before the original launchd exec. It also applies upstream's `chmod /dev/console 0666`, so
non-root jobs can open their configured standard output/error. The kernel rejects
`TIOCSCTTY` on Pong's shared console, preserving it across daemon exits.

Before handing over to the original launchd, init changes stdin/stdout to
`/dev/null` and retains stderr on `/dev/console`. The 20G81 launchd rejects a
terminal on stdout (`cannot be run directly (stdout: 1)`); its filename suffix
is not the cause of that error. Helper progress still appears before the exec,
and launchd/daemon errors remain visible on stderr.

For this curated 20G81 job layout, package with
`build-pong-prelinked-boot.py --launchd-ramdisk`. It supplies `launchd_rd=md0`
to select the original launchd's RAM boot policy, as HackiOS does with
`rd=md0` after switching its root. XNU parses complete argument names and
continues to choose Pong's UFS root by UUID. This avoids running the signed
service-cache loader after that cache has deliberately been moved away.
Do not pass an actual `rd=md0` to this UFS-root layout.

The helper also repairs old Data seeds whose timezone link points back into
`/usr/share/zoneinfo`. It uses the shipped `/usr/share/zoneinfo.default`,
preserves a working localtime selection, and creates a missing cores directory.

The imported HackiOS development AMFI/MAC code is enabled only for a Pong DEBUG
or DEVELOPMENT build with `amfi_get_out_of_my_way=1`. Entitlement queries read the
running executable's embedded XML entitlement dictionary; an absent entitlement
remains absent. No Redwood hardware addresses or 20F66 injection addresses are used.
