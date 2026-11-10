// Metal backend for the GL 1.x state tracker.
//
// The scene renders into an offscreen RGBA8 + Depth32F/Stencil8 target whose row 0 is GL's bottom row (the vertex
// shader negates clip-space y), so glReadPixels / glCopyTexSubImage2D / viewport / scissor use GL coordinates
// unchanged. SwapBuffers draws that target into the CAMetalLayer drawable (vertical flip, gamma ramp, letterbox).
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "../rt.h"
#include "../platform.h"
#include "gl_ctx.h"
#include <dispatch/dispatch.h>

#define FRAMES_IN_FLIGHT 3
#define RING_SIZE (48u << 20)

typedef struct {
    float mvp[16], mv[16], texm[GL_MAX_UNITS][16];
    float fog_color[4];
    float fog_params[4];        // start, end, density, mode (0 off, 1 linear, 2 exp, 3 exp2)
    float clip_plane[4];
    float env_color[GL_MAX_UNITS][4];
    int32_t unit[GL_MAX_UNITS][4];     // enabled, mode, combine_rgb, combine_alpha
    int32_t src_rgb[GL_MAX_UNITS][4], src_alpha[GL_MAX_UNITS][4], op_rgb[GL_MAX_UNITS][4], op_alpha[GL_MAX_UNITS][4];
    float scale[GL_MAX_UNITS][4];      // rgb scale, alpha scale
    int32_t misc[4];                   // alpha func (0..7 = NEVER..ALWAYS, -1 off), clip enabled, opaque0, opaque1
    float misc2[4];                    // alpha ref
    // fixed-function lighting
    float nmat[3][4];                  // inverse-transpose of modelview 3x3, as columns
    int32_t lit[4];                    // enabled, color material, color material mode (0 amb+dif,1 amb,2 dif,3 emi,4 spec)
    float model_amb[4], mat_amb[4], mat_dif[4], mat_spec[4], mat_emi[4], mat_shin[4];
    float l_amb[8][4], l_dif[8][4], l_spec[8][4], l_pos[8][4], l_spot[8][4], l_par[8][4];   // spot: dir.xyz,exp; par: cutoff,att0,1,2
    int32_t l_on[8][4];
} Uniforms;

static NSString *const kShaderSrc = @R"MSL(
#include <metal_stdlib>
using namespace metal;
#define NU 2
struct Uniforms {
    float4x4 mvp; float4x4 mv; float4x4 texm[NU];
    float4 fog_color; float4 fog_params; float4 clip_plane; float4 env_color[NU];
    int4 unit[NU]; int4 src_rgb[NU]; int4 src_alpha[NU]; int4 op_rgb[NU]; int4 op_alpha[NU];
    float4 scale[NU]; int4 misc; float4 misc2;
    float4 nmat[3]; int4 lit;
    float4 model_amb, mat_amb, mat_dif, mat_spec, mat_emi, mat_shin;
    float4 l_amb[8], l_dif[8], l_spec[8], l_pos[8], l_spot[8], l_par[8];
    int4 l_on[8];
};
struct VIn { float4 pos [[attribute(0)]]; float2 tc0 [[attribute(1)]]; float2 tc1 [[attribute(2)]]; float4 col [[attribute(3)]];
             float3 nrm [[attribute(4)]]; };

// GL 1.x per-vertex lighting (eye space, infinite viewer)
static float4 gl_lighting(float4 eye, float3 n_obj, float4 vcol, constant Uniforms &u) {
    float3 n = normalize(u.nmat[0].xyz * n_obj.x + u.nmat[1].xyz * n_obj.y + u.nmat[2].xyz * n_obj.z);
    float4 ma = u.mat_amb, md = u.mat_dif, ms = u.mat_spec, me = u.mat_emi;
    if (u.lit.y != 0) {
        int m = u.lit.z;
        if (m == 0) { ma = vcol; md = vcol; } else if (m == 1) ma = vcol; else if (m == 2) md = vcol;
        else if (m == 3) me = vcol; else ms = vcol;
    }
    float3 c = me.rgb + u.model_amb.rgb * ma.rgb;
    float3 p = eye.xyz / (eye.w != 0 ? eye.w : 1.0);
    for (int i = 0; i < 8; i++) {
        if (u.l_on[i].x == 0) continue;
        float4 lp = u.l_pos[i];
        float3 L; float att = 1.0;
        if (lp.w != 0) {
            float3 d = lp.xyz / lp.w - p;
            float dist = length(d);
            L = d / max(dist, 1e-6);
            float4 par = u.l_par[i];
            att = 1.0 / max(par.y + par.z * dist + par.w * dist * dist, 1e-6);
            if (par.x < 180.0) {
                float sd = dot(-L, normalize(u.l_spot[i].xyz));
                att *= (sd < cos(par.x * 0.0174532925)) ? 0.0 : pow(max(sd, 0.0), u.l_spot[i].w);
            }
        } else L = normalize(lp.xyz);
        float nl = max(dot(n, L), 0.0);
        float3 term = u.l_amb[i].rgb * ma.rgb + nl * u.l_dif[i].rgb * md.rgb;
        if (nl > 0 && any(ms.rgb != 0)) {
            float3 h = normalize(L + float3(0, 0, 1));
            term += pow(max(dot(n, h), 0.0), u.mat_shin.x) * u.l_spec[i].rgb * ms.rgb;
        }
        c += att * term;
    }
    return clamp(float4(c, md.a), 0.0, 1.0);
}
struct VOut { float4 pos [[position]]; float4 col; float2 tc0; float2 tc1; float fog; float clip [[clip_distance]] [1]; };
struct FIn { float4 pos [[position]]; float4 col; float2 tc0; float2 tc1; float fog; };

vertex VOut vs_main(VIn in [[stage_in]], constant Uniforms &u [[buffer(1)]]) {
    VOut o;
    float4 p = u.mvp * in.pos;
    p.z = (p.z + p.w) * 0.5;      // GL [-1,1] depth -> Metal [0,1]
    p.y = -p.y;                   // offscreen target stores GL rows bottom-up
    o.pos = p;
    float4 eye0 = u.mv * in.pos;
    o.col = u.lit.x != 0 ? gl_lighting(eye0, in.nrm, in.col, u) : in.col;
    o.tc0 = (u.texm[0] * float4(in.tc0, 0, 1)).xy;
    o.tc1 = (u.texm[1] * float4(in.tc1, 0, 1)).xy;
    float4 eye = u.mv * in.pos;
    float d = abs(eye.z);
    int fm = int(u.fog_params.w);
    float f = 1.0;
    if (fm == 1) f = (u.fog_params.y - d) / max(u.fog_params.y - u.fog_params.x, 1e-6);
    else if (fm == 2) f = exp(-u.fog_params.z * d);
    else if (fm == 3) { float k = u.fog_params.z * d; f = exp(-k * k); }
    o.fog = clamp(f, 0.0, 1.0);
    o.clip[0] = u.misc.y != 0 ? dot(eye, u.clip_plane) : 1.0;
    return o;
}

