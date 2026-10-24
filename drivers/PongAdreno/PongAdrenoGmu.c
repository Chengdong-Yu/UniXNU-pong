/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Startup structure from UniXNU-Sheng's ShengGmu; register numbers, RSC
 * sequence and chip-id encoding from Nothing's SM8475 gen7 driver. */
#include "PongAdrenoGmu.h"
#include "AdrenoFirmware.h"
#include <PongAdrenoRegisters.h>
#if !defined(__aarch64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error Pong GMU requires little-endian AArch64
#endif
const PongGpuWindow PongAdrenoWindows[PongGpuWindowCount]={
    {0x03d00000,0x40000},{0x03d68000,0x37000},{0x03d50000,0x10000},
    {0x0b290000,0x10000},{0x03d9e000,0x1000},{0x03d90000,0xa000},
    {0x03da0000,0x40000},{0x03d40000,0x10000}
};
static size_t offset(uint32_t word) {return (size_t)word*4-0x68000;}
static int rd(PongGpuGmuDevice *d,unsigned w,size_t at,uint32_t *v)
{
    if(w>=PongGpuWindowCount || (at&3) || at>PongAdrenoWindows[w].bytes-4)return 1;
    return d->io.read32(d->io.cookie,w,at,v);
}
static int wr(PongGpuGmuDevice *d,unsigned w,size_t at,uint32_t v,PongGpuGmuResult *r)
{
    if(w>=PongGpuWindowCount || (at&3) || at>PongAdrenoWindows[w].bytes-4)return 1;
    ++r->writes;r->last_register=(uint32_t)at;r->last_value=v;
    return d->io.write32(d->io.cookie,w,at,v);
}
static int core_read(PongGpuGmuDevice *d,uint32_t reg,uint32_t *v)
{return rd(d,PongGpuGmu,offset(reg),v);}
static int core_write(PongGpuGmuDevice *d,uint32_t reg,uint32_t v,PongGpuGmuResult *r)
{return wr(d,PongGpuGmu,offset(reg),v,r);}
static int notify(void *cookie)
{
    PongGpuGmuDevice *d=cookie;
    return d->io.barrier(d->io.cookie) || d->io.write32(d->io.cookie,
        PongGpuGmu,offset(GEN7_GMU_HOST2GMU_INTR_SET),1) || d->io.barrier(d->io.cookie);
}
static PongGpuStatus poll(PongGpuGmuDevice *d,uint32_t reg,uint32_t mask,uint32_t want,PongGpuGmuResult *r)
{
    uint64_t began,last,now;
    if(d->io.now_us(d->io.cookie,&began))return PongGpuIo;
    last=began;
    for(unsigned n=0;n<1000;n++) {
        uint32_t v;
        if(d->io.platform_ready(d->io.cookie))return PongGpuPrerequisite;
        if(d->io.now_us(d->io.cookie,&now) || now<last)return PongGpuIo;
        if(now-began>=100000)return PongGpuTimeout;
        last=now;
        if(core_read(d,reg,&v))return PongGpuIo;
        r->last_register=reg;r->last_value=v;
        if(reg==GEN7_GMU_CM3_FW_INIT_RESULT && (v&0xe00))return PongGpuIo;
        if((v&mask)==want)return PongGpuSuccess;
        if(d->io.wait_us(d->io.cookie,100))return PongGpuIo;
    }
    return PongGpuTimeout;
}
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int memory(const PongGpuMemory *m,size_t bytes,uint32_t iova)
{
    return m->cpu && m->bytes==bytes && !((uintptr_t)m->cpu&4095) &&
        (uintptr_t)m->cpu<=UINTPTR_MAX-(bytes-1) && m->iova==iova;
}
static int overlaps(const PongGpuMemory *a,const PongGpuMemory *b)
{
    uintptr_t x=(uintptr_t)a->cpu,y=(uintptr_t)b->cpu;
    return x<=y?y-x<a->bytes:x-y<b->bytes;
}
static PongGpuStatus prerequisites(PongGpuGmuDevice *d,const PongGpuGmuConfig *c)
{
    if(!d || !c || c->chip_id!=PongAdrenoChipId || !c->firmware || !c->firmware_bytes ||
        c->firmware_bytes>UINTPTR_MAX-(uintptr_t)c->firmware || !d->io.platform_ready ||
        !d->io.read32 || !d->io.write32 || !d->io.barrier || !d->io.publish ||
        !d->io.now_us || !d->io.wait_us ||
        !memory(&c->cache,0xffc000,0x4000) || !memory(&c->dummy,0x2000,0x60000000) ||
        !memory(&c->hfi,0x4000,0x60002000) || !memory(&c->log,0x4000,0x60006000))return PongGpuArgument;
    const PongGpuMemory *m[]={&c->cache,&c->dummy,&c->hfi,&c->log};
    for(unsigned i=0;i<4;i++) {
        uintptr_t p=(uintptr_t)c->firmware,q=(uintptr_t)m[i]->cpu;
        if(p<=q?q-p<c->firmware_bytes:p-q<m[i]->bytes)return PongGpuArgument;
        for(unsigned j=0;j<i;j++)if(overlaps(m[i],m[j]))return PongGpuArgument;
    }
    if(d->poisoned)return PongGpuPoisoned;
    if(d->attempted)return PongGpuBusy;
    return d->io.platform_ready(d->io.cookie)?PongGpuPrerequisite:PongGpuSuccess;
}
PongGpuStatus PongAdrenoGmuStart(PongGpuGmuDevice *d,const PongGpuGmuConfig *c,PongGpuGmuResult *r)
{
    if(!r)return PongGpuArgument;
    *r=(PongGpuGmuResult){0};
    PongGpuStatus status=prerequisites(d,c);
    if(status)return status;
    const AdrenoFirmwareRegion regions[]={{0,0x4000,1},{0x4000,0xffc000,2},{0x10004000,0x4000,3}};
    AdrenoFirmwareBlock blocks[32];size_t count=0;uint32_t value;
    if(AdrenoFirmwarePlanGmu(c->firmware,c->firmware_bytes,regions,3,blocks,32,&count)!=AdrenoFirmwareSuccess)
        return PongGpuFirmware;
    if(core_read(d,GEN7_GMU_CM3_SYSRESET,&value))return PongGpuIo;
    if(value!=1)return PongGpuBusy;
    if(rd(d,PongGpuRscc,GEN7_RSCC_SEQ_BUSY_DRV0*4,&value))return PongGpuIo;
    if(value)return PongGpuBusy;
    if(core_read(d,GEN7_GMU_HFI_CTRL_STATUS,&value))return PongGpuIo;
    if(value&1)return PongGpuBusy;
    d->attempted=1;r->phase=1;r->resources_must_retain=1;
    static const struct {uint32_t reg,value;} setup[]={
        {GEN7_GPU_RSCC_RSC_STATUS0_DRV0,1u<<24},{GEN7_RSCC_PDC_SLAVE_ID_DRV0,1},
        {GEN7_RSCC_HIDDEN_TCS_CMD0_DATA,0},{GEN7_RSCC_HIDDEN_TCS_CMD0_ADDR,0},
        {GEN7_RSCC_HIDDEN_TCS_CMD0_DATA+2,0},{GEN7_RSCC_HIDDEN_TCS_CMD0_ADDR+2,0},
        {GEN7_RSCC_HIDDEN_TCS_CMD0_DATA+4,0x80000000},{GEN7_RSCC_HIDDEN_TCS_CMD0_ADDR+4,0},
        {GEN7_RSCC_OVERRIDE_START_ADDR,0},{GEN7_RSCC_PDC_SEQ_START_ADDR,0x4520},
        {GEN7_RSCC_PDC_MATCH_VALUE_LO,0x4510},{GEN7_RSCC_PDC_MATCH_VALUE_HI,0x4514},
        {GEN7_RSCC_SEQ_MEM_0_DRV0,0xeaaae5a0},{GEN7_RSCC_SEQ_MEM_0_DRV0+1,0xe1a1ebab},
        {GEN7_RSCC_SEQ_MEM_0_DRV0+2,0xa2e0a581},{GEN7_RSCC_SEQ_MEM_0_DRV0+3,0xecac82e2},
        {GEN7_RSCC_SEQ_MEM_0_DRV0+4,0x0020edad}
    };
    status=PongGpuIo;
    for(unsigned i=0;i<sizeof(setup)/sizeof(setup[0]);i++)
        if(wr(d,PongGpuRscc,setup[i].reg*4,setup[i].value,r))goto failed;
    if(wr(d,PongGpuPdc,GEN7_PDC_GPU_SEQ_START_ADDR*4,0,r) ||
       wr(d,PongGpuPdc,GEN7_PDC_GPU_ENABLE_PDC*4,0x80000001,r) ||
       core_write(d,GEN7_GPU_GMU_CX_GMU_CX_FALNEXT_INTF,1,r) ||
       core_write(d,GEN7_GPU_GMU_CX_GMU_CX_FAL_INTF,1,r) ||
       wr(d,PongGpuCxMisc,GEN7_GPU_CX_MISC_TCM_RET_CNTL*4,1,r))goto failed;
    const PongGpuMemory *m[]={&c->cache,&c->dummy,&c->hfi,&c->log};
    for(unsigned n=0;n<4;n++)for(size_t i=0;i<m[n]->bytes/4;i++)m[n]->cpu[i]=0;
    for(size_t i=0;i<count;i++) {
        const AdrenoFirmwareBlock *b=&blocks[i];
        for(size_t at=0;at<b->bytes;at+=4) {
            uint32_t word=le32(c->firmware+b->payload_offset+at);
            if(b->region_identifier==2)c->cache.cpu[(b->address-0x4000+at)/4]=word;
            else {
                uint32_t reg=b->region_identifier==1?GEN7_GMU_CM3_ITCM_START+b->address/4:
                    GEN7_GMU_CM3_DTCM_START+(b->address-0x10004000)/4;
                if(core_write(d,reg+(uint32_t)(at/4),word,r))goto failed;
            }
        }
    }
    /* Actual vendor firmware version words in the uploaded DTCM. They are
     * observations, not a substituted profile ID or an image hash check.
     */
    if(core_read(d,GEN7_GMU_CM3_DTCM_START+0xff8,&r->core_version) ||
       core_read(d,GEN7_GMU_CM3_DTCM_START+0xffc,&r->hfi_version))goto failed;
    if(PongAdrenoHfiInitializeStopped(&d->hfi,c->hfi.cpu,c->hfi.bytes,c->hfi.iova,d,notify)!=PongAdrenoHfiSuccess)
        goto failed;
    for(unsigned n=0;n<4;n++)if(d->io.publish(d->io.cookie,(void *)m[n]->cpu,m[n]->bytes))goto failed;
    r->phase=2;
    /* ADRENO_GMU_CHIPID(0x07030002), not Sheng's A740 identifier. */
    static const struct {uint32_t reg,value;} config[]={
        {GEN7_GMU_CM3_FW_INIT_RESULT,0},{GEN7_GMU_CM3_BOOT_CONFIG,2},
        {GEN7_GMU_HFI_QTBL_INFO,1},{GEN7_GMU_CM3_CFG,0x4052},
        {GEN7_GMU_GENERAL_10,0x07030200},{GEN7_GMU_GENERAL_9,0},
        {GEN7_GMU_AHB_FENCE_RANGE_0,0x80c808a0},
        {GEN7_GMU_SYS_BUS_CONFIG,1},{GEN7_GMU_ICACHE_CONFIG,1},{GEN7_GMU_DCACHE_CONFIG,1}
    };
    for(unsigned i=0;i<sizeof(config)/sizeof(config[0]);i++)
        if(core_write(d,config[i].reg,config[i].value,r))goto failed;
    /* Nothing gen7_gmu_register_config releases the CX GBIF halt BEFORE
     * CM3 boot. GDSC retention may preserve the boot firmware's halt; waiting
     * until CP initialization leaves the firmware starting on a halted bus.
     * This CX register is accessible with GX still off. */
    if(wr(d,PongGpuCore,0x3c45u*4,0,r) ||
       d->io.barrier(d->io.cookie) || rd(d,PongGpuCore,0x3c45u*4,&value) || value)goto failed;
    if(core_write(d,GEN7_GMU_HFI_QTBL_ADDR,c->hfi.iova,r) ||
       core_write(d,GEN7_GMU_GENERAL_8,c->log.iova|3,r) ||
       d->io.barrier(d->io.cookie) || d->io.platform_ready(d->io.cookie) ||
       core_write(d,GEN7_GMU_CM3_SYSRESET,0,r) || d->io.barrier(d->io.cookie))goto failed;
    r->phase=3;
    status=poll(d,GEN7_GMU_CM3_FW_INIT_RESULT,0x1ff,0x100,r);
    if(status)goto failed;
    r->fw_ready=1;d->fw_ready=1;status=PongGpuIo;
    if(core_write(d,GEN7_GMU_HFI_CTRL_INIT,1,r) || d->io.barrier(d->io.cookie))goto failed;
    status=poll(d,GEN7_GMU_HFI_CTRL_STATUS,1,1,r);
    if(status)goto failed;
    r->hfi_ready=1;d->hfi_ready=1;r->phase=4;return PongGpuSuccess;
failed:
    d->poisoned=1;d->hfi.poisoned=1;d->fw_ready=d->hfi_ready=0;
    (void)core_write(d,GEN7_GMU_CM3_SYSRESET,1,r);
    (void)d->io.barrier(d->io.cookie);
    return status;
}

