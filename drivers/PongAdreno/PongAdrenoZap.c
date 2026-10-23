/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "PongAdrenoZap.h"

static uint16_t u16(const uint8_t *p)
{return (uint16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8));}
static uint32_t u32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}

PongZapStatus PongAdrenoPlanZap(const uint8_t *data,size_t bytes,PongZapPlan *out)
{
    if (!data || !out) return PongZapArgument;
    /* Vendor metadata is ELF32 little endian, including the complete program
     * table in its first non-load segment. The signature is a separate file.
     */
    if (bytes<52 || data[0]!=0x7f || data[1]!='E' || data[2]!='L' || data[3]!='F' ||
        data[4]!=1 || data[5]!=1 || data[6]!=1 || u16(data+16)!=2 ||
        u16(data+18)!=164 || u32(data+20)!=1 || u16(data+40)!=52 ||
        u32(data+28)!=52 || u16(data+42)!=32) return PongZapFirmware;
    unsigned count=u16(data+44);
    if (count<2 || count>16 || bytes<52u+32u*count) return PongZapFirmware;
    const uint8_t *first=data+52;
    size_t header=u32(first+16),signature=0;
    if (u32(first)==1 || u32(first+4)!=0 || header!=52u+32u*count || header>bytes)
        return PongZapFirmware;
    PongZapPlan plan={0};unsigned loads=0,signatures=0;
    for (unsigned i=1;i<count;i++) {
        const uint8_t *p=data+52+32*i;
        uint32_t type=u32(p),flags=u32(p+24);
        if ((flags&0x07000000u)==0x02000000u) {
            if (type==1 || !u32(p+16)) return PongZapFirmware;
            signature=u32(p+16);plan.signature_file_index=i;signatures++;
        } else if (type==1 && u32(p+20)) {
            uint32_t file=u32(p+16),memory=u32(p+20),base=u32(p+12);
            if (!(flags&0x08000000u) || !file || file>memory ||
                memory>PongZapReservedBytes || (base&4095u)) return PongZapFirmware;
            uint64_t end=(uint64_t)base+memory;
            if (end>UINT32_MAX || end+4095>UINT32_MAX) return PongZapFirmware;
            plan.load_file_index=i;plan.segment_bytes=file;
            plan.segment_memory_bytes=memory;plan.relocation_base=base;
            plan.image_span=(uint32_t)(((end+4095)&~UINT64_C(4095))-base);
            loads++;
        } else if (type==1) return PongZapFirmware;
    }
    if (loads!=1 || signatures!=1 || header>4096 || signature>4096-header ||
        plan.image_span>PongZapReservedBytes)
        return PongZapFirmware;
    plan.header_bytes=header;plan.signature_bytes=signature;
    plan.metadata_bytes=header+signature;
    *out=plan;
    return PongZapSuccess;
}

static int overlaps(uintptr_t a,size_t an,uintptr_t b,size_t bn)
{
    if (an>UINTPTR_MAX-a || bn>UINTPTR_MAX-b) return 1;
    return a<b+bn && b<a+an;
}

static PongZapStatus call(const PongZapOps *ops,PongZapResult *result,
                          unsigned command,uint32_t arginfo,uint64_t arg1,uint64_t arg2)
{
    uint64_t args[3]={PongZapPasId,arg1,arg2};
    result->function=0x02000200u|command|
        (ops->smccc_width==64 ? 0x40000000u:0u);
    result->transport=-1;result->secure_result=-1;
    if (ops->scm(ops->cookie,result->function,arginfo,args,
                  &result->transport,&result->secure_result)) return PongZapIo;
    return result->transport==0 && result->secure_result==0 ?
        PongZapSuccess:PongZapSecureError;
}

