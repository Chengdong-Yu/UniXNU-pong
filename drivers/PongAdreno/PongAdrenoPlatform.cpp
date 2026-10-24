/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* SM8475 cold GMU startup. No A740/Redwood platform owner or ready device.
 * All MMIO belongs to Pong's captured DT. This driver is the sole XNU owner
 * of the GPU SMMU and APSS DRV2 active TCS3 (no generic RPMh driver is linked).
 * Do not steal a live master, a busy TCS or unrelated stream/context bank.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <libkern/c++/OSSharedPtr.h>
#include <pexpert/pexpert.h>
#include "PongAdrenoGmu.h"
#include "PongAdrenoClocks.h"
#include "PongAdrenoSmmu.h"
#include "PongAdrenoRegisters.h"
#include "PongAdrenoCp.h"

extern "C" const uint8_t PongAdrenoGmuFirmware[];
extern "C" const size_t PongAdrenoGmuFirmwareBytes;
extern "C" void pong_ramoops_diagnostic_write(const char *,int);
namespace {
struct Mapping {OSSharedPtr<IOMemoryMap> keep;uintptr_t cpu=0;size_t bytes=0;};
struct Memory {IOBufferMemoryDescriptor *keep=nullptr;uint8_t *cpu=nullptr;uint64_t pa=0;size_t bytes=0;};
static Mapping windows[PongGpuWindowCount],gcc,rsc,db;
static Memory cache,dummy,hfi,logmem,tables;
static PongClockState clocks;
static PongGpuGmuDevice gmu;
static PongAdrenoSmmuSession context;
static ArmSmmu4KConfig table_config;
static ArmSmmu4KResult table_result;
static ArmSmmu4KRange ranges[4];
static unsigned char visited[32];
static ArmSmmu4KAuditRegion region;
static ArmSmmu4KAuditConfig audit;
static ArmSmmu4KContextConfig config;
static ArmSmmu4KIo smmu_io;
static uint64_t bank_offset;
static unsigned parents_ready,attempted;
static void record(const char *phase,unsigned result,uint64_t a=0,uint64_t b=0)
{
    char text[240];int n=snprintf(text,sizeof(text),
        "PONG ADRENO: %s result=%u a=0x%llx b=0x%llx\n",phase,result,a,b);
    if(n>0){IOLog("%s",text);pong_ramoops_diagnostic_write(text,n<(int)sizeof(text)?n:(int)sizeof(text)-1);}
}
static bool map(Mapping &m,uint64_t physical,size_t bytes)
{
    OSSharedPtr<IOMemoryDescriptor> memory(IOMemoryDescriptor::withPhysicalAddress(
        physical,bytes,kIODirectionInOut),OSNoRetain);
    if(memory)m.keep=OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere|kIOMapInhibitCache),OSNoRetain);
    if(!m.keep)return false;
    m.cpu=m.keep->getVirtualAddress();m.bytes=bytes;return m.cpu!=0;
}
static uint32_t rd(const Mapping &m,size_t at)
{return *reinterpret_cast<volatile uint32_t *>(m.cpu+at);}
static void wr(const Mapping &m,size_t at,uint32_t value)
{__asm__ volatile("dsb sy":::"memory");*reinterpret_cast<volatile uint32_t *>(m.cpu+at)=value;__asm__ volatile("dsb sy":::"memory");}
static int barrier(void *){__asm__ volatile("dsb sy":::"memory");return 0;}
static uint64_t microseconds()
{clock_sec_t s;clock_usec_t u;clock_get_system_microtime(&s,&u);return uint64_t(s)*1000000+u;}
static int now(void *,uint64_t *out){*out=microseconds();return 0;}
static int wait(void *,unsigned us){IODelay(us);return 0;}
static void clean(const void *memory,size_t bytes)
{
    uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
    uintptr_t line=4u<<((ctr>>16)&15),at=reinterpret_cast<uintptr_t>(memory);
    uintptr_t end=at+bytes;
    for(at&=~(line-1);at<end;at+=line)__asm__ volatile("dc civac, %0"::"r"(at):"memory");
    __asm__ volatile("dsb sy":::"memory");
}
static bool allocate(Memory &m,size_t bytes)
{
    m.keep=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        IOOptionBits(kIODirectionInOut)|IOOptionBits(kIOMemoryPhysicallyContiguous)|
        IOOptionBits(kIOMemoryMapperNone),bytes,4096);
    if(!m.keep)return false;
    IOByteCount contiguous=0;m.cpu=static_cast<uint8_t *>(m.keep->getBytesNoCopy());
    m.pa=m.keep->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone);m.bytes=bytes;
    if(!m.cpu || !m.pa || (m.pa&4095) || contiguous<bytes || m.pa+bytes>UINT64_C(1)<<36)return false;
    bzero(m.cpu,bytes);clean(m.cpu,bytes);return true;
}
static bool poll(const Mapping &m,size_t at,uint32_t mask,uint32_t wanted,unsigned us)
{
    uint64_t begin=microseconds();
    for(unsigned n=0;n<us && microseconds()-begin<us;n++) {
        if((rd(m,at)&mask)==wanted)return true;IODelay(1);
    }
    return false;
}
static unsigned retained_corner(const PongPowerResource &rail)
{
    // APSS DRV2 previously served all boot-firmware consumers. Until XNU
    // has a shared RPMh aggregator, do not lower an unknown retained vote.
    // Hold the highest ACTUAL allowed AUX corner, without changing CPU/GPU
    // frequency. Never invent a corner or exceed the firmware's own table.
    unsigned index=16,highest=0;
    for(unsigned i=0;i<rail.bytes/2 && i<16;i++) {
        unsigned v=rail.aux[2*i]|unsigned(rail.aux[2*i+1])<<8;
        if(v>highest){highest=v;index=i;}
    }
    return index;
}
static bool vote_parents(const PongPowerInputs &in)
{
    // Pong live DT: APSS DRV2, TCS layout sleep0..2, active3..4, wake5..6.
    // Only active TCS3 is leased here. Sleep/wake and other drivers are intact.
    uint32_t caps=rd(rsc,0xc);
    // DRV_NUM_TCS_SHIFT6 * Pong drv-id2. DT reserves eight TCSes including
    // fast-path7. NCPT is the actual five-bit per-TCS command capacity.
    unsigned count=(caps>>12)&63,ncpt=(caps>>27)&31;
    if(count<8 || ncpt<2){record("SKIP APSS DRV2 capacity",1,caps,(uint64_t(count)<<32)|ncpt);return false;}
    const size_t base=0xd00+3*0x2a0;
    if(!rd(rsc,base+0x18)){record("SKIP active TCS3 busy",1,rd(rsc,base+0x14),rd(rsc,base+0x1c));return false;}
    uint32_t cx=retained_corner(in.cx);
    uint32_t mx=retained_corner(in.mx);
    if(cx==16 || mx==16){record("SKIP ARC AUX levels",1,cx,mx);return false;}
    // Check active TCS4 as well: the AOP must never receive concurrent
    // commands for the same ARC address. Preserve all foreign in-flight work.
    const size_t other=0xd00+4*0x2a0;
    if(!rd(rsc,other+0x18)) {
        uint32_t enabled=rd(rsc,other+0x1c);
        for(unsigned i=0;i<ncpt && i<16;i++)if(enabled&(1u<<i)) {
            uint32_t address=rd(rsc,other+0x34+20*i);
            if(address==in.mx.address || address==in.cx.address) {
                record("SKIP conflicting active TCS4",1,address,enabled);return false;
            }
        }
    }
    // ARC commands use AUX indices (not the common corner number).
    wr(rsc,0xd00,rd(rsc,0xd00)&~(1u<<3));
    uint32_t control=rd(rsc,base+0x14)&~(1u<<24);
    wr(rsc,base+0x14,control);
    if(!poll(rsc,base+0x14,~0u,control,500)){record("FAIL TCS trigger clear",1,rd(rsc,base+0x14),control);return false;}
    control&=~(1u<<16);wr(rsc,base+0x14,control);
    if(!poll(rsc,base+0x14,~0u,control,500)){record("FAIL TCS enable clear",1,rd(rsc,base+0x14),control);return false;}
    wr(rsc,base+0x1c,0);wr(rsc,0xd08,1u<<3);
    if(!poll(rsc,0xd04,1u<<3,0,500)){record("FAIL stale TCS completion clear",1,rd(rsc,0xd04));return false;}
    const uint32_t addresses[]={in.mx.address,in.cx.address},values[]={mx,cx};
    for(unsigned i=0;i<2;i++) {
        wr(rsc,base+0x30+20*i,0x10108); // 8bytes, write, real response required.
        wr(rsc,base+0x34+20*i,addresses[i]);wr(rsc,base+0x38+20*i,values[i]);
    }
    wr(rsc,base+0x10,3);wr(rsc,base+0x1c,3);
    wr(rsc,base+0x14,1u<<16);
    if(!poll(rsc,base+0x14,~0u,1u<<16,500)){record("FAIL TCS AMC enable",1,rd(rsc,base+0x14));return false;}
    wr(rsc,base+0x14,(1u<<16)|(1u<<24));
    // No XNU RSC IRQ handler is installed. The TCS3 IRQ is intentionally
    // masked, so IRQ_STATUS is not a completion prerequisite. Require BOTH
    // real commands ISSUED|COMPL and actual TCS idle, as the vendor's polled
    // path observes hardware status rather than an enabled IRQ indication.
    // A finished old transaction must never be mistaken for this one. See a
    // fresh busy/incomplete transition OR a freshly cleared completion bit.
    bool complete=false,progressed=false;uint32_t first=0,second=0,idle=0;
    uint64_t began=microseconds();
    for(unsigned n=0;n<10000 && microseconds()-began<100000;n++) {
        first=rd(rsc,base+0x3c);second=rd(rsc,base+0x50);idle=rd(rsc,base+0x18);
        bool commands=(first&0x10100)==0x10100 && (second&0x10100)==0x10100;
        if(!commands || !idle)progressed=true;
        if(commands && idle && (progressed || (rd(rsc,0xd04)&(1u<<3)))){complete=true;break;}
        IODelay(10);
    }
    if(!complete){record("FAIL ARC completion",1,(uint64_t(first)<<32)|second,idle);return false;}
    wr(rsc,base+0x1c,0);wr(rsc,0xd08,1u<<3);
    // Clear only the completed transaction's trigger, matching vendor reclaim.
    // Keep the acknowledged votes and parent resources retained.
    wr(rsc,base+0x14,rd(rsc,base+0x14)&~(1u<<24));
    record("parent ARC votes acknowledged",0,mx,cx);
    return true;
}
static int clock_read(void *,PongClockWindow w,size_t at,uint32_t *v)
{const Mapping &m=w==PongClockGcc?gcc:windows[PongGpuClock];if(at>m.bytes-4)return 1;*v=rd(m,at);return 0;}
static int clock_write(void *,PongClockWindow w,size_t at,uint32_t v)
{const Mapping &m=w==PongClockGcc?gcc:windows[PongGpuClock];if(at>m.bytes-4)return 1;wr(m,at,v);return 0;}
static int clock_prerequisite(void *)
{
    // GCC fixed LUCID_EVO GPLL0, actual L/alpha/lock, TCXO19.2MHz from DT.
    // Its hardware-decoded rate must be600MHz; no A740 PLL is programmed.
    uint64_t rate=19200000ULL*((rd(gcc,0x10)&0xffff)*65536ULL+(rd(gcc,0x14)&0xffff))/65536;
    return !parents_ready || !(rd(gcc,0)&0x80000000) || rate!=600000000;
}
static int smmu_read(void *,size_t at,uint32_t *v)
{const Mapping &m=windows[PongGpuSmmu];if((at&3)||at>m.bytes-4)return 1;*v=rd(m,at);return 0;}
static int smmu_write(void *,size_t at,uint32_t v)
{const Mapping &m=windows[PongGpuSmmu];if((at&3)||at>m.bytes-4)return 1;wr(m,at,v);return 0;}
static int smmu_read64(void *,size_t at,uint64_t *v)
{const Mapping &m=windows[PongGpuSmmu];if((at&7)||at>m.bytes-8)return 1;*v=*reinterpret_cast<volatile uint64_t *>(m.cpu+at);return 0;}
static int smmu_write64(void *,size_t at,uint64_t v)
{const Mapping &m=windows[PongGpuSmmu];if((at&7)||at>m.bytes-8)return 1;*reinterpret_cast<volatile uint64_t *>(m.cpu+at)=v;return barrier(nullptr);}
static int read_table(void *,uint64_t physical,uint64_t *value)
{
    if((physical&7)||physical<tables.pa||physical>tables.pa+tables.bytes-8)return 1;
    *value=*reinterpret_cast<volatile uint64_t *>(tables.cpu+physical-tables.pa);return 0;
}
static bool start_smmu()
{
    Mapping &m=windows[PongGpuSmmu];uint32_t id0=rd(m,0x20),id1=rd(m,0x24),id2=rd(m,0x28);
    record("GPU SMMU actual IDs",0,(uint64_t(id0)<<32)|id1,id2);
    unsigned groups=id0&255,banks=id1&255;
    if((id1>>31)||groups<2||banks<2||rd(m,0x48))return false;
    // ID1 reports22 on actual Pong; DT intentionally exposes only21.
    // A capacity override limits the usable prefix, not the hardware ID.
    if(groups>PongAdrenoStreamSlots)groups=PongAdrenoStreamSlots;
    if(banks>PongAdrenoContextBanks)banks=PongAdrenoContextBanks;
    unsigned pages=1u<<(((id1>>28)&7)+1);bool ex=(id0>>8)&1;
    // Shared ASID namespaces: reserve1 for GMU and2 for GPU, independently
    // of the chosen context bank. The two stopped owned masters can be
    // replaced, but never collide with an enabled foreign translation.
    for(unsigned i=0;i<groups;i++) {
        uint32_t smr=rd(m,0x800+4*i),sc=rd(m,0xc00+4*i);
        if(!(ex?!!(sc&1024):!!(smr&0x80000000)) || (sc&0x30000))continue;
        uint32_t mask=(smr>>16)&(ex?65535:32767),sid=smr&65535;
        if((mask==0 || mask==0x400) && ((sid&~0x400u)==0 || (sid&~0x400u)==5))continue;
        unsigned b=sc&255;
        if(b>=(id1&255))return false;
        size_t at=(pages+b)*4096;
        if(!(rd(m,at)&1))continue;
        uint64_t lower=*reinterpret_cast<volatile uint64_t *>(m.cpu+at+0x20);
        uint64_t upper=*reinterpret_cast<volatile uint64_t *>(m.cpu+at+0x28);
        if((lower>>48)==1 || (lower>>48)==2 || (upper>>48)==1 || (upper>>48)==2) {
            record("SKIP ASID lease conflicts with foreign stream",1,b,sid);return false;
        }
    }
    unsigned slots[2]={~0u,~0u},found=0;
    // Masters are ACTUALLY stopped: GX is off, CM3 was observed held in reset.
    // Quarantine only exact5/405 or the vendor mask0x400 for these two SIDs.
    for(unsigned i=0;i<groups;i++) {
        uint32_t smr=rd(m,0x800+4*i),s2cr=rd(m,0xc00+4*i);
        bool valid=ex?!!(s2cr&1024):!!(smr&0x80000000);
        if(!valid)continue;
        uint32_t mask=(smr>>16)&(ex?65535:32767),sid=smr&65535;
        bool matches=!((sid^5)&~mask&65535)||!((sid^0x405)&~mask&65535);
        if(!matches)continue;
        if((mask!=0&&mask!=0x400)||(sid&~0x400u)!=5)return false;
        wr(m,0xc00+4*i,0x20000|(ex?1024:0));
        if(rd(m,0xc00+4*i)!=(0x20000u|(ex?1024u:0u)))return false;
        if(found<2)slots[found++]=i;else return false;
    }
    for(unsigned i=0;i<groups&&found<2;i++) {
        uint32_t smr=rd(m,0x800+4*i),s2cr=rd(m,0xc00+4*i);
        bool valid=ex?!!(s2cr&1024):!!(smr&0x80000000);
        if(!valid && i!=slots[0])slots[found++]=i;
    }
    if(found!=2)return false;
    unsigned bank=0;
    for(unsigned b=1;b<banks;b++) {
        bool other=false;
        for(unsigned i=0;i<groups;i++) {
            uint32_t smr=rd(m,0x800+4*i),sc=rd(m,0xc00+4*i);
            if((ex?!!(sc&1024):!!(smr&0x80000000)) && !(sc&0x30000) && (sc&255)==b)other=true;
        }
        if(!other){bank=b;break;}
    }
    if(!bank)return false;
    bank_offset=uint64_t(pages+bank)*4096;
    // The old context has no other live stream and its sole master is reset.
    wr(m,bank_offset,0);if(rd(m,bank_offset)!=0)return false;
    wr(m,bank_offset+0x618,0);wr(m,bank_offset+0x7f0,0);
    if(!poll(m,bank_offset+0x7f4,1,0,2000)||(rd(m,bank_offset+0x58)&0xc00001feu))return false;
    wr(m,bank_offset+4,0x32b); // Pong qcom,actlr mask0/0x7ff from actual DT.
    if(rd(m,bank_offset+4)!=0x32b)return false;
    uint32_t scr=(rd(m,0)&~13u)|0x402u|(ex?8u:0u);wr(m,0,scr);
    for(unsigned i=0;i<2;i++) {
        wr(m,0xc00+4*slots[i],0x20000);
        wr(m,0x800+4*slots[i],(ex?0:0x80000000u)|(i?0x405u:5u));
        wr(m,0xc00+4*slots[i],0x20000|(ex?1024:0));
    }
    if(!allocate(cache,0xffc000)||!allocate(dummy,0x2000)||!allocate(hfi,0x4000)||
       !allocate(logmem,0x4000)||!allocate(tables,32*4096))return false;
    /* HFI is bidirectional DMA. A volatile cached CPU view cannot observe a
     * fresh GMU ACK merely by DSB; use matching normal-NC/WC CPU attributes
     * before either agent touches these pages. Firmware cache is upload-only. */
    Memory *shared_pages[]={&dummy,&hfi,&logmem};
    for(Memory *shared:shared_pages) {
        NativeAdrenoPhysicalSpan span={0,shared->pa,shared->bytes};
        if(NativeAdrenoSetPagesCacheMode(&span,1,true))return false;
    }
    Memory *mem[]={&cache,&dummy,&hfi,&logmem};uint64_t iovas[]={0x4000,0x60000000,0x60002000,0x60006000};
    for(unsigned i=0;i<4;i++)ranges[i]={iovas[i],mem[i]->pa,mem[i]->bytes,i?3u:15u,0,2};
    table_config={32,36,tables.pa,8192};
    if(ArmSmmu4KBuild(&table_config,ranges,4,reinterpret_cast<uint64_t *>(tables.cpu),32,&table_result))return false;
    clean(tables.cpu,tables.bytes);
    region={ranges,4,32,32*512,visited,sizeof(visited)};
    audit={{nullptr,read_table},&region,nullptr};
    config={};config.lower=&table_config;config.lower_tables=&table_result;
    config.id0=id0;config.id2=id2;config.asid=1;config.continue_after_fault=1;config.polled_faults=1;
    config.ignore_top_byte=1;config.shared_asid=1;
    smmu_io={nullptr,m.bytes,smmu_read,smmu_read64,smmu_write,smmu_write64,barrier,1};
    PongAdrenoSmmuResult result={};
    auto status=PongAdrenoSmmuStart(&smmu_io,0x03da0000,PongAdrenoSmmuGmu,bank,slots,
        &config,&audit,4096,&context,&result);
    record("GMU native4K context",status,bank,result.phase);
    if(status==ArmSmmu4KStateMismatch) {
        const auto &v=result.context.readback;
        record("GMU context mismatch CBAR/CBA2R",status,v.cbar,v.cba2r);
        record("GMU context mismatch TCR/TCR2",status,v.tcr,v.tcr2);
        record("GMU context mismatch SCTLR/MAIR",status,v.sctlr,(uint64_t(v.mair1)<<32)|v.mair0);
        record("GMU context mismatch roots",status,v.ttbr0,v.ttbr1);
    }
    return status==ArmSmmu4KSuccess;
}
static int healthy(void *)
{
    if(!clocks.ready||!context.ready||context.poisoned)return 1;
    Mapping &m=windows[PongGpuSmmu];
    uint32_t global=rd(m,0x48),fault=rd(m,bank_offset+0x58);
    if(global || (fault&0xc00001fe)) {record("SMMU FAULT",1,global,fault);return 1;}
    return 0;
}
static int gpu_read(void *,unsigned w,size_t at,uint32_t *v)
{if(w>=PongGpuWindowCount||at>windows[w].bytes-4||(at&3))return 1;*v=rd(windows[w],at);return 0;}
static int gpu_write(void *,unsigned w,size_t at,uint32_t v)
{if(w>=PongGpuWindowCount||at>windows[w].bytes-4||(at&3))return 1;wr(windows[w],at,v);return 0;}
static int publish(void *,void *cpu,size_t bytes){clean(cpu,bytes);return 0;}
static int cp_read(void *,uint32_t reg,uint32_t *value)
{
    uint64_t pa=0x03d00000ULL+uint64_t(reg)*4;
    const unsigned order[]={PongGpuCore,PongGpuAo,PongGpuRscc,PongGpuGmu};
    for(unsigned w:order)if(pa>=PongAdrenoWindows[w].physical && pa+4<=PongAdrenoWindows[w].physical+windows[w].bytes)
        return gpu_read(nullptr,w,size_t(pa-PongAdrenoWindows[w].physical),value);
    return 1;
}
static int cp_write(void *,uint32_t reg,uint32_t value)
{
    uint64_t pa=0x03d00000ULL+uint64_t(reg)*4;
    const unsigned order[]={PongGpuCore,PongGpuAo,PongGpuRscc,PongGpuGmu};
    for(unsigned w:order)if(pa>=PongAdrenoWindows[w].physical && pa+4<=PongAdrenoWindows[w].physical+windows[w].bytes)
        return gpu_write(nullptr,w,size_t(pa-PongAdrenoWindows[w].physical),value);
    return 1;
}
static void run(void *,wait_result_t)
{
    record("startup requested; no accelerator published",0);
    if(!map(gcc,0x100000,0x1f4200)||!map(rsc,0x17a20000,0x10000)||!map(db,0x80860000,0x20000))
        {record("SKIP resource mapping",1);return;}
    PongPowerInputs inputs={};auto input_status=PongAdrenoReadPowerInputs(
        reinterpret_cast<const uint8_t *>(db.cpu),db.bytes,PongAdrenoChipId,&inputs);
    if(input_status){record("SKIP command-db",input_status);return;}
    if(!vote_parents(inputs)){record("FAIL parent votes",1);return;}
    parents_ready=1;
    // GPUCC register interface must be clocked before even reading GPUCC.
    wr(gcc,0x81004,rd(gcc,0x81004)|1);IODelay(10);
    if(!(rd(gcc,0x81004)&1)){record("FAIL CFG AHB",1);return;}
    if(!map(windows[PongGpuClock],0x03d90000,0xa000)){record("SKIP GPUCC map",1);return;}
    uint32_t gx=rd(windows[PongGpuClock],0x905c);
    if(gx&0x80000000){record("SKIP GX already powered; require cold boot",1,gx);return;}
    PongClockOps clock_ops={nullptr,clock_read,clock_write,barrier,now,wait,clock_prerequisite};
    PongClockResult cr={};auto cs=PongAdrenoEnableCxClocks(&clock_ops,&clocks,&cr);
    record("CX clocks",cs,(uint64_t(cr.offset)<<32)|cr.value,cr.phase);
    if(cs)return;
    record("Cape GPU PLL configuration",0,
        (uint64_t(rd(windows[PongGpuClock],0x10))<<32)|rd(windows[PongGpuClock],0x1010),
        (uint64_t(rd(windows[PongGpuClock],0))<<32)|rd(windows[PongGpuClock],0x1000));
    for(unsigned i=0;i<PongGpuWindowCount;i++)if(i!=PongGpuClock &&
        !map(windows[i],PongAdrenoWindows[i].physical,PongAdrenoWindows[i].bytes))
        {record("SKIP GPU window map",1,i);return;}
    uint32_t reset=rd(windows[PongGpuGmu],GEN7_GMU_CM3_SYSRESET*4-0x68000);
    if(reset!=1){record("SKIP CM3 not held in reset",1,reset);return;}
    if(!start_smmu()){record("FAIL GMU SMMU ownership/configuration",1);return;}
    PongCpOps cp={{nullptr,cp_write,cp_read,barrier},smmu_io,record};
    auto prepared=PongAdrenoCpPrepare(&cp);
    if(prepared){record("FAIL GPU context preparation",prepared);return;}
    gmu.io={nullptr,healthy,gpu_read,gpu_write,barrier,publish,now,wait};
    PongGpuGmuConfig gc={PongAdrenoChipId,PongAdrenoGmuFirmware,PongAdrenoGmuFirmwareBytes,
        {reinterpret_cast<volatile uint32_t *>(cache.cpu),cache.bytes,0x4000},
        {reinterpret_cast<volatile uint32_t *>(dummy.cpu),dummy.bytes,0x60000000},
        {reinterpret_cast<volatile uint32_t *>(hfi.cpu),hfi.bytes,0x60002000},
        {reinterpret_cast<volatile uint32_t *>(logmem.cpu),logmem.bytes,0x60006000}};
    PongGpuGmuResult gr={};auto gs=PongAdrenoGmuStart(&gmu,&gc,&gr);
    record("GMU firmware/HFI",gs,(uint64_t(gr.last_register)<<32)|gr.last_value,gr.phase);
    if(gs)return;
    PongGpuPowerResult pr={};auto ps=PongAdrenoGmuStartPower(&gmu,&inputs,&pr);
    record("GMU power handshake",ps,(uint64_t(pr.command)<<32)|pr.firmware_error,pr.acknowledged);
    if(ps)return;
    auto cp_status=PongAdrenoCpStart(&gmu);
    record("CP/user-client startup",cp_status,gr.core_version,gr.hfi_version);
    if(cp_status)record("GX clock state after CP failure",rd(windows[PongGpuClock],0x90a8),
        (uint64_t(rd(windows[PongGpuClock],0))<<32)|rd(windows[PongGpuClock],0x1000),
        (uint64_t(rd(windows[PongGpuClock],4))<<32)|rd(windows[PongGpuClock],0x1004));
}
static void worker(void *parameter,wait_result_t result)
{
    run(parameter,result);
    thread_terminate(current_thread());
    thread_block(THREAD_CONTINUE_NULL);
}
}
extern "C" void pong_adreno_start_optional(void)
{
    unsigned requested=0;
    if(!PE_parse_boot_argn("pong_adreno_start",&requested,sizeof(requested))||requested!=1)return;
    if(__atomic_exchange_n(&attempted,1,__ATOMIC_ACQ_REL))return;
    thread_t thread=THREAD_NULL;auto status=kernel_thread_start(worker,nullptr,&thread);
    if(status!=KERN_SUCCESS){record("FAIL startup thread",static_cast<unsigned>(status));return;}
    thread_deallocate(thread);
}
