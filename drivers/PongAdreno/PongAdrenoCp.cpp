/* SPDX-License-Identifier: MIT */
#include "PongAdrenoCp.h"
#include "PongAdrenoCpProfile.h"
#include "PongAdrenoZap.h"
#include "submit/NativeAdrenoSmmuContext.h"
#include "submit/NativeAdrenoCpInit.h"
#include "submit/NativeAdrenoIrq.h"
#include "submit/NativeAdrenoPwrup.h"
#include "submit/AdrenoPM4.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern "C" const uint8_t PongAdrenoSqeFirmware[],PongAdrenoZapMdt[],PongAdrenoZapCode[],PongAdrenoZapSignature[];
extern "C" const size_t PongAdrenoSqeFirmwareBytes,PongAdrenoZapMdtBytes,PongAdrenoZapCodeBytes,PongAdrenoZapSignatureBytes;
extern "C" volatile uint32_t native_adreno_wc_enabled;
extern "C" unsigned int pong_adreno_zap_reserved;
extern "C" IOReturn armcpu_spi_attach(uint32_t,bool,void (*)(void *),void *,uint32_t *,uint64_t *);
extern "C" int get_preemption_level(void);
namespace {
constexpr uint64_t iovas[]={0x100000,0x200000,0x300000,0x301000,0x302000};
constexpr size_t sizes[]={0x13000,0x8000,0x1000,0x1000,0x1000};
/* Pong DT gfx3d_user: SID0/mask0x400; gfx3d_lpac: SID1/mask0x400.
 * SQE boots LPAC before consuming the first PM4 packet. Both engines use
 * this driver's single address space; every BO and table update is then
 * visible to both. SID2/0x402 belongs to gfx3d_secure and is not leased.
 */
constexpr uint16_t gpu_sids[]={0,0x400,1,0x401};
constexpr unsigned gpu_stream_count=sizeof(gpu_sids)/sizeof(*gpu_sids);
/* Same fault bits as Sheng's ArmSmmu4KProgram. FSR[10:9] is FORMAT;
 * 0x400 denotes the long-descriptor format, not an active fault. */
constexpr uint32_t SmmuContextFaultMask=0xc00001feu;
struct Device {
    PongCpOps ops{};
    NativeAdrenoMemory tables,buffers[5],metadata;
    NativeAdrenoSmmuContext context;
    NativeAdrenoVm vm;
    NativeAdrenoRing ring;
    NativeAdrenoCpInit init;
    NativeAdrenoSubmit submit;
    NativeAdrenoTimer timer;
    NativeAdrenoStatus status;
    NativeAdrenoIrq irq;
    uint32_t irq_fatal=0,irq_count=0;
    PongCpBackend backend{};
    OSSharedPtr<IOMemoryMap> zap_map;
    PongZapState zap{};
    unsigned output_bits=0;
    uint32_t alias_scratch[262144]{};
    bool prepared=false,started=false,poisoned=false,irq_attached=false;
};
Device *gpu;
static uint64_t now(void *) {uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000;}
/* Sheng's IRQ-driven wait avoids a 1ms sleep on every GPU completion. */
static int idle(void *)
{
    if(gpu->irq_attached && ml_get_interrupts_enabled() && !ml_at_interrupt_context() && !get_preemption_level()) {
        assert_wait_timeout(event_t(&gpu->irq_count),THREAD_UNINT,100,NSEC_PER_USEC);
        thread_block(THREAD_CONTINUE_NULL);
    } else IODelay(20);
    return 0;
}
static void note(const char *phase,unsigned result,uint64_t a=0,uint64_t b=0)
{gpu->ops.record(phase,result,a,b);}
static uint32_t rd(uint32_t reg) {uint32_t v=0;if(gpu->ops.registers.read32(gpu->ops.registers.cookie,reg,&v))gpu->poisoned=true;return v;}
static bool wr(uint32_t reg,uint32_t value) {return !gpu->ops.registers.write32(gpu->ops.registers.cookie,reg,value);}
static bool wr64(uint32_t reg,uint64_t value) {return wr(reg,uint32_t(value)) && wr(reg+1,uint32_t(value>>32));}
static uint32_t sr(size_t at) {uint32_t v=0;if(gpu->ops.smmu.read32(gpu->ops.smmu.cookie,at,&v))gpu->poisoned=true;return v;}
static bool sw(size_t at,uint32_t v) {return !gpu->ops.smmu.write32(gpu->ops.smmu.cookie,at,v);}
static bool faulted()
{
    if(!gpu || gpu->poisoned)return true;
    const auto *p=gpu->context.programResult();
    uint32_t fsr=sr(p->context_offset+0x58),global=sr(0x48),irq=rd(GEN7_RBBM_INT_0_STATUS),cp=rd(GEN7_CP_HW_FAULT);
    /* Timestamp, RB-done and software/preemption indications are not errors.
     * Poll real status; no GPU interrupt line is enabled without its owner. */
    const uint32_t fatal=0x33c082c2u;
    const uint32_t captured=__atomic_load_n(&gpu->irq_fatal,__ATOMIC_ACQUIRE);
    if((fsr&SmmuContextFaultMask) || global || ((irq|captured)&fatal) || cp || captured) {
        gpu->poisoned=true;gpu->status.noteFault();
        note("CP/SMMU fault; retain BOs",1,(uint64_t(fsr)<<32)|global,(uint64_t(irq|captured)<<32)|cp);
        if(fsr&SmmuContextFaultMask)
            note("GPU fault FAR/syndrome",fsr,(uint64_t(sr(p->context_offset+0x64))<<32)|sr(p->context_offset+0x60),
                (uint64_t(sr(p->context_offset+0x68))<<32)|sr(p->context_offset+0x6c));
    }
    return gpu->poisoned;
}
static bool client_healthy() {return gpu && gpu->started && !faulted();}
/* No logging, allocation or ring mutation in interrupt context. The GIC
 * owner masks this SPI around the callback; only this decoder clears RBBM
 * status. Retain fatal status for the submitting thread before acknowledging. */
static void interrupt(void *)
{
    NativeAdrenoIrqResult result={};
    const IOReturn status=gpu->irq.handle(&result);
    __atomic_fetch_add(&gpu->irq_count,1,__ATOMIC_RELAXED);
    thread_wakeup(event_t(&gpu->irq_count));
    if(status || (result.flags&NativeAdrenoIrqFatal)) {
        __atomic_fetch_or(&gpu->irq_fatal,result.status?result.status:0x80000000u,__ATOMIC_RELEASE);
        gpu->ops.registers.write32(gpu->ops.registers.cookie,GEN7_RBBM_INT_0_MASK,0);
        gpu->ops.registers.barrier(gpu->ops.registers.cookie);
    }
}
static int publish_ring(void *,const volatile uint32_t *words,size_t bytes)
{
    const auto &m=gpu->buffers[1];auto base=reinterpret_cast<uintptr_t>(m.mapping()),at=reinterpret_cast<uintptr_t>(words);
    if(at<base || at-base>m.bytes() || bytes>m.bytes()-(at-base))return 1;
    const auto result=gpu->buffers[1].publishForDevice(at-base,bytes,false);
    asm volatile("dsb sy":::"memory");return result!=kIOReturnSuccess;
}
static int acquire_fence(void *,volatile uint32_t *words,size_t bytes)
{
    const auto &m=gpu->buffers[2];auto base=reinterpret_cast<uintptr_t>(m.mapping()),at=reinterpret_cast<uintptr_t>(words);
    if(at<base || at-base>m.bytes() || bytes>m.bytes()-(at-base))return 1;
    const auto result=gpu->buffers[2].acquireDeviceWrites(at-base,bytes);
    asm volatile("dsb sy":::"memory");return result!=kIOReturnSuccess;
}
static bool wait_fence(uint32_t fence)
{
    const NativeAdrenoRingClock clock{nullptr,now,idle};uint32_t observed=0;
    auto result=gpu->ring.wait(fence,&clock,500000,&observed);
    note("CP fence",result,fence,observed);
    if(result!=NativeAdrenoRingFenceComplete) {
        note("CP fence failure state",rd(GEN7_RBBM_INT_0_STATUS),
            (uint64_t(rd(GEN7_CP_RB_RPTR))<<32)|rd(GEN7_CP_RB_WPTR),
            (uint64_t(rd(GEN7_RBBM_STATUS))<<32)|rd(GEN7_CP_HW_FAULT));
        const auto *p=gpu->context.programResult();
        note("CP timeout SMMU/status",sr(p->context_offset+0x58),
            (uint64_t(sr(0x48))<<32)|rd(GEN7_CP_STATUS_1),
            (uint64_t(rd(GEN7_CP_SQE_CNTL))<<32)|rd(GEN7_RBBM_CLOCK_CNTL));
    }
    /* A timeout must still observe and latch the real fault, retaining BOs.
     * The previous short-circuit skipped SMMU fault handling on every timeout. */
    const bool failed=faulted();
    return result==NativeAdrenoRingFenceComplete && !failed;
}
static bool wait_bootstrap(uint32_t wptr,const char *phase)
{
    /* Matching Nothing gen7_send_cp_init / gen7_rb_start: drain packets
     * without normal-memory timestamps until ZAP's nonsecure transition.
     * RBBM_STATUS.GPU_BUSY_IGN_AHB is bit23; do not apply that mask to the
     * different GMU AO busy register. Neither condition alone is completion.
     */
    const uint64_t begin=now(nullptr);
    while(now(nullptr)-begin<2000000) {
        if(faulted())return false;
        if(rd(GEN7_CP_RB_RPTR)==wptr && !(rd(GEN7_RBBM_STATUS)&(1u<<23))) {
            note(phase,0,wptr,rd(GEN7_RBBM_STATUS));return true;
        }
        IOSleep(1);
    }
    note(phase,1,(uint64_t(rd(GEN7_CP_RB_RPTR))<<32)|rd(GEN7_CP_RB_WPTR),
        (uint64_t(rd(GEN7_RBBM_STATUS))<<32)|rd(GEN7_CP_HW_FAULT));
    /* One bounded fault snapshot, using Pong gen7 offsets: index0 is SQE
     * PC, index1 the current packet. It distinguishes init from a fetch
     * stall without recurring probes or a synthetic command workload. */
    if(wr(GEN7_CP_SQE_STAT_ADDR,0)) {
        const uint32_t pc=rd(GEN7_CP_SQE_STAT_DATA),packet=rd(GEN7_CP_SQE_STAT_DATA);
        note("bootstrap SQE PC/packet",rd(GEN7_CP_STATUS_1),pc,packet);
    }
    if(wr(GEN7_CP_BV_SQE_STAT_ADDR,0)) {
        const uint32_t pc=rd(GEN7_CP_BV_SQE_STAT_DATA),packet=rd(GEN7_CP_BV_SQE_STAT_DATA);
        note("bootstrap BV PC/packet",rd(GEN7_CP_BV_HW_FAULT),pc,packet);
    }
    note("bootstrap LPAC state",rd(GEN7_CP_LPAC_RB_RPTR),
        (uint64_t(rd(GEN7_CP_LPAC_IB1_BASE))<<32)|rd(GEN7_CP_LPAC_IB1_REM_SIZE),
        rd(GEN7_LPAC_RBBM_STATUS));
    note("bootstrap bus/translation",sr(gpu->context.programResult()->context_offset+0x58),
        (uint64_t(rd(GEN7_GBIF_HALT_ACK))<<32)|rd(GEN7_RBBM_STATUS3),
        (uint64_t(rd(GEN7_GMU_AHB_FENCE_STATUS))<<32)|rd(GEN7_CP_PROTECT_STATUS));
    (void)faulted();return false;
}
/* Same SMCCC64 SIP convention and QCOM A6 resume as Nothing's qcom_scm-smc.c.
 * A WAITQ response requires an IRQ owner we do not have: report it, never ACK it.
 */
static int scm(void *,uint32_t fn,uint32_t info,const uint64_t args[3],int32_t *transport,int64_t *result)
{
    uint64_t first=fn,a6=0,begin=now(nullptr);unsigned busy=0;
    for(unsigned resumes=0;resumes<10000 && now(nullptr)-begin<1000000;) {
        register uint64_t x0 asm("x0")=first,x1 asm("x1")=info,x2 asm("x2")=args[0],x3 asm("x3")=args[1];
        register uint64_t x4 asm("x4")=args[2],x5 asm("x5")=0,x6 asm("x6")=a6,x7 asm("x7")=0;
        asm volatile("smc #0":"+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7)
            ::"x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
        if(x0==1){first=1;a6=x6;++resumes;continue;}
        if(int64_t(x0)==-12 && busy++<20){first=fn;a6=0;IOSleep(30);continue;}
        *transport=int32_t(x0);*result=int64_t(x1);return 0;
    }
    return 1;
}
static int zap_ready(void *) {return gpu->ring.outstanding() || faulted();}
static int zap_publish(void *,const PongZapMemory *,size_t) {asm volatile("dsb sy":::"memory");return 0;}
static bool authorize()
{
    if(!pong_adreno_zap_reserved){note("SKIP ZAP reservation overlaps boot payload",1);return false;}
    if(gpu->metadata.allocate(4096,gpu->output_bits,true,4096) || gpu->metadata.makeWriteCombined())return false;
    OSSharedPtr<IOMemoryDescriptor> memory(IOMemoryDescriptor::withPhysicalAddress(PongZapReservedPhysical,PongZapReservedBytes,kIODirectionInOut),OSNoRetain);
    if(memory)gpu->zap_map=OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere|kIOMapWriteCombineCache),OSNoRetain);
    if(!gpu->zap_map)return false;
    PongZapConfig c={PongAdrenoZapMdt,PongAdrenoZapMdtBytes,PongAdrenoZapSignature,PongAdrenoZapSignatureBytes,2,
        PongAdrenoZapCode,PongAdrenoZapCodeBytes,1,
        {static_cast<volatile uint8_t *>(gpu->metadata.mapping()),gpu->metadata.spans()[0].physical,4096},
        {reinterpret_cast<volatile uint8_t *>(gpu->zap_map->getVirtualAddress()),PongZapReservedPhysical,PongZapReservedBytes}};
    PongZapOps ops={nullptr,64,zap_ready,zap_publish,scm};PongZapResult r={};
    auto status=PongAdrenoStartZap(&ops,&c,&gpu->zap,&r);
    note("PAS13 ZAP auth",status,(uint64_t(r.phase)<<32)|uint32_t(r.transport),uint64_t(r.secure_result));
    return status==PongZapSuccess;
}
static bool hardware_init()
{
    /* GDSC/reset does not necessarily clear the firmware's GBIF halt.
     * Sheng and Nothing's gen7 GMU path explicitly release it before fetch. */
    if(!wr(GEN7_GBIF_HALT,0) || !wr(GEN7_RBBM_GBIF_HALT,0))return false;
    /* ACK is a halt-completion indication, not a release prerequisite.
     * Sheng/vendor clear HALT and flush the posted writes; do the same. */
    const uint32_t cx=rd(GEN7_GBIF_HALT),gx=rd(GEN7_RBBM_GBIF_HALT);
    note("GBIF traffic released",0,(uint64_t(cx)<<32)|gx,
        (uint64_t(rd(GEN7_GBIF_HALT_ACK))<<32)|rd(GEN7_RBBM_GBIF_HALT_ACK));
    /* No protected workloads: same empty trusted range as Sheng. ZAP/PAS13
     * still authenticates the actual A730 firmware before nonsecure use. */
    if(!wr(GEN7_RBBM_SECVID_TSB_CNTL,0) || !wr64(GEN7_RBBM_SECVID_TSB_TRUSTED_BASE_LO,0) ||
       !wr(GEN7_RBBM_SECVID_TSB_TRUSTED_SIZE,0))return false;
    for(const auto &p:PongCpGbif)if(!wr(p[0],p[1]))return false;
    if(!wr(GEN7_RBBM_INT_0_MASK,0) || !wr(GEN7_UCHE_GBIF_GX_CONFIG,0x10240e0) ||
       !wr(GEN7_RBBM_PERFCTR_GPU_BUSY_MASKED,0xffffffff) ||
       !wr64(GEN7_UCHE_TRAP_BASE_LO,0x1fffffffff000ULL) ||
       !wr64(GEN7_UCHE_WRITE_THRU_BASE_LO,0x1fffffffff000ULL) ||
       !wr(GEN7_UCHE_CACHE_WAYS,0x800000) || !wr(GEN7_UCHE_CMDQ_CONFIG,0x6690e) ||
       !wr(GEN7_CP_AHB_CNTL,1) || !wr(GEN7_RBBM_PERFCTR_CNTL,1))return false;
    /* gen7_4_0 has uche_gmem_alignment=0: unlike Sheng/A740, do not
     * invent/program a 16MiB UCHE GMEM window. GMEM base advertised as0. */
    const uint32_t bank=(PongAdrenoHighestBankBit-13)&3;
    if(!wr(GEN7_RB_NC_MODE_CNTL,(1u<<11)|(1u<<4)|(bank<<1)) ||
       !wr(GEN7_TPL1_NC_MODE_CNTL,bank<<1) || !wr(GEN7_SP_NC_MODE_CNTL,(2u<<4)|(bank<<1)) ||
       !wr(GEN7_UCHE_MODE_CNTL,bank<<21) ||
       !wr(GEN7_RBBM_INTERFACE_HANG_INT_CNTL,(1u<<30)|0xcfffff) || !wr(GEN7_UCHE_CLIENT_PF,0x81) ||
       !wr(GEN7_RB_CMP_DBG_ECO_CNTL,rd(GEN7_RB_CMP_DBG_ECO_CNTL)|(1u<<11)) ||
       !wr(GEN7_CP_PROTECT_CNTL,11))return false;
    /* GRAS is per BR/BV pipe on A7xx, while the bank bit comes from Pong. */
    for(uint32_t pipe=1;pipe<=2;pipe++)
        if(!wr(GEN7_CP_APERTURE_CNTL_HOST,pipe<<12) || !wr(GEN7_GRAS_NC_MODE_CNTL,bank<<5))return false;
    if(!wr(GEN7_CP_APERTURE_CNTL_HOST,0) || !wr(GEN7_RBBM_NC_MODE_CNTL,1))return false;
    for(const auto &p:PongCpProtect)if(!wr(p[0],p[1]))return false;
    if(!wr(GEN7_CP_APRIV_CNTL,0x6f) || !wr(GEN7_CP_BV_APRIV_CNTL,0xf) || !wr(GEN7_CP_LPAC_APRIV_CNTL,0xf) ||
       !wr(GEN7_CP_CHICKEN_DBG,1) || !wr(GEN7_CP_BV_CHICKEN_DBG,1) || !wr(GEN7_CP_LPAC_CHICKEN_DBG,1) ||
       !wr(GEN7_GPU_GMU_AO_GMU_CGC_MODE_CNTL,0x20000) || !wr(GEN7_GPU_GMU_AO_GMU_CGC_DELAY_CNTL,0x10111) ||
       !wr(GEN7_GPU_GMU_AO_GMU_CGC_HYST_CNTL,0x5555))return false;
    for(const auto &p:PongCpHwcg)if(!wr(p[0],p[1]))return false;
    /* gen7_hwcg_set finishes with the top-level enable after ALL leaf
     * programming, even when the table itself contains an earlier copy. */
    if(!wr(GEN7_RBBM_CLOCK_CNTL,0x8aa8aa82) || rd(GEN7_RBBM_CLOCK_CNTL)!=0x8aa8aa82)return false;
    /* IFPC/preemption/dynamic counters disabled. cpu_gpu_lock=16bytes,
     * followed by actual gen7 static register/value pairs. */
    constexpr unsigned count=sizeof(PongCpRestore)/sizeof(*PongCpRestore);
    uint32_t values[count]={};for(unsigned i=0;i<count;i++)values[i]=rd(PongCpRestore[i]);
    /* gen7_patch_pwrup_reglist always terminates the dynamic triplets with
     * PIPE_NONE/RBBM_PERFCTR_CNTL=1, even without dynamic counters. */
    const uint32_t dynamic[]={0,GEN7_RBBM_PERFCTR_CNTL,1};
    size_t written=0;const NativeAdrenoPwrupCounts counts={0,count,1};
    if(NativeAdrenoPwrupList::pack(counts,nullptr,nullptr,PongCpRestore,values,dynamic,
        static_cast<uint8_t *>(gpu->buffers[3].mapping()),sizes[3],&written))return false;
    asm volatile("dsb sy":::"memory");
    return !gpu->poisoned && !gpu->buffers[3].publishForDevice(0,sizes[3],false);
}
}

