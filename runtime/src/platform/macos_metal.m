#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <GameController/GameController.h>
#import <simd/simd.h>
#include "mimips1_runtime.h"

// Metal Shading Language source embedded as string
static const char* metal_shader_src = 
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"struct VertexOut {\n"
"    float4 position [[position]];\n"
"    float2 uv;\n"
"};\n"
"\n"
"vertex VertexOut vertex_main(uint vertexID [[vertex_id]], constant float4& uv_bounds [[buffer(0)]]) {\n"
"    float2 pos[4] = { float2(-1, 1), float2(-1, -1), float2(1, 1), float2(1, -1) };\n"
"    float2 uvs[4] = {\n"
"        float2(uv_bounds.x, uv_bounds.y),\n"
"        float2(uv_bounds.x, uv_bounds.w),\n"
"        float2(uv_bounds.z, uv_bounds.y),\n"
"        float2(uv_bounds.z, uv_bounds.w)\n"
"    };\n"
"    VertexOut out;\n"
"    out.position = float4(pos[vertexID], 0.0, 1.0);\n"
"    out.uv = uvs[vertexID];\n"
"    return out;\n"
"}\n"
"\n"
"struct FragParams {\n"
"    int crt;          // 1: scanlines\n"
"    int mode;         // 0 nearest, 1 bilinear, 2 sharp bilinear\n"
"    float2 scale;     // output pixels per source pixel\n"
"};\n"
"\n"
"fragment float4 fragment_main(VertexOut in [[stage_in]],\n"
"                             texture2d<float> tex [[texture(0)]],\n"
"                             sampler smp [[sampler(0)]],\n"
"                             constant FragParams& p [[buffer(0)]]) {\n"
"    float2 uv = in.uv;\n"
"    if (p.mode == 2) {\n"
"        // Sharp bilinear: nearest-neighbour inside each source pixel, bilinear only across its edges\n"
"        float2 size = float2(tex.get_width(), tex.get_height());\n"
"        float2 texel = uv * size;\n"
"        float2 s = max(p.scale, float2(1.0));\n"
"        float2 range = 0.5 - 0.5 / s;\n"
"        float2 dist = fract(texel) - 0.5;\n"
"        float2 f = (dist - clamp(dist, -range, range)) * s + 0.5;\n"
"        uv = (floor(texel) + f) / size;\n"
"    }\n"
"    float4 color = tex.sample(smp, uv);\n"
"    if (p.crt == 1) {\n"
"        float scanline = sin(in.uv.y * 480.0 * 3.14159) * 0.08;\n"
"        color.rgb -= scanline;\n"
"    }\n"
"    return color;\n"
"}\n"
"\n"
"// FXAA (Lottes, console variant): smooths the stair-steps left by upscaling polygon edges\n"
"fragment float4 fragment_fxaa(VertexOut in [[stage_in]],\n"
"                             texture2d<float> tex [[texture(0)]],\n"
"                             sampler smp [[sampler(0)]]) {\n"
"    float2 px = 1.0 / float2(tex.get_width(), tex.get_height());\n"
"    float3 rgbNW = tex.sample(smp, in.uv + float2(-1.0, -1.0) * px).rgb;\n"
"    float3 rgbNE = tex.sample(smp, in.uv + float2( 1.0, -1.0) * px).rgb;\n"
"    float3 rgbSW = tex.sample(smp, in.uv + float2(-1.0,  1.0) * px).rgb;\n"
"    float3 rgbSE = tex.sample(smp, in.uv + float2( 1.0,  1.0) * px).rgb;\n"
"    float4 center = tex.sample(smp, in.uv);\n"
"    float3 rgbM = center.rgb;\n"
"    const float3 luma = float3(0.299, 0.587, 0.114);\n"
"    float lumaNW = dot(rgbNW, luma), lumaNE = dot(rgbNE, luma);\n"
"    float lumaSW = dot(rgbSW, luma), lumaSE = dot(rgbSE, luma), lumaM = dot(rgbM, luma);\n"
"    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));\n"
"    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));\n"
"    float2 dir;\n"
"    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));\n"
"    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));\n"
"    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * (1.0 / 8.0)), 1.0 / 128.0);\n"
"    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);\n"
"    dir = clamp(dir * rcpDirMin, -8.0, 8.0) * px;\n"
"    float3 rgbA = 0.5 * (tex.sample(smp, in.uv + dir * (1.0 / 3.0 - 0.5)).rgb +\n"
"                         tex.sample(smp, in.uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
"    float3 rgbB = rgbA * 0.5 + 0.25 * (tex.sample(smp, in.uv + dir * -0.5).rgb +\n"
"                                       tex.sample(smp, in.uv + dir *  0.5).rgb);\n"
"    float lumaB = dot(rgbB, luma);\n"
"    float3 outRgb = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;\n"
"    return float4(outRgb, 1.0);\n"
"}\n";

