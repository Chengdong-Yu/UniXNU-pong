/* SPDX-License-Identifier: MIT */
#include "PongAdrenoCp.h"
#include "PongAdrenoCpProfile.h"
#include "PongAdrenoZap.h"
#include "submit/NativeAdrenoSmmuContext.h"
#include "submit/NativeAdrenoCpInit.h"
#include "submit/AdrenoPM4.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/clock.h>

extern "C" const uint8_t PongAdrenoSqeFirmware[],PongAdrenoZapMdt[],PongAdrenoZapCode[],PongAdrenoZapSignature[];
extern "C" const size_t PongAdrenoSqeFirmwareBytes,PongAdrenoZapMdtBytes,PongAdrenoZapCodeBytes,PongAdrenoZapSignatureBytes;
extern "C" volatile uint32_t native_adreno_wc_enabled;
extern "C" unsigned int pong_adreno_zap_reserved;
namespace {
constexpr uint64_t iovas[]={0x100000,0x200000,0x300000,0x301000};
constexpr size_t sizes[]={0x13000,0x8000,0x1000,0x1000};
struct Device {
    PongCpOps ops{};
    NativeAdrenoMemory tables,buffers[4],metadata;
    NativeAdrenoSmmuContext context;
    NativeAdrenoVm vm;
    NativeAdrenoRing ring;
    NativeAdrenoCpInit init;
    NativeAdrenoSubmit submit;
    NativeAdrenoTimer timer;
    NativeAdrenoStatus status;
    PongCpBackend backend{};
    OSSharedPtr<IOMemoryMap> zap_map;
    PongZapState zap{};
    unsigned output_bits=0;
    uint32_t alias_scratch[262144]{};
    bool prepared=false,started=false,poisoned=false;
};
Device *gpu;
static uint64_t now(void *) {clock_sec_t s;clock_usec_t u;clock_get_system_microtime(&s,&u);return uint64_t(s)*1000000+u;}
static int idle(void *) {IOSleep(1);return 0;}
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
    if(fsr || global || (irq&fatal) || cp) {
        gpu->poisoned=true;gpu->status.noteFault();
        note("CP/SMMU fault; retain BOs",1,(uint64_t(fsr)<<32)|global,(uint64_t(irq)<<32)|cp);
    }
    return gpu->poisoned;
}
static bool client_healthy() {return gpu && gpu->started && !faulted();}
static int publish_ring(void *,const volatile uint32_t *words,size_t bytes)
{
    const auto &m=gpu->buffers[1];auto base=reinterpret_cast<uintptr_t>(m.mapping()),at=reinterpret_cast<uintptr_t>(words);
    if(at<base || at-base>m.bytes() || bytes>m.bytes()-(at-base))return 1;
    return gpu->buffers[1].publishForDevice(at-base,bytes,false)!=kIOReturnSuccess;
}
static int acquire_fence(void *,volatile uint32_t *words,size_t bytes)
{
    const auto &m=gpu->buffers[2];auto base=reinterpret_cast<uintptr_t>(m.mapping()),at=reinterpret_cast<uintptr_t>(words);
    if(at<base || at-base>m.bytes() || bytes>m.bytes()-(at-base))return 1;
    return gpu->buffers[2].acquireDeviceWrites(at-base,bytes)!=kIOReturnSuccess;
}
static bool wait_fence(uint32_t fence)
{
    const NativeAdrenoRingClock clock{nullptr,now,idle};uint32_t observed=0;
    auto result=gpu->ring.wait(fence,&clock,500000,&observed);
    note("CP fence",result,fence,observed);
    return result==NativeAdrenoRingFenceComplete && !faulted();
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
       !wr(GEN7_GRAS_NC_MODE_CNTL,bank<<5) || !wr(GEN7_UCHE_MODE_CNTL,bank<<21) ||
       !wr(GEN7_RBBM_INTERFACE_HANG_INT_CNTL,(1u<<30)|0xcfffff) || !wr(GEN7_UCHE_CLIENT_PF,0x81) ||
       !wr(GEN7_RB_CMP_DBG_ECO_CNTL,rd(GEN7_RB_CMP_DBG_ECO_CNTL)|(1u<<11)) ||
       !wr(GEN7_CP_PROTECT_CNTL,11))return false;
    for(const auto &p:PongCpProtect)if(!wr(p[0],p[1]))return false;
    if(!wr(GEN7_CP_APRIV_CNTL,0x6f) || !wr(GEN7_CP_BV_APRIV_CNTL,0xf) || !wr(GEN7_CP_LPAC_APRIV_CNTL,0xf) ||
       !wr(GEN7_CP_CHICKEN_DBG,1) || !wr(GEN7_CP_BV_CHICKEN_DBG,1) || !wr(GEN7_CP_LPAC_CHICKEN_DBG,1) ||
       !wr(GEN7_GPU_GMU_AO_GMU_CGC_MODE_CNTL,0x20000) || !wr(GEN7_GPU_GMU_AO_GMU_CGC_DELAY_CNTL,0x10111) ||
       !wr(GEN7_GPU_GMU_AO_GMU_CGC_HYST_CNTL,0x5555))return false;
    for(const auto &p:PongCpHwcg)if(!wr(p[0],p[1]))return false;
    /* IFPC/preemption/dynamic counters disabled. cpu_gpu_lock=16bytes,
     * followed by actual gen7 static register/value pairs. */
    uint32_t *list=static_cast<uint32_t *>(gpu->buffers[3].mapping());
    constexpr unsigned count=sizeof(PongCpRestore)/sizeof(*PongCpRestore);
    list[3]=count<<8;
    for(unsigned i=0;i<count;i++){list[4+2*i]=PongCpRestore[i];list[5+2*i]=rd(PongCpRestore[i]);}
    return !gpu->poisoned && !gpu->buffers[3].publishForDevice(0,16+count*8,false);
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
    if((id1>>31) || count<2 || (id1&255)<2 || code>=6)return kIOReturnUnsupported;
    gpu->output_bits=bits[code]>40?40:bits[code];
    unsigned slots[2]={~0u,~0u},found=0;
    for(unsigned i=0;i<count;i++) {
        uint32_t smr=sr(0x800+4*i),sc=sr(0xc00+4*i);bool valid=ex?!!(sc&1024):!!(smr&0x80000000);
        if(!valid)continue;
        uint32_t mask=(smr>>16)&(ex?65535:32767),sid=smr&65535;
        bool matches=!((sid^0)&~mask&65535)||!((sid^0x400)&~mask&65535);
        if(matches) {
            if((mask!=0 && mask!=0x400) || (sid&~0x400u) || found==2)return kIOReturnBusy;
            slots[found++]=i;
            if(!sw(0xc00+4*i,0x20000|(ex?1024:0)))return kIOReturnIOError;
        } else if(!(sc&0x30000) && !(sc&255))return kIOReturnBusy;
    }
    for(unsigned i=0;i<count && found<2;i++) {
        bool valid=ex?!!(sr(0xc00+4*i)&1024):!!(sr(0x800+4*i)&0x80000000);
        if(!valid && i!=slots[0])slots[found++]=i;
    }
    if(found!=2)return kIOReturnNoResources;
    const size_t bank=pages*4096;
    if(!sw(bank,0) || sr(bank))return kIOReturnIOError;
    if(!sw(bank+4,0x32b) || sr(bank+4)!=0x32b || sr(bank+0x58))return kIOReturnIOError;
    for(unsigned i=0;i<2;i++) {
        if(!sw(0xc00+slots[i]*4,0x20000) || !sw(0x800+slots[i]*4,(ex?0:0x80000000u)|(i?0x400:0)) ||
           !sw(0xc00+slots[i]*4,0x20000|(ex?1024:0)))return kIOReturnIOError;
    }
    IOReturn status=gpu->tables.allocate(1027*4096,gpu->output_bits,true,1027*4096);
    if(!status)status=gpu->context.prepare(&gpu->tables,0,0,0x80000000,524288,0,2,4096,gpu->output_bits);
    if(!status)gpu->context.setAliasScratch(gpu->alias_scratch,262144);
    if(!status)status=gpu->context.program(&ops->smmu);
    ArmSmmu4KStream streams[2]={{slots[0],0},{slots[1],0x400}};
    if(!status)status=gpu->context.attach(streams,2);
    NativeAdrenoVmRegion lower={&gpu->tables,gpu->context.arenaHandle(),gpu->context.stateHandle()};
    if(!status)status=gpu->vm.attach(&ops->smmu,gpu->context.configHandle(),gpu->context.streamsHandle(),&lower,nullptr,131072,262144);
    /* This first Pong client uses explicit cache maintenance, not Sheng's
     * device-qualified WC/coherence trust or per-IB content probes. */
    if(!status)gpu->vm.distrustWriteCombined();
    for(unsigned i=0;i<4 && !status;i++) {
        status=gpu->buffers[i].allocate(sizes[i],gpu->output_bits,true,sizes[i]);uint64_t handle=0;
        if(!status)status=gpu->vm.map(&gpu->buffers[i],0,iovas[i],i?3:5,0,2,&handle);
    }
    if(!status && (PongAdrenoSqeFirmwareBytes<8 || PongAdrenoSqeFirmwareBytes-4>sizes[0]))status=kIOReturnBadArgument;
    if(!status) {
        memcpy(gpu->buffers[0].mapping(),PongAdrenoSqeFirmware+4,PongAdrenoSqeFirmwareBytes-4);
        status=gpu->buffers[0].publishForDevice(0,PongAdrenoSqeFirmwareBytes-4,false);
    }
    note("GPU native4K/SQE prepared",status,slots[0],slots[1]);
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
    NativeAdrenoCpInitConfig config={iovas[0],iovas[3],1,0};uint32_t fence=0;
    if(!status)status=gpu->init.run(&gpu->ops.registers,&gpu->ring,&config,&fence);
    note("CP init submitted",status,fence,rd(GEN7_RBBM_STATUS));
    if(status || !wait_fence(fence))return kIOReturnIOError;
    if(!authorize())return kIOReturnNotPermitted;
    uint32_t words[2]={};
    if(AdrenoPM4Type7(0x66,1,words)!=AdrenoPM4Success || gpu->ring.submit(words,2,&fence) || !wait_fence(fence))return kIOReturnIOError;
    status=gpu->submit.attach(&gpu->vm,&gpu->ring);
    NativeAdrenoTimeIo timer={gpu->ops.registers.cookie,gpu->ops.registers.read32};
    if(!status)status=gpu->timer.attach(&timer,NativeAdrenoTimeCp);
    uint64_t ticks=0;if(!status)status=gpu->timer.read(&ticks);
    if(status || !ticks)return status?status:kIOReturnNotReady;
    gpu->started=true;native_adreno_wc_enabled=0;
    gpu->backend={&gpu->vm,&gpu->submit,&gpu->ring,&gpu->timer,&gpu->status,{nullptr,now,idle},
        {PongAdrenoChipId,PongAdrenoGmemBytes,0,0x1fffffffff000ULL,0x20000000,0x60000000,
         PongAdrenoHighestBankBit,6,1,4,NativeAdrenoDeviceClientIova,0},gpu->output_bits,client_healthy};
    note("CP ready; authenticated nonsecure fence",0,fence,ticks);
    return PongAdrenoPublishClient(&gpu->backend)?kIOReturnSuccess:kIOReturnNoResources;
}