static PongGpuStatus healthy(PongGpuGmuDevice *d)
{
    uint32_t fw,hfi;
    if(d->io.platform_ready(d->io.cookie) ||
       core_read(d,GEN7_GMU_CM3_FW_INIT_RESULT,&fw) ||
       core_read(d,GEN7_GMU_HFI_CTRL_STATUS,&hfi))return PongGpuIo;
    return (fw&0x1ff)==0x100 && !(fw&0xe00) && (hfi&1)?PongGpuSuccess:PongGpuPrerequisite;
}
PongGpuStatus PongAdrenoGmuStartPower(PongGpuGmuDevice *d,const PongPowerInputs *in,
    PongGpuPowerResult *r)
{
    if(!r)return PongGpuArgument;
    *r=(PongGpuPowerResult){0};r->resources_must_retain=1;
    if(!d || !in || !d->attempted || !d->fw_ready || !d->hfi_ready ||
       !d->io.platform_ready || !d->io.read32 || !d->io.now_us || !d->io.wait_us)
        return PongGpuPrerequisite;
    if(d->poisoned || d->hfi.poisoned)return PongGpuPoisoned;
    if(d->power_attempted || d->power_started || d->hfi.pending_header)return PongGpuBusy;
    PongPowerTables tables;
    if(PongAdrenoBuildPowerTables(in,&tables)!=PongPowerSuccess)return PongGpuArgument;
    PongGpuStatus status=healthy(d);
    if(status)return status;
    uint64_t begin,last,now;
    if(d->io.now_us(d->io.cookie,&begin))return PongGpuIo;
    last=begin;d->power_attempted=1;
    const unsigned ids[]={4,3,14,30};
    uint32_t core_start=0,default_vote[]={1,tables.default_gpu_index,tables.default_bus_index};
    const uint32_t *payloads[]={tables.performance,tables.bandwidth,&core_start,default_vote};
    const size_t words[]={58,159,1,3};
    for(unsigned step=0;step<4;step++) {
        r->phase=step+1;r->command=ids[step];r->header=0;
        if(d->io.now_us(d->io.cookie,&now) || now<last) {status=PongGpuIo;goto failed;}
        last=now;uint64_t command_begin=now;
        if(now-begin>=4000000) {status=PongGpuTimeout;goto failed;}
        status=healthy(d);if(status)goto failed;
        r->transport=PongAdrenoHfiSend(&d->hfi,ids[step],payloads[step],words[step],&r->header);
        if(r->header)++r->published;
        if(r->transport!=PongAdrenoHfiSuccess) {status=PongGpuIo;goto failed;}
        unsigned acknowledged=0;
        for(unsigned n=0;n<10000;n++) {
            if(d->io.now_us(d->io.cookie,&now) || now<last) {status=PongGpuIo;goto failed;}
            last=now;
            if(now-command_begin>=1000000 || now-begin>=4000000) {status=PongGpuTimeout;goto failed;}
            status=healthy(d);if(status)goto failed;
            uint32_t output[16];size_t count;
            ++r->polls;
            r->transport=PongAdrenoHfiPoll(&d->hfi,output,16,&count,&r->firmware_error);
            if(r->transport==PongAdrenoHfiSuccess) {acknowledged=1;++r->acknowledged;break;}
            if(r->transport!=PongAdrenoHfiEmpty) {status=PongGpuIo;goto failed;}
            if(d->io.wait_us(d->io.cookie,100)) {status=PongGpuIo;goto failed;}
        }
        if(!acknowledged) {status=PongGpuTimeout;goto failed;}
    }
    status=healthy(d);if(status)goto failed;
    d->power_started=1;r->phase=5;return PongGpuSuccess;
failed:
    d->poisoned=1;d->hfi.poisoned=1;d->power_started=0;
    return status;
}