@interface MimiPS1ViewDelegate : NSObject <MTKViewDelegate>
@property (nonatomic, assign) PS1Context* context;
@property (nonatomic, strong) id<MTLDevice> device;
@property (nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property (nonatomic, strong) id<MTLRenderPipelineState> pipelineState;
@property (nonatomic, strong) id<MTLTexture> vramTexture;
@property (nonatomic, strong) id<MTLSamplerState> samplerState;
@property (nonatomic, strong) id<MTLSamplerState> smoothSamplerState;
@property (nonatomic, strong) id<MTLRenderPipelineState> fxaaPipeline;
@property (nonatomic, strong) id<MTLTexture> fxaaTarget;
@property (nonatomic, assign) int scalingMode;        // Settings menu: 0 nearest, 1 bilinear, 2 sharp bilinear
@property (nonatomic, assign) BOOL fxaa;              // Settings menu: edge anti-aliasing pass on the scaled image
@end

@implementation MimiPS1ViewDelegate {
    uint32_t _rgba_buffer[PS1_VRAM_HEIGHT * PS1_VRAM_WIDTH];
}

- (instancetype)initWithContext:(PS1Context*)ctx device:(id<MTLDevice>)device {
    self = [super init];
    if (self) {
        _context = ctx;
        _device = device;
        _commandQueue = [device newCommandQueue];

        // Compile Metal Shaders
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:metal_shader_src]
                                                      options:nil
                                                        error:&error];
        if (!library) {
            NSLog(@"[Metal Error] Shader compile failed: %@", error);
            return nil;
        }

        MTLRenderPipelineDescriptor* pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
        pipelineDesc.vertexFunction = [library newFunctionWithName:@"vertex_main"];
        pipelineDesc.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
        pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

        _pipelineState = [device newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
        pipelineDesc.fragmentFunction = [library newFunctionWithName:@"fragment_fxaa"];
        _fxaaPipeline = [device newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
        _scalingMode = 2;

        // Create 1024x512 RGBA Texture for VRAM
        MTLTextureDescriptor* texDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:PS1_VRAM_WIDTH
                                                                                         height:PS1_VRAM_HEIGHT
                                                                                      mipmapped:NO];
        texDesc.usage = MTLTextureUsageShaderRead;
        _vramTexture = [device newTextureWithDescriptor:texDesc];

        // Create Bilinear/Nearest Sampler
        MTLSamplerDescriptor* samplerDesc = [[MTLSamplerDescriptor alloc] init];
        samplerDesc.minFilter = MTLSamplerMinMagFilterLinear;
        samplerDesc.magFilter = MTLSamplerMinMagFilterNearest;
        _samplerState = [device newSamplerStateWithDescriptor:samplerDesc];
        samplerDesc.magFilter = MTLSamplerMinMagFilterLinear;
        _smoothSamplerState = [device newSamplerStateWithDescriptor:samplerDesc];
    }
    return self;
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
}

