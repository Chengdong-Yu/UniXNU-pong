# DarwinBoot24 for Pong

This device-local copy targets XNU 11417.140.69 / Darwin 24. Its boot_args command
line is 1024 bytes, memSizeActual is at 0x478, and the structure is 0x480 bytes.
The older shared DarwinBoot source and saved firmware volume are preserved.

Build with `scripts/build-darwinboot24-macos.py` from the device project root.
The script builds only this loader in an independent workspace and exports the
EFI binary, log and build record under `mac-build/darwinboot24/`.

An explicit `[[driver]]` entry injects the loader from the temporary FAT payload.
It replaces one matching loader protocol in RAM and restores the older interface
on unload. The MMU-off transition is position-independent code copied into an
extra page within the allocated kernel window. That page is included in
topOfKernelData. No firmware or device partition is written.

## Additional Pong DRAM

The contiguous bootstrap window remains 4 GiB in the iPadOS18 package. Its
256 MiB..8 GiB parser bound is not a limit on total banked memory; Pong's
physical address holes make a synthetic contiguous 12 GiB window invalid.
The ADT reports physical capacity separately and supplies the allowed ranges.
The loader leases actual write-back conventional-memory pages above 4 GiB,
leaves the low DXE heap intact, and reserves another 256 MiB high firmware
pool until ExitBootServices. Each firmware allocation is at most 1 GiB and
adjacent successful leases are coalesced in the XNU wire contract.

Contract validation precedes all extra allocations. Invalid optional range
metadata retains the primary boot window. Reserved, BootServices, RuntimeServices,
framebuffer and uncached/device descriptors never become additional OS RAM.
The actual managed capacity is reported by XNU and will be below physical
capacity. `pong_ram_banks=0` requests only the bootstrap window.

Visible loader stages and the actual EFI status on a failed load distinguish
a return to the boot menu from a kernel entry failure. They do not qualify
successful kernel startup or the full memory capacity.

The MMU-off trampoline cleans the populated payload span (`Used`) rather than
walking every cache line in the entire 4 GiB allocation. Mach-O file/BSS,
boot_args, ADT, optional initrd/trustcache and the trampoline are all inside
that span. The framebuffer is cleaned separately. The boot_args window and
additional owned-bank declarations retain their full sizes; this changes
cache maintenance, not RAM accounting.

The Darwin 24 build has handed off to the native Pong kernel on hardware:
saved cpu2/apfs3 logs reached GICv3/CNTV, UFS, APFS and the matching 22H374
userland helper. This does not qualify the iPadOS 18 UI or Metal interfaces.
Boot evidence is kept under the project's ignored `out/ipados18/logs/` directory.

The copied Test fixtures are retained as reference material; their memory-size
offset was updated for Darwin 24. They are excluded from this standalone build
and were not rerun in this Mac round.