// GL enums used by the combiner
constant int E_REPLACE = 0x1E01, E_MODULATE = 0x2100, E_DECAL = 0x2101, E_ADD = 0x0104, E_BLEND = 0x0BE2;
constant int E_COMBINE = 0x8570, E_ADD_SIGNED = 0x8574, E_INTERPOLATE = 0x8575, E_SUBTRACT = 0x84E7;
constant int E_CONSTANT = 0x8576, E_PRIMARY = 0x8577, E_PREVIOUS = 0x8578, E_TEXTURE = 0x1702;
constant int E_SRC_COLOR = 0x0300, E_ONE_MINUS_SRC_COLOR = 0x0301, E_SRC_ALPHA = 0x0302, E_ONE_MINUS_SRC_ALPHA = 0x0303;

static float4 csrc(int s, float4 tex, float4 prev, float4 prim, float4 cst) {
    if (s == E_CONSTANT) return cst;
    if (s == E_PRIMARY) return prim;
    if (s == E_PREVIOUS) return prev;
    return tex;                                  // GL_TEXTURE (and GL_TEXTUREn: single-unit approximation)
}
static float3 oprgb(int op, float4 v) {
    if (op == E_ONE_MINUS_SRC_COLOR) return 1.0 - v.rgb;
    if (op == E_SRC_ALPHA) return float3(v.a);
    if (op == E_ONE_MINUS_SRC_ALPHA) return float3(1.0 - v.a);
    return v.rgb;
}
static float opa(int op, float4 v) { return (op == E_ONE_MINUS_SRC_ALPHA || op == E_ONE_MINUS_SRC_COLOR) ? 1.0 - v.a : v.a; }

static float4 apply_env(int k, float4 t, float4 prev, float4 prim, constant Uniforms &u, bool opaque) {
    int mode = u.unit[k].y;
    float4 ec = u.env_color[k];
    if (mode == E_REPLACE) return float4(t.rgb, opaque ? prev.a : t.a);
    if (mode == E_MODULATE) return prev * t;
    if (mode == E_DECAL) return float4(mix(prev.rgb, t.rgb, t.a), prev.a);
    if (mode == E_ADD) return float4(prev.rgb + t.rgb, prev.a * t.a);
    if (mode == E_BLEND) return float4(prev.rgb * (1.0 - t.rgb) + ec.rgb * t.rgb, prev.a * t.a);
    if (mode == E_COMBINE) {
        float3 a0 = oprgb(u.op_rgb[k].x, csrc(u.src_rgb[k].x, t, prev, prim, ec));
        float3 a1 = oprgb(u.op_rgb[k].y, csrc(u.src_rgb[k].y, t, prev, prim, ec));
        float3 a2 = oprgb(u.op_rgb[k].z, csrc(u.src_rgb[k].z, t, prev, prim, ec));
        float b0 = opa(u.op_alpha[k].x, csrc(u.src_alpha[k].x, t, prev, prim, ec));
        float b1 = opa(u.op_alpha[k].y, csrc(u.src_alpha[k].y, t, prev, prim, ec));
        float b2 = opa(u.op_alpha[k].z, csrc(u.src_alpha[k].z, t, prev, prim, ec));
        float3 rgb; float a;
        int cr = u.unit[k].z, ca = u.unit[k].w;
        if (cr == E_REPLACE) rgb = a0; else if (cr == E_MODULATE) rgb = a0 * a1; else if (cr == E_ADD) rgb = a0 + a1;
        else if (cr == E_ADD_SIGNED) rgb = a0 + a1 - 0.5; else if (cr == E_INTERPOLATE) rgb = a0 * a2 + a1 * (1.0 - a2);
        else if (cr == E_SUBTRACT) rgb = a0 - a1; else rgb = a0 * a1;
        if (ca == E_REPLACE) a = b0; else if (ca == E_MODULATE) a = b0 * b1; else if (ca == E_ADD) a = b0 + b1;
        else if (ca == E_ADD_SIGNED) a = b0 + b1 - 0.5; else if (ca == E_INTERPOLATE) a = b0 * b2 + b1 * (1.0 - b2);
        else if (ca == E_SUBTRACT) a = b0 - b1; else a = b0 * b1;
        return clamp(float4(rgb * u.scale[k].x, a * u.scale[k].y), 0.0, 1.0);
    }
    return prev * t;
}

fragment float4 fs_main(FIn in [[stage_in]], constant Uniforms &u [[buffer(1)]],
                        texture2d<float> t0 [[texture(0)]], texture2d<float> t1 [[texture(1)]],
                        sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]]) {
    float4 prim = in.col;
    float4 c = prim;
    if (u.unit[0].x != 0) {
        float4 t = t0.sample(s0, in.tc0);
        if (u.misc.z != 0) t.a = 1.0;
        c = apply_env(0, t, c, prim, u, u.misc.z != 0);
    }
    if (u.unit[1].x != 0) {
        float4 t = t1.sample(s1, in.tc1);
        if (u.misc.w != 0) t.a = 1.0;
        c = apply_env(1, t, c, prim, u, u.misc.w != 0);
    }
    c = clamp(c, 0.0, 1.0);
    int af = u.misc.x;
    if (af >= 0) {
        float r = u.misc2.x; bool pass = true;
        switch (af) {
            case 0: pass = false; break; case 1: pass = c.a < r; break; case 2: pass = c.a == r; break;
            case 3: pass = c.a <= r; break; case 4: pass = c.a > r; break; case 5: pass = c.a != r; break;
            case 6: pass = c.a >= r; break; default: break;
        }
        if (!pass) discard_fragment();
    }
    if (u.fog_params.w > 0) c.rgb = mix(u.fog_color.rgb, c.rgb, in.fog);
    return c;
}

// clear: full-screen triangle at a given depth with a given colour
struct CVOut { float4 pos [[position]]; };
vertex CVOut clear_vs(uint vid [[vertex_id]], constant float4 &p [[buffer(0)]]) {
    float2 xy = float2((vid == 1) ? 3.0 : -1.0, (vid == 2) ? 3.0 : -1.0);
    CVOut o; o.pos = float4(xy, p.x, 1.0); return o;
}
fragment float4 clear_fs(constant float4 &c [[buffer(0)]]) { return c; }

