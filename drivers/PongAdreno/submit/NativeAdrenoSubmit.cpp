/* SPDX-License-Identifier: MIT */
#define NATIVE_GPU_TRACE_OPTIONAL
#include "NativeGpuTrace.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoABI.h"
#include "AdrenoPM4.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
extern "C" uint64_t (*native_adreno_gpu_ticks)(void);
extern "C" uint64_t (*native_adreno_vm_now_us)(void);
extern "C" volatile uint64_t native_adreno_explicit_sync_stats[3];
extern "C" volatile uint64_t native_adreno_cache_batches[3];
extern "C" volatile uint64_t native_adreno_host_batch_stats[4];
extern "C" volatile uint64_t native_adreno_vm_phase_us[2][8],native_adreno_cache_stats[5],native_adreno_reassert_ticks;

/* CP_INDIRECT_BUFFER is opcode 0x3f with a 3-word payload: 64-bit GPU address
 * and IB_SIZE in dwords (bits 0..19), exactly as the pinned adreno_pm4.xml
 * domain states and Linux msm_gem_submit.c converts the userspace byte size. */
enum : uint32_t {SubmitIndirectBuffer=0x3f,SubmitIbWords=3,SubmitMaxJobs=8,
    SubmitMaxBuffers=128,SubmitMaxIbBytes=0xfffffu*4u,
    SubmitAuditBytes=256u*1024u};   /* IB bytes the write-combine audit compares per submission */
extern "C" {volatile uint32_t native_adreno_coherence[8];}
/* The submission (1-based count) at which the write-combine audit found a dirty IB line or failed; 0 = never. */
extern "C" {volatile uint32_t native_adreno_wc_distrusted;}
extern "C" {volatile uint32_t native_adreno_acquire_batches[2];}
/* The write-combine audit before each submission while trusted: [0] submissions audited, [1] IB lines compared, [2] dirty lines,
 * [3] stale clean lines, [4] the last audit failure (any failure also ends the trust), [5] pages found back at the default attribute
 * and write-combined again. */
extern "C" {volatile uint32_t native_adreno_wc_audit[6];}
/* Fence waits: [0] calls, [1] timeouts, [2] the fence of the last timed-out wait, [3] the fence value the GPU had written then, [4] the newest fence handed out,
 * [5] the newest completed, [6] waits for a fence no job holds (observeFence path), [7] the last fence refused as never submitted. */
