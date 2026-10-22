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
"fragment float4 fragment_main(VertexOut in [[stage_in]],\n"
"                             texture2d<float> tex [[texture(0)]],\n"
"                             sampler smp [[sampler(0)]],\n"
"                             constant int& crt_mode [[buffer(0)]]) {\n"
"    float4 color = tex.sample(smp, in.uv);\n"
"    if (crt_mode == 1) {\n"
"        float scanline = sin(in.uv.y * 480.0 * 3.14159) * 0.08;\n"
"        color.rgb -= scanline;\n"
"    }\n"
"    return color;\n"
"}\n";

@interface MimiPS1ViewDelegate : NSObject <MTKViewDelegate>
@property (nonatomic, assign) PS1Context* context;
@property (nonatomic, strong) id<MTLDevice> device;
@property (nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property (nonatomic, strong) id<MTLRenderPipelineState> pipelineState;
@property (nonatomic, strong) id<MTLTexture> vramTexture;
@property (nonatomic, strong) id<MTLSamplerState> samplerState;
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
    }
    return self;
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
}

- (void)drawInMTKView:(MTKView *)view {
    if (!_context) return;

    // Convert PS1 BGR555 VRAM to 32-bit BGRA host framebuffer (using front_vram snapshot for tear-free rendering)
    const uint16_t (*source_vram)[PS1_VRAM_WIDTH] = _context->gpu.front_disp_w > 0 ? _context->gpu.front_vram : _context->gpu.vram;
    bool is24 = _context->gpu.front_disp_w > 0 && _context->gpu.front_is_24bit;
    int fx = _context->gpu.front_disp_x, fy = _context->gpu.front_disp_y;
    int fw = _context->gpu.front_disp_w, fh = _context->gpu.front_disp_h;
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
        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPass];
        [encoder setRenderPipelineState:_pipelineState];
        [encoder setFragmentTexture:_vramTexture atIndex:0];
        [encoder setFragmentSamplerState:_samplerState atIndex:0];

        // Maintain authentic 4:3 PS1 aspect ratio with pillarbox / letterbox
        CGSize drawableSize = view.drawableSize;
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
        }

        int disp_x = _context->gpu.front_disp_w > 0 ? _context->gpu.front_disp_x : _context->gpu.disp_x;
        int disp_y = _context->gpu.front_disp_w > 0 ? _context->gpu.front_disp_y : _context->gpu.disp_y;
        int disp_w = _context->gpu.front_disp_w > 0 ? _context->gpu.front_disp_w : (_context->gpu.disp_w > 0 ? _context->gpu.disp_w : 320);
        int disp_h = _context->gpu.front_disp_h > 0 ? _context->gpu.front_disp_h : (_context->gpu.disp_h > 0 ? _context->gpu.disp_h : 240);

        int crop_top = 0;
        int crop_bottom = 0;
        int16_t y1 = _context->gpu.disp_y1;
        int16_t y2 = _context->gpu.disp_y2;
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

        if (is24) { disp_x = 0; disp_y = 0; }
        simd_float4 uv_bounds = simd_make_float4(
            (float)disp_x / (float)PS1_VRAM_WIDTH,
            (float)(disp_y + crop_top) / (float)PS1_VRAM_HEIGHT,
            (float)(disp_x + disp_w) / (float)PS1_VRAM_WIDTH,
            (float)(disp_y + disp_h - crop_bottom) / (float)PS1_VRAM_HEIGHT
        );
        [encoder setVertexBytes:&uv_bounds length:sizeof(simd_float4) atIndex:0];

        int crt = _context->crt_scanlines ? 1 : 0;
        [encoder setFragmentBytes:&crt length:sizeof(int) atIndex:0];

        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [encoder endEncoding];

        [commandBuffer presentDrawable:view.currentDrawable];
    }
    [commandBuffer commit];
}

@end
