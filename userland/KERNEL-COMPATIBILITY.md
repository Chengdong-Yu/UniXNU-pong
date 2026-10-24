# Pong 20G81 / HackiOS kernel comparison

## Evidence from v58

Source: `/Volumes/PongDev/build/Pong-iOS16-v58-init4k/evidence-rec/console-ramoops-0`.

- Genuine Apple APFS 2142.140.9 loads; SM8475 UFS publishes OS and SystemOS.
- SystemOS mounts at `/private/preboot`; the shared-cache path lookup succeeds.
- Data mounts at `/private/var`, read-write, 536870912 bytes.
- The previous 16 KiB code-signature page-size rejection is absent.
- Last retained kernel line is the missing-AMFI entitlement-acceleration bypass.
  No `Pong init` milestone or original launchd startup is recorded. This does not
  establish that iOS reached userspace, nor that UFS timed out.
- pmsg contains Android/recovery records, not XNU's userspace log.

## Interfaces and adaptation

| Component | HackiOS 16.5 / 20F66 | Pong 16.6.1 / 20G81 decision |
|---|---|---|
| XNU | 8796.121.2, Darwin 22.5 | Retain 8796.141.3, Darwin 22.6; compile imported interfaces against these headers |
| APFS | Custom APFS implementation / string mount spec | Retain genuine Apple APFS 2142.140.9 and existing native mount payload; v58 mounts succeed despite restore snapshot/group messages |
| Storage | Redwood MMIO/SMMU/firmware | Retain SM8475 NativeUfs; optimize copies, aligned writes and successful-command diagnostics |
| Crypto | minicrypto | Retain existing genuine 20G81 corecrypto dependency closure |
| AMFI interface | miniamfi function table and XML entitlements | Import development implementation, guarded by Pong build + explicit AMFI boot argument |
| MAC requests | AMFI dyld policy/bootarg queries, Sandbox requests | Import generic policy handling and entitlement-based privilege decisions; no device MMIO |
| Writable Data | ramfs | Keep native writable APFS PackedRAMDisk; no ramfs/custom APFS ABI dependency |
| Helper layout | separate helper image, stage0 -> init -> original launchd | Add Data-resident `/private/var/pong/init`, selected explicitly by kernel boot argument |
| User logging | loader log mirror | Existing Pong `console_write` already mirrors to ramoops; ordinary console milestones; private milestone sysctl removed in v63 |
| Console lifetime | TIOCSCTTY refused; console mode 0666 | Port these two changes for shared daemon logging |
| Jobs | selected core services, remaining jobs on-demand | Existing 20G81 policy selects all core labels; retain actual 20G81 plists and curated database |
| dyld/cache | 20F66 cryptex + skipIgnition | Keep 20G81 matching split cache at `/private/preboot`, existing skipIgnition support |
| Graphics/input/audio | Redwood-specific substitutes and injections | Still require Pong adaptation; do not copy SM7325 hardware constants or fixed 20F66 addresses |
| App cryptex | matching 20F66 AppOS | No matching 20G81 AppOS supplied; jobs requiring it remain excluded |

The generic AMFI/MAC sources come from the local `hackios-xnu-kernel-main/xnu/files`
project under its APSL 2.0 license. The userland helper remains under HackiOS
userland's MIT license. These changes are development compatibility code and do
not establish a completed iOS or graphical boot.

## Construction policy

Image hashing, image audits and device readback verification are skipped by user
request. Compiler/linker diagnostics and parsing needed to construct relocation,
filesystem and boot-format records remain part of building. No tests/benchmarks
are run, and no throughput improvement is claimed without runtime evidence.


## v59 runtime and v60 correction

The v59 recovery console confirms all three development policy registrations
(AMFI interface, AMFI MAC policy, Sandbox MAC policy), both APFS mounts and
`Pong init: helper exec result=0`. It contains no first helper milestone. The
mount errors printed earlier do not stop the boot in this trace.

Comparing the board headers revealed that HackiOS's generic Cortex profile
unconditionally defines `__ARM_PAN_AVAILABLE__`, while Pong's PONG.h did not.
Pong also selected `CONFIG_XNU_NATIVE_PAN=n`. With the default SCTLR.SPAN=0,
exception entry on a PAN-capable CPU sets PSTATE.PAN, but an A53-style copyio
build omits the temporary PAN clear around user memory copies. v60 enables the
Pong PAN definition and the existing armv8.1 compiler flags for copyio/pcb.
The v60 recovery log still ends at helper exec result=0. PAN is now configured, but this change alone did not advance the observed boot. The retained log has no first helper milestone, panic, or UFS timeout. Bounded first PID 1 user exception records are added for the next candidate to locate the actual transition failure.

## Independent HackiOS 16.6.1 build

