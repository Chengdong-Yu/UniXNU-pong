/* SPDX-License-Identifier: MIT */
#include "ArmSmmu4KContext.h"
static int width_code(unsigned bits)
{
    switch(bits) {case 32:return 0;case 36:return 1;case 40:return 2;
        case 42:return 3;case 44:return 4;case 48:return 5;default:return -1;}
}
static unsigned width_bits(unsigned code)
{
    switch(code) {case 0:return 32;case 1:return 36;case 2:return 40;
        case 3:return 42;case 4:return 44;case 5:return 48;default:return 0;}
}
static int region_valid(const ArmSmmu4KConfig *c,const ArmSmmu4KResult *r,unsigned upper)
{
    if(!c || !r)return !c && !r;
    if(c->input_bits<32 || c->input_bits>48 || width_code(c->output_bits)<0 ||
        (c->table_physical&4095) || r->root_physical!=c->table_physical ||
        r->mair!=0xff44 || r->upper_region!=upper || !r->table_pages ||
        r->leaf_pages>c->leaf_budget || r->table_pages>UINT64_MAX/4096 ||
        r->start_level!=4-(c->input_bits-12+8)/9)return 0;
    uint64_t limit=UINT64_C(1)<<c->output_bits;
    return r->root_physical<limit && (uint64_t)r->table_pages*4096<=limit-r->root_physical;
}
ArmSmmu4KStatus ArmSmmu4KEncodeContext(const ArmSmmu4KContextConfig *c,
    ArmSmmu4KContextImage *out)
{
    if(!out)return ArmSmmu4KArgument;
    out->cbar=out->cba2r=out->tcr=out->tcr2=out->mair0=out->mair1=out->sctlr=0;
    out->reserved_zero=0;
    out->ttbr0=out->ttbr1=0;
    if(!c || (!c->lower && !c->upper) || c->asid>65535 || c->coherent_walk>1 ||
        c->stall_on_fault>1 || c->continue_after_fault>1 || c->polled_faults>1 ||
        c->ignore_top_byte>1 || c->shared_asid>1 ||
        !region_valid(c->lower,c->lower_tables,0) || !region_valid(c->upper,c->upper_tables,1))return ArmSmmu4KArgument;
    unsigned va=width_bits((c->id2>>8)&15),pa=width_bits((c->id2>>4)&15);
    if(!(c->id0&(UINT32_C(1)<<30)) || !(c->id2&(1u<<12)) || !va || !pa)return ArmSmmu4KUnsupported;
    const ArmSmmu4KConfig *basis=c->lower?c->lower:c->upper;
    /* Pong admits only untagged low32-bit addresses. Ignoring bits63:56
     * cannot change any admitted address; do not extend this policy to an
     * unqualified upper or wider address range. */
    if(c->ignore_top_byte && (c->upper || !c->lower || c->lower->input_bits!=32))
        return ArmSmmu4KUnsupported;
    if(basis->output_bits>pa || (c->lower && c->lower->input_bits>va) ||
        (c->upper && (c->upper->input_bits>va || c->upper->output_bits!=basis->output_bits)))return ArmSmmu4KUnsupported;
    if(c->lower && c->upper) {
        const ArmSmmu4KResult *a=c->lower_tables,*b=c->upper_tables;
        if(a->root_physical<b->root_physical+(uint64_t)b->table_pages*4096 &&
           b->root_physical<a->root_physical+(uint64_t)a->table_pages*4096)return ArmSmmu4KOverlap;
    }
    ArmSmmu4KContextImage v;
    v.cbar=0x1f300; /* Stage1 translation, stage2 bypass; weakest bypass attrs. */
    v.cba2r=1;      /* AArch64. No stage2 VMID or guest translation. */
    v.tcr=UINT32_C(2)<<30; /* TG1=4K, TG0=4K. */
    unsigned walk=c->coherent_walk?0x3500:0x2000;
    if(c->lower)v.tcr|=walk|(64-c->lower->input_bits);
    else v.tcr|=1u<<7; /* EPD0 */
    if(c->upper)v.tcr|=(walk|(64-c->upper->input_bits))<<16;
    else v.tcr|=1u<<23; /* EPD1 */
    v.tcr2=(7u<<15)|(1u<<4)|(unsigned)width_code(basis->output_bits);
    if(c->ignore_top_byte)v.tcr2|=0x60;
    v.ttbr0=(uint64_t)c->asid<<48;v.ttbr1=v.ttbr0;
    if(c->lower)v.ttbr0|=c->lower_tables->root_physical;
    if(c->upper)v.ttbr1|=c->upper_tables->root_physical;
    v.mair0=0xff44;v.mair1=0;
    v.sctlr=0x1067|(c->stall_on_fault<<7)|(c->continue_after_fault<<8);
    if(c->polled_faults)v.sctlr&=~UINT32_C(0x40); /* CFRE stays on; CFIE off. */
    if(c->shared_asid)v.sctlr&=~UINT32_C(0x1000);
    v.reserved_zero=0;
    *out=v;
    return ArmSmmu4KSuccess;
}
ArmSmmu4KStatus ArmSmmu4KValidateContextReadback(const ArmSmmu4KContextConfig *c,
    const ArmSmmu4KContextImage *observed)
{
    if(!observed)return ArmSmmu4KArgument;
    ArmSmmu4KContextImage expected;
    ArmSmmu4KStatus status=ArmSmmu4KEncodeContext(c,&expected);
    if(status!=ArmSmmu4KSuccess)return status;
    /* Compare the explicitly requested policy exactly, including TBI and
     * ASIDPNE. No broad mask or automatic acceptance of a different root,
     * granule, stage, address width, cache mode or fault policy.
     */
    if(expected.cbar!=observed->cbar || expected.cba2r!=observed->cba2r ||
        expected.tcr!=observed->tcr || expected.tcr2!=observed->tcr2 ||
        expected.mair0!=observed->mair0 || expected.mair1!=observed->mair1 ||
        expected.sctlr!=observed->sctlr || observed->reserved_zero || expected.ttbr0!=observed->ttbr0 ||
        expected.ttbr1!=observed->ttbr1)return ArmSmmu4KStateMismatch;
    return ArmSmmu4KSuccess;
}