extern "C" {volatile uint32_t native_adreno_fence_wait[8];}
/* The IBs of the last submissions: [slot] fence, iova low/high, bytes, then the first 28 dwords of the first IB (read as the kernel maps it). */
extern "C" {volatile uint32_t native_adreno_ib_log[4][4+28];volatile uint32_t native_adreno_ib_log_count;}
IOReturn NativeAdrenoSubmit::poison()
{
    poisoned_=true;return kIOReturnIOError;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::find(uint32_t fence)
{
    if(fence)for(auto &job:jobs_)if(job.active && job.fence==fence)return &job;
    return nullptr;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::free()
{
    for(auto &job:jobs_)if(!job.active)return &job;
    return nullptr;
}
IOReturn NativeAdrenoSubmit::attach(NativeAdrenoVm *vm,NativeAdrenoRing *ring)
{
    if(attached_ || poisoned_ || vm_ || ring_)return kIOReturnBusy;
    if(!vm || !ring || vm->poisoned() || ring->poisoned())return kIOReturnBadArgument;
    vm_=vm;ring_=ring;attached_=true;stats_={};current_context_=UINT32_MAX;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::release(Job &job)
{
    IOReturn status=kIOReturnSuccess;
    for(size_t i=0;i<job.count;++i) {
        IOReturn retired=vm_->retireJobAfterCompletion(job.handles[i]);
        if(retired)status=retired;else ++stats_.references_retired;
    }
    if(status)return poison();
    const uint64_t acquire_began=native_adreno_vm_now_us?native_adreno_vm_now_us():0;
    // One acquire covers all GPU writes up to the fence actually observed
    // before it. Retiring several jobs of the same client in one pass must
    // not flush every imported surface again for each already-complete job.
    // Any new submit invalidates this record, as does a changed completion
    // or VA lease. Real fence observation/BO reference retirement stay per job.
    const unsigned context=job.queue/NativeAdrenoAbiMaxQueues;
    const uint32_t completed=ring_->completedFence();
    AcquiredWindow *acquired=context<16?&acquired_[context]:nullptr;
    if(!acquired || !acquired->valid || acquired->completed!=completed ||
       acquired->start!=job.window_start || acquired->bytes!=job.window_bytes) {
        if(vm_->acquireImported(job.window_start,job.window_bytes))return poison();
        native_adreno_acquire_batches[0]++;
        if(acquired)*acquired={job.window_start,job.window_bytes,completed,true};
    } else native_adreno_acquire_batches[1]++;
    const uint64_t now=native_adreno_vm_now_us?native_adreno_vm_now_us():0;
    const uint64_t acquire_us=now>=acquire_began?now-acquire_began:0;
    uint64_t start=0,end=0;
    if(job.submit_us && now>=job.submit_us && now-job.submit_us>=33000) {
        if(now-slow_window_>=1000000){slow_window_=now;slow_logged_=0;}
        if(slow_logged_<8) {
            ++slow_logged_;
            const bool stamps=ring_->timeline(job.fence,&start,&end);
            const uint64_t tick=native_adreno_gpu_ticks?native_adreno_gpu_ticks():0;
            // The A730 CP counter is19.2MHz. Zero means unavailable;
            // never substitute CPU elapsed time for a GPU execution stamp.
            const uint64_t queue=stamps && start>=job.submit_tick && job.submit_tick?(start-job.submit_tick)*10/192:0;
            const uint64_t execute=stamps?(end-start)*10/192:0;
            const uint64_t retire=stamps && tick>=end?(tick-end)*10/192:0;
            IOLog("[gpu slow] pid=%d role=%d priority=%d fence=%u prior_pid=%d total_us=%llu cpu_sync_us=%llu acquire_us=%llu queue_us=%llu gpu_us=%llu retire_us=%llu stamps=%u rptr=%u admitted_yields=%llu\n",
                job.pid,job.role,job.priority,job.fence,job.preceding_pid,(unsigned long long)(now-job.submit_us),
                (unsigned long long)job.sync_us,(unsigned long long)acquire_us,(unsigned long long)queue,(unsigned long long)execute,(unsigned long long)retire,
                stamps?1:0,ring_->readPointer(),(unsigned long long)admission_yields_);
        }
    }
    (void)ring_->timeline(job.fence,&start,&end);
    gpuTraceEvent(GpuTraceRetire,job.pid,job.fence,job.queue,0,job.submit_us,start,end,end>start,0);
    job={};return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::detach()
{
    if(poisoned_ || outstanding())return kIOReturnBusy;
    if(!attached_)return kIOReturnNotReady;
    attached_=false;vm_=nullptr;ring_=nullptr;return kIOReturnSuccess;
}
bool NativeAdrenoSubmit::queueOutstanding(uint32_t queue) const
{
    return rangeOutstanding(queue,1);
}
bool NativeAdrenoSubmit::rangeOutstanding(uint32_t first,uint32_t count) const
{
    for(const auto &job:jobs_)if(job.active && job.queue-first<count)return true;
    return false;
}
size_t NativeAdrenoSubmit::outstanding() const
{
    size_t count=0;for(const auto &job:jobs_)if(job.active)++count;return count;
}
IOReturn NativeAdrenoSubmit::submit(const NativeAdrenoSubmitBuffer *buffers,size_t count,
    uint32_t queue,uint32_t *fence,uint64_t window_start,uint64_t window_bytes)
{
    if(!fence)return kIOReturnBadArgument;*fence=0;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!buffers || !count || count>SubmitMaxBuffers)return kIOReturnBadArgument;
    if(window_bytes && window_start>UINT64_MAX-window_bytes)return kIOReturnBadArgument;
    /* Every buffer is validated against the live VM before any reference. */
    uint64_t *handles=scratch_handles_;size_t handle_count=0;
    uint32_t *words=scratch_words_;size_t word_count=0;
    for(size_t i=0;i<count;++i) {
        const NativeAdrenoSubmitBuffer &buffer=buffers[i];
        if(!buffer.handle || !buffer.bytes || (buffer.bytes&3) || buffer.bytes>SubmitMaxIbBytes)
            return kIOReturnBadArgument;
        uint64_t iova=0;size_t bytes=0;unsigned permissions=0;
        IOReturn status=vm_->describe(buffer.handle,&iova,&bytes,&permissions);
        if(status)return status;
        if(window_bytes && (iova<window_start || iova-window_start>=window_bytes ||
            bytes>window_bytes-(iova-window_start)))return kIOReturnBadArgument;
        if(!(permissions&ArmSmmu4KRead) || buffer.iova<iova ||
            buffer.iova-iova>bytes || buffer.bytes>bytes-(buffer.iova-iova))return kIOReturnBadArgument;
        uint32_t header=0;
        if(AdrenoPM4Type7(SubmitIndirectBuffer,SubmitIbWords,&header)!=AdrenoPM4Success)
            return poison();
        words[word_count++]=header;
        words[word_count++]=(uint32_t)buffer.iova;
        words[word_count++]=(uint32_t)(buffer.iova>>32);
        words[word_count++]=(uint32_t)(buffer.bytes/4);
        bool known=false;
        for(size_t j=0;j<handle_count;++j)if(handles[j]==buffer.handle) {known=true;break;}
        if(!known)handles[handle_count++]=buffer.handle;
    }
    IOReturn swept=retireCompleted();
    if(swept)return swept;
    // Single-ring admission, not fabricated A740 hardware preemption.
    // Bound low-priority work already ahead of visible UI. FIFO and real
    // fences of every accepted submission stay unchanged.
    size_t client_pending=0;
    for(const auto &pending:jobs_)if(pending.active && pending.pid==next_pid_)++client_pending;
    const bool low=next_priority_==2 || next_priority_==3;
    const size_t cap=low?1:next_priority_==1?2:8;
    if(client_pending>=cap){++admission_yields_;return kIOReturnBusy;}
    const uint64_t began=native_adreno_vm_now_us?native_adreno_vm_now_us():0;
    const unsigned cache_context=queue/NativeAdrenoAbiMaxQueues;
    if(cache_context<16)acquired_[cache_context].valid=false;
    Job *job=free();
    if(!job)return kIOReturnBusy;
    for(size_t i=0;i<handle_count;++i) {
        IOReturn status=vm_->holdJob(handles[i]);
        if(status) {
            while(i) {vm_->retireJobAfterCompletion(handles[--i]);++stats_.references_retired;}
            return status;
        }
        ++stats_.references_held;
    }
    uint32_t value=0;
    /* int-070: the first IB word as the kernel's cacheable alias and DRAM see it before any maintenance, then DRAM after the CPU
     * caches over every mapped BO are cleaned and invalidated. The board shows the last submission's record on a stall. */
    uint32_t cached=~0u,before=~0u,ignored=0,after=~0u;
    /* Write-combined stores this CPU made before the call (the client's command words) complete before anything below. */
    __asm__ volatile("dsb sy":::"memory");
    /* int-098/099: with write-combined user BOs and no maintenance the CP stopped at the end of the first IB, as in int-065..069 when an
     * IB sat in a CPU cache. While write-combined BOs are trusted, every line of this submission's IBs is compared as a CPU cache and
     * DRAM hold it (bounded). A dirty line means a client view still caches its BOs: maintain every BO from now on, this submission
     * included (the audit already wrote the IB lines back; the rest of the BOs may hold more). */
    if(vm_->writeCombinedTrusted()) {
        /* int-100: the dirty IB page was back at XNU's default (cacheable) attribute. Write-combine every such page again first; the
         * pmap writes its lines back on the way, so the audit below should then find none. */
        uint32_t reset=0;
        IOReturn audited=vm_->reassertWriteCombined(&reset,window_start,window_bytes);
        native_adreno_wc_audit[5]=native_adreno_wc_audit[5]+reset;
        uint32_t lines=0,dirty=0,stale=0;size_t budget=SubmitAuditBytes;
        /* The re-assertion above is the correction and runs every time; the audit only detects, and found no dirty line in int-141's
         * 133798, so after the first 16 submissions it compares every eighth one's IBs (it cost about 2 ms each). */
        ++audit_tick_;
        const bool audit_now=audit_tick_<=16;
        if(!audit_now)budget=0;
        for(size_t i=0;i<count && budget && !audited;++i) {   /* a failed re-assertion skips the audit and ends the trust */
            const size_t bytes=buffers[i].bytes<budget?(size_t)buffers[i].bytes:budget;
            audited=vm_->auditRange(buffers[i].iova,bytes,&lines,&dirty,&stale);budget-=bytes;
        }
        if(audit_now && audited!=kIOReturnUnsupported) {   /* no board hook (host): nothing to compare */
            native_adreno_wc_audit[0]=native_adreno_wc_audit[0]+1;
            native_adreno_wc_audit[1]=native_adreno_wc_audit[1]+lines;
            native_adreno_wc_audit[2]=native_adreno_wc_audit[2]+dirty;
            native_adreno_wc_audit[3]=native_adreno_wc_audit[3]+stale;
            if(audited)native_adreno_wc_audit[4]=audited;
        }
        if(dirty || (audited && audited!=kIOReturnUnsupported)) {
            vm_->distrustWriteCombined();
            native_adreno_wc_distrusted=native_adreno_coherence[0]+1;
        }
    }
    // Upstream samples coherence every 64 submissions: these two fresh
    // uncached mappings are observations, not the publication operation.
    // Keep Pong's actual BO maintenance and V500 invalidation below.
    const bool sample=(stats_.submissions&63)==0;
    if(sample)(void)vm_->sampleWord(buffers[0].iova,&cached,&before);
    /* Only this client's IOVA window can be referenced by its validated
     * command buffers. Maintaining every other process's IOSurfaces here
     * was both expensive and outside this submission's ownership handoff. */
    IOReturn status=vm_->publishMapped(window_start,window_bytes);
    if(!status && sample) {
        (void)vm_->sampleWord(buffers[0].iova,&ignored,&after);
        native_adreno_coherence[0]=native_adreno_coherence[0]+1;
        native_adreno_coherence[1]=cached;native_adreno_coherence[2]=before;native_adreno_coherence[3]=after;
        if(cached!=before || before!=after) {
            if(!native_adreno_coherence[4]) {
                native_adreno_coherence[5]=cached;native_adreno_coherence[6]=before;native_adreno_coherence[7]=after;
            }
            native_adreno_coherence[4]=native_adreno_coherence[4]+1;
        }
    }
    // Map/unmap transactions publish their tables and invalidate before returning.
    // Repeating TLBIALL for an unchanged address space stalls every submission.
    // The existing fence-timeout recovery still invalidates the context.
    if(status) {
        for(size_t i=0;i<handle_count;++i) {vm_->retireJobAfterCompletion(handles[i]);++stats_.references_retired;}
        return status;
    }
    {
        const uint32_t slot=native_adreno_ib_log_count&3;uint32_t head[28];
        (void)vm_->peekWords(buffers[0].iova,head,28);
        native_adreno_ib_log[slot][0]=ring_->lastSubmitted()+1;native_adreno_ib_log[slot][1]=(uint32_t)buffers[0].iova;
        native_adreno_ib_log[slot][2]=(uint32_t)(buffers[0].iova>>32);native_adreno_ib_log[slot][3]=(uint32_t)buffers[0].bytes;
        for(unsigned i=0;i<28;++i)native_adreno_ib_log[slot][4+i]=head[i];
        native_adreno_ib_log_count=native_adreno_ib_log_count+1;
    }
    /* The single hardware ring alternates Vulkan clients. Reset the BR/BV
     * timestamps/resource table on each actual context change, as both
     * Nothing's gen7_rb_context_switch and Linux a6xx_set_pagetable do.
     * Queues within one client share that context. The prologue waits for
     * the preceding real fence before resetting any synchronization state. */
    const uint32_t context=queue/NativeAdrenoAbiMaxQueues;
    if(context!=current_context_)ring_->requestContextReset();
    const uint64_t before_submit=native_adreno_vm_now_us?native_adreno_vm_now_us():0;
    const uint64_t sync_time=before_submit>=began?before_submit-began:0;
    const uint64_t submit_tick=native_adreno_gpu_ticks?native_adreno_gpu_ticks():0;
    int32_t preceding_pid=0;uint32_t preceding_fence=0;
    for(const auto &pending:jobs_)if(pending.active && pending.fence>=preceding_fence){preceding_fence=pending.fence;preceding_pid=pending.pid;}
    status=ring_->submitFramed(words,word_count,&value);
    if(status) {
        /* A poisoned ring may already have consumed the WPTR write, so the
         * referenced buffers cannot be retired here. */
        if(ring_->poisoned()) {
            *job=Job{};++stats_.poisoned_jobs;
            for(size_t i=0;i<handle_count;++i)job->handles[i]=handles[i];
            job->count=handle_count;job->queue=queue;job->active=true;
            job->window_start=window_start;job->window_bytes=window_bytes;
            return poison();
        }
        while(handle_count) {vm_->retireJobAfterCompletion(handles[--handle_count]);++stats_.references_retired;}
        return status;
    }
    for(size_t i=0;i<handle_count;++i)job->handles[i]=handles[i];
    job->count=handle_count;job->fence=value;job->queue=queue;job->active=true;
    job->window_start=window_start;job->window_bytes=window_bytes;
    job->priority=next_priority_;job->pid=next_pid_;job->role=next_role_;
    job->submit_us=began;job->sync_us=sync_time;
    job->submit_tick=submit_tick;job->preceding_pid=preceding_pid;
    current_context_=context;
    gpuTraceEvent(GpuTraceSubmit,job->pid,value,queue,0,job->priority,job->role,0,stats_.peak_jobs,context,job->submit_us,handle_count);
    ++stats_.submissions;
    if(!(stats_.submissions&1023u)) {
        IOLog("[gpu host sync] calls=%llu MiB=%llu total_us=%llu completion-acquire=active-opt-in\n",
            (unsigned long long)native_adreno_explicit_sync_stats[0],(unsigned long long)(native_adreno_explicit_sync_stats[1]>>20),
            (unsigned long long)native_adreno_explicit_sync_stats[2]);
        uint64_t reassert_ns=0,cache_ns=0;
        absolutetime_to_nanoseconds(native_adreno_reassert_ticks,&reassert_ns);
        absolutetime_to_nanoseconds(native_adreno_cache_stats[0],&cache_ns);
        IOLog("[gpu cache] submits=%u completed=%u distrust=%u dirty=%u stale=%u reset=%u reassert_us=%llu maintain_us=%llu MiB=%llu\n",
            stats_.submissions,stats_.completed,native_adreno_wc_distrusted,native_adreno_wc_audit[2],native_adreno_wc_audit[3],native_adreno_wc_audit[5],
            (unsigned long long)(reassert_ns/1000),(unsigned long long)(cache_ns/1000),(unsigned long long)(native_adreno_cache_stats[1]>>20));
        IOLog("[gpu cache batches] calls=%llu batches=%llu pages=%llu max_pages=64\n",
            (unsigned long long)native_adreno_cache_batches[0],(unsigned long long)native_adreno_cache_batches[1],
            (unsigned long long)native_adreno_cache_batches[2]);
        IOLog("[gpu acquire] handoffs=%u duplicate-retirements-skipped=%u\n",native_adreno_acquire_batches[0],native_adreno_acquire_batches[1]);
        IOLog("[gpu host batches] calls=%llu ranges=%llu entries=%llu total_us=%llu\n",
            (unsigned long long)native_adreno_host_batch_stats[0],(unsigned long long)native_adreno_host_batch_stats[1],
            (unsigned long long)native_adreno_host_batch_stats[2],(unsigned long long)native_adreno_host_batch_stats[3]);
        for(unsigned row=0;row<2;++row)
            IOLog("[vm %s] n=%llu observe_us=%llu handoff_us=%llu tables_us=%llu bookkeeping_us=%llu preflight_us=%llu max_us=%llu\n",row?"unmap":"map",
                (unsigned long long)native_adreno_vm_phase_us[row][0],(unsigned long long)native_adreno_vm_phase_us[row][1],
                (unsigned long long)native_adreno_vm_phase_us[row][2],(unsigned long long)native_adreno_vm_phase_us[row][3],
                (unsigned long long)native_adreno_vm_phase_us[row][4],(unsigned long long)native_adreno_vm_phase_us[row][5],
                (unsigned long long)native_adreno_vm_phase_us[row][6]);
    }
    if(outstanding()>stats_.peak_jobs)stats_.peak_jobs=outstanding();
    *fence=value;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::observeFence(uint32_t fence,const NativeAdrenoRingClock *clock,
    uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!fence)return kIOReturnBadArgument;
    const NativeAdrenoRingFence result=ring_->observeFence(fence,clock,timeout_us,nullptr);
    if(result==NativeAdrenoRingFencePoisoned)return poison();
    if(result==NativeAdrenoRingFenceArgument)return kIOReturnBadArgument;
    if(result==NativeAdrenoRingFenceTimeout)return kIOReturnTimeout;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::wait(uint32_t fence,const NativeAdrenoRingClock *clock,
    uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    Job *job=find(fence);
    uint32_t observed=0;
    native_adreno_fence_wait[0]=native_adreno_fence_wait[0]+1;
    if(!job) {
        native_adreno_fence_wait[6]=native_adreno_fence_wait[6]+1;
        const NativeAdrenoRingFence passed=ring_->observeFence(fence,clock,timeout_us,&observed);
        if(passed==NativeAdrenoRingFencePoisoned)return poison();
        if(passed==NativeAdrenoRingFenceArgument) {native_adreno_fence_wait[7]=fence;return kIOReturnBadArgument;}
        if(passed==NativeAdrenoRingFenceTimeout) {
            ++stats_.timeouts;
            native_adreno_fence_wait[1]=native_adreno_fence_wait[1]+1;native_adreno_fence_wait[2]=fence;native_adreno_fence_wait[3]=observed;
            native_adreno_fence_wait[4]=ring_->lastSubmitted();native_adreno_fence_wait[5]=ring_->completedFence();
            return kIOReturnTimeout;
        }
        return kIOReturnSuccess;
    }
    const NativeAdrenoRingFence result=ring_->wait(fence,clock,timeout_us,&observed);
    if(result==NativeAdrenoRingFencePoisoned)return poison();
    if(result==NativeAdrenoRingFenceArgument) {native_adreno_fence_wait[7]=fence;return kIOReturnBadArgument;}
    if(result==NativeAdrenoRingFenceTimeout) {
        ++stats_.timeouts;
        native_adreno_fence_wait[1]=native_adreno_fence_wait[1]+1;native_adreno_fence_wait[2]=fence;native_adreno_fence_wait[3]=observed;
        native_adreno_fence_wait[4]=ring_->lastSubmitted();native_adreno_fence_wait[5]=ring_->completedFence();
        return kIOReturnTimeout;
    }
    IOReturn status=release(*job);
    if(status)return status;
    ++stats_.completed;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::waitAll(const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    while(outstanding()) {
        Job *oldest=nullptr;
        for(auto &job:jobs_)if(job.active && (!oldest || job.fence-oldest->fence>0x80000000u))oldest=&job;
        const IOReturn status=wait(oldest->fence,clock,timeout_us);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::waitRange(uint32_t first,uint32_t count,const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    while(rangeOutstanding(first,count)) {
        Job *oldest=nullptr;
        for(auto &job:jobs_)if(job.active && job.queue-first<count && (!oldest || job.fence-oldest->fence>0x80000000u))oldest=&job;
        const IOReturn status=wait(oldest->fence,clock,timeout_us);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::retireCompleted()
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    for(auto &job:jobs_) {
        if(!job.active)continue;
        const NativeAdrenoRingFence state=ring_->observeFence(job.fence,nullptr,0,nullptr);
        if(state==NativeAdrenoRingFencePoisoned)return poison();
        if(state!=NativeAdrenoRingFenceComplete)continue;
        const IOReturn status=release(job);
        if(status)return status;
        ++stats_.completed;
    }
    return kIOReturnSuccess;
}
