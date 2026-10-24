/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Cape clock and GDSC hardware facts from Nothing's gpucc-waipio.c,
 * gcc-waipio.c, gdsc-regulator.c and Pong's live DT. No A642L/A740 offsets.
 */
#include "PongAdrenoClocks.h"
static int rd(const PongClockOps *io,PongClockWindow window,uint32_t at,uint32_t *value)
{
    size_t bytes=window==PongClockGpucc?0xa000:0x1f4200;
    if ((window!=PongClockGpucc && window!=PongClockGcc) || (at&3) || at>bytes-4) return 1;
    return io->read32(io->cookie,window,at,value);
}
static int update(const PongClockOps *io,PongClockWindow window,uint32_t at,
    uint32_t mask,uint32_t value,PongClockResult *r)
{
    uint32_t current;
    if (rd(io,window,at,&current)) return 1;
    uint32_t wanted=(current&~mask)|(value&mask);
    if (wanted==current) return 0;
    r->window=window;r->offset=at;r->value=wanted;++r->writes;
    return io->write32(io->cookie,window,at,wanted) || io->barrier(io->cookie);
}
static PongClockStatus poll(const PongClockOps *io,PongClockWindow window,uint32_t at,
    uint32_t mask,uint32_t wanted,unsigned max_us,PongClockResult *r)
{
    uint64_t begin,last,now;
    if (io->now_us(io->cookie,&begin)) return PongClockIo;
    last=begin;
    for (unsigned i=0;i<max_us;i++) {
        if (io->now_us(io->cookie,&now) || now<last) return PongClockIo;
        last=now;
        if (now-begin>=max_us) return PongClockTimeout;
        uint32_t value;
        if (rd(io,window,at,&value)) return PongClockIo;
        r->window=window;r->offset=at;r->value=value;++r->polls;
        if ((value&mask)==wanted) return PongClockSuccess;
        if (io->wait_us(io->cookie,1)) return PongClockIo;
    }
    return PongClockTimeout;
}
static PongClockStatus branch(const PongClockOps *io,PongClockWindow window,
    uint32_t at,unsigned delayed,PongClockResult *r)
{
    if (update(io,window,at,1,1,r)) return PongClockIo;
    if (delayed) return io->wait_us(io->cookie,10)?PongClockIo:PongClockSuccess;
    /* Exact vendor branch2 semantics: CLK_OFF clear or NOC FSM ON. */
    uint64_t begin,last,now;
    if (io->now_us(io->cookie,&begin)) return PongClockIo;
    last=begin;
    for (unsigned i=0;i<200;i++) {
        if (io->now_us(io->cookie,&now) || now<last) return PongClockIo;
        last=now;if (now-begin>=200) return PongClockTimeout;
        uint32_t value;
        if (rd(io,window,at,&value)) return PongClockIo;
        r->window=window;r->offset=at;r->value=value;++r->polls;
        /* Vendor clk_branch_wait skips halt polling in HWCG mode. */
        if(window==PongClockGcc && at==0x81004 && (value&3)==3) return PongClockSuccess;
        if (!(value&0x80000000) || (value&0xf0000000)==0x20000000) return PongClockSuccess;
        if (io->wait_us(io->cookie,1)) return PongClockIo;
    }
    return PongClockTimeout;
}
static PongClockStatus source(const PongClockOps *io,uint32_t cmd,uint32_t cfg,PongClockResult *r)
{
    uint32_t value;
    if (rd(io,PongClockGpucc,cmd,&value)) return PongClockIo;
    if (value&1) return PongClockBusy;
    /* Force the existing root on while changing only its SRC/DIV/MODE/HW
     * fields, update, then release the temporary force after active branches
     * are enabled by the caller below. All masters remain stopped.
     */
    if (update(io,PongClockGpucc,cmd,2,2,r)) return PongClockIo;
    PongClockStatus s=poll(io,PongClockGpucc,cmd,0x80000000,0,500,r);
    if (s) return s;
    if (update(io,PongClockGpucc,cmd+4,0x10371f,cfg,r) ||
        update(io,PongClockGpucc,cmd,1,1,r)) return PongClockIo;
    return poll(io,PongClockGpucc,cmd,1,0,500,r);
}
/* gpu_cc_cape_fixup + clk_lucid_evo_pll_configure, with the LUCID_OLE
 * register layout used by the actual qcom,cape-gpucc DT node. GPLL0 can
 * clock the GMU while both GPU PLLs are unconfigured; a working HFI then
 * says nothing about the CP's GX clock. Initialize the PLLs before handing
 * rate control to the GMU. As in the vendor driver, preserve a running PLL.
 */