- (void)drawInMTKView:(MTKView *)view {
    if (!_context) return;

    // Convert PS1 BGR555 VRAM to 32-bit BGRA host framebuffer (using front_vram snapshot for tear-free rendering)
    const PS1GPUFrame* frame = ps1_gpu_acquire_frame(&_context->gpu);
    static PS1GPUFrame* held = NULL;       // keeps showing the last frame if none is available
    if (frame) { if (!held) held = malloc(sizeof(PS1GPUFrame)); memcpy(held, frame, sizeof(PS1GPUFrame)); }
    ps1_gpu_release_frame(&_context->gpu);
    if (!held) return;
    frame = held;
    const uint16_t (*source_vram)[PS1_VRAM_WIDTH] = frame->vram;
    bool is24 = frame->is_24bit;
    int fx = frame->disp_x, fy = frame->disp_y;
    int fw = frame->disp_w, fh = frame->disp_h;
    if (is24) {
        // 24-bit display (FMV): three bytes per pixel straight out of VRAM, drawn at the texture origin
        for (int y = 0; y < fh && y < PS1_VRAM_HEIGHT; y++) {
            const uint8_t* row = (const uint8_t*)source_vram[(fy + y) & (PS1_VRAM_HEIGHT - 1)];
            for (int x = 0; x < fw && x < PS1_VRAM_WIDTH; x++) {
                int off = (fx * 2 + x * 3) & (PS1_VRAM_WIDTH * 2 - 1);
                uint8_t r = row[off], g = row[(off + 1) & (PS1_VRAM_WIDTH * 2 - 1)], b = row[(off + 2) & (PS1_VRAM_WIDTH * 2 - 1)];
                _rgba_buffer[y * PS1_VRAM_WIDTH + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b | 0xFF000000;
            }
        }
    } else {
        for (int y = 0; y < PS1_VRAM_HEIGHT; y++) {
            for (int x = 0; x < PS1_VRAM_WIDTH; x++) {
                uint16_t c = source_vram[y][x];
                uint8_t r = (uint8_t)(((c >> 0)  & 0x1F) << 3);
                uint8_t g = (uint8_t)(((c >> 5)  & 0x1F) << 3);
                uint8_t b = (uint8_t)(((c >> 10) & 0x1F) << 3);
                _rgba_buffer[y * PS1_VRAM_WIDTH + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b | 0xFF000000;
            }
        }
    }

    {   // MIMIPS1_PRESENT_DUMP=<n>: write the presented frame number n as /tmp/present.ppm (debug aid)
        static int dump_at = -2, frame_no = 0;
        if (dump_at == -2) { const char* e = getenv("MIMIPS1_PRESENT_DUMP"); dump_at = e ? atoi(e) : -1; }
        static int every = -2;
        if (every == -2) { const char* e = getenv("MIMIPS1_PRESENT_EVERY"); every = e ? atoi(e) : 0; }
        ++frame_no;
        if ((dump_at >= 0 && frame_no == dump_at) || (every > 0 && frame_no % every == 0 && frame_no >= dump_at)) {
            char path[64]; snprintf(path, sizeof(path), every > 0 ? "/tmp/present_%d.ppm" : "/tmp/present.ppm", frame_no);
            int w = is24 ? fw : fw, h = fh; int ox = is24 ? 0 : fx, oy = is24 ? 0 : fy;
            FILE* f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", w, h);
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                    uint32_t px = _rgba_buffer[((oy + y) & 511) * PS1_VRAM_WIDTH + ((ox + x) & 1023)];
                    fputc((px >> 16) & 255, f); fputc((px >> 8) & 255, f); fputc(px & 255, f);
                }
                fclose(f);
            }
        }
    }

    // Upload texture region
    [_vramTexture replaceRegion:MTLRegionMake2D(0, 0, PS1_VRAM_WIDTH, PS1_VRAM_HEIGHT)
                    mipmapLevel:0
                      withBytes:_rgba_buffer
                    bytesPerRow:PS1_VRAM_WIDTH * 4];

    id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    MTLRenderPassDescriptor* renderPass = view.currentRenderPassDescriptor;

    if (renderPass) {
        CGSize drawableSize = view.drawableSize;
        bool useFxaa = _fxaa && _fxaaPipeline && drawableSize.width > 0 && drawableSize.height > 0;
        MTLRenderPassDescriptor* scenePass = renderPass;
        if (useFxaa) {
            NSUInteger w = (NSUInteger)drawableSize.width, h = (NSUInteger)drawableSize.height;
            if (!_fxaaTarget || _fxaaTarget.width != w || _fxaaTarget.height != h) {
                MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                td.storageMode = MTLStorageModePrivate;
                _fxaaTarget = [_device newTextureWithDescriptor:td];
            }
            scenePass = [MTLRenderPassDescriptor renderPassDescriptor];
            scenePass.colorAttachments[0].texture = _fxaaTarget;
            scenePass.colorAttachments[0].loadAction = MTLLoadActionClear;
            scenePass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            scenePass.colorAttachments[0].storeAction = MTLStoreActionStore;
        }

        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:scenePass];
        [encoder setRenderPipelineState:_pipelineState];
        [encoder setFragmentTexture:_vramTexture atIndex:0];
        [encoder setFragmentSamplerState:(_scalingMode == 0 ? _samplerState : _smoothSamplerState) atIndex:0];

        int disp_x = frame->disp_x, disp_y = frame->disp_y;
        int disp_w = frame->disp_w > 0 ? frame->disp_w : 320;
        int disp_h = frame->disp_h > 0 ? frame->disp_h : 240;

        int crop_top = 0;
        int crop_bottom = 0;
        int16_t y1 = frame->disp_y1;
        int16_t y2 = frame->disp_y2;
        if (y1 > 0x10 && y2 > y1) {
            int active_h = y2 - y1;
            if (active_h < disp_h) {
                crop_top = (disp_h - active_h) / 2;
                crop_bottom = disp_h - active_h - crop_top;
            }
        }
        if (disp_h >= 240) {
            if (crop_top < 8) crop_top = 8;
            if (crop_bottom < 8) crop_bottom = 8;
        }

        // Maintain authentic 4:3 PS1 aspect ratio with pillarbox / letterbox
        float vpW = (float)drawableSize.width, vpH = (float)drawableSize.height;
        if (drawableSize.width > 0 && drawableSize.height > 0) {
            float targetAspect = 4.0f / 3.0f;
            float currentAspect = (float)drawableSize.width / (float)drawableSize.height;
            MTLViewport vp;
            if (currentAspect > targetAspect) {
                float vpWidth = drawableSize.height * targetAspect;
                float vpX = (drawableSize.width - vpWidth) * 0.5f;
                vp = (MTLViewport){ vpX, 0.0, vpWidth, drawableSize.height, 0.0, 1.0 };
            } else {
                float vpHeight = drawableSize.width / targetAspect;
                float vpY = (drawableSize.height - vpHeight) * 0.5f;
                vp = (MTLViewport){ 0.0, vpY, drawableSize.width, vpHeight, 0.0, 1.0 };
            }
            [encoder setViewport:vp];
            vpW = (float)vp.width; vpH = (float)vp.height;
        }

        if (is24) { disp_x = 0; disp_y = 0; }
        int vis_h = disp_h - crop_top - crop_bottom;
        simd_float4 uv_bounds = simd_make_float4(
            (float)disp_x / (float)PS1_VRAM_WIDTH,
            (float)(disp_y + crop_top) / (float)PS1_VRAM_HEIGHT,
            (float)(disp_x + disp_w) / (float)PS1_VRAM_WIDTH,
            (float)(disp_y + disp_h - crop_bottom) / (float)PS1_VRAM_HEIGHT
        );
        [encoder setVertexBytes:&uv_bounds length:sizeof(simd_float4) atIndex:0];

        struct { int crt; int mode; simd_float2 scale; } params = {
            _context->crt_scanlines ? 1 : 0, _scalingMode,
            simd_make_float2(vpW / (float)disp_w, vpH / (float)(vis_h > 0 ? vis_h : disp_h))
        };
        [encoder setFragmentBytes:&params length:sizeof(params) atIndex:0];

        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [encoder endEncoding];

        if (useFxaa) {
            id<MTLRenderCommandEncoder> post = [commandBuffer renderCommandEncoderWithDescriptor:renderPass];
            [post setRenderPipelineState:_fxaaPipeline];
            simd_float4 full = simd_make_float4(0, 0, 1, 1);
            [post setVertexBytes:&full length:sizeof(full) atIndex:0];
            [post setFragmentTexture:_fxaaTarget atIndex:0];
            [post setFragmentSamplerState:_smoothSamplerState atIndex:0];
            [post drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [post endEncoding];
        }

        [commandBuffer presentDrawable:view.currentDrawable];
    }
    id<MTLTexture> shown = view.currentDrawable.texture;
    [commandBuffer commit];

    {   // MIMIPS1_RENDER_DUMP=<n>: after the n-th drawn frame, write what the window shows (post-scaling, post-FXAA) as /tmp/render.ppm
        static int dump_at = -2, n = 0;
        if (dump_at == -2) { const char* e = getenv("MIMIPS1_RENDER_DUMP"); dump_at = e ? atoi(e) : -1; }
        if (dump_at >= 0 && ++n == dump_at && shown && !view.framebufferOnly) {
            [commandBuffer waitUntilCompleted];
            NSUInteger w = shown.width, h = shown.height;
            uint8_t* px = malloc(w * h * 4);
            [shown getBytes:px bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
            FILE* f = fopen("/tmp/render.ppm", "wb");
            if (f) {
                fprintf(f, "P6\n%lu %lu\n255\n", (unsigned long)w, (unsigned long)h);
                for (NSUInteger i = 0; i < w * h; i++) { fputc(px[i * 4 + 2], f); fputc(px[i * 4 + 1], f); fputc(px[i * 4], f); }
                fclose(f);
            }
            free(px);
            printf("[Render] dumped %lux%lu to /tmp/render.ppm\n", (unsigned long)w, (unsigned long)h);
        }
    }
}

@end
