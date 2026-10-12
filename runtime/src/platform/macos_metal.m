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

    // Synchronize hardware timers, CD-ROM and 60 FPS V-Blank IRQ
    ps1_timers_tick(_context, PS1_CYCLES_PER_FRAME);
    ps1_cdrom_tick(_context, PS1_CYCLES_PER_FRAME);
    ps1_trigger_irq(_context, IRQ_VBLANK);
    _context->frame_count++;

    // Convert PS1 BGR555 VRAM to 32-bit BGRA host framebuffer
    for (int y = 0; y < PS1_VRAM_HEIGHT; y++) {
        for (int x = 0; x < PS1_VRAM_WIDTH; x++) {
            uint16_t c = _context->gpu.vram[y][x];
            uint8_t r = (uint8_t)(((c >> 0)  & 0x1F) << 3);
            uint8_t g = (uint8_t)(((c >> 5)  & 0x1F) << 3);
            uint8_t b = (uint8_t)(((c >> 10) & 0x1F) << 3);
            _rgba_buffer[y * PS1_VRAM_WIDTH + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b | 0xFF000000;
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
        
        int disp_x = _context->gpu.disp_x;
        int disp_y = _context->gpu.disp_y;
        int disp_w = _context->gpu.disp_w > 0 ? _context->gpu.disp_w : 320;
        int disp_h = _context->gpu.disp_h > 0 ? _context->gpu.disp_h : 240;

        simd_float4 uv_bounds = simd_make_float4(
            (float)disp_x / (float)PS1_VRAM_WIDTH,
            (float)disp_y / (float)PS1_VRAM_HEIGHT,
            (float)(disp_x + disp_w) / (float)PS1_VRAM_WIDTH,
            (float)(disp_y + disp_h) / (float)PS1_VRAM_HEIGHT
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
