/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Wire/register facts from Nothing SM8475 adreno_gen7_rpmh.c and the live
 * Pong DT. Command-db parsing structure follows UniXNU-Sheng, with Pong's
 * real resources and table layout. No A740 voltage or bandwidth policy.
 */
#include "PongAdrenoPower.h"
#include "PongAdrenoPowerProfile.h"
static uint16_t le16(const uint8_t *p)
{return (uint16_t)((uint16_t)p[0]|(uint16_t)p[1]<<8);}
static uint32_t le32(const uint8_t *p)
{return (uint32_t)le16(p)|(uint32_t)le16(p+2)<<16;}
static int extent(size_t at,size_t n,size_t bytes)
{return at<=bytes && n<=bytes-at;}
static int name_is(const uint8_t *p,const char *name)
{
    unsigned end=0;
    for (unsigned i=0;i<8;i++) {
        unsigned char c=end?0:(unsigned char)name[i];
        if (!c) end=1;
        if (p[i]!=c) return 0;
    }
    return 1;
}
PongPowerStatus PongAdrenoReadPowerInputs(const uint8_t *db,size_t bytes,
    uint32_t chip,PongPowerInputs *out)
{
    if (!db || !out || chip!=0x07030002 || bytes!=0x20000) return PongPowerArgument;
    if (!le32(db) || le32(db+4)!=0x0c0330db) return PongPowerCorrupt;
    size_t headers[8],sizes[8],aux[8];unsigned rows=0;
    for (unsigned i=0;i<8;i++) {
        const uint8_t *r=db+8+16*i;
        if (!le16(r)) break;
        headers[i]=144u+le16(r+2);sizes[i]=(size_t)le16(r+6)*24;
        aux[i]=144u+le16(r+4);
        if (!extent(headers[i],sizes[i],bytes) || !extent(aux[i],0,bytes)) return PongPowerCorrupt;
        ++rows;
    }
    const char *names[]={"gfx.lvl","mx.lvl","cx.lvl","SH0","MC0","ACV","CN0"};
    PongPowerResource resources[7]={{0}};unsigned seen=0;
    for (unsigned i=0;i<rows;i++) for (size_t at=headers[i];at<headers[i]+sizes[i];at+=24) {
        const uint8_t *entry=db+at;
        for (unsigned n=0;n<7;n++) if (name_is(entry,names[n])) {
            if (seen&(1u<<n)) return PongPowerDuplicate;
            uint32_t address=le32(entry+16);
            size_t len=le16(entry+20),start=aux[i]+le16(entry+22);
            unsigned slave=le16(db+8+16*i);
            if (!address || (address&3) || ((address>>16)&7)!=slave ||
                slave!=(n<3?3u:5u) || !len || !extent(start,len,bytes)) return PongPowerCorrupt;
            for (unsigned j=0;j<rows;j++)
                if (start<headers[j]+sizes[j] && headers[j]<start+len) return PongPowerCorrupt;
            resources[n]=(PongPowerResource){address,db+start,len};seen|=1u<<n;
        }
    }
    if (seen!=0x7f) return PongPowerMissing;
    *out=(PongPowerInputs){chip,le32(db),resources[0],resources[1],resources[2],
        resources[3],resources[4],resources[5],resources[6]};
    return PongPowerSuccess;
}
static unsigned rail_count(const PongPowerResource *r)
{
    if (!r->aux || !r->bytes || (r->bytes&1) || r->bytes>512 ||
        !r->address || (r->address&3) || ((r->address>>16)&7)!=3 || le16(r->aux)) return 0;
    unsigned n=(unsigned)r->bytes/2,active=n;uint16_t previous=0;
    for (unsigned i=1;i<n;i++) {
        uint16_t level=le16(r->aux+2*i);
        if (!level) {if (active==n) active=i;}
        else if (active!=n || level<previous) return 0;
        previous=level;
    }
    return active>1?active:0;
}
static int index_for(const PongPowerResource *r,unsigned count,uint32_t level,unsigned *out)
{
    for (unsigned i=0;i<count;i++) if (le16(r->aux+2*i)>=level) {*out=i;return 0;}
    return 1;
}
static int vote(const PongPowerResource *primary,unsigned pn,const PongPowerResource *secondary,
    unsigned sn,uint32_t level,uint32_t *out)
{
    unsigned p,s;
    if (index_for(primary,pn,level,&p)) return 1;
    uint32_t actual=le16(primary->aux+2*p);
    /* Vendor caps a dependency at the highest supported secondary corner,
     * instead of wrapping the search into zero padding or using the requested
     * unrounded primary level. Keep that actual vendor corner policy.
     */
    if (index_for(secondary,sn,actual,&s)) s=sn-1;
    *out=(actual<<16)|(s<<8)|p;return 0;
}
static int bcm(const PongPowerResource *r,uint32_t kbps,unsigned width,unsigned fixed,
    unsigned commit,uint32_t *out)
{
    if (!r->aux || r->bytes!=8 || !r->address || (r->address&3) ||
        ((r->address>>16)&7)!=5 || r->aux[7]) return 1;
    uint32_t unit=le32(r->aux);unsigned connection=le16(r->aux+4);
    if (!fixed && (!unit || !connection || !width)) return 1;
    uint32_t word=commit?0x40000000:0;
    if (kbps) {
        uint64_t peak=fixed?8:((uint64_t)kbps*connection/width)*1000/unit;
        if (!peak) peak=1;
        if (peak>0x3fff) peak=0x3fff;
        word|=0x20000000|(uint32_t)peak;
    }
    *out=word;return 0;
}
PongPowerStatus PongAdrenoBuildPowerTables(const PongPowerInputs *in,PongPowerTables *out)
{
    if (!in || !out || in->chip_id!=0x07030002 || !in->db_version) return PongPowerArgument;
    unsigned gn=rail_count(&in->gfx),mn=rail_count(&in->mx),cn=rail_count(&in->cx);
    if (!gn || !mn || !cn) return PongPowerCorrupt;
    const PongPowerResource *all[]={&in->gfx,&in->mx,&in->cx,&in->sh0,&in->mc0,&in->acv,&in->cn0};
    for (unsigned i=0;i<7;i++) for (unsigned j=0;j<i;j++)
        if (all[i]->address==all[j]->address) return PongPowerCorrupt;
    PongPowerTables table={0};
    table.performance[0]=14;table.performance[1]=3;
    for (unsigned i=0;i<14;i++) {
        uint32_t level=i?PongAdrenoPowerLevels[13-i].level:0,v,cx;
        unsigned ci;
        if (vote(&in->gfx,gn,&in->mx,mn,level,&v) ||
            index_for(&in->cx,cn,i?PongAdrenoPowerLevels[13-i].cx_level:0,&ci)) return PongPowerRailRange;
        cx=ci;
        table.performance[2+3*i]=v;table.performance[3+3*i]=cx;
        table.performance[4+3*i]=i?PongAdrenoPowerLevels[13-i].hz/1000:0;
    }
    /* No qcom,gmu-freq-table override exists in captured Pong DT: these are
     * the matching vendor's explicit 200/500MHz LOW_SVS/SVS source policy.
     */
    const uint32_t gmu_hz[]={0,200000000,500000000},gmu_level[]={0,64,128};
    for (unsigned i=0;i<3;i++) {
        uint32_t v;
        if (vote(&in->cx,cn,&in->mx,mn,gmu_level[i],&v)) return PongPowerRailRange;
        table.performance[50+2*i]=v;table.performance[51+2*i]=gmu_hz[i]/1000;
    }
    const PongPowerResource *ddr[]={&in->sh0,&in->mc0,&in->acv};
    const unsigned widths[]={16,4,0};
    table.bandwidth[0]=12;table.bandwidth[1]=1;table.bandwidth[2]=3;
    table.bandwidth[3]=1;table.bandwidth[5]=in->cn0.address;
    for (unsigned i=0;i<2;i++)
        if (bcm(&in->cn0,i?100:0,4,0,1,&table.bandwidth[11+6*i])) return PongPowerCorrupt;
    for (unsigned j=0;j<3;j++) {
        if (!ddr[j]->aux || ddr[j]->bytes!=8) return PongPowerCorrupt;
        unsigned commit=j==2 || (ddr[j+1]->aux && ddr[j+1]->bytes==8 && ddr[j]->aux[6]!=ddr[j+1]->aux[6]);
        if (commit) table.bandwidth[4]|=1u<<j;
        table.bandwidth[23+j]=ddr[j]->address;
        for (unsigned i=0;i<12;i++)
            if (bcm(ddr[j],PongAdrenoDdrBandwidth[i],widths[j],j==2,commit,
                &table.bandwidth[31+8*i+j])) return PongPowerCorrupt;
    }
    table.default_gpu_index=2; /* DT initial-pwrlevel11 -> ascending wire index2 */
    table.default_bus_index=PongAdrenoPowerLevels[11].bus;
    *out=table;return PongPowerSuccess;
}
