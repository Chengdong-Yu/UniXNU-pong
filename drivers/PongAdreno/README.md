# Pong / SM8475 Adreno 730 bring-up

This is SM8475 platform support with an **optional cold GMU/CP startup attempt**.
The native BO client is registered only after real authenticated CP fences.
Metal discovery/presentation is not registered; UI still uses software
rendering. `CONFIG_XNU_PONG_ADRENO_PLATFORM=y` links the implementation;
`pong_adreno_start=1` explicitly requests one asynchronous startup attempt.
Absent that argument, this entry performs no GPU MMIO.

## Sources and differences

- Pong's captured `evidence/recovery-20261020/live.dts`: GPU chip id
  `0x07030002`, model Adreno730v3, Cape GPUCC, GMU `0x03d68000/0x37000`,
  RSCC `0x03d50000/0x10000`, GPU SMMU `0x03da0000/0x40000`, GMU SID 5.
- [Nothing SM8475 source](https://github.com/NothingOSS/android_kernel_msm-5.10_nothing_sm8475/tree/sm8475/t/drivers/gpu/msm):
  gen7_reg.h, adreno_gen7_gmu.c, adreno_gen7_hfi.c, adreno_hfi.h and
  adreno-gpulist.h. The matching gen7-4-0 profile uses 2 MiB GMEM,
  gmu_gen70000.bin, a730_sqe.fw and a730_zap. Its clocks are described in
  drivers/clk/qcom/gpucc-waipio.c, with the `qcom,cape-gpucc` fixup.
- [UniXNU AdrenoFirmware](https://github.com/UniXNU/AdrenoFirmware):
  original firmware structure planner and the basis of the isolated HFI fork.
- UniXNU-Sheng/package/ShengGmu.c: startup structure and one-shot failure
  policy. No A740 register map, firmware hash or clock/voltage votes were copied.
- HackiOS/xnu/files/bsd/dev/redwood_adreno.c: separation of platform power,
  native4K GMU/GPU mappings, ZAP authentication, CP start and user-client
  publication. Its A642L register/firmware values are not applicable here.

In particular, this Pong vendor HFI implementation reserves 17 header slots:
the table is 210 dwords, followed by queue storage at byte 848 (16-byte
alignment). Three queues are active; the debug priority is 40. The current
Sheng transport instead uses table field 168, page-spaced queues and other
header fields. The Pong fork uses the actual vendor layout and leaves its
reserved fields zero. A matching sequence alone is insufficient: require an
actual ACK packet type. RSCC sequencer memory starts at dword 0x180 here.

Mesa's freedreno device table names this exact `0x07030002` revision
Adreno730v3 / FD725 (A7XX Gen1) with a revision-specific compiler property.
Do not change the ID to FD730/`0x07030001` or A740 to pass a profile check.
The GMU timer's offset relative to Pong's mapping is 0x16220/0x16224,
not Sheng's 0x14220/0x14224. The supplied DT declares 21 context banks and
24 stream slots; these are capacity facts, not permission to take a bank.

## Implemented module

- `PongAdrenoPlatform.cpp`: actual Pong GCC, APSS DRV2 active TCS3 and
  command-db adapters, cold-master ownership, contiguous native4K allocations,
  real cache maintenance, exact GMU SID5/0x405 context setup and bounded
  firmware/HFI/power startup. It checks actual GX-off/CM3-reset prerequisites
  and refuses to take over a live GPU. Until a shared RPMh aggregator exists,
  parent CX/MX votes retain the highest ACTUAL allowed AUX corner without
  changing CPU/GPU frequency. Only TCS3 is used; sleep/wake votes are intact.
  GMU faults are polled during every hardware wait with IRQs explicitly
  masked, fault reporting enabled and retained DMA backing on failure.
  Stage/results are written to the independent ramoops diagnostic pmsg area.
  Normal boot continues after PASS/FAIL/SKIP. GMU completion alone does not
  qualify CP execution or Metal readiness.
  v95 uses real command ISSUED|COMPL plus idle/fresh-transaction observation
  for polled RSC completion, not an enabled-IRQ prerequisite. It checks
  active TCS4 for conflicting ARC work and reclaims only its own trigger.
- `PongAdrenoGmu.c`: SM8475 resource windows/identity, native4K backing
  requirements, firmware load plan, Pong RSCC/PDC sequence, real cache/TCM
  upload, GMU chip-id conversion to 0x07030200, fresh CM3/HFI readiness waits.
  All I/O uses bounded window callbacks and actual platform prerequisite
  callbacks. Timeout and iteration bounds apply; failure after mutation
  poisons the device, attempts reset and retains DMA backing.
- `PongAdrenoHfi.c`: isolated actual-Pong queue serialization and matching
  ACK transport; no fabricated ACK/fence and no GPU-ready service publication.
- `AdrenoFirmware.c`: original structural firmware planner. No runtime
  image hash, image readback audit or benchmark is introduced.
- `PongAdrenoSmmu.c` / `ArmSmmu4K/`: leased native4K GPU/GMU context and
  exact stream attachment, physical table checks, observation and fault-first
  stop. GPU SID0/0x400 and GMU SID5/0x405 are separate exact routes. Capacity
  does not establish ownership. The v94 adapter leases only cold GMU routes,
  applies Pong's actual ACTLR policy and uses explicit synchronous fault
  polling. The CP now has a separate owned bank0 lower-region context in
  `PongAdrenoCp.cpp`; secure/LPAC/GMU routes are not stolen.
  The local fork no longer silently accepts TBI/ASIDPNE readback differences
  observed only on Sheng. A Pong exception requires actual Pong evidence.
- `PongAdrenoPower.c`: reads actual retained command-db aux data and builds
  Pong's 58-word PERF and159-word BW payloads. It uses rounded primary ARC
  corners to select secondary dependencies, actual gfx/mx/cx levels, zero
  DDR average, peak votes and the actual ACV/CN0 encoding. This serializer
  does not invent command-db addresses; the platform adapter issues the
  actual parent RPMh votes before calling it.
- `PongAdrenoClocks.c`: bounded CX GDSC and Cape200MHz GMU/150MHz HUB/XO
  clock setup, GCC and GPUCC branches. Parent clocks/votes and exclusive
  stopped-master ownership are prerequisites. No A740 PLL program is reused.
- `PongAdrenoGmuStartPower`: real matching-ACK PERF -> BW -> CORE_FW_START
  -> default blocking GX/BW vote after actual HFI readiness. Vendor START10
  belongs later in GPU boot before OOB_GPU, not in this cold HFI sequence.
- `PongAdrenoZap.c`: structural split ELF32 MDT planner and actual PAS13
  shutdown/init/mem-setup/auth-reset flow through a bounded SCM adapter.
  Pong's load header selects `a730_zap.b01`; its `.b02` is certificate/hash
  metadata. Repack the actual header and separate signature file as the
  vendor loader does; no assumption about the packed MDT tail is needed.
  Reservation is0x8b91a000/0x2000, with the PAS image span derived
  from the ELF and rounded to4KiB. Metadata must use retained contiguous
  non-cacheable native4K backing (the vendor XPU requirement). No SCM error
  is turned into an assumed previously-loaded success. No host hash is run.
- `submit/`: generic A7xx PM4/ring/CP-init/timer fork from NativeAdreno.
  Pong CP_ME_INIT disables unnegotiated DMS with bit11; GMU timestamps use
  actual SM8475 offsets0x16220/24. These modules compile and link but do not
  run until GX, actual a730 SQE/ZAP, GPU context ownership and a completed
  init fence are established. No A740 hardware-init table is imported.
- `scripts/build-pong-adreno-platform.py`: derives register facts from the
  matching Nothing gen7_reg.h and builds an arm64 kernel archive. The
  generated register header and build report live beside that archive. Use
  kernel mode, general registers only (no SIMD), and the kernel's iOS16.0
  deployment floor; the runtime target remains the existing 16.6.1 system.

All CPU pages/backing translations must be 4 KiB. A 16 KiB hardware TCM
extent or four-page HFI allocation is not a 16 KiB CPU subpage configuration.
The caller must provide real powered/exclusive mappings, real native4K SMMU
ownership and real cache publication before calling the GMU loader. Merely
copying a firmware file or setting a software flag does not establish them.

## Build artifacts and phone state

Generated kernel archives, firmware inputs and build reports belong in local
output directories and are not part of this source repository. The v95
bring-up fixed RAM-helper boot-argument inheritance and the polled completion
path. Adreno hardware startup remains unqualified; the software-rendering and
touch path is the known working baseline.

## v96 CP and native user client

`PongAdrenoCp.cpp` connects actual gen7-4-0 HWCG/GBIF/protection/restore
facts generated from Nothing's matching source. It uses2MiB GMEM and
uche_gmem_alignment0, not Sheng's3MiB/16MiB window. SQE skips its vendor
header dword. DMS/IFPC/preemption remain disabled; CP init keeps DMS bit11.
GPU SID0/0x400, ACTLR0x32b and bank0 are leased while masters are stopped.
GMU OOB_GPU and START10 require fresh real ACKs; GX power/clock status is
checked before any GX MMIO. The ring then submits CP init, waits for its
real timestamp, authenticates Pong's b01 code/b02 metadata through PAS13,
and submits CP_SET_SECURE_MODE0 with another real fence. No synthetic
workload or content/hash verification runs.

ZAP0x8b91a000/0x2000 is retained before VM allocations when inside the
XNU window; overlap with an existing boot payload refuses authorization.
SCM uses bounded SMCCC64/QCOM A6 resume and real errors; no unconditional
shutdown of an unloaded PAS. HFI bidirectional pages use matching normal
NC/WC CPU attributes, rather than cached volatile reads of stale ACKs.

`PongAdrenoClient.cpp` serves the fixed NativeAdreno1..16 methods on
`NativeAdrenoGpuPublished`: native4KiB BO allocation/import, scoped GPU VA
windows, matching CPU-map descriptors/lease teardown, queues, submission,
real fences/status/counter. Every map/unmap drains actual jobs first;
failed retirement retains ownership. Import pins only the caller's pages.
No arbitrary physical-memory API, AGX ABI, sandbox bypass or mailbox is
included. The first Pong client uses explicit cache maintenance; no
Sheng WC/coherence qualification or per-IB content probes are inherited.

## Remaining hardware qualification

The connected v96 path compiles/links; only actual phone results establish
which stages executed. General asynchronous IRQ/scheduling, reset/resume,
Metal discovery and GPU IOSurface presentation remain unqualified. Current
work uses bounded synchronous completion/fault polling with IRQs masked.
The userspace20G81 bundle is staged privately, with optional Apple heap and
compression assumptions disabled; it is not an automatically exposed Metal
accelerator. No GMU-only success is reported as hardware acceleration.