`/Volumes/PongDev/build/Pong-HackiOS-xnu-8796.141.3` contains a separate source
checkout: genuine Apple XNU 8796.141.3 plus HackiOS's redwood.patch and added
files. Patch application produced no rejected hunks; this alone does not prove
runtime compatibility. The candidate uses a dedicated single-core PONG profile
with native PAN, no Raspberry Pi UART MMIO, native Xcode toolchain integration
and libpthread rebuilt from source against the candidate's exported headers.

It retains HackiOS's custom APFS/minicrypto and GenericARMPE/Mini driver API.
Its original loader-return, storage mailbox and `/arm-io/gic` DT contract differ
from UniXNU/DarwinBoot. A completed Mach-O build must not be represented as an
already bootable UniXNU image. The SM8475 UFS/SMMU handoff and loader integration
remain separate work before attempting this candidate on the phone.

Independent build result: native macOS make completed with return code 0. Kernel and unstripped symbols are saved under `Pong-HackiOS-xnu-8796.141.3/artifacts/`; build and loader limitations are documented in `MACOS-PONG.md`. This candidate has not been booted.

## v61 recovery evidence and v62 correction

The retained v61 log proves PID 1 reached EL0: PC 0x100001bac, SP 0x16fdffd30,
with an EL0 write translation fault (ESR 0x92000047) at 0x1000042b8. Such a fault
can be normal demand paging; it does not establish invalid helper memory.

The v61 entry diagnostic incorrectly called printf before sleh_synchronous
restored the inherited interrupt mask. This triggered the genuine console
invariant panic at serial_console.c:168. v62 moves the entry message after the
existing interrupt restoration and guards all new exception printf calls with
ml_get_interrupts_enabled(). It preserves the console assertion and the normal
vm_fault handling. No no-intr console boot argument or panic suppression is used.

The first helper milestone's absence alone therefore did not prove no user-mode
execution. The next log must establish whether the translation fault resolves
and which syscall executes next. SystemOS/shared-cache lookup and writable Data
remain successful in the v61 trace.

## v62 / v63: remove diagnostic sysctl from the critical path

v62 recovery evidence shows four successful user-mode page-fault returns, then
SVC 274 (0x112, sysctlbyname) with no return record. No new panic or UFS timeout
is retained. That syscall is the helper's first kern.pong_boot_log milestone.
The precise internal wait location is not yet established.

v63 retires that private diagnostic sysctl and emits milestones through the
ordinary /dev/console after opening and duplicating it. Optional helper tuning
of hw.memsize/maxvnodes and kern.dyld_flags is omitted: the kernel owner hook
already sets skipIgnition in the commpage and exec strings. The writable Data
seed and real original 20G81 launchd handoff remain.

Two bounded, interrupt-safe PID 1 syscall checkpoints distinguish credential
update from BSD handler dispatch. They preserve the actual syscall and error
handling; no sysctl success or storage response is fabricated. The modified
helper is packaged only in a cloned RAM Data seed, requiring no phone userdata
rewrite.

## v63 recovery and v64 boundary records

v63 confirms credential update and BSD dispatch for open syscall 5. It has no
open return or helper console milestone. Removing the milestone sysctl did not
resolve the underlying stop; v62/v63 do not establish a sysctl-specific deadlock.

v64 adds a single bounded (32 message maximum) kernel progress sink, enabled
only by pong_init_trace and only after PID 1's first EL0 exception. Its call
sites are outside the user-access/PAN-clear window and outside TTY/FD locks.
They cover the open handler, state/fd allocation, vnode authorization, copyin/
copyinstr return, and kmopen entry/return. The existing dispatch line also
records the selected handler address and native open address. These are runtime
location records, not image audits or evidence that open/iOS startup is fixed.
No APFS, storage, signature policy or helper image changes are made in v64.

## v65: actual uaccess policy/recovery repair

v64 reaches copyinstr after successful open/fd setup, before kmopen. v65 changes
the development PAN policy consistently across copyio, initial CPSR and SCTLR.SPAN
and adds existing-EFAULT recovery for recoverable PAN permission faults. The
precise fault state was not captured, so PAN remains a working diagnosis until
the next log. See Pong-iOS16-v65-uaccess/NOTES.md for scope and runtime status.
No new diagnostic points are added in this candidate.

## v65 evidence and v66 effective build configuration

v65 progresses through user copy recovery, but every copyinstr/copyin returns
EFAULT. PID 1 exits 127 and XNU reports the genuine initproc panic. The missing
development no-PAN mode line led to a build audit: XNU 8796 never consumes the
CFLAGS_EXTRA variable used by the caller. Requested settings in the old result
metadata therefore did not prove those options reached compilation.

