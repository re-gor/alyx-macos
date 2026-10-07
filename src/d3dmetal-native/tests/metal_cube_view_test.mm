#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <unistd.h>
#include "dmn_share.h"

static bool cube_view(id<MTLDevice> device, NSUInteger count, bool second) {
    MTLTextureDescriptor* desc = [[MTLTextureDescriptor alloc] init];
    desc.textureType = count == 1 ? MTLTextureTypeCube : MTLTextureTypeCubeArray;
    desc.pixelFormat = MTLPixelFormatRGBA8Unorm;
    desc.width = desc.height = 16;
    desc.arrayLength = count;
    desc.mipmapLevelCount = 1;
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
    [desc release];
    if (!texture) return false;
    MTLTextureSwizzleChannels swizzle = {
        MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
        MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha
    };
    id<MTLTexture> view = [texture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm
        textureType:second ? MTLTextureTypeCube : texture.textureType
        levels:NSMakeRange(0, 1)
        slices:NSMakeRange(second ? 6 : 0, second ? 6 : count * 6)
        swizzle:swizzle];
    bool ok = view && view.arrayLength == (second ? 1 : count);
    [view release];
    [texture release];
    return ok;
}

int main() {
    @autoreleasepool {
        dmn_share_install_swizzles();
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return 2;
        // Install the texture-class hooks via an actual shared impostor.
        MTLTextureDescriptor* desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:16 height:16 mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        dmn_share_arm_producer(0);
        id<MTLTexture> shared = [device newTextureWithDescriptor:desc];
        DmnShareArm arm{};
        bool captured = dmn_share_disarm(&arm);
        if (!shared || !captured) return 3;
        if (arm.out_fd >= 0) close(arm.out_fd);
        id<MTLTexture> shared_view = [shared newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm
            textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
        if (!shared_view) return 4;
        [shared_view release];
        bool ok = cube_view(device, 1, false) && cube_view(device, 2, false)
               && cube_view(device, 2, true);
        [shared release];
        [device release];
        if (!ok) return 5;
        puts("PASS: shared 2D view and ordinary cube/cube-array views preserve all faces");
        return 0;
    }
}
