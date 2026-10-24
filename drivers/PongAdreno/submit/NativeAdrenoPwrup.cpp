/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoPwrup.h"
#include <IOKit/IOLib.h>
#include <string.h>
enum : size_t { HeaderBytes=16, PairBytes=8, TripletBytes=12 };
size_t NativeAdrenoPwrupList::requiredBytes(const NativeAdrenoPwrupCounts &counts)
{
    return HeaderBytes+size_t(counts.ifpc+counts.preemption)*PairBytes+
        size_t(counts.dynamic)*TripletBytes;
}
IOReturn NativeAdrenoPwrupList::pack(const NativeAdrenoPwrupCounts &counts,
    const uint32_t *ifpc_regs,const uint32_t *ifpc_values,
    const uint32_t *preemption_regs,const uint32_t *preemption_values,
    const uint32_t *dynamic_triplets,uint8_t *out,size_t capacity,size_t *written)
{
    if(!out || !written)return kIOReturnBadArgument;
    *written=0;
    if(counts.ifpc>0xff || counts.preemption>0xff || counts.dynamic>0xffff)
        return kIOReturnBadArgument;
    const size_t needed=requiredBytes(counts);
    if(capacity<needed)return kIOReturnNoResources;
    if((counts.ifpc && (!ifpc_regs || !ifpc_values)) ||
        (counts.preemption && (!preemption_regs || !preemption_values)) ||
        (counts.dynamic && !dynamic_triplets))return kIOReturnBadArgument;
    memset(out,0,needed);
    /* gpu_req/cpu_req/turn stay zero; unions carry the list lengths. */
    out[12]=static_cast<uint8_t>(counts.ifpc);
    out[13]=static_cast<uint8_t>(counts.preemption);
    out[14]=static_cast<uint8_t>(counts.dynamic&0xff);
    out[15]=static_cast<uint8_t>((counts.dynamic>>8)&0xff);
    uint32_t *cursor=reinterpret_cast<uint32_t *>(out+HeaderBytes);
    for(uint32_t i=0;i<counts.ifpc;++i) {
        *cursor++=ifpc_regs[i];*cursor++=ifpc_values[i];
    }
    for(uint32_t i=0;i<counts.preemption;++i) {
        *cursor++=preemption_regs[i];*cursor++=preemption_values[i];
    }
    for(uint32_t i=0;i<counts.dynamic;++i) {
        *cursor++=dynamic_triplets[i*3];
        *cursor++=dynamic_triplets[i*3+1];
        *cursor++=dynamic_triplets[i*3+2];
    }
    *written=needed;
    return kIOReturnSuccess;
}
