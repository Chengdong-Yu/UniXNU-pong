/* SPDX-License-Identifier: MIT */
/* Port of parity11420f4/805659b to the supplied arm64 22H374 cache.
 * 27's backdrop-compute option and offsets do not exist on this guest. */
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
static void pongQuartzCore18DisplayPolicy()
{
#if PONG_GUEST_22H374
    if(strcmp(getprogname(),"backboardd"))return;
    setenv("CA_ALLOW_EDR_MIRRORING","0",1);
    setenv("CA_FORCE_FP16_FRAMEBUFFER","0",1);
    setenv("CA_DISPLAY_EDR_HEADROOM","1",1);
    const uint8_t uuid[16]={0xfa,0xf7,0xd7,0xea,0x18,0xf0,0x3c,0x2d,
                           0x81,0x6c,0xac,0x56,0xd1,0x6c,0x40,0xa3};
    for(uint32_t i=0;i<_dyld_image_count();++i) {
        const char *path=_dyld_get_image_name(i);
        if(!path || !strstr(path,"/QuartzCore.framework/QuartzCore"))continue;
        const mach_header_64 *h=(const mach_header_64 *)_dyld_get_image_header(i);
        if(!h || h->magic!=MH_MAGIC_64 || h->sizeofcmds>1024*1024)return;
        const uint8_t *p=(const uint8_t *)(h+1),*end=p+h->sizeofcmds;
        bool matches=false;uintptr_t dataStart=0,dataEnd=0;
        const intptr_t slide=_dyld_get_image_vmaddr_slide(i);
        for(uint32_t n=0;n<h->ncmds && p+sizeof(load_command)<=end;++n) {
            const load_command *c=(const load_command *)p;
            if(c->cmdsize<sizeof(*c) || (size_t)(end-p)<c->cmdsize)return;
            if(c->cmd==LC_UUID && c->cmdsize>=sizeof(uuid_command))
                matches=!memcmp(((const uuid_command *)c)->uuid,uuid,sizeof(uuid));
            if(c->cmd==LC_SEGMENT_64 && c->cmdsize>=sizeof(segment_command_64)) {
                const segment_command_64 *s=(const segment_command_64 *)c;
                if((!strcmp(s->segname,"__DATA") || !strcmp(s->segname,"__DATA_DIRTY")) &&
                   (s->initprot&VM_PROT_WRITE) &&
                   UINT64_C(0x1f29cc5d0)>=s->vmaddr &&
                   UINT64_C(0x1f29cc6d8)+147<=s->vmaddr+s->vmsize) {
                    dataStart=s->vmaddr+slide;dataEnd=dataStart+s->vmsize;
                }
            }
            p+=c->cmdsize;
        }
        const uintptr_t flags=UINT64_C(0x1f29cc6d8)+slide;
        const uintptr_t headroom=UINT64_C(0x1f29cc5d0)+slide;
        if(!matches || headroom<dataStart || flags+147>dataEnd)return;
        // init_debug's actual option table: 121=force FP16, 146=EDR mirror.
        // Update RW option storage only; no code patch or VM permission change.
        *(volatile uint8_t *)(flags+121)=0;
        *(volatile uint8_t *)(flags+146)=0;
        *(volatile float *)headroom=1.0f;
        report("22H374 8-bit compositor options installed",0);
        return;
    }
#endif
}
