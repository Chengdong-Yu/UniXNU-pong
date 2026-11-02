/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoRing.h"
#include "AdrenoPM4.h"
#include <IOKit/IOLib.h>

/* Values are taken from the pinned Linux source, not guessed:
 * drivers/gpu/drm/msm/registers/adreno/a6xx.xml (register offsets), and
 * registers/adreno/adreno_pm4.xml (opcode 0x46 CP_EVENT_WRITE7, event 0x31
 * CACHE_CLEAN, WRITE_SRC bits 20..22, WRITE_DST bit 24, WRITE_ENABLED bit 27,
 * IRQ bit 31). CP_RB_CNTL BUFSZ bits 0..5 and BLKSZ bits 8..13 are the same
 * fields MSM_GPU_RB_CNTL_DEFAULT programs. All are dword register indices. */
enum : uint32_t {
    RingBase = 0x0800,        /* 64-bit CP_RB_BASE */
    RingControl = 0x0802,     /* CP_RB_CNTL */
    RingRptrAddress = 0x0804, /* 64-bit CP_RB_RPTR_ADDR */
    RingRptr = 0x0806,
    RingWptr = 0x0807,
    RingScratch = 0x0885,     /* CP_SCRATCH(2) for A6XX..A7XX */
    RingFenceOpcode = 0x46,   /* CP_EVENT_WRITE7 on A7XX */
    RingFenceEvent = 0x31,    /* CACHE_CLEAN */
    RingFenceEnable = 1u << 27,
    RingFenceIrq = 1u << 31,
    RingBlockBytes = 32,
    /* A7XX framing, as a7xx_submit() in the pinned a6xx_gpu.c emits it: CP_THREAD_CONTROL (0x17; THREAD bits 0..1,
     * BR 1, BV 2, BOTH 3; SYNC_THREADS bit 31), CP_SET_MARKER (0x65; 0x101 IFPC disable, 0x100 enable), CCU_INVALIDATE_DEPTH
     * 0x18 / CCU_INVALIDATE_COLOR 0x19 and CP_WAIT_TIMESTAMP (0x14) from adreno_pm4.xml. User command streams synchronize
     * the BR and BV threads themselves (Turnip waits on BV timestamps), so the IBs must run with both threads enabled. */
    RingThreadControl = 0x17,
    RingThreadBr = 1,
    RingThreadBv = 2,
    RingThreadBoth = 3,
    RingSyncThreads = 1u << 31,
    RingSetMarker = 0x65,
    RingWaitTimestamp = 0x14,
    RingCcuInvalidateDepth = 0x18,
    RingCcuInvalidateColor = 0x19,
    /* A7XX CACHE_FLUSH7/CACHE_INVALIDATE7 with CP_WAIT_FOR_IDLE. This fork
     * keeps the explicit generic cache sequence; it does not infer behavior
     * of Pong's a730_sqe from a different GPU's firmware disassembly. */
    RingCacheFlush7 = 0x32,
    RingCacheInvalidate7 = 0x33,
    RingWaitForIdle = 0x26,
    RingNop = 0x10,
    RingPrologueWords = 13,
    /* a6xx_set_pagetable()'s A7XX context entry: CP_RESET_CONTEXT_STATE (0x1f; CLEAR_ON_CHIP_TS, CLEAR_RESOURCE_TABLE,
     * CLEAR_BV_BR_COUNTER, RESET_GLOBAL_LOCAL_TS = bits 0..3) between thread syncs, LRZ_FLUSH_INVALIDATE (0x26), the
     * RBBM_PERFCTR_SRAM_INIT_CMD (0x050e) write and CACHE_INVALIDATE (0x31), after a CP_WAIT_TIMESTAMP on the last fence, then the
     * CP_WAIT_REG_MEM (0x3c, WRITE_EQ) on RBBM_PERFCTR_SRAM_INIT_STATUS (0x050f) bit 0 that Linux always emits after the SRAM init
     * write. A board must qualify its actual context switch and first fence
     * before publication; inherited Sheng runtime results do not qualify Pong. */
    RingResetContextState = 0x1f,
    RingResetAll = 0xf,
    RingLrzFlushInvalidate = 0x26,
    RingCacheInvalidate = 0x31,
    RingPerfctrSramInit = 0x050e,
    RingPerfctrSramInitStatus = 0x050f,
    RingWaitRegMem = 0x3c,
    RingWaitWriteEq = 3,
    RingResetWords = 26,
    RingFenceWords = 7,       /* kernel bring-up submissions: TYPE4 scratch + TYPE7 event */
    RingFramedFenceWords = 35,/* IB list end, scratch, BV timestamp, BR wait, ring timestamp, BR idle */
    RingStampWords=4, RingStampOffset=512, RingAlwaysOnCounter=0x0980, RingRegToMem=0x3e,
    RingBvFenceOffset = 12    /* msm_rbmemptrs.bv_fence; +8 is bv_rptr, the BV RPTR shadow hw init programs */
};
static uint32_t ilog2(size_t value)
{
    uint32_t result=0;while(value>1) {value>>=1;++result;}return result;
}
IOReturn NativeAdrenoRing::poison()
{
    poisoned_=true;return kIOReturnIOError;
}
IOReturn NativeAdrenoRing::readRptr(uint32_t *value)
{
    uint32_t rptr=0;
    if(io_.read32(io_.cookie,RingRptr,&rptr))return poison();
    if(rptr>=ring_bytes_/4)return poison(); /* unusable read pointer */
    *value=rptr;rptr_shadow_=rptr;return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::publishWritten(uint32_t begin,uint32_t count)
{
    const size_t words=ring_bytes_/4;
    if(begin>=words || !count || count>=words)return poison();
    // The board's cache API owns native 4 KiB pages. Publish only pages
    // touched by this submission, including prologue/fence and both parts
    // of a wrap, before WPTR. No cacheability or ordering change is needed.
    const size_t start=(size_t(begin)*4)&~size_t(4095);
    const size_t end=(size_t(begin)+count)*4;
    if(end<=ring_bytes_) {
        const size_t rounded=(end+4095)&~size_t(4095);
        if(cache_.publish(cache_.cookie,ring_+start/4,rounded-start))return poison();
    } else {
        const size_t head=(end-ring_bytes_+4095)&~size_t(4095);
        if(head>=start) {
            if(cache_.publish(cache_.cookie,ring_,ring_bytes_))return poison();
        } else if(cache_.publish(cache_.cookie,ring_+start/4,ring_bytes_-start) ||
                  cache_.publish(cache_.cookie,ring_,head))return poison();
    }
    return kIOReturnSuccess;
}
/* Packs one TYPE7 packet into words; returns the next free index or 0 when the header cannot be built. */
static size_t packet(uint32_t *words,size_t at,uint32_t opcode,const uint32_t *payload,uint32_t count)
{
    if(AdrenoPM4Type7(opcode,count,&words[at])!=AdrenoPM4Success)return 0;
    for(uint32_t i=0;i<count;++i)words[at+1+i]=payload[i];
    return at+1+count;
}
// A730 CP_REG_TO_MEM, same gen7 counter and packet as the upstream
// timeline.64 slots of16 bytes lie within the owned4KiB fence page.
static size_t stamp(uint32_t *words,size_t at,uint64_t base,uint32_t fence,bool end) {
    const uint64_t to=base+RingStampOffset+uint64_t(fence&63)*16+(end?8:0);
    const uint32_t payload[]={RingAlwaysOnCounter|(2u<<18)|(1u<<30),uint32_t(to),uint32_t(to>>32)};
    return packet(words,at,RingRegToMem,payload,3);
}
/* Thread selection and IFPC disable before the user buffers, as a7xx_submit() does; the first submission after
 * requestContextReset() also carries the context entry a6xx_set_pagetable() emits when a new context first submits.
 * Every prologue ends its BR-only part with the GPU cache flush and invalidate described at RingCacheFlush7. */
IOReturn NativeAdrenoRing::emitPrologue(uint32_t at,bool reset,uint32_t *end)
{
    const uint32_t mask=(uint32_t)(ring_bytes_/4-1);
    uint32_t words[RingPrologueWords+RingResetWords+RingStampWords]={0};
    const uint32_t br=RingSyncThreads|RingThreadBr,syncBoth=RingSyncThreads|RingThreadBoth,both=RingThreadBoth,marker=0x101;
    const uint32_t count=RingPrologueWords+(reset?RingResetWords:0)+(stamps_?RingStampWords:0);
    size_t n=packet(words,0,RingThreadControl,&br,1);
    if(reset) {
        const uint64_t at4=fence_iova_+4;
        const uint32_t wait[4]={0,(uint32_t)at4,(uint32_t)(at4>>32),seqno_},all=RingResetAll,lrz=RingLrzFlushInvalidate,
            invalidate=RingCacheInvalidate;
        if(n)n=packet(words,n,RingWaitTimestamp,wait,4);
        if(n)n=packet(words,n,RingThreadControl,&syncBoth,1);
        if(n)n=packet(words,n,RingResetContextState,&all,1);
        if(n)n=packet(words,n,RingThreadControl,&syncBoth,1);
        if(n)n=packet(words,n,RingFenceOpcode,&lrz,1);
        if(n)n=packet(words,n,RingThreadControl,&br,1);
        if(n && AdrenoPM4Type4(RingPerfctrSramInit,1,&words[n])==AdrenoPM4Success) {words[n+1]=1;n+=2;} else n=0;
        if(n)n=packet(words,n,RingFenceOpcode,&invalidate,1);
        const uint32_t sram[6]={RingWaitWriteEq,RingPerfctrSramInitStatus,0,1,1,0};
        if(n)n=packet(words,n,RingWaitRegMem,sram,6);
    }
    const uint32_t flush=RingCacheFlush7,invalidate7=RingCacheInvalidate7;
    if(n)n=packet(words,n,RingFenceOpcode,&flush,1);
    if(n)n=packet(words,n,RingFenceOpcode,&invalidate7,1);
    if(n)n=packet(words,n,RingWaitForIdle,nullptr,0);
    if(n && stamps_)n=stamp(words,n,fence_iova_,seqno_+1,false);
    if(n)n=packet(words,n,RingThreadControl,&both,1);
    if(n)n=packet(words,n,RingSetMarker,&marker,1);
    // The A730 SQE needs IB1LIST boundaries, as Nothing's
    // gen7_ringbuffer_submitcmd and Linux a7xx_submit emit them. IFPC markers
    // alone do not terminate a user list before the BR/BV fence protocol.
    const uint32_t ibListStart=0x00d;
    if(n)n=packet(words,n,RingSetMarker,&ibListStart,1);
    if(n!=count)return poison();
    for(uint32_t i=0;i<count;++i)ring_[(at+i)&mask]=words[i];
    *end=(at+count)&mask;return kIOReturnSuccess;
}
/* Writes the timestamp fence that makes a submission observable. */
IOReturn NativeAdrenoRing::emitFence(uint32_t at,uint32_t seqno,uint32_t *end)
{
    const uint32_t mask=(uint32_t)(ring_bytes_/4-1);
    uint32_t words[RingFenceWords]={0};
    if(AdrenoPM4Type4(RingScratch,1,&words[0])!=AdrenoPM4Success ||
        AdrenoPM4Type7(RingFenceOpcode,4,&words[2])!=AdrenoPM4Success)return poison();
    words[1]=seqno;
    words[3]=RingFenceEvent|RingFenceEnable|RingFenceIrq;
    /* The timestamp goes to +4 (msm_rbmemptrs.fence), where wait() reads it; +0 is the CP's RPTR shadow. */
    const uint64_t at4=fence_iova_+4;
    words[4]=(uint32_t)at4;words[5]=(uint32_t)(at4>>32);words[6]=seqno;
    for(uint32_t i=0;i<RingFenceWords;++i)ring_[(at+i)&mask]=words[i];
    *end=(at+RingFenceWords)&mask;return kIOReturnSuccess;
}
/* The fence of a framed submission: CP_SCRATCH(2), the BV thread's timestamp, a BR wait for it (so BR cannot report the
 * submission while BV still works on it), then the ring timestamp with an interrupt, both threads and IFPC back on. */
IOReturn NativeAdrenoRing::emitFramedFence(uint32_t at,uint32_t seqno,uint32_t *end)
{
    const uint32_t mask=(uint32_t)(ring_bytes_/4-1);
    uint32_t words[RingFramedFenceWords+RingStampWords]={0};
    const uint32_t ibListEnd=0x00e;
    size_t n=packet(words,0,RingSetMarker,&ibListEnd,1);
    if(!n || AdrenoPM4Type4(RingScratch,1,&words[n])!=AdrenoPM4Success)return poison();
    words[n+1]=seqno;n+=2;
    /* The timestamp goes to +4 (msm_rbmemptrs.fence), where wait() reads it; +0 is the CP's RPTR shadow. */
    const uint64_t at4=fence_iova_+4,bv=fence_iova_+RingBvFenceOffset;
    const uint32_t br=RingThreadBr,syncBr=RingSyncThreads|RingThreadBr,bvThread=RingThreadBv,both=RingThreadBoth,depth=RingCcuInvalidateDepth,
        color=RingCcuInvalidateColor,marker=0x100;
    const uint32_t bvWrite[4]={RingFenceEvent|RingFenceEnable,(uint32_t)bv,(uint32_t)(bv>>32),seqno};
    const uint32_t bvWait[4]={0,(uint32_t)bv,(uint32_t)(bv>>32),seqno};
    const uint32_t fence[4]={RingFenceEvent|RingFenceEnable|RingFenceIrq,(uint32_t)at4,(uint32_t)(at4>>32),seqno};
    // Drain both CP threads at the user-list/kernel-fence boundary. Pong's
    // repeated BV 0x17 faults occur here after otherwise completed frames.
    n=packet(words,n,RingThreadControl,&syncBr,1);
    if(n)n=packet(words,n,RingFenceOpcode,&depth,1);
    if(n)n=packet(words,n,RingFenceOpcode,&color,1);
    if(n)n=packet(words,n,RingThreadControl,&bvThread,1);
    if(n)n=packet(words,n,RingFenceOpcode,bvWrite,4);
    if(n)n=packet(words,n,RingThreadControl,&br,1);
    if(n)n=packet(words,n,RingWaitTimestamp,bvWait,4);
    if(n && stamps_)n=stamp(words,n,fence_iova_,seqno,true);
    if(n)n=packet(words,n,RingFenceOpcode,fence,4);
    if(n)n=packet(words,n,RingThreadControl,&both,1);
    if(n)n=packet(words,n,RingSetMarker,&marker,1);
    // Nothing's gen7 ring ends on BR after the IFPC marker. Do not leave
    // an idle ring's kernel tail selected for both thread parsers.
    if(n)n=packet(words,n,RingThreadControl,&br,1);
    const uint32_t total=RingFramedFenceWords+(stamps_?RingStampWords:0);
    if(n!=total)return poison();
    for(uint32_t i=0;i<total;++i)ring_[(at+i)&mask]=words[i];
    *end=(at+total)&mask;return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::attach(const NativeAdrenoRingIo *io,const NativeAdrenoRingCache *cache,
    void *ring,size_t ring_bytes,uint64_t ring_iova,void *fence,size_t fence_bytes,uint64_t fence_iova)
{
    if(attached_ || poisoned_ || ring_ || fence_)return kIOReturnBusy;
    if(!io || !io->write32 || !io->read32 || !io->barrier || !cache ||
        !cache->publish || !cache->acquire || !ring || !fence ||
        ring_bytes<8192 || ring_bytes>(1u<<20) || (ring_bytes&(ring_bytes-1)) ||
        ((ring_iova|ring_bytes)&(ring_bytes-1)) || (ring_iova&4095) ||
        fence_bytes<64 || (fence_iova&7) || fence_bytes>=(1u<<24))return kIOReturnBadArgument;
    /* No register may be touched before every argument is acceptable. */
    io_=*io;cache_=*cache;ring_=static_cast<volatile uint32_t *>(ring);
    ring_bytes_=ring_bytes;ring_iova_=ring_iova;
    fence_=static_cast<volatile uint32_t *>(fence);fence_bytes_=fence_bytes;fence_iova_=fence_iova;
    for(size_t i=0;i<fence_bytes_/4;++i)fence_[i]=0;
    const uint32_t control=(uint32_t)(ilog2(ring_bytes/8)&0x3f)|(ilog2(RingBlockBytes/8)<<8);
    const uint32_t base_lo=(uint32_t)ring_iova_,base_hi=(uint32_t)(ring_iova_>>32);
    const uint32_t rptr_lo=(uint32_t)fence_iova_,rptr_hi=(uint32_t)(fence_iova_>>32);
    const uint32_t writes[]={RingBase,base_lo,RingBase+1,base_hi,RingControl,control,
        RingRptrAddress,rptr_lo,RingRptrAddress+1,rptr_hi,RingWptr,0,RingScratch,0};
    for(size_t i=0;i<sizeof(writes)/sizeof(writes[0]);i+=2)
        if(io_.write32(io_.cookie,writes[i],writes[i+1]))return poison();
    uint32_t rptr=0;IOReturn status=readRptr(&rptr);
    if(status)return status;
    next_=rptr;seqno_=0;completed_=0;submissions_=0;
    attached_=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::detach()
{
    if(poisoned_)return kIOReturnBusy;
    if(!attached_)return kIOReturnNotReady;
    if(outstanding())return kIOReturnBusy;
    attached_=false;ring_=nullptr;fence_=nullptr;ring_bytes_=fence_bytes_=0;
    ring_iova_=fence_iova_=0;next_=rptr_shadow_=seqno_=completed_=submissions_=0;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::submit(const uint32_t *words,size_t word_count,uint32_t *fence)
{
    return submitWords(words,word_count,fence,false);
}
IOReturn NativeAdrenoRing::submitBootstrap(const uint32_t *words,size_t word_count,uint32_t *writePointer)
{
    if(!writePointer)return kIOReturnBadArgument;*writePointer=0;
    if(!attached_ || poisoned_ || seqno_ || submissions_)return kIOReturnNotReady;
    const uint32_t size_words=(uint32_t)(ring_bytes_/4),mask=size_words-1;
    if(!words || !word_count || word_count>size_words-8)return kIOReturnBadArgument;
    uint32_t rptr=0;IOReturn status=readRptr(&rptr);
    if(status)return status;
    if(rptr!=next_)return kIOReturnBusy;
    for(size_t i=0;i<word_count;++i)ring_[(next_+i)&mask]=words[i];
    const uint32_t end=(next_+(uint32_t)word_count)&mask;
    if(publishWritten(next_,(uint32_t)word_count) || io_.barrier(io_.cookie) ||
       io_.write32(io_.cookie,RingWptr,end))return poison();
    next_=end;*writePointer=end;return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::submitFramed(const uint32_t *words,size_t word_count,uint32_t *fence)
{
    return submitWords(words,word_count,fence,true);
}
IOReturn NativeAdrenoRing::submitWords(const uint32_t *words,size_t word_count,uint32_t *fence,bool framed)
{
    if(!fence)return kIOReturnBadArgument;*fence=0;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!words || !word_count || word_count>SIZE_MAX/sizeof(uint32_t))return kIOReturnBadArgument;
    const uint32_t size_words=(uint32_t)(ring_bytes_/4),mask=size_words-1;
    const bool reset=framed && reset_pending_;
    const uint32_t extra=framed?RingPrologueWords+(reset?RingResetWords:0)+RingFramedFenceWords+(stamps_?2*RingStampWords:0):RingFenceWords;
    if(word_count>size_words-extra-8)return kIOReturnNoResources;
    uint32_t rptr=0;IOReturn status=readRptr(&rptr);
    if(status)return status;
    const uint32_t used=(next_-rptr)&mask;
    const uint32_t total=(uint32_t)word_count+extra;
    /* KGSL adreno_ringbuffer_allocspace pads the tail with CP_NOP and starts
     * the whole batch at zero. Merely wrapping each copied word can split a
     * PM4 header/payload across the physical ring end on Pong's A730 SQE.
     * Reserve the tail and batch together; never overwrite unread commands. */
    const uint32_t padding=next_>size_words-2-total?size_words-next_:0;
    if(uint64_t(total)+padding+8>size_words-used)return kIOReturnNoResources;
    if(padding) {
        uint32_t cursor=next_,left=padding;
        while(left) {
            const uint32_t payload=left-1>0x3fffu?0x3fffu:left-1;
            uint32_t header=0;
            if(AdrenoPM4Type7(RingNop,payload,&header)!=AdrenoPM4Success)return poison();
            ring_[cursor]=header;cursor+=payload+1;left-=payload+1;
        }
    }
    uint32_t at=padding?0:next_;
    if(framed) {
        status=emitPrologue(at,reset,&at);
        if(status)return status;
    }
    for(size_t i=0;i<word_count;++i)ring_[(at+i)&mask]=words[i];
    at=(at+(uint32_t)word_count)&mask;
    uint32_t end=0;
    status=framed?emitFramedFence(at,seqno_+1,&end):emitFence(at,seqno_+1,&end);
    if(status)return status;
    if(publishWritten(next_,total+padding))return poison();
    if(io_.barrier(io_.cookie))return poison();
    if(io_.write32(io_.cookie,RingWptr,end))return poison();
    next_=end;*fence=++seqno_;++submissions_;
    if(reset)reset_pending_=false;
    return kIOReturnSuccess;
}
bool NativeAdrenoRing::fencePending(uint32_t fence) const
{
    if(!__atomic_load_n(&attached_,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&poisoned_,__ATOMIC_ACQUIRE))return false;
    const uint32_t submitted=__atomic_load_n(&seqno_,__ATOMIC_ACQUIRE);
    if(!fence || fence>submitted || !fence_ || !cache_.acquire)return false;
    if(cache_.acquire(cache_.cookie,fence_,fence_bytes_))return false;
    const uint32_t retired=__atomic_load_n(fence_+1,__ATOMIC_ACQUIRE);
    return retired<=submitted && retired<fence;
}
NativeAdrenoRingFence NativeAdrenoRing::observeFence(uint32_t fence,
    const NativeAdrenoRingClock *clock,uint32_t timeout_us,uint32_t *observed)
{
    if(observed)*observed=0;
    if(!attached_ || poisoned_)return NativeAdrenoRingFencePoisoned;
    if(!fence || fence>seqno_)return NativeAdrenoRingFenceArgument;
    uint64_t start=clock && clock->now_us?clock->now_us(clock->cookie):0;
    for(;;) {
        if(cache_.acquire(cache_.cookie,fence_,fence_bytes_)) {poison();return NativeAdrenoRingFencePoisoned;}
        const uint32_t value=fence_[1];
        if(observed)*observed=value;
        if(value>seqno_) {poison();return NativeAdrenoRingFencePoisoned;}
        if(value>=fence)return NativeAdrenoRingFenceComplete;
        if(!clock || !clock->now_us)return NativeAdrenoRingFenceTimeout;
        if(clock->now_us(clock->cookie)-start>=timeout_us)return NativeAdrenoRingFenceTimeout;
        if(clock->idle && clock->idle(clock->cookie))return NativeAdrenoRingFenceTimeout;
    }
}
NativeAdrenoRingFence NativeAdrenoRing::wait(uint32_t fence,const NativeAdrenoRingClock *clock,
    uint32_t timeout_us,uint32_t *observed)
{
    if(observed)*observed=0;
    if(!attached_ || poisoned_)return NativeAdrenoRingFencePoisoned;
    if(!fence || fence>seqno_)return NativeAdrenoRingFenceArgument;
    uint64_t start=clock && clock->now_us?clock->now_us(clock->cookie):0;
    for(;;) {
        if(cache_.acquire(cache_.cookie,fence_,fence_bytes_)) {poison();return NativeAdrenoRingFencePoisoned;}
        const uint32_t value=fence_[1];
        if(observed)*observed=value;
        if(value>=fence) {if(value>seqno_) {poison();return NativeAdrenoRingFencePoisoned;}completed_=value;
            return NativeAdrenoRingFenceComplete;}
        if(!clock || !clock->now_us)return NativeAdrenoRingFenceTimeout;
        if(clock->now_us(clock->cookie)-start>=timeout_us)return NativeAdrenoRingFenceTimeout;
        if(clock->idle && clock->idle(clock->cookie))return NativeAdrenoRingFenceTimeout;
    }
}

bool NativeAdrenoRing::timeline(uint32_t fence,uint64_t *start,uint64_t *end) {
    if(!start || !end || !attached_ || !stamps_ || fence_bytes_<1536 || cache_.acquire(cache_.cookie,fence_,fence_bytes_))return false;
    const volatile uint32_t *slot=fence_+(RingStampOffset+(fence&63)*16)/4;
    *start=(uint64_t(slot[1])<<32)|slot[0];*end=(uint64_t(slot[3])<<32)|slot[2];
    return *start && *end>=*start;
}