PongGpuStatus PongAdrenoGmuHoldGpu(PongGpuGmuDevice *d,PongGpuPowerResult *r)
{
    if(!d || !r || !d->power_started || d->gpu_held || d->poisoned || d->hfi.pending_header)
        return PongGpuPrerequisite;
    *r=(PongGpuPowerResult){0};r->resources_must_retain=1;
    PongGpuGmuResult mmio={0};
    PongGpuStatus status=healthy(d);
    if(status)return status;
    r->phase=1;
    /* oob_gpu is0: SET30, ACK31, CLEAR31. Clear stale ACK before SET. */
    if(core_write(d,GEN7_GMU_GMU2HOST_INTR_CLR,1u<<31,&mmio) || d->io.barrier(d->io.cookie))return PongGpuIo;
    uint32_t pending=0;
    if(core_read(d,GEN7_GMU_GMU2HOST_INTR_INFO,&pending) || (pending&(1u<<31)))return PongGpuBusy;
    if(core_write(d,GEN7_GMU_HOST2GMU_INTR_SET,1u<<30,&mmio))return PongGpuIo;
    status=poll(d,GEN7_GMU_GMU2HOST_INTR_INFO,1u<<31,1u<<31,&mmio);
    if(status)goto failed_gpu;
    if(core_write(d,GEN7_GMU_GMU2HOST_INTR_CLR,1u<<31,&mmio)) {status=PongGpuIo;goto failed_gpu;}
    r->phase=2;r->command=10;
    r->transport=PongAdrenoHfiSend(&d->hfi,10,0,0,&r->header);
    if(r->transport!=PongAdrenoHfiSuccess){status=PongGpuIo;goto failed_gpu;}
    r->published=1;
    uint64_t begin,now;
    if(d->io.now_us(d->io.cookie,&begin)){status=PongGpuIo;goto failed_gpu;}
    for(unsigned n=0;n<10000;n++) {
        uint32_t output[16];size_t count;
        if(d->io.now_us(d->io.cookie,&now) || now<begin){status=PongGpuIo;goto failed_gpu;}
        if(now-begin>=1000000){status=PongGpuTimeout;goto failed_gpu;}
        ++r->polls;r->transport=PongAdrenoHfiPoll(&d->hfi,output,16,&count,&r->firmware_error);
        if(r->transport==PongAdrenoHfiSuccess) {
            r->acknowledged=1;r->phase=3;d->gpu_held=1;return healthy(d);
        }
        if(r->transport!=PongAdrenoHfiEmpty){status=PongGpuIo;goto failed_gpu;}
        if(d->io.wait_us(d->io.cookie,100)){status=PongGpuIo;goto failed_gpu;}
    }
    status=PongGpuTimeout;
failed_gpu:
    /* No CP has been started by this function; release only our OOB request. */
    (void)core_write(d,GEN7_GMU_HOST2GMU_INTR_SET,1u<<31,&mmio);
    d->poisoned=1;d->hfi.poisoned=1;
    return status;
}
