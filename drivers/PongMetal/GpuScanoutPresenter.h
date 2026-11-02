// SPDX-License-Identifier: MIT
// GPU presentation: the GPU copies a finished BGRA frame (the compositor's IOSurface pages) into the firmware scanout memory. Both are
// imported without a copy (newBufferWithBytesNoCopy: pinned by the kernel and mapped into the GPU's 4 KiB context), the copy is a blit on
// the GPU's queue, and the call returns after the GPU retired it. No CPU touches a pixel; there is no staging copy and no software fallback
// here. Used through the public Metal API, on any device (the native frontend on the tablet, the Mac's device for the host check).
#pragma once
#import <Metal/Metal.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
extern "C" const void *nmb_scanout_convert_air(size_t *);
extern "C" id native_bridge_metal_library_with_air(const void *,size_t);

namespace gpupresent {

struct Scanout {
    void *base;       // writable mapping of the firmware framebuffer (page aligned)
    size_t bytes;     // whole extent, a multiple of the page size
    size_t stride;    // bytes per row
    uint32_t width, height;
};

struct Frame {
    const void *base; // page-aligned mapping of the surface pages
    size_t bytes;     // allocation size, a multiple of the page size
    size_t stride;
    uint32_t width, height;
    uint32_t source_id=0; // kernel generation, not a recyclable virtual address
};

enum Result { kOk = 0, kBadFrame, kBadScanout, kNoResource, kGpuFailed };

class Presenter {
public:
    Presenter(id<MTLDevice> device, const Scanout &scanout) : device_(device), scanout_(scanout) {
        queue_ = [device newCommandQueue];
        if (!validScanout() || !queue_) return;
        target_ = [device newBufferWithBytesNoCopy:scanout.base length:scanout.bytes options:MTLResourceStorageModeShared deallocator:nil];
    }
    bool ready() const { return target_ != nil; }
    // Called only after a frame was refused before any command submission.
    void resetSources() {for(auto &source:sources_)source={};}
    // One frame: encodes the GPU copy, commits, waits for the GPU. Rows are copied as whole rows of width*4 bytes.
    Result present(const Frame &frame) {
        if (!ready()) return kBadScanout;
        const size_t page = (size_t)getpagesize(), row = (size_t)frame.width * 4;
        if (!frame.base || ((uintptr_t)frame.base & (page - 1)) || !frame.bytes || (frame.bytes & (page - 1)) ||
            frame.width != scanout_.width || frame.height != scanout_.height || frame.stride < row ||
            (size_t)frame.height > frame.bytes / frame.stride || frame.stride * (frame.height - 1) + row > frame.bytes) return kBadFrame;
        id<MTLBuffer> source=nil;
        Source *slot=nullptr;
        if(frame.source_id)for(auto &entry:sources_) {
            if(entry.key==frame.source_id && entry.base==frame.base && entry.bytes==frame.bytes) {
                source=entry.buffer;entry.used=++use_;slot=&entry;break;
            }
            if(!slot || !entry.key || (slot->key && entry.used<slot->used))slot=&entry;
        }
        if(!source) {
            source=[device_ newBufferWithBytesNoCopy:(void *)frame.base length:frame.bytes options:MTLResourceStorageModeShared deallocator:nil];
            if(source && slot)*slot={frame.source_id,frame.base,frame.bytes,++use_,source,nil,0};
        }
        if (!source) return kNoResource;
        // Upstream 278333f: the panel blends the scanout alpha, whereas the
        // compositor's framebuffer alpha is not display opacity. One GPU
        // dispatch copies pitched BGRA rows and supplies opaque alpha. Keep
        // the existing blit if this optional pipeline cannot be made.
        if (!(frame.stride&3) && !(scanout_.stride&3) && opaquePipeline()) {
            id<MTLCommandBuffer> commands=[queue_ commandBuffer];
            id<MTLComputeCommandEncoder> encoder=commands?[commands computeCommandEncoder]:nil;
            if(!encoder)return kNoResource;
            const uint32_t params[4]={frame.width,frame.height,(uint32_t)(frame.stride/4),(uint32_t)(scanout_.stride/4)};
            [encoder setComputePipelineState:opaque_];
            [encoder setBuffer:source offset:0 atIndex:0];
            [encoder setBuffer:target_ offset:0 atIndex:1];
            [encoder setBytes:params length:sizeof params atIndex:3];
            [encoder dispatchThreadgroups:MTLSizeMake((frame.width+15)/16,(frame.height+7)/8,1)
                   threadsPerThreadgroup:MTLSizeMake(16,8,1)];
            [encoder endEncoding];[commands commit];[commands waitUntilCompleted];
            return commands.status==MTLCommandBufferStatusCompleted?kOk:kGpuFailed;
        }
        id<MTLCommandBuffer> commands = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> blit = commands ? [commands blitCommandEncoder] : nil;
        if (!blit) return kNoResource;
        if (frame.stride == scanout_.stride) {
            // Equal pitch: the rows are one contiguous run.
            [blit copyFromBuffer:source sourceOffset:0 toBuffer:target_ destinationOffset:0 size:frame.stride * (frame.height - 1) + row];
        } else {
            // A linear texture over the surface pages reads them with the surface's pitch and writes the scanout's pitch.
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:frame.width
                                                                                       height:frame.height mipmapped:NO];
            d.storageMode = MTLStorageModeShared;
            d.usage = MTLTextureUsageShaderRead;
            if (frame.stride % [device_ minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm]) {
                for (uint32_t y = 0; y < frame.height; ++y)
                    [blit copyFromBuffer:source sourceOffset:y * frame.stride toBuffer:target_ destinationOffset:y * scanout_.stride size:row];
            } else {
                // The compositor reuses a small surface pool. Keep the linear
                // image/view with its pinned buffer, instead of constructing
                // and destroying Vulkan image resources on every refresh.
                id<MTLTexture> texture = slot && slot->stride==frame.stride ? slot->texture : nil;
                if(!texture) {
                    texture=[source newTextureWithDescriptor:d offset:0 bytesPerRow:frame.stride];
                    if(slot && texture){slot->texture=texture;slot->stride=frame.stride;}
                }
                if (!texture) {[blit endEncoding]; return kNoResource;}
                [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(frame.width, frame.height, 1) toBuffer:target_ destinationOffset:0
                    destinationBytesPerRow:scanout_.stride destinationBytesPerImage:scanout_.stride * frame.height];
            }
        }
        [blit endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        return commands.status == MTLCommandBufferStatusCompleted ? kOk : kGpuFailed;
    }
private:
    bool opaquePipeline() {
        if(opaque_ || opaqueTried_)return opaque_!=nil;
        opaqueTried_=true;
        size_t length=0;const void *air=nmb_scanout_convert_air(&length);
        id<MTLLibrary> lib=air&&length?native_bridge_metal_library_with_air(air,length):nil;
        id<MTLFunction> function=[lib newFunctionWithName:@"scanout_opaque"];
        NSError *error=nil;
        opaque_=function?[device_ newComputePipelineStateWithFunction:function error:&error]:nil;
        if(opaque_ && opaque_.maxTotalThreadsPerThreadgroup<128)opaque_=nil;
        return opaque_!=nil;
    }
    struct Source {uint32_t key=0;const void *base=nullptr;size_t bytes=0;uint64_t used=0;id<MTLBuffer> buffer=nil;id<MTLTexture> texture=nil;size_t stride=0;};
    // Bound imported scanout BOs to the compositor's triple buffer.
    // Every retained import participates in the non-coherent DMA handoff.
    Source sources_[3];uint64_t use_=0;
    bool validScanout() const {
        const size_t page = (size_t)getpagesize();
        return scanout_.base && !((uintptr_t)scanout_.base & (page - 1)) && scanout_.bytes && !(scanout_.bytes & (page - 1)) &&
               scanout_.width && scanout_.height && scanout_.stride >= (size_t)scanout_.width * 4 &&
               scanout_.stride * scanout_.height <= scanout_.bytes;
    }
    id<MTLDevice> device_;
    Scanout scanout_;
    id<MTLCommandQueue> queue_;
    id<MTLBuffer> target_;
    id<MTLComputePipelineState> opaque_=nil;
    bool opaqueTried_=false;
};

}  // namespace gpupresent