IOReturn PongAdrenoCpPrepare(const PongCpOps *ops)
{
    if(gpu || !ops || !ops->record)return kIOReturnBadArgument;
    gpu=new Device;if(!gpu)return kIOReturnNoMemory;gpu->ops=*ops;
    const uint32_t id0=sr(0x20),id1=sr(0x24),id2=sr(0x28);
    const unsigned count=(id0&255)>PongAdrenoStreamSlots?PongAdrenoStreamSlots:(id0&255);
    const unsigned pages=1u<<(((id1>>28)&7)+1);const bool ex=(id0>>8)&1;
    const uint8_t bits[]={32,36,40,42,44,48};unsigned code=(id2>>4)&15;
    if((id1>>31) || count<gpu_stream_count || (id1&255)<2 || code>=6)return kIOReturnUnsupported;
    gpu->output_bits=bits[code]>40?40:bits[code];
    unsigned slots[gpu_stream_count]={~0u,~0u,~0u,~0u},found=0;
    for(unsigned i=0;i<count;i++) {
        uint32_t smr=sr(0x800+4*i),sc=sr(0xc00+4*i);bool valid=ex?!!(sc&1024):!!(smr&0x80000000);
        if(!valid)continue;
        uint32_t mask=(smr>>16)&(ex?65535:32767),sid=smr&65535;
        bool matches=false;
        for(uint16_t wanted:gpu_sids)matches|=!((sid^wanted)&~mask&65535);
        if(matches) {
            if((mask!=0 && mask!=0x400) || (sid&~0x401u) || found==gpu_stream_count)return kIOReturnBusy;
            slots[found++]=i;
            if(!sw(0xc00+4*i,0x20000|(ex?1024:0)))return kIOReturnIOError;
        } else if(!(sc&0x30000) && !(sc&255))return kIOReturnBusy;
    }
    for(unsigned i=0;i<count && found<gpu_stream_count;i++) {
        bool valid=ex?!!(sr(0xc00+4*i)&1024):!!(sr(0x800+4*i)&0x80000000);
        bool leased=false;for(unsigned j=0;j<found;j++)leased|=slots[j]==i;
        if(!valid && !leased)slots[found++]=i;
    }
    if(found!=gpu_stream_count)return kIOReturnNoResources;
    const size_t bank=pages*4096;
    if(!sw(bank,0) || sr(bank))return kIOReturnIOError;
    /* Actual Pong is qcom,qsmmu-v500, with ACTLR 0x32b in its DT.
     * Sheng's ARM MMU-500 CPRE erratum workaround is not a V500 policy. */
    constexpr uint32_t actlr=0x32b;
    if(!sw(bank+4,actlr) || sr(bank+4)!=actlr || (sr(bank+0x58)&SmmuContextFaultMask))return kIOReturnIOError;
    for(unsigned i=0;i<gpu_stream_count;i++) {
        if(!sw(0xc00+slots[i]*4,0x20000) || !sw(0x800+slots[i]*4,(ex?0:0x80000000u)|gpu_sids[i]) ||
           !sw(0xc00+slots[i]*4,0x20000|(ex?1024:0)))return kIOReturnIOError;
    }
    /* Sheng reserves the user BO pool before fragmentation. Pong has a
     * smaller boot memory window: start at 256MiB, halve on allocation failure. */
    unsigned pool=256;PE_parse_boot_argn("pong_gpu_pool_mib",&pool,sizeof(pool));
    if(pool>256)pool=256;
    for(unsigned mib=pool;mib>=16;mib>>=1)
        if(!NativeAdrenoMemory::reservePool(gpu->output_bits,size_t(mib)<<20))break;
    if(NativeAdrenoMemory::poolBytes()) {
        const auto status=NativeAdrenoMemory::writeCombinePool();
        note("native BO pool",status,NativeAdrenoMemory::poolBytes());
    }
    // 32-bit / native 4K: one L1 root, four L2 pages, 2048 L3 pages.
    // The GMU owns a different context and retains its original aperture.
    IOReturn status=gpu->tables.allocate(2053*4096,gpu->output_bits,true,2053*4096);
    if(!status)status=gpu->context.prepare(&gpu->tables,0,0,UINT64_C(0x100000000),1048576,0,2,4096,gpu->output_bits);
    if(!status)gpu->context.setAliasScratch(gpu->alias_scratch,262144);
    if(!status)status=gpu->context.program(&ops->smmu);
    ArmSmmu4KStream streams[gpu_stream_count]={};
    for(unsigned i=0;i<gpu_stream_count;i++)streams[i]={slots[i],gpu_sids[i]};
    if(!status)status=gpu->context.attach(streams,gpu_stream_count);
    NativeAdrenoVmRegion lower={&gpu->tables,gpu->context.arenaHandle(),gpu->context.stateHandle()};
    if(!status)status=gpu->vm.attach(&ops->smmu,gpu->context.configHandle(),gpu->context.streamsHandle(),&lower,nullptr,131072,262144);
    /* Use Sheng's WC submission path. NativeAdrenoSubmit reasserts each
     * object's actual pmap cache attributes before GPU execution and falls
     * back to explicit maintenance if the coherence check finds a mismatch. */
    /* Match Sheng's NC GPU PTEs with write-combined CPU views for the SQE,
     * ring, memptrs and restore/scratch pages. Page tables remain cached. */
    for(unsigned i=0;i<5 && !status;i++) {
        status=gpu->buffers[i].allocate(sizes[i],gpu->output_bits,true,sizes[i]);uint64_t handle=0;
        if(!status)status=gpu->buffers[i].makeWriteCombined();
        if(!status)status=gpu->vm.map(&gpu->buffers[i],0,iovas[i],i?3:5,0,2,&handle);
    }
    if(!status && (PongAdrenoSqeFirmwareBytes<8 || PongAdrenoSqeFirmwareBytes-4>sizes[0]))status=kIOReturnBadArgument;
    if(!status) {
        memcpy(gpu->buffers[0].mapping(),PongAdrenoSqeFirmware+4,PongAdrenoSqeFirmwareBytes-4);
        status=gpu->buffers[0].publishForDevice(0,sizes[0],false);
        asm volatile("dsb sy":::"memory");
    }
    note("GPU native4K/SQE prepared",status,slots[0],slots[1]);
    if(!status)note("LPAC native4K shared context",0,slots[2],slots[3]);
    gpu->prepared=!status;return status;
}