v66 passes RC_NONARCH_CFLAGS, which MakeInc.def actually folds into both C and
assembly flags, and invalidates all kernel objects on global flag changes. This
makes the existing development uaccess/PSTATE/SCTLR policy consistent. The
generated component file lists and the v65 log confirm generic HackiOS AMFI/MAC
registration. Prelink construction metadata already includes genuine 20G81
pthread, corecrypto and APFS; no duplicate source provider is enabled. Redwood
hardware and 20F66 fixed-address injection are not part of this UniXNU candidate.
No new runtime probes or image verification are added. The v66 recovery log
subsequently confirms native PAN and working user copies/helper execution;
the legacy no-PAN fallback is disabled. Its remaining shared-cache signature
error is handled in v67 by authenticating the original 16 KiB CodeDirectory
blocks and deriving 4 KiB VM-validation hashes.

## v68 evidence and v69 launchd compatibility

v68 permits shared-cache mapping from the exact owner-authorized read-only
SystemOS mount. The retained log reaches the unmodified Apple 20G81 launchd,
which exits with `launchd.ios cannot be run directly (stdout: 1)`. Inspection
of that binary's entry shows the rejection follows a nonzero `isatty(1)`;
its separate PID check is later. The filename is only printed in the error.

v69 fixes the helper's descriptor handoff: Data preparation retains screen
progress, then stdin/stdout go to `/dev/null`, with stderr on `/dev/console`.
This removes the interactive-stdout condition without patching Apple's code
or suppressing PID 1 failures. Only the RAM Data helper is replaced.

The Pong DEVELOPMENT kernel additionally imports HackiOS's xpcproxy identity
version handling, preserving launchd's saved audit token across a proxy-to-
service exec. `xpcproxy_idversion=1` restores upstream behavior. Its optional
`applog=1` spawn file-action handling exposes daemon fd 1/2 previously sent to
`/dev/null`; direct exec of PID 1 is unaffected. No global verbose OS activity
environment or 20F66 injected libraries are introduced. Existing native AMFI/MAC,
PAN, original 20G81 dependency providers and SM8475 UFS remain enabled.

## v69 evidence and v70 boot-policy/path corrections

v69 reaches launchd's system bootstrap and executes many original boot tasks.
PID 22, launchd_cache_loader, then aborts because the adapted OS has deliberately
moved the signed `/System/Library/xpc/launchd.plist` cache away. Its crash is
fatal because launchd marks that boot task RequireRun/RequireSuccess. Original
mount tasks also look for the absent Apple fstab, boot-manifest firmware and
FactoryData paths. None of those messages establishes a failed native UFS or
SystemOS mount; both and the writable RAM Data mount succeed in this log.

HackiOS retains a RAM boot policy even after switching to its flash root, which
skips incompatible original-device boot tasks and allows directory-based jobs.
The inspected 20G81 launchd detects `rd=md0` using strstr, whereas XNU parses
complete names. Packaging v70 with `--launchd-ramdisk` supplies the userspace
marker `launchd_rd=md0`, leaving actual UFS root selection unchanged. No panic
handling or fake cache-success return is added. This marker is explicitly
specific to the inspected 20G81 launchd parsing.

The old Data seed's `/var/db/timezone/zoneinfo -> /usr/share/zoneinfo` loops
through the original OS symlink back into Data. Both future seed construction
and the helper now use `/usr/share/zoneinfo.default`, matching HackiOS's
timezone fix. A valid localtime link is retained; only a still-invalid link
falls back to UTC. A missing writable cores directory is created. XNU is
reused from v69; v70 changes boot policy and the RAM Data helper only.

## v71: UI kernel foundation, prepared while v70 runs

The UniXNU kernel now includes HackiOS's software IOSurface service and the
software IOMobileFramebuffer presentation/user-client implementation. They start
after real IOKitBSDInit only with `pong_software_ui=1`. The actual Pong boot
framebuffer supplies base/stride/geometry; invalid handoff data is rejected.
The port removes Redwood loader-return/recording hooks, empty accelerator
publication and iPhone-X default geometry; it does not start additional CPUs.
No Metal, GPU MMIO, input hardware or fake Secure Enclave backend is enabled.

The kernel also imports root-configured `kern.exec_inject`, with a locked
configuration snapshot, for future per-program 20G81 compatibility libraries.
Its default is empty. The unsafe alternative of loading the existing 20F66
QuartzCore fixed-address/vtable hooks is not used. Memory-pressure percentages
use usable `sane_size`, following HackiOS, and Pong's vnode capacity is 16384.

New surface checks cover plane geometry, row strides, allocation overflow and
wrapped-client ranges. The kernel and boot image compile/package successfully;
the IOSurface/framebuffer private ABI still needs 20G81 runtime evidence, and
the userland graphics/input adaptation remains. No phone reboot or deployment
was performed while the user was collecting v70 logs. Full decisions and paths
are in `/Volumes/PongDev/build/Pong-iOS16-v71-ui-foundation/NOTES.md`.
