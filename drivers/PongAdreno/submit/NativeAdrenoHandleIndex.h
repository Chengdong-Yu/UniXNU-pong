/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_HANDLE_INDEX_H
#define NATIVE_ADRENO_HANDLE_INDEX_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <IOKit/IOLib.h>
/* Handle -> slot of a fixed table whose handles are never 0: open addressing over twice the slots with linear probing, deletion by
 * backward shift (no tombstones), so a lookup is a few probes instead of a walk over every slot. i567: the submit path looked each IB's
 * handle up three times by walking a 4096-entry table, and the table had to grow to 16384 entries. */
template<size_t Slots> class NativeAdrenoHandleIndex {
    enum : size_t {Size=Slots*2,Mask=Size-1};
    static_assert(Slots && !(Size&Mask),"power of two");
    struct Cell {uint64_t key;uint32_t value;};
    Cell *cells_=nullptr;
    static size_t home(uint64_t key) {return (size_t)((key*UINT64_C(0x9e3779b97f4a7c15))>>40)&Mask;}
public:
    bool allocate()
    {
        if(!cells_)cells_=IONewData(Cell,Size);
        if(cells_)memset(cells_,0,sizeof(Cell)*Size);
        return cells_!=nullptr;
    }
    void release() {if(cells_)IODeleteData(cells_,Cell,Size);cells_=nullptr;}
    /* UINT32_MAX: not present. */
    uint32_t find(uint64_t key) const
    {
        if(!cells_ || !key)return UINT32_MAX;
        for(size_t i=home(key),n=0;n<Size;i=(i+1)&Mask,++n) {
            if(!cells_[i].key)return UINT32_MAX;
            if(cells_[i].key==key)return cells_[i].value;
        }
        return UINT32_MAX;
    }
    /* At most Slots keys are ever present, so a free cell always exists. */
    void insert(uint64_t key,uint32_t value)
    {
        if(!cells_ || !key)return;
        size_t i=home(key);
        while(cells_[i].key && cells_[i].key!=key)i=(i+1)&Mask;
        cells_[i]={key,value};
    }
    void erase(uint64_t key)
    {
        if(!cells_ || !key)return;
        size_t i=home(key);
        for(size_t n=0;;i=(i+1)&Mask,++n) {
            if(n==Size || !cells_[i].key)return;
            if(cells_[i].key==key)break;
        }
        /* Pull back each later cell of the run whose home is not cyclically inside (i, j]. */
        for(size_t j=i;;) {
            j=(j+1)&Mask;
            if(!cells_[j].key)break;
            const size_t k=home(cells_[j].key);
            if(i<=j?(i<k && k<=j):(i<k || k<=j))continue;
            cells_[i]=cells_[j];i=j;
        }
        cells_[i]={0,0};
    }
};
#endif