static PongClockStatus cape_pll(const PongClockOps *io,uint32_t base,
    uint32_t l,uint32_t alpha,PongClockResult *r)
{
    uint32_t mode,opmode;
    if(rd(io,PongClockGpucc,base,&mode) || rd(io,PongClockGpucc,base+4,&opmode))return PongClockIo;
    if((mode&1) && (opmode&1))return PongClockSuccess;
    const uint32_t offsets[]={0x10,0x14,0x20,0x24,0x28,0x1c,0x30,0x34,0x38};
    const uint32_t values[]={0x44440000u|l,alpha,0x20485699,0x00182261,0x82aa299c,5,3,0x9000,0x34};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++)
        if(update(io,PongClockGpucc,base+offsets[i],~0u,values[i],r))return PongClockIo;
    /* Vendor skips zero USER_CTL/TEST_CTL values; retain their reset and
     * firmware-owned bits. Leave output disabled in STANDBY with RESET_N
     * asserted. GMU firmware owns enabling/rate changes after HFI START.
     */
    if(update(io,PongClockGpucc,base,1,0,r) ||
       update(io,PongClockGpucc,base+4,~0u,0,r) ||
       update(io,PongClockGpucc,base,4,4,r))return PongClockIo;
    return PongClockSuccess;
}
PongClockStatus PongAdrenoEnableCxClocks(const PongClockOps *io,
    PongClockState *state,PongClockResult *r)
{
    if (!r) return PongClockArgument;
    *r=(PongClockResult){0};r->resources_must_retain=1;
    if (!io || !state || !io->read32 || !io->write32 || !io->barrier ||
        !io->now_us || !io->wait_us || !io->prerequisites) return PongClockArgument;
    if (state->poisoned) return PongClockPoisoned;
    if (state->attempted || state->ready) return PongClockBusy;
    if (io->prerequisites(io->cookie)) return PongClockPrerequisite;
    uint32_t cx;
    if (rd(io,PongClockGpucc,0x9108,&cx)) return PongClockIo;
    if (cx&2) return PongClockBusy; /* existing hardware power owner */
    state->attempted=1;r->phase=1;
    PongClockStatus status=PongClockIo;
    /* DT CLK_DIS_WAIT=8, retain-regs, software controlled CX. The actual
     * power status belongs to separate HW_CTRL0x953c, not GDSCR bit31.
     */
    if (update(io,PongClockGpucc,0x9108,0xf007,0x8000,r) ||
        io->wait_us(io->cookie,1)) goto failed;
    status=poll(io,PongClockGpucc,0x953c,0x80000000,0x80000000,1000,r);
    if (status) goto failed;
    status=PongClockIo;
    if (update(io,PongClockGpucc,0x9108,0x800,0x800,r) ||
        io->wait_us(io->cookie,2)) goto failed;
    r->phase=2;
    if (update(io,PongClockGcc,0x62000,(1u<<15)|(1u<<16),(1u<<15)|(1u<<16),r) ||
        io->wait_us(io->cookie,10)) goto failed;
    status=branch(io,PongClockGcc,0x81004,0,r);if (status) goto failed;
    status=branch(io,PongClockGcc,0x81010,0,r);if (status) goto failed;
    status=branch(io,PongClockGcc,0x81018,1,r);if (status) goto failed;
    r->phase=3;
    status=cape_pll(io,0,0x1d,0xb000,r);if(status)goto failed;
    status=cape_pll(io,0x1000,0x34,0x1555,r);if(status)goto failed;
    /* Final Cape GPUCC probe write, before any GMU/GX rate requests. */
    if(update(io,PongClockGpucc,0x9534,~0u,0,r)){status=PongClockIo;goto failed;}
    status=source(io,0x9318,0x100602,r);if (status) goto failed; /*300MHz/1.5*/
    status=source(io,0x93ec,0x100603,r);if (status) goto failed; /*300MHz/2*/
    status=source(io,0x9010,0x100001,r);if (status) goto failed; /*TCXO/1*/
    r->phase=4;
    const uint32_t branches[]={0x9004,0x9144,0x93e8,0x913c,0x9148,0x911c,0x7000};
    for (unsigned i=0;i<sizeof(branches)/sizeof(branches[0]);i++) {
        status=branch(io,PongClockGpucc,branches[i],branches[i]==0x911c,r);
        if (status) goto failed;
    }
    r->phase=5;status=PongClockIo;
    const uint32_t roots[]={0x9318,0x93ec,0x9010};
    for (unsigned i=0;i<3;i++) if (update(io,PongClockGpucc,roots[i],2,0,r)) goto failed;
    if (io->prerequisites(io->cookie)) {status=PongClockPrerequisite;goto failed;}
    state->ready=1;r->phase=6;return PongClockSuccess;
failed:
    state->poisoned=1;return status;
}
