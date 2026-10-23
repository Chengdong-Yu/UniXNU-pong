/* SPDX-License-Identifier: MIT */
/* Actual 20G81 Preferences failure: MobileIcons' Lanczos helper creates a
 * CoreImage EAGL context, whose missing GL dispatch jumps to address zero.
 * Replace that private image scaler in Preferences only with real CPU
 * CoreGraphics rasterization. This is not a GPU/OpenGL implementation.
 */
#include "icons_cpu.h"
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern const void *_dyld_get_shared_cache_range(size_t *);
static void (*diagnostic)(const char *);
static bool installed;

/* The original helper takes CGImageRef in x0 and the uniform pixel scale in
 * d0, returning a retained CGImageRef. Both were resolved from this actual
 * 20G81 helper and its callers, not the HackiOS 16.5 cache.
 */
static CGImageRef scale_icon(CGImageRef input, double scale)
{
    if (!input || !isfinite(scale) || scale<=0) return NULL;
    size_t iw=CGImageGetWidth(input),ih=CGImageGetHeight(input);
    double dw=(double)iw*scale,dh=(double)ih*scale;
    if (!iw || !ih || !isfinite(dw) || !isfinite(dh) ||
        dw<0.5 || dh<0.5 || dw>4096 || dh>4096) return NULL;
    size_t width=(size_t)floor(dw+0.5),height=(size_t)floor(dh+0.5);
    if (width==iw && height==ih) return CGImageRetain(input);
    CGColorSpaceRef rgb=CGColorSpaceCreateDeviceRGB();
    if (!rgb) return NULL;
    CGContextRef bitmap=CGBitmapContextCreate(NULL,width,height,8,0,rgb,
        kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst);
    CGColorSpaceRelease(rgb);
    if (!bitmap) return NULL;
    CGContextSetInterpolationQuality(bitmap,kCGInterpolationHigh);
    CGContextSetBlendMode(bitmap,kCGBlendModeCopy);
    CGContextDrawImage(bitmap,CGRectMake(0,0,(CGFloat)width,(CGFloat)height),input);
    CGImageRef image=CGBitmapContextCreateImage(bitmap);
    CGContextRelease(bitmap);
    static unsigned calls;
    if (__atomic_fetch_add(&calls,1,__ATOMIC_RELAXED)<4 && diagnostic) {
        char line[160];
        snprintf(line,sizeof(line),"CPU icon scale %zux%zu -> %zux%zu result=%s",
            iw,ih,width,height,image?"image":"allocation failed");
        diagnostic(line);
    }
    return image;
}

static void install(void)
{
    if (installed) return;
    size_t length=0;
    uintptr_t base=(uintptr_t)_dyld_get_shared_cache_range(&length);
    const uintptr_t offset=0x1d2aabec8ULL-0x180000000ULL;
    if (!base || offset>length || length-offset<16 || base>UINTPTR_MAX-offset) return;
    void *entry=(void *)(base+offset);
    Dl_info image;
    if (!dladdr(entry,&image) || !image.dli_fname ||
        !strstr(image.dli_fname,"/MobileIcons.framework/MobileIcons")) return;
    /* Only this known 20G81 entry ABI is supported. Refuse other instruction
     * layouts instead of applying 16.5/other-version private addresses.
     * This is a local ABI guard, not an image hash or a disk image audit.
     */
    static const uint32_t original[]={0xd10283ff,0x6d0323e9,0xa9046ffc,0xa90567fa};
    if (memcmp(entry,original,sizeof(original))) {
        if (diagnostic) diagnostic("CPU icon scaler: 20G81 entry ABI unavailable; SKIP");
        return;
    }
    vm_address_t page=(vm_address_t)entry&~((vm_address_t)vm_page_size-1);
    if (((uintptr_t)entry-page)>vm_page_size-16) return;
    kern_return_t kr=vm_protect(mach_task_self(),page,vm_page_size,FALSE,
        VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
    if (kr!=KERN_SUCCESS) {
        if (diagnostic) diagnostic("CPU icon scaler: private text mapping unavailable; SKIP");
        return;
    }
    /* Preserve x0/d0 and caller LR: ldr x16, literal; br x16; address. No
     * trampoline executes or relocates the displaced original prologue.
     */
    struct {uint32_t load,branch;uintptr_t address;} jump={0x58000050,0xd61f0200,(uintptr_t)scale_icon};
    _Static_assert(sizeof(jump)==16,"AArch64 absolute tail branch");
    memcpy(entry,&jump,sizeof(jump));
    sys_icache_invalidate(entry,sizeof(jump));
    kr=vm_protect(mach_task_self(),page,vm_page_size,FALSE,VM_PROT_READ|VM_PROT_EXECUTE);
    if (kr!=KERN_SUCCESS) {
        memcpy(entry,original,sizeof(original));
        sys_icache_invalidate(entry,sizeof(original));
        (void)vm_protect(mach_task_self(),page,vm_page_size,FALSE,VM_PROT_READ|VM_PROT_EXECUTE);
        if (diagnostic) diagnostic("CPU icon scaler: execute protection restore failed; original restored");
        return;
    }
    installed=true;
    if (diagnostic) diagnostic("20G81 MobileIcons CPU scaler installed; no EAGL/CoreImage context");
}

static void image_added(const struct mach_header *header,intptr_t slide)
{
    (void)header;(void)slide;
    install();
}

void pong_settings_icons_start(void (*record)(const char *))
{
    diagnostic=record;
    /* Called during Preferences' injected-library initialization, before
     * UIApplication starts its icon workers. Future image loads retry while
     * still under dyld's image-load serialization.
     */
    _dyld_register_func_for_add_image(image_added);
}