IOReturn PongAdrenoCpStart(PongGpuGmuDevice *gmu)
{
    if(!gpu || !gpu->prepared || gpu->started || !gmu)return kIOReturnNotReady;
    PongGpuPowerResult power={};auto held=PongAdrenoGmuHoldGpu(gmu,&power);
    note("GX OOB/START",held,power.phase,power.firmware_error);
    if(held)return kIOReturnNotReady;
    const uint64_t began=now(nullptr);
    while((rd(GEN7_GMU_GFX_PWR_CLK_STATUS)&3) && now(nullptr)-began<100000)IODelay(10);
    if(gpu->poisoned || (rd(GEN7_GMU_GFX_PWR_CLK_STATUS)&3)) {
        note("GX power/clock not on; no GX MMIO",1);return kIOReturnNotReady;
    }
    if(!hardware_init()){note("CP hardware initialization failed",1);return kIOReturnIOError;}
    NativeAdrenoRingCache cache={nullptr,publish_ring,acquire_fence};
    auto status=gpu->ring.attach(&gpu->ops.registers,&cache,gpu->buffers[1].mapping(),sizes[1],iovas[1],gpu->buffers[2].mapping(),sizes[2],iovas[2]);
    if(!status && (!wr64(GEN7_CP_BV_RB_RPTR_ADDR_LO,iovas[2]+8) || !wr64(GEN7_CP_SQE_INSTR_BASE_LO,iovas[0]) || !wr(GEN7_CP_SQE_CNTL,1)))status=kIOReturnIOError;
    NativeAdrenoCpInitConfig config={iovas[0],iovas[3],1,0};uint32_t wptr=0,fence=0;
    if(!status)status=gpu->init.run(&gpu->ops.registers,&gpu->ring,&config,&wptr);
    note("Pong CP_ME_INIT bootstrap submitted",status,wptr,
        (uint64_t(rd(GEN7_CP_RB_RPTR))<<32)|rd(GEN7_CP_RB_WPTR));
    if(status || !wait_bootstrap(wptr,"CP ME_INIT drained"))return kIOReturnIOError;
    if(!authorize())return kIOReturnNotPermitted;
    uint32_t words[2]={};
    if(AdrenoPM4Type7(0x66,1,words)!=AdrenoPM4Success || gpu->ring.submitBootstrap(words,2,&wptr) ||
       !wait_bootstrap(wptr,"CP nonsecure switch drained"))return kIOReturnIOError;
    /* Require actual CP-written memory AND its timestamp after the board's
     * authenticated switch; never replace the first real fence with idle. */
    auto *scratch=static_cast<volatile uint32_t *>(gpu->buffers[4].mapping());
    scratch[0]=0;asm volatile("dsb sy":::"memory");
    uint32_t write[4]={0,uint32_t(iovas[4]),uint32_t(iovas[4]>>32),0x7300302};
    if(AdrenoPM4Type7(0x3d,3,write)!=AdrenoPM4Success || gpu->ring.submit(write,4,&fence) ||
       !wait_fence(fence))return kIOReturnIOError;
    asm volatile("dsb sy":::"memory");
    if(scratch[0]!=write[3]){note("CP nonsecure write incomplete",1,scratch[0]);return kIOReturnIOError;}
    status=gpu->irq.attach(&gpu->ops.registers);
    uint32_t route=0;uint64_t affinity=0;
    /* Pong DT: SPI 0x12c, level high; GIC architectural INTID is 300+32. */
    if(!status)status=armcpu_spi_attach(332,false,interrupt,nullptr,&route,&affinity);
    if(!status)status=gpu->irq.programMask(nullptr);
    note("GPU IRQ332 owner",status,route,affinity);
    if(status){wr(GEN7_RBBM_INT_0_MASK,0);return status;}
    gpu->irq_attached=true;
    status=gpu->submit.attach(&gpu->vm,&gpu->ring);
    NativeAdrenoTimeIo timer={gpu->ops.registers.cookie,gpu->ops.registers.read32};
    if(!status)status=gpu->timer.attach(&timer,NativeAdrenoTimeCp);
    uint64_t ticks=0;if(!status)status=gpu->timer.read(&ticks);
    if(status || !ticks)return status?status:kIOReturnNotReady;
    gpu->started=true;native_adreno_wc_enabled=1;
    gpu->backend={&gpu->vm,&gpu->submit,&gpu->ring,&gpu->timer,&gpu->status,{nullptr,now,idle},
        {PongAdrenoChipId,PongAdrenoGmemBytes,0,0x1fffffffff000ULL,0x20000000,0xE0000000,
         PongAdrenoHighestBankBit,6,1,4,NativeAdrenoDeviceClientIova,0},gpu->output_bits,client_healthy};
    note("CP ready; authenticated nonsecure fence",0,fence,ticks);
    return PongAdrenoPublishClient(&gpu->backend)?kIOReturnSuccess:kIOReturnNoResources;
}