PongZapStatus PongAdrenoStartZap(const PongZapOps *ops,const PongZapConfig *cfg,
                                PongZapState *state,PongZapResult *result)
{
    if (!ops || !cfg || !state || !result || !ops->platform_ready ||
        !ops->publish || !ops->scm || (ops->smccc_width!=32 && ops->smccc_width!=64))
        return PongZapArgument;
    *result=(PongZapResult){0};
    if (state->attempted || state->poisoned) return PongZapPoisoned;
    PongZapStatus status=PongAdrenoPlanZap(cfg->mdt,cfg->mdt_bytes,&result->plan);
    if (status!=PongZapSuccess) return status;
    const PongZapPlan *plan=&result->plan;
    const PongZapMemory *meta=&cfg->metadata,*reserved=&cfg->reserved;
    if (!cfg->segment || !cfg->signature || cfg->signature_bytes!=plan->signature_bytes ||
        cfg->signature_file_index!=plan->signature_file_index ||
        cfg->segment_bytes!=plan->segment_bytes ||
        cfg->segment_file_index!=plan->load_file_index || !meta->cpu ||
        !reserved->cpu || meta->bytes!=4096 || !meta->physical ||
        ((uintptr_t)meta->cpu&4095u) || (meta->physical&4095u) ||
        ((uintptr_t)reserved->cpu&4095u) ||
        reserved->physical!=PongZapReservedPhysical || reserved->bytes!=PongZapReservedBytes ||
        meta->physical>UINT64_MAX-meta->bytes ||
        (ops->smccc_width==32 && meta->physical+meta->bytes>UINT64_C(0x100000000)) ||
        (meta->physical<reserved->physical+reserved->bytes &&
         reserved->physical<meta->physical+meta->bytes) ||
        overlaps((uintptr_t)meta->cpu,meta->bytes,(uintptr_t)reserved->cpu,reserved->bytes) ||
        overlaps((uintptr_t)meta->cpu,meta->bytes,(uintptr_t)cfg->mdt,cfg->mdt_bytes) ||
        overlaps((uintptr_t)meta->cpu,meta->bytes,(uintptr_t)cfg->signature,cfg->signature_bytes) ||
        overlaps((uintptr_t)meta->cpu,meta->bytes,(uintptr_t)cfg->segment,cfg->segment_bytes) ||
        overlaps((uintptr_t)reserved->cpu,reserved->bytes,(uintptr_t)cfg->mdt,cfg->mdt_bytes) ||
        overlaps((uintptr_t)reserved->cpu,reserved->bytes,(uintptr_t)cfg->signature,cfg->signature_bytes) ||
        overlaps((uintptr_t)reserved->cpu,reserved->bytes,(uintptr_t)cfg->segment,cfg->segment_bytes))
        return PongZapArgument;
    result->phase=1;
    if (ops->platform_ready(ops->cookie)) return PongZapPrerequisite;
    state->attempted=1;state->ready=0;state->poisoned=1;
    result->resources_must_retain=1;
    for (size_t i=0;i<meta->bytes;i++) {
        uint8_t value=0;
        if (i<plan->header_bytes) value=cfg->mdt[i];
        else if (i<plan->metadata_bytes) value=cfg->signature[i-plan->header_bytes];
        meta->cpu[i]=value;
    }
    if (ops->publish(ops->cookie,meta,meta->bytes)) return PongZapIo;
    /* First cold load follows qcom_mdt_load/adreno_zap_shader_load. PAS13
     * need not have been started before; an unconditional shutdown rejects
     * that valid state on firmware returning EINVAL for an unloaded PAS. */
    result->phase=3;
    status=call(ops,result,1,2u|(2u<<6),meta->physical,0);
    if (status!=PongZapSuccess) return status;
    result->phase=4;
    status=call(ops,result,2,3,reserved->physical,plan->image_span);
    if (status!=PongZapSuccess) return status;
    result->phase=5;
    for (size_t i=0;i<plan->segment_memory_bytes;i++)
        reserved->cpu[i]=i<cfg->segment_bytes ? cfg->segment[i]:0;
    if (ops->publish(ops->cookie,reserved,plan->segment_memory_bytes)) return PongZapIo;
    result->phase=6;
    status=call(ops,result,5,1,0,0);
    if (status!=PongZapSuccess) return status;
    state->ready=1;state->poisoned=0;result->phase=7;
    return PongZapSuccess;
}
