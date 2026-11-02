/* SPDX-License-Identifier: MIT
 * Pong EPSS requests, native LUTs and 4+3+1 topology. Register facts:
 * NothingOSS SM8475 cpufreq-hw; per-core DCVS follows upstream EPSS.
 * pong_cpu_policy=0 disables votes, 1 selects load-based scaling and
 * 2 (default) holds the highest allowed non-boost request in each domain.
 * EPSS/LMh still controls actual voltage, frequency and thermal mitigation.
 */
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
extern "C" uint64_t pong_cpu_idle_mach(unsigned int);
namespace PongCpuPolicy {
struct Domain {
    unsigned first,count,cap_khz;
    IOMemoryMap *map=nullptr;
    volatile uint32_t *regs=nullptr,*pdmem=nullptr;
    unsigned khz[40]{},levels=0,down=0;
    bool live=false;
};
static Domain domains[]={{0,4,1804800},{4,3,2496000},{7,1,2995200}};
static IOMemoryMap *pdmem_map;
static unsigned mode=2,started;
static void stop() __attribute__((noreturn));
static void stop()
{thread_terminate(current_thread());thread_block(THREAD_CONTINUE_NULL);__builtin_unreachable();}
static bool prepare(Domain &d,unsigned index)
{
    auto memory=IODeviceMemory::withRange(0x17d91000ull+index*4096,4096);
    if(!memory)return false;
    d.map=memory->map(kIOMapAnywhere|kIOMapInhibitCache);memory->release();
    if(!d.map)return false;
    d.regs=reinterpret_cast<volatile uint32_t *>(d.map->getVirtualAddress());
    // EPSS must already be enabled by board firmware. Never start a PLL by
    // writing a performance index into a disabled frequency domain.
    if(!(d.regs[0]&1))return false;
    unsigned previous=0;
    for(unsigned i=0;i<40;++i) {
        const uint32_t row=d.regs[(0x100+4*i)/4];
        const unsigned src=row>>30,cores=(row>>16)&15;
        const unsigned khz=src?(row&255)*19200u:300000u;
        if(i && khz==previous)break;
        if(khz<300000 || khz>3200000 || (i && khz<previous))return false;
        previous=khz;
        // DT max_cores=4 in all three frequency domains. Different core
        // counts mark boost rows; preserve their LUT indices but skip them.
        if(cores==4 && khz<=d.cap_khz)d.khz[i]=khz;
        d.levels=i+1;
    }
    const unsigned current=d.regs[0x320/4];
    if(d.levels<2 || current>=d.levels)return false;
    bool usable=false;for(unsigned i=0;i<d.levels;++i)usable|=d.khz[i]!=0;
    if(!usable)return false;
    d.pdmem=reinterpret_cast<volatile uint32_t *>(pdmem_map->getVirtualAddress()+0x804+4*index);
    d.live=true;
    IOLog("Pong CPU: EPSS domain%u CPU%u-%u LUT=%u cap=%u kHz per-core=%u\n",
        index,d.first,d.first+d.count-1,d.levels,d.cap_khz,unsigned(d.regs[0xb0/4]&1));
    return true;
}
static void request(Domain &d,unsigned selected)
{
    // The domain's first PERF_STATE is insufficient in per-core DCVS mode.
    // All offsets remain inside this domain's Pong EPSS window. PDMEM is
    // the matching vendor perf-lock request, not a voltage/thermal override.
    const unsigned count=(d.regs[0xb0/4]&1)?d.count:1;
    bool changed=*d.pdmem!=selected;
    for(unsigned c=0;c<count;++c)changed|=d.regs[0x320/4+c]!=selected;
    if(!changed)return;
    *d.pdmem=selected;
    for(unsigned c=0;c<count;++c)d.regs[0x320/4+c]=selected;
    __asm__ volatile("dsb oshst":::"memory");
}
static unsigned highest(const Domain &d)
{
    unsigned selected=40;
    for(unsigned i=0;i<d.levels;++i)if(d.khz[i])selected=i;
    return selected;
}
static void worker(void *,wait_result_t)
{
    IOSleep(3000);
    auto memory=IODeviceMemory::withRange(0x17d09000,4096);
    if(memory){pdmem_map=memory->map(kIOMapAnywhere|kIOMapInhibitCache);memory->release();}
    if(!pdmem_map){IOLog("Pong CPU: EPSS skipped: PDMEM unavailable\n");stop();}
    unsigned live=0;
    for(unsigned i=0;i<3;++i) {
        Domain &d=domains[i];
        if(prepare(d,i))++live;
        else if(d.map){d.map->release();d.map=nullptr;d.regs=nullptr;}
    }
    if(!live){IOLog("Pong CPU: EPSS skipped: no valid native LUT\n");stop();}
    if(mode==2) {
        for(auto &d:domains)if(d.live) {
            const unsigned selected=highest(d);
            request(d,selected);
            IOLog("Pong CPU: CPU%u-%u fixed request %u kHz (LUT%u), hardware limits retained\n",
                d.first,d.first+d.count-1,d.khz[selected],selected);
        }
        for(;;) {
            // No idle accounting or 32 ms governor in fixed mode. Only
            // restore a lost request; do not clear LMh status or interpret
            // a thermally clamped DOMAIN_STATE sample as a permanent cap.
            IOSleep(500);live=0;
            for(auto &d:domains)if(d.live) {
                if(!(d.regs[0]&1)){d.live=false;continue;}
                request(d,highest(d));++live;
            }
            if(!live)stop();
        }
    }
    uint64_t idle[8];for(unsigned c=0;c<8;++c)idle[c]=pong_cpu_idle_mach(c);
    uint64_t stamp=mach_absolute_time();
    for(;;) {
        IOSleep(32);
        const uint64_t now=mach_absolute_time(),elapsed=now-stamp;stamp=now;
        if(!elapsed)continue;
        unsigned busy[8]{};bool valid[8]{};
        for(unsigned c=0;c<8;++c) {
            const uint64_t value=pong_cpu_idle_mach(c);
            if(value!=UINT64_MAX && idle[c]!=UINT64_MAX && value>=idle[c]) {
                const uint64_t delta=value-idle[c];
                busy[c]=delta>=elapsed?0u:unsigned(100-(delta*100/elapsed));valid[c]=true;
            }
            idle[c]=value;
        }
        for(auto &d:domains)if(d.live) {
            unsigned load=0;bool sampled=false;
            for(unsigned c=d.first;c<d.first+d.count;++c)if(valid[c]) {
                sampled=true;if(busy[c]>load)load=busy[c];
            }
            if(!sampled)continue;
            const unsigned current=d.regs[0x320/4];
            if(current>=d.levels){d.live=false;continue;}
            unsigned freq=d.khz[current];if(!freq)freq=d.cap_khz;
            uint64_t requested=uint64_t(freq)*load*125/10000;
            if(load>=90)requested=uint64_t(freq)*2;
            unsigned cap=d.cap_khz;
            if(d.regs[0x30c/4]&4) {
                const unsigned hardware=(d.regs[0x20/4]&255)*19200u;
                if(hardware && hardware<cap)cap=hardware;
            }
            unsigned selected=40,last=40;
            for(unsigned i=0;i<d.levels;++i)if(d.khz[i] && d.khz[i]<=cap) {
                last=i;if(d.khz[i]>=requested){selected=i;break;}
            }
            if(selected==40)selected=last;
            if(selected==40){d.down=0;continue;}
            if(selected==current){d.down=0;request(d,selected);continue;}
            if(d.khz[selected]<freq && ++d.down<4)continue;
            d.down=0;
            request(d,selected);
        }
    }
}
static void start()
{
    if(__atomic_exchange_n(&started,1,__ATOMIC_ACQ_REL))return;
    PE_parse_boot_argn("pong_cpu_policy",&mode,sizeof mode);
    if(!mode)return;
    if(mode>2){IOLog("Pong CPU: invalid pong_cpu_policy=%u; EPSS votes skipped\n",mode);return;}
    thread_t thread=THREAD_NULL;
    kern_return_t result=kernel_thread_start(worker,nullptr,&thread);
    if(result==KERN_SUCCESS)thread_deallocate(thread);
    else IOLog("Pong CPU: optional EPSS worker unavailable %d\n",result);
}
}
