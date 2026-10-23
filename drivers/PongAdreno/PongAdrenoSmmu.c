/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Lifecycle adapted from UniXNU-Sheng's ShengGpuSmmu; resources and masked
 * stream expansion are from Pong's captured Nothing SM8475 DT.
 */
#include "PongAdrenoSmmu.h"
static int mapped(const ArmSmmu4KIo *io,uint64_t physical)
{return io && physical==0x03da0000 && io->register_bytes==0x40000;}
static void initialize(PongAdrenoSmmuResult *r)
{*r=(PongAdrenoSmmuResult){0};r->must_retain_resources=1;r->cleanup=ArmSmmu4KBusy;}
static ArmSmmu4KStatus observe(const ArmSmmu4KIo *io,
    const PongAdrenoSmmuSession *s,PongAdrenoSmmuResult *r)
{
    ArmSmmu4KStatus status=ArmSmmu4KObserveStreams(io,&s->streams,&s->context,&r->streams);
    if (status) return status;
    status=ArmSmmu4KObserveAuditedContext(io,s->streams.bank,&s->context,&s->tables,&r->tables);
    if (status) return status;
    return ArmSmmu4KObserveStreams(io,&s->streams,&s->context,&r->streams);
}
ArmSmmu4KStatus PongAdrenoSmmuStart(const ArmSmmu4KIo *io,uint64_t physical,
    PongAdrenoSmmuRole role,unsigned bank,const unsigned slots[2],
    const ArmSmmu4KContextConfig *c,const ArmSmmu4KAuditConfig *tables,unsigned budget,
    PongAdrenoSmmuSession *s,PongAdrenoSmmuResult *r)
{
    if (!r) return ArmSmmu4KArgument;
    initialize(r);
    if (!mapped(io,physical) || !s || !slots || !c || !tables || !budget ||
        budget>1048576 || slots[0]>=24 || slots[1]>=24 || slots[0]==slots[1] || bank>=21 ||
        c->continue_after_fault!=1 || c->polled_faults!=io->polled_faults ||
        (role!=PongAdrenoSmmuGpu && role!=PongAdrenoSmmuGmu) ||
        (role==PongAdrenoSmmuGpu ? (bank!=0 || !c->upper) :
            (!bank || !c->lower || c->upper || c->upper_tables))) return ArmSmmu4KArgument;
    if (s->attempted || s->ready || s->poisoned) return ArmSmmu4KBusy;
    /* The 21/24 DT limits are upper bounds, never proof of actual leases.
     * Generic primitives read actual IDs again and reject smaller hardware.
     */
    ArmSmmu4KStatus status=ArmSmmu4KAuditDeclaredContext(c,tables,&r->tables);
    if (status) return status;
    uint16_t sid=role==PongAdrenoSmmuGpu?0:5;
    ArmSmmu4KStream streams[]={{slots[0],sid},{slots[1],(uint16_t)(sid|0x400)}};
    r->phase=1;
    status=ArmSmmu4KCheckFaultedStreams(io,streams,2,bank,&r->streams);
    if (status) return status;
    s->attempted=1;s->role=role;s->context=*c;s->tables=*tables;
    r->phase=2;
    status=ArmSmmu4KProgramDisabledContext(io,bank,&s->context,budget,&r->context);
    if (status) {s->poisoned=1;return status;}
    r->phase=3;
    status=ArmSmmu4KAttachStreams(io,&s->streams,streams,2,bank,&s->context,&r->streams);
    if (!status) {r->phase=4;status=observe(io,s,r);}
    if (status) {
        s->poisoned=1;
        ArmSmmu4KStreamResult check;
        if (s->streams.attached) {
            r->cleanup=ArmSmmu4KFaultStreams(io,&s->streams,&check);
            if (r->cleanup) {r->phase=5;return status;}
        }
        r->cleanup=ArmSmmu4KCheckFaultedStreams(io,streams,2,bank,&check);
        if (!r->cleanup)
            r->cleanup=ArmSmmu4KDisableOwnedContext(io,bank,&s->context,budget,&r->context);
        r->phase=5;return status;
    }
    s->ready=1;r->phase=6;return ArmSmmu4KSuccess;
}
ArmSmmu4KStatus PongAdrenoSmmuObserve(const ArmSmmu4KIo *io,uint64_t physical,
    const PongAdrenoSmmuSession *s,PongAdrenoSmmuResult *r)
{
    if (!r) return ArmSmmu4KArgument;
    initialize(r);
    if (!mapped(io,physical) || !s || !s->ready || s->poisoned) return ArmSmmu4KArgument;
    r->phase=7;return observe(io,s,r);
}
ArmSmmu4KStatus PongAdrenoSmmuStop(const ArmSmmu4KIo *io,uint64_t physical,
    unsigned budget,PongAdrenoSmmuSession *s,PongAdrenoSmmuResult *r)
{
    if (!r) return ArmSmmu4KArgument;
    initialize(r);
    if (!mapped(io,physical) || !s || !s->ready || s->poisoned || !budget || budget>1048576)
        return ArmSmmu4KArgument;
    r->phase=8;
    ArmSmmu4KStatus status=ArmSmmu4KFaultStreams(io,&s->streams,&r->streams);
    if (!status) {r->phase=9;status=ArmSmmu4KDisableOwnedContext(io,s->streams.bank,
        &s->context,budget,&r->context);}
    if (status) s->poisoned=1;
    s->ready=0;r->phase=status?10:11;return status;
}