// present: offscreen -> drawable, flip vertically, gamma ramp
struct PVOut { float4 pos [[position]]; float2 uv; };
vertex PVOut present_vs(uint vid [[vertex_id]]) {
    float2 xy = float2((vid == 1) ? 3.0 : -1.0, (vid == 2) ? 3.0 : -1.0);
    PVOut o; o.pos = float4(xy, 0, 1); o.uv = float2((xy.x + 1) * 0.5, (xy.y + 1) * 0.5); return o;
}
fragment float4 present_fs(PVOut in [[stage_in]], texture2d<float> src [[texture(0)]], constant float *ramp [[buffer(0)]],
                           constant int &identity [[buffer(1)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float4 c = src.sample(s, in.uv);       // uv.y = 0 at the bottom of the drawable == GL row 0
    if (identity == 0) {
        float3 i = clamp(c.rgb, 0.0, 1.0) * 255.0;
        int3 a = int3(floor(i)); int3 b = min(a + 1, int3(255)); float3 f = i - float3(a);
        c.r = mix(ramp[a.r], ramp[b.r], f.r);
        c.g = mix(ramp[256 + a.g], ramp[256 + b.g], f.g);
        c.b = mix(ramp[512 + a.b], ramp[512 + b.b], f.b);
    }
    return float4(c.rgb, 1.0);
}
)MSL";

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static CAMetalLayer *layer;
static id<MTLLibrary> lib;
static id<MTLTexture> color_rt, depth_rt;
static int rt_w, rt_h;
static id<MTLCommandBuffer> cmd;
static id<MTLRenderCommandEncoder> enc;
static dispatch_semaphore_t frame_sem;
static bool frame_waited;
static id<MTLBuffer> ring[FRAMES_IN_FLIGHT];
static uint32_t ring_off;
static int frame_idx;
static uint64_t frame_no, completed_frame;
static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *pipelines;
static NSMutableDictionary<NSNumber *, id<MTLDepthStencilState>> *dss_cache;
static NSMutableDictionary<NSNumber *, id<MTLSamplerState>> *samplers;
static id<MTLRenderPipelineState> present_pso;
static id<MTLTexture> white_tex;
static id<MTLSamplerState> default_sampler;
static bool pending_clear;
static MTLClearColor pending_color; static double pending_depth; static int pending_stencil;
static bool pending_clear_color, pending_clear_depth, pending_clear_stencil;
static bool flip_winding;
static struct { uint32_t draws, tris; } stats;
static int g_trace_frame;   // trace every draw of the frame after a dump request
int gl_tracing(void) { return g_trace_frame; }

// ------------------------------------------------------------------ init / targets
void mtl_init(void) {
    if (dev) return;
    @autoreleasepool {
        dev = MTLCreateSystemDefaultDevice();
        queue = [dev newCommandQueue];
        layer = (__bridge CAMetalLayer *)plat_metal_layer();
        layer.device = dev;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.maximumDrawableCount = 3;
        NSError *err = nil;
        lib = [dev newLibraryWithSource:kShaderSrc options:nil error:&err];
        if (!lib) { LOG("Metal shader compile failed: %s", err.localizedDescription.UTF8String); exit(1); }
        frame_sem = dispatch_semaphore_create(FRAMES_IN_FLIGHT);
        for (int i = 0; i < FRAMES_IN_FLIGHT; i++) ring[i] = [dev newBufferWithLength:RING_SIZE options:MTLResourceStorageModeShared];
        pipelines = [NSMutableDictionary new];
        dss_cache = [NSMutableDictionary new];
        samplers = [NSMutableDictionary new];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"present_vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"present_fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        present_pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        white_tex = [dev newTextureWithDescriptor:td];
        uint32_t w = 0xffffffffu;
        [white_tex replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&w bytesPerRow:4];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        default_sampler = [dev newSamplerStateWithDescriptor:sd];
        flip_winding = getenv("COD_FLIP_WINDING") != NULL;
        LOG("Metal device: %s", dev.name.UTF8String);
    }
}

void mtl_resize_framebuffer(int w, int h) {
    if (w == rt_w && h == rt_h && color_rt) return;
    rt_w = w; rt_h = h;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    color_rt = [dev newTextureWithDescriptor:td];
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    depth_rt = [dev newTextureWithDescriptor:td];
    g_gl.fb_w = w; g_gl.fb_h = h;
    LOG("GL framebuffer %dx%d", w, h);
}

void mtl_begin_frame_if_needed(void) {
    if (cmd) return;
    if (!frame_waited) {
        dispatch_semaphore_wait(frame_sem, DISPATCH_TIME_FOREVER);
        frame_waited = true;
        frame_idx = (frame_idx + 1) % FRAMES_IN_FLIGHT;
        ring_off = 0;
        frame_no++;
    }
    int w = plat_mode_w(), h = plat_mode_h();
    mtl_resize_framebuffer(w, h);
    cmd = [queue commandBuffer];
}

static void end_encoder(void) {
    if (enc) { [enc endEncoding]; enc = nil; }
}

static void begin_encoder(void) {
    mtl_begin_frame_if_needed();
    if (enc) return;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = color_rt;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.depthAttachment.texture = depth_rt;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    rp.stencilAttachment.texture = depth_rt;
    rp.stencilAttachment.storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.depthAttachment.loadAction = MTLLoadActionLoad;
    rp.stencilAttachment.loadAction = MTLLoadActionLoad;
    if (pending_clear) {
        if (pending_clear_color) { rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = pending_color; }
        if (pending_clear_depth) { rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.clearDepth = pending_depth; }
        if (pending_clear_stencil) { rp.stencilAttachment.loadAction = MTLLoadActionClear; rp.stencilAttachment.clearStencil = (uint32_t)pending_stencil; }
        pending_clear = pending_clear_color = pending_clear_depth = pending_clear_stencil = false;
    }
    enc = [cmd renderCommandEncoderWithDescriptor:rp];
}

// ------------------------------------------------------------------ ring allocation
static id<MTLBuffer> ring_alloc(size_t n, uint32_t *off) {
    mtl_begin_frame_if_needed();
    uint32_t o = (ring_off + 255) & ~255u;
    if (o + n > RING_SIZE) {
        // overflow: a one-off buffer (rare: huge uploads)
        *off = 0;
        return [dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    }
    ring_off = o + (uint32_t)n;
    *off = o;
    return ring[frame_idx];
}

// ------------------------------------------------------------------ pipelines & states
static MTLBlendFactor blend_factor(GLenum f) {
    switch (f) {
    case GL_ZERO: return MTLBlendFactorZero;
    case GL_ONE: return MTLBlendFactorOne;
    case GL_SRC_COLOR: return MTLBlendFactorSourceColor;
    case GL_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
    case GL_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
    case GL_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case GL_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
    case GL_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case GL_DST_COLOR: return MTLBlendFactorDestinationColor;
    case GL_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case GL_SRC_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
    }
    return MTLBlendFactorOne;
}
static int blend_index(GLenum f) {
    if (f <= 1) return (int)f;
    if (f >= 0x0300 && f <= 0x0308) return 2 + (int)(f - 0x0300);
    return 1;
}

static id<MTLRenderPipelineState> get_pipeline(bool clear_pso, bool blend, GLenum src, GLenum dst, int wmask) {
    uint32_t key = (clear_pso ? 1u : 0u) | (blend ? 2u : 0u) | ((uint32_t)blend_index(src) << 2) | ((uint32_t)blend_index(dst) << 6) | ((uint32_t)wmask << 10);
    NSNumber *k = @(key);
    id<MTLRenderPipelineState> p = pipelines[k];
    if (p) return p;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    if (clear_pso) {
        pd.vertexFunction = [lib newFunctionWithName:@"clear_vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"clear_fs"];
    } else {
        pd.vertexFunction = [lib newFunctionWithName:@"vs_main"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fs_main"];
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].offset = offsetof(GLVtx, pos); vd.attributes[0].bufferIndex = 0;
        vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = offsetof(GLVtx, tc[0]); vd.attributes[1].bufferIndex = 0;
        vd.attributes[2].format = MTLVertexFormatFloat2; vd.attributes[2].offset = offsetof(GLVtx, tc[1]); vd.attributes[2].bufferIndex = 0;
        vd.attributes[3].format = MTLVertexFormatUChar4Normalized; vd.attributes[3].offset = offsetof(GLVtx, col); vd.attributes[3].bufferIndex = 0;
        vd.attributes[4].format = MTLVertexFormatFloat3; vd.attributes[4].offset = offsetof(GLVtx, nrm); vd.attributes[4].bufferIndex = 0;
        vd.layouts[0].stride = sizeof(GLVtx);
        pd.vertexDescriptor = vd;
    }
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    MTLColorWriteMask m = 0;
    if (wmask & 1) m |= MTLColorWriteMaskRed;
    if (wmask & 2) m |= MTLColorWriteMaskGreen;
    if (wmask & 4) m |= MTLColorWriteMaskBlue;
    if (wmask & 8) m |= MTLColorWriteMaskAlpha;
    pd.colorAttachments[0].writeMask = m;
    if (blend) {
        pd.colorAttachments[0].blendingEnabled = YES;
        pd.colorAttachments[0].sourceRGBBlendFactor = pd.colorAttachments[0].sourceAlphaBlendFactor = blend_factor(src);
        pd.colorAttachments[0].destinationRGBBlendFactor = pd.colorAttachments[0].destinationAlphaBlendFactor = blend_factor(dst);
        if (src == GL_SRC_ALPHA_SATURATE) pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    }
    NSError *err = nil;
    p = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) { LOG("pipeline creation failed: %s", err.localizedDescription.UTF8String); exit(1); }
    pipelines[k] = p;
    return p;
}

static MTLCompareFunction cmp_fn(GLenum f) {
    switch (f) {
    case GL_NEVER: return MTLCompareFunctionNever;
    case GL_LESS: return MTLCompareFunctionLess;
    case GL_EQUAL: return MTLCompareFunctionEqual;
    case GL_LEQUAL: return MTLCompareFunctionLessEqual;
    case GL_GREATER: return MTLCompareFunctionGreater;
    case GL_NOTEQUAL: return MTLCompareFunctionNotEqual;
    case GL_GEQUAL: return MTLCompareFunctionGreaterEqual;
    }
    return MTLCompareFunctionAlways;
}
static MTLStencilOperation st_op(GLenum o) {
    switch (o) {
    case GL_ZERO: return MTLStencilOperationZero;
    case GL_REPLACE: return MTLStencilOperationReplace;
    case GL_INCR: return MTLStencilOperationIncrementClamp;
    case GL_DECR: return MTLStencilOperationDecrementClamp;
    case GL_INVERT: return MTLStencilOperationInvert;
    case GL_INCR_WRAP: return MTLStencilOperationIncrementWrap;
    case GL_DECR_WRAP: return MTLStencilOperationDecrementWrap;
    }
    return MTLStencilOperationKeep;
}

static id<MTLDepthStencilState> get_dss(bool dtest, GLenum dfunc, bool dwrite, bool stest, GLenum sfunc, uint32_t rmask,
                                       uint32_t wmask, GLenum sf, GLenum zf, GLenum zp) {
    uint64_t key = (uint64_t)dtest | ((uint64_t)(dfunc & 7) << 1) | ((uint64_t)dwrite << 4) | ((uint64_t)stest << 5);
    if (stest) key |= ((uint64_t)(sfunc & 7) << 6) | ((uint64_t)(rmask & 0xff) << 9) | ((uint64_t)(wmask & 0xff) << 17) |
                      ((uint64_t)(sf & 0xffff) << 25) | ((uint64_t)(zf & 0xffff) << 41) | ((uint64_t)(zp & 0x7f) << 57);
    if (stest) key ^= ((uint64_t)(zp >> 7) * 0x9E3779B97F4A7C15ull) & ~0x1FFFFFFull;
    NSNumber *k = @(key);
    id<MTLDepthStencilState> s = dss_cache[k];
    if (s) return s;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = dtest ? cmp_fn(dfunc) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = dtest && dwrite;      // GL: no depth writes when the depth test is disabled
    if (stest) {
        MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = cmp_fn(sfunc);
        sd.readMask = rmask; sd.writeMask = wmask;
        sd.stencilFailureOperation = st_op(sf);
        sd.depthFailureOperation = st_op(zf);
        sd.depthStencilPassOperation = st_op(zp);
        d.frontFaceStencil = sd; d.backFaceStencil = sd;
    }
    s = [dev newDepthStencilStateWithDescriptor:d];
    dss_cache[k] = s;
    return s;
}

static MTLSamplerAddressMode wrap_mode(GLenum w) { return w == GL_REPEAT ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge; }

static id<MTLSamplerState> get_sampler(GLTexture *t) {
    if (t->sampler) return (__bridge id<MTLSamplerState>)t->sampler;
    bool mip = t->min_filter >= GL_NEAREST_MIPMAP_NEAREST && t->min_filter <= GL_LINEAR_MIPMAP_LINEAR && t->max_level_loaded > 0;
    bool minlin = t->min_filter == GL_LINEAR || t->min_filter == GL_LINEAR_MIPMAP_NEAREST || t->min_filter == GL_LINEAR_MIPMAP_LINEAR;
    bool miplin = t->min_filter == GL_NEAREST_MIPMAP_LINEAR || t->min_filter == GL_LINEAR_MIPMAP_LINEAR;
    int an = (int)t->aniso;
    if (!mip || an < 1) an = 1;
    uint32_t key = (mip ? 1u : 0) | (minlin ? 2u : 0) | (miplin ? 4u : 0) | (t->mag_filter == GL_LINEAR ? 8u : 0) |
                   (t->wrap_s == GL_REPEAT ? 16u : 0) | (t->wrap_t == GL_REPEAT ? 32u : 0) | ((uint32_t)an << 6) |
                   ((uint32_t)(mip ? t->max_level_loaded : 0) << 12);
    NSNumber *k = @(key);
    id<MTLSamplerState> s = samplers[k];
    if (!s) {
        MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
        d.minFilter = minlin ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.magFilter = t->mag_filter == GL_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.mipFilter = !mip ? MTLSamplerMipFilterNotMipmapped : miplin ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        d.sAddressMode = wrap_mode(t->wrap_s);
        d.tAddressMode = wrap_mode(t->wrap_t);
        d.maxAnisotropy = (NSUInteger)an;
        d.lodMaxClamp = mip ? (float)t->max_level_loaded : 0.0f;
        s = [dev newSamplerStateWithDescriptor:d];
        samplers[k] = s;
    }
    t->sampler = (__bridge void *)s;   // samplers live in the cache dictionary for the whole run
    return s;
}

// ------------------------------------------------------------------ draw
static GLTexture *bound_tex(int u) {
    GLuint n = g_gl.bound[u];
    return n < GL_MAX_TEXTURES ? g_gl.tex[n] : NULL;
}
extern GLTexture *gl_bound_texture(int unit);

static void set_viewport_scissor(void) {
    GLContext *c = &g_gl;
    MTLViewport vp = {c->viewport[0], c->viewport[1], c->viewport[2], c->viewport[3], c->depth_near, c->depth_far};
    [enc setViewport:vp];
    int x0 = 0, y0 = 0, x1 = rt_w, y1 = rt_h;
    if (c->scissor_test) {
        x0 = c->scissor[0]; y0 = c->scissor[1]; x1 = x0 + c->scissor[2]; y1 = y0 + c->scissor[3];
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > rt_w) x1 = rt_w; if (y1 > rt_h) y1 = rt_h;
        if (x1 <= x0 || y1 <= y0) { x0 = y0 = 0; x1 = y1 = 0; }
    }
    MTLScissorRect sr = {(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
    [enc setScissorRect:sr];
}

void mtl_draw(int prim, const GLVtx *v, uint32_t nv, const uint32_t *idx, uint32_t ni) {
    GLContext *c = &g_gl;
    if (c->cull && c->cull_face == GL_FRONT_AND_BACK && prim == PRIM_TRIANGLES) return;
    begin_encoder();
    if (c->scissor_test && (c->scissor[2] <= 0 || c->scissor[3] <= 0)) return;
    int wmask = (c->color_mask[0] ? 1 : 0) | (c->color_mask[1] ? 2 : 0) | (c->color_mask[2] ? 4 : 0) | (c->color_mask[3] ? 8 : 0);
    [enc setRenderPipelineState:get_pipeline(false, c->blend, c->blend_src, c->blend_dst, wmask)];
    [enc setDepthStencilState:get_dss(c->depth_test, c->depth_func, c->depth_mask, c->stencil_test, c->stencil_func,
                                      c->stencil_mask, c->stencil_writemask, c->stencil_fail, c->stencil_zfail, c->stencil_zpass)];
    if (c->stencil_test) [enc setStencilReferenceValue:(uint32_t)c->stencil_ref];
    set_viewport_scissor();
    bool ccw = c->front_face == GL_CCW;
    if (flip_winding) ccw = !ccw;
    // the y flip in the vertex shader mirrors screen-space winding
    [enc setFrontFacingWinding:ccw ? MTLWindingClockwise : MTLWindingCounterClockwise];
    static int nocull = -1;
    if (nocull < 0) nocull = getenv("COD_NOCULL") != NULL;
    // Sky passes (glDepthRange(1,1)) put vertices exactly on the far plane (z == w); rounding would clip some of
    // their triangles away, so clamp depth instead of clipping for them (the depth written is 1.0 either way).
    bool sky = c->depth_near >= 1.0 && c->depth_far >= 1.0;
    [enc setDepthClipMode:sky ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
    if (c->cull && prim == PRIM_TRIANGLES && !nocull) [enc setCullMode:c->cull_face == GL_FRONT ? MTLCullModeFront : MTLCullModeBack];
    else [enc setCullMode:MTLCullModeNone];
    bool offs = prim == PRIM_TRIANGLES ? c->poly_offset_fill : c->poly_offset_line;
    if (offs) [enc setDepthBias:c->poly_units slopeScale:c->poly_factor clamp:0];
    else [enc setDepthBias:0 slopeScale:0 clamp:0];

    Uniforms u;
    memset(&u, 0, sizeof u);
    gl_mat_mul(u.mvp, gl_top(1), gl_top(0));
    memcpy(u.mv, gl_top(0), 64);
    for (int k = 0; k < GL_MAX_UNITS; k++) memcpy(u.texm[k], gl_top(2 + k), 64);
    if (c->fog) {
        memcpy(u.fog_color, c->fog_color, 16);
        u.fog_params[0] = c->fog_start; u.fog_params[1] = c->fog_end; u.fog_params[2] = c->fog_density;
        u.fog_params[3] = c->fog_mode == GL_LINEAR ? 1 : c->fog_mode == GL_EXP2 ? 3 : 2;
    }
    memcpy(u.clip_plane, c->clip_plane, 16);
    if (c->lighting) {
        const float *m = gl_top(0);
        // inverse transpose of the upper 3x3 (cofactor matrix / det)
        float a = m[0], b = m[4], cc = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], k = m[10];
        float A = e * k - f * h, B = -(d * k - f * g), Cc = d * h - e * g;
        float D = -(b * k - cc * h), E = a * k - cc * g, F = -(a * h - b * g);
        float G = b * f - cc * e, H = -(a * f - cc * d), K = a * e - b * d;
        float det = a * A + b * B + cc * Cc;
        float id = det != 0 ? 1.0f / det : 1.0f;
        // n_eye = invT * n: column j of invT = (cofactor row j)/det
        float cols[3][3] = {{A * id, B * id, Cc * id}, {D * id, E * id, F * id}, {G * id, H * id, K * id}};
        for (int j = 0; j < 3; j++) for (int r = 0; r < 3; r++) u.nmat[j][r] = cols[j][r];
        u.lit[0] = 1;
        u.lit[1] = c->color_material;
        GLenum cm = c->color_material_mode;
        u.lit[2] = cm == 0x1602 ? 0 : cm == 0x1200 ? 1 : cm == 0x1201 ? 2 : cm == 0x1600 ? 3 : 4;
        memcpy(u.model_amb, c->model_ambient, 16); memcpy(u.mat_amb, c->mat_amb, 16); memcpy(u.mat_dif, c->mat_dif, 16);
        memcpy(u.mat_spec, c->mat_spec, 16); memcpy(u.mat_emi, c->mat_emi, 16); u.mat_shin[0] = c->mat_shin;
        for (int i = 0; i < 8; i++) {
            u.l_on[i][0] = c->light_on[i];
            if (!c->light_on[i]) continue;
            const struct GLLight *L = &c->light[i];
            memcpy(u.l_amb[i], L->amb, 16); memcpy(u.l_dif[i], L->dif, 16); memcpy(u.l_spec[i], L->spec, 16); memcpy(u.l_pos[i], L->pos, 16);
            memcpy(u.l_spot[i], L->spot_dir, 12); u.l_spot[i][3] = L->spot_exp;
            u.l_par[i][0] = L->spot_cut; u.l_par[i][1] = L->att[0]; u.l_par[i][2] = L->att[1]; u.l_par[i][3] = L->att[2];
        }
    }
    u.misc[0] = c->alpha_test ? (int)(c->alpha_func - GL_NEVER) : -1;
    u.misc[1] = c->clip0;
    u.misc2[0] = c->alpha_ref;
    for (int k = 0; k < GL_MAX_UNITS; k++) {
        GLTexture *t = c->tex2d[k] ? bound_tex(k) : NULL;
        bool on = t && t->mtl;
        u.unit[k][0] = on;
        if (on) {
            const GLTexEnv *e = &c->env[k];
            u.unit[k][1] = (int)e->mode; u.unit[k][2] = (int)e->combine_rgb; u.unit[k][3] = (int)e->combine_alpha;
            for (int s = 0; s < 3; s++) {
                u.src_rgb[k][s] = (int)e->src_rgb[s]; u.src_alpha[k][s] = (int)e->src_alpha[s];
                u.op_rgb[k][s] = (int)e->op_rgb[s]; u.op_alpha[k][s] = (int)e->op_alpha[s];
            }
            u.scale[k][0] = e->rgb_scale; u.scale[k][1] = e->alpha_scale;
            memcpy(u.env_color[k], e->color, 16);
            if (k < 2) u.misc[2 + k] = t->opaque;
            [enc setFragmentTexture:(__bridge id<MTLTexture>)t->mtl atIndex:k];
            [enc setFragmentSamplerState:get_sampler(t) atIndex:k];
            ((GLTexture *)t)->max_level_loaded += 0;
        } else {
            if (c->tex2d[k] && getenv("COD_GLDEBUG")) {
                static uint8_t seen[GL_MAX_TEXTURES];
                GLuint n = c->bound[k];
                if (n < GL_MAX_TEXTURES && !seen[n]) {
                    seen[n] = 1;
                    LOG("GLDEBUG: unit %d enabled with texture %u without storage (exists=%d) env=%04x blend=%d %04x/%04x",
                        k, n, t != NULL, c->env[k].mode, c->blend, c->blend_src, c->blend_dst);
                }
            }
            [enc setFragmentTexture:white_tex atIndex:k];
            [enc setFragmentSamplerState:default_sampler atIndex:k];
        }
    }
    if (getenv("COD_GLCOMBOS")) {   // log each distinct texenv/blend combination once
        static uint64_t seen[512]; static int ns;
        for (int k = 0; k < GL_MAX_UNITS; k++) if (u.unit[k][0]) {
            const GLTexEnv *e = &c->env[k];
            uint64_t h = 1469598103934665603ull;
            int32_t vals[] = {k, (int)e->mode, (int)e->combine_rgb, (int)e->combine_alpha, (int)e->src_rgb[0], (int)e->src_rgb[1], (int)e->src_rgb[2],
                              (int)e->op_rgb[0], (int)e->op_rgb[1], (int)e->src_alpha[0], (int)e->src_alpha[1], (int)(e->rgb_scale * 4), (int)(e->alpha_scale * 4),
                              c->blend, (int)c->blend_src, (int)c->blend_dst, u.misc[2 + k], u.unit[k ^ 1][0]};
            for (size_t q = 0; q < sizeof vals / 4; q++) h = (h ^ (uint32_t)vals[q]) * 1099511628211ull;
            int f = 0; for (int q = 0; q < ns; q++) if (seen[q] == h) f = 1;
            if (!f && ns < 512) {
                seen[ns++] = h;
                LOG("COMBO unit%d mode=%04x crgb=%04x calpha=%04x src=%04x,%04x,%04x op=%04x,%04x asrc=%04x,%04x scale=%.1f/%.1f blend=%d %04x/%04x opaque=%d other=%d tex=%u fmt=%04x",
                    k, e->mode, e->combine_rgb, e->combine_alpha, e->src_rgb[0], e->src_rgb[1], e->src_rgb[2], e->op_rgb[0], e->op_rgb[1],
                    e->src_alpha[0], e->src_alpha[1], e->rgb_scale, e->alpha_scale, c->blend, c->blend_src, c->blend_dst, u.misc[2 + k], u.unit[k ^ 1][0],
                    c->bound[k], bound_tex(k)->ifmt);
            }
        }
    }
    [enc setVertexBytes:&u length:sizeof u atIndex:1];
    [enc setFragmentBytes:&u length:sizeof u atIndex:1];

    uint32_t voff, ioff;
    size_t vbytes = (size_t)nv * sizeof(GLVtx), ibytes = (size_t)ni * 4;
    id<MTLBuffer> vb = ring_alloc(vbytes, &voff);
    memcpy((uint8_t *)vb.contents + voff, v, vbytes);
    id<MTLBuffer> ib = ring_alloc(ibytes, &ioff);
    memcpy((uint8_t *)ib.contents + ioff, idx, ibytes);
    [enc setVertexBuffer:vb offset:voff atIndex:0];
    MTLPrimitiveType pt = prim == PRIM_LINES ? MTLPrimitiveTypeLine : prim == PRIM_POINTS ? MTLPrimitiveTypePoint : MTLPrimitiveTypeTriangle;
    [enc drawIndexedPrimitives:pt indexCount:ni indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:ioff];
    stats.draws++;
    stats.tris += ni / 3;
    if (g_trace_frame) {
        GLTexture *t0 = u.unit[0][0] ? bound_tex(0) : NULL, *t1 = u.unit[1][0] ? bound_tex(1) : NULL;
        fprintf(stderr, "[draw %u] prim=%d nv=%u ni=%u blend=%d %04x/%04x cmask=%x dt=%d df=%04x dw=%d dr=%.2f-%.2f at=%d af=%04x ref=%.2f "
                "cull=%d/%04x/%04x st=%d fog=%d clip=%d t0=%u(%dx%d env %04x op%d) t1=%u(%dx%d env %04x) col0=%02x%02x%02x%02x vp=%d,%d,%d,%d sc=%d\n",
                stats.draws, prim, nv, ni, c->blend, c->blend_src, c->blend_dst, wmask, c->depth_test, c->depth_func, c->depth_mask,
                c->depth_near, c->depth_far, c->alpha_test, c->alpha_func, c->alpha_ref, c->cull, c->cull_face, c->front_face, c->stencil_test, c->fog, c->clip0,
                t0 ? c->bound[0] : 0, t0 ? t0->w : 0, t0 ? t0->h : 0, t0 ? c->env[0].mode : 0, t0 ? t0->opaque : 0,
                t1 ? c->bound[1] : 0, t1 ? t1->w : 0, t1 ? t1->h : 0, t1 ? c->env[1].mode : 0,
                v[0].col[0], v[0].col[1], v[0].col[2], v[0].col[3], c->viewport[0], c->viewport[1], c->viewport[2], c->viewport[3], c->scissor_test);
        for (uint32_t k = 0; k < nv && k < 4; k++)
            fprintf(stderr, "    v%u pos=(%.1f,%.1f,%.2f) tc=(%.3f,%.3f)\n", k, v[k].pos[0], v[k].pos[1], v[k].pos[2], v[k].tc[0][0], v[k].tc[0][1]);
    }
    for (int k = 0; k < GL_MAX_UNITS; k++) {   // remember GPU use for upload hazard tracking
        GLTexture *t = u.unit[k][0] ? bound_tex(k) : NULL;
        if (t) t->levels = t->levels;          // (frame tracking below)
    }
}

// ------------------------------------------------------------------ clear
void mtl_clear(GLbitfield mask) {
    GLContext *c = &g_gl;
    mtl_begin_frame_if_needed();
    bool cc = (mask & GL_COLOR_BUFFER_BIT) && (c->color_mask[0] || c->color_mask[1] || c->color_mask[2] || c->color_mask[3]);
    bool cd = (mask & GL_DEPTH_BUFFER_BIT) && c->depth_mask;
    bool cs = (mask & GL_STENCIL_BUFFER_BIT) && (c->stencil_writemask & 0xff);
    if (!cc && !cd && !cs) return;
    bool full_scissor = !c->scissor_test || (c->scissor[0] <= 0 && c->scissor[1] <= 0 && c->scissor[0] + c->scissor[2] >= rt_w && c->scissor[1] + c->scissor[3] >= rt_h);
    bool full_mask = (!cc || (c->color_mask[0] && c->color_mask[1] && c->color_mask[2] && c->color_mask[3])) && (!cs || (c->stencil_writemask & 0xff) == 0xff);
    if (!enc && full_scissor && full_mask) {
        pending_clear = true;
        if (cc) { pending_clear_color = true; pending_color = MTLClearColorMake(c->clear_color[0], c->clear_color[1], c->clear_color[2], c->clear_color[3]); }
        if (cd) { pending_clear_depth = true; pending_depth = c->clear_depth; }
        if (cs) { pending_clear_stencil = true; pending_stencil = c->clear_stencil; }
        return;
    }
    begin_encoder();
    int wmask = cc ? ((c->color_mask[0] ? 1 : 0) | (c->color_mask[1] ? 2 : 0) | (c->color_mask[2] ? 4 : 0) | (c->color_mask[3] ? 8 : 0)) : 0;
    [enc setRenderPipelineState:get_pipeline(true, false, GL_ONE, GL_ZERO, wmask)];
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = MTLCompareFunctionAlways;
    d.depthWriteEnabled = cd;
    if (cs) {
        MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = MTLCompareFunctionAlways;
        sd.depthStencilPassOperation = MTLStencilOperationReplace;
        sd.writeMask = c->stencil_writemask;
        d.frontFaceStencil = sd; d.backFaceStencil = sd;
    }
    [enc setDepthStencilState:[dev newDepthStencilStateWithDescriptor:d]];
    if (cs) [enc setStencilReferenceValue:(uint32_t)c->clear_stencil];
    MTLViewport vp = {0, 0, rt_w, rt_h, 0, 1};
    [enc setViewport:vp];
    int x0 = 0, y0 = 0, x1 = rt_w, y1 = rt_h;
    if (c->scissor_test) {
        x0 = c->scissor[0]; y0 = c->scissor[1]; x1 = x0 + c->scissor[2]; y1 = y0 + c->scissor[3];
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > rt_w) x1 = rt_w; if (y1 > rt_h) y1 = rt_h;
        if (x1 <= x0 || y1 <= y0) return;
    }
    [enc setScissorRect:(MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)}];
    [enc setCullMode:MTLCullModeNone];
    [enc setDepthBias:0 slopeScale:0 clamp:0];
    float p[4] = {(float)c->clear_depth, 0, 0, 0};
    [enc setVertexBytes:p length:16 atIndex:0];
    [enc setFragmentBytes:c->clear_color length:16 atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

// ------------------------------------------------------------------ textures
void mtl_texture_storage(GLTexture *t, int w, int h, int levels, GLenum kind) {
    mtl_init();
    MTLPixelFormat pf = kind == TEXK_BC1 ? MTLPixelFormatBC1_RGBA : kind == TEXK_BC2 ? MTLPixelFormatBC2_RGBA :
                        kind == TEXK_BC3 ? MTLPixelFormatBC3_RGBA : MTLPixelFormatRGBA8Unorm;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:levels > 1];
    td.mipmapLevelCount = levels;
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    if (!tex) { LOG("texture %dx%d alloc failed", w, h); return; }
    t->mtl = (__bridge_retained void *)tex;
    t->sampler = NULL;
}
void mtl_texture_release(GLTexture *t) {
    if (t->mtl) { id<MTLTexture> x = (__bridge_transfer id<MTLTexture>)t->mtl; (void)x; t->mtl = NULL; }
    t->w = t->h = 0;
}
void mtl_texture_upload(GLTexture *t, int level, int x, int y, int w, int h, const void *data, size_t bpr, size_t bytes) {
    if (!t->mtl) return;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)t->mtl;
    int lw = (int)tex.width >> level, lh = (int)tex.height >> level;
    if (lw < 1) lw = 1; if (lh < 1) lh = 1;
    if (x + w > lw) w = lw - x;
    if (y + h > lh) h = lh - y;
    if (w <= 0 || h <= 0) return;
    if (!cmd && !enc) {
        // between frames nothing references the texture from the CPU side; previous frames may still read it, but
        // engine uploads at load time / reused textures are written before first use in the new frame.
        [tex replaceRegion:MTLRegionMake2D(x, y, w, h) mipmapLevel:level withBytes:data bytesPerRow:bpr];
        return;
    }
    // mid-frame update (cinematics, dynamic lightmaps): order it in the command stream with a blit
    end_encoder();
    uint32_t off;
    id<MTLBuffer> b = ring_alloc(bytes, &off);
    memcpy((uint8_t *)b.contents + off, data, bytes);
    id<MTLBlitCommandEncoder> bl = [cmd blitCommandEncoder];
    size_t rows = tex.pixelFormat == MTLPixelFormatRGBA8Unorm ? (size_t)h : (size_t)(h + 3) / 4;
    [bl copyFromBuffer:b sourceOffset:off sourceBytesPerRow:bpr sourceBytesPerImage:bpr * rows sourceSize:MTLSizeMake(w, h, 1)
             toTexture:tex destinationSlice:0 destinationLevel:level destinationOrigin:MTLOriginMake(x, y, 0)];
    [bl endEncoding];
}
void mtl_copy_framebuffer_to_texture(GLTexture *t, int level, int dx, int dy, int sx, int sy, int w, int h) {
    if (!t->mtl) return;
    begin_encoder();
    end_encoder();
    id<MTLTexture> tex = (__bridge id<MTLTexture>)t->mtl;
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (sx + w > rt_w) w = rt_w - sx;
    if (sy + h > rt_h) h = rt_h - sy;
    int lw = (int)tex.width >> level, lh = (int)tex.height >> level;
    if (dx + w > lw) w = lw - dx;
    if (dy + h > lh) h = lh - dy;
    if (w <= 0 || h <= 0 || tex.pixelFormat != MTLPixelFormatRGBA8Unorm) return;
    id<MTLBlitCommandEncoder> bl = [cmd blitCommandEncoder];
    [bl copyFromTexture:color_rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(sx, sy, 0) sourceSize:MTLSizeMake(w, h, 1)
              toTexture:tex destinationSlice:0 destinationLevel:level destinationOrigin:MTLOriginMake(dx, dy, 0)];
    [bl endEncoding];
}

// Commit what is encoded so far and wait (readbacks); the frame continues in a new command buffer.
static void flush_and_wait(void) {
    end_encoder();
    if (!cmd) return;
    [cmd commit];
    [cmd waitUntilCompleted];
    cmd = [queue commandBuffer];
}

void mtl_read_pixels(int x, int y, int w, int h, uint8_t *out) {
    begin_encoder();
    end_encoder();
    if (x < 0 || y < 0 || x + w > rt_w || y + h > rt_h) { memset(out, 0, (size_t)w * h * 4); return; }
    id<MTLBuffer> b = [dev newBufferWithLength:(size_t)w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> bl = [cmd blitCommandEncoder];
    [bl copyFromTexture:color_rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(w, h, 1)
               toBuffer:b destinationOffset:0 destinationBytesPerRow:(size_t)w * 4 destinationBytesPerImage:(size_t)w * h * 4];
    [bl endEncoding];
    flush_and_wait();
    memcpy(out, b.contents, (size_t)w * h * 4);   // row 0 = GL bottom row, as glReadPixels expects
}

void mtl_finish(void) { if (cmd) flush_and_wait(); }

// ------------------------------------------------------------------ present
static void dump_frame(void) {
    static unsigned n;
    const char *dir = getenv("COD_DUMP");
    if (!dir) dir = ".";
    uint8_t *px = malloc((size_t)rt_w * rt_h * 4);
    mtl_read_pixels(0, 0, rt_w, rt_h, px);
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%05u.bmp", dir, n++);
    plat_dump_rgba(px, rt_w, rt_h, 1, path);   // rows already bottom-up (GL order) == BMP order
    free(px);
    LOG("dumped %s", path);
}

void mtl_present(void) {
    @autoreleasepool {
        mtl_begin_frame_if_needed();
        static unsigned frames;
        frames++;
        const char *every = getenv("COD_DUMP_EVERY");
        if (g_trace_frame) { g_trace_frame = 0; fprintf(stderr, "[draw trace end]\n"); }
        if (plat_take_dump_request() || (every && frames % (unsigned)atoi(every) == 0)) {
            dump_frame();
            if (getenv("COD_GLTRACE")) { g_trace_frame = 1; fprintf(stderr, "[draw trace begin]\n"); }
        }
        end_encoder();
        // flush any pending clear that never got a draw (e.g. a cleared loading screen)
        if (pending_clear) { begin_encoder(); end_encoder(); }
        layer.displaySyncEnabled = g_gl.swap_interval != 0;
        int dw, dh;
        plat_drawable_size(&dw, &dh);
        if (dw > 0 && dh > 0 && ((int)layer.drawableSize.width != dw || (int)layer.drawableSize.height != dh))
            layer.drawableSize = CGSizeMake(dw, dh);
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (drawable) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> pe = [cmd renderCommandEncoderWithDescriptor:rp];
            // letterbox to preserve the mode's aspect ratio
            double sa = (double)rt_w / rt_h, da = (double)dw / dh;
            double vw = dw, vh = dh;
            if (da > sa) vw = dh * sa; else vh = dw / sa;
            MTLViewport vp = {(dw - vw) / 2, (dh - vh) / 2, vw, vh, 0, 1};
            [pe setViewport:vp];
            [pe setRenderPipelineState:present_pso];
            [pe setFragmentTexture:color_rt atIndex:0];
            [pe setFragmentBytes:g_gl.gamma length:sizeof g_gl.gamma atIndex:0];
            int ident = g_gl.gamma_identity ? 1 : 0;
            [pe setFragmentBytes:&ident length:sizeof ident atIndex:1];
            [pe drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [pe endEncoding];
            [cmd presentDrawable:drawable];
        }
        uint64_t fno = frame_no;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer> b) {
            (void)b;
            completed_frame = fno;
            dispatch_semaphore_signal(frame_sem);
        }];
        [cmd commit];
        cmd = nil;
        frame_waited = false;
        if (getenv("COD_GLSTATS") && frames % 120 == 0) LOG("frame %u: %u draws, %u tris", frames, stats.draws, stats.tris);
        stats.draws = stats.tris = 0;
    }
}
