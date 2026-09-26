// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitroview: minimal, very fast JPEG viewer for macOS.
//
//   nitroview [-f] [-s ms] [-j threads] file.jpg [more files or directories...]
//   nitroview --bench files...     decode everything, print timings, no window
//   nitroview --selftest files...  compare GPU colour conversion with libjpeg-turbo
//                                 (only in builds with the optional libjpeg-turbo fallback)
//
// Keys: PgDn/Space next, PgUp/Backspace previous, Home/End first/last,
//       +/- zoom, 0 fit, 1 actual size, arrows pan, F or Enter toggle
//       full screen, P pause slideshow,
//       Esc/Q quit.
//
// Pipeline: file -> nitrojpeg (multi-threaded, planar Y/Cb/Cr straight into a
// Metal buffer) -> GPU compute kernel (upsampling + YCbCr->RGB) -> mipmaps ->
// trilinear scaled draw. Neighbouring images are decoded ahead in the
// background, so paging is usually instantaneous.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <sys/stat.h>
#ifdef NV_TURBOJPEG
#include <turbojpeg.h>   // optional fallback for files nitrojpeg does not handle
#endif
#include <unistd.h>

#include "nitrojpeg.h"

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

// Where decoded planes live. Shared: system memory, the GPU kernel reads it
// directly over PCIe (no driver copy). Managed: driver uploads to VRAM first.
static MTLResourceOptions g_storage = MTLResourceStorageModeShared;
static BOOL g_use_textures = NO;

#define MAX_TEX 16384
#define AHEAD 3     // images decoded ahead in the paging direction
#define BEHIND 2    // ... and kept / decoded behind

// ---------------------------------------------------------------------------
// decoded image

typedef enum { KIND_YUV = 0, KIND_BGRA = 1 } Kind;

@interface Decoded : NSObject
@property(nonatomic) id<MTLBuffer> buffer;
@property(nonatomic) Kind kind;
@property(nonatomic) int width, height;      // decoded size
@property(nonatomic) int ncomp, orientation;
@property(nonatomic) int h0, v0, h1, v1, hmax, vmax;  // sampling (Y, chroma)
- (int *)pw;
- (int *)ph;
- (size_t *)offset;                         // plane offsets in the buffer
- (size_t *)pitch;
@property(nonatomic) double read_ms, decode_ms;
@property(nonatomic) int mode, bands;
@property(nonatomic) id<MTLTexture> texture; // converted RGBA + mips (main thread only)
@end
@implementation Decoded {
    int _pw[3], _ph[3];
    size_t _offset[3], _pitch[3];
}
- (int *)pw { return _pw; }
- (int *)ph { return _ph; }
- (size_t *)offset { return _offset; }
- (size_t *)pitch { return _pitch; }
@end

// Buffer pool: 50-100 MB Metal buffers are expensive to create (page faults),
// so evicted ones are reused.
@interface BufferPool : NSObject
- (instancetype)initWithDevice:(id<MTLDevice>)dev;
- (id<MTLBuffer>)get:(size_t)len;
- (void)put:(id<MTLBuffer>)b;
- (id<MTLTexture>)textureWidth:(int)w height:(int)h;   // RGBA8, mipmapped, private
- (void)putTexture:(id<MTLTexture>)t;
@end
@implementation BufferPool {
    id<MTLDevice> _dev;
    NSMutableArray<id<MTLBuffer>> *_free;
    NSMutableArray<id<MTLTexture>> *_freeTex;
}
- (instancetype)initWithDevice:(id<MTLDevice>)dev {
    if ((self = [super init])) { _dev = dev; _free = [NSMutableArray array]; _freeTex = [NSMutableArray array]; }
    return self;
}
- (id<MTLBuffer>)get:(size_t)len {
    @synchronized(self) {
        id<MTLBuffer> best = nil;
        for (id<MTLBuffer> b in _free)
            if (b.length >= len && b.length <= len * 2 && (!best || b.length < best.length)) best = b;
        if (best) { [_free removeObject:best]; return best; }
    }
    return [_dev newBufferWithLength:len options:g_storage];
}
- (void)put:(id<MTLBuffer>)b {
    if (!b) return;
    @synchronized(self) {
        [_free addObject:b];
        while (_free.count > 4) [_free removeObjectAtIndex:0];
    }
}
- (id<MTLTexture>)textureWidth:(int)w height:(int)h {
    @synchronized(self) {
        for (NSUInteger i = 0; i < _freeTex.count; i++) {
            id<MTLTexture> t = _freeTex[i];   // strong: removal must not free it
            if ((int)t.width == w && (int)t.height == h) { [_freeTex removeObjectAtIndex:i]; return t; }
        }
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:w height:h mipmapped:YES];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    return [_dev newTextureWithDescriptor:td];
}
- (void)putTexture:(id<MTLTexture>)t {
    if (!t) return;
    @synchronized(self) {
        [_freeTex addObject:t];
        while (_freeTex.count > 4) [_freeTex removeObjectAtIndex:0];
    }
}
@end

static size_t align_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

static NSData *read_file(NSString *path) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY);
    if (fd < 0) return nil;
    struct stat st;
    fstat(fd, &st);
    NSMutableData *d = [NSMutableData dataWithLength:(NSUInteger)st.st_size];
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t r = read(fd, (uint8_t *)d.mutableBytes + got, (size_t)st.st_size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    return got == (size_t)st.st_size ? d : nil;
}

// Decode one file into a Metal buffer. Runs on the decoder thread.
static Decoded *decode_file(NSString *path, BufferPool *pool, size_t align) {
    double t0 = now_ms();
    NSData *data = read_file(path);
    if (!data) return nil;
    double t1 = now_ms();
    Decoded *d = [Decoded new];
    d.read_ms = t1 - t0;
    const uint8_t *bytes = data.bytes;
    size_t len = data.length;
    nj_info fi;
    int ok = !nj_read_info(bytes, len, &fi);
    d.orientation = ok ? fi.orientation : 1;
    if (ok && fi.supported && fi.width <= MAX_TEX && fi.height <= MAX_TEX) {
        size_t total = 0;
        for (int c = 0; c < fi.ncomp; c++) {
            d.pitch[c] = align_up((size_t)fi.plane_w[c], align);
            d.offset[c] = total;
            total = align_up(total + d.pitch[c] * fi.plane_h[c], align);
            d.pw[c] = fi.plane_w[c];
            d.ph[c] = fi.plane_h[c];
        }
        id<MTLBuffer> buf = [pool get:total];
        if (!buf) return nil;
        uint8_t *base = buf.contents;
        uint8_t *planes[3] = {base + d.offset[0], base + d.offset[1], base + d.offset[2]};
        nj_stats st;
        if (!nj_decode_planes(bytes, len, &fi, planes, d.pitch, 0, &st)) {
            if (buf.storageMode == MTLStorageModeManaged) [buf didModifyRange:NSMakeRange(0, total)];
            d.buffer = buf;
            d.kind = KIND_YUV;
            d.width = fi.width;
            d.height = fi.height;
            d.ncomp = fi.ncomp;
            d.h0 = fi.h[0]; d.v0 = fi.v[0];
            d.h1 = fi.ncomp > 1 ? fi.h[1] : 1; d.v1 = fi.ncomp > 1 ? fi.v[1] : 1;
            d.hmax = fi.hmax; d.vmax = fi.vmax;
            d.mode = st.mode;
            d.bands = st.bands;
            d.decode_ms = now_ms() - t1;
            return d;
        }
        [pool put:buf];
    }
#ifndef NV_TURBOJPEG
    return nil;   // built without the fallback: progressive, CMYK, damaged ... files are skipped
#else
    // fallback: TurboJPEG (progressive, CMYK, RGB, huge images, damaged files)
    tjhandle h = tj3Init(TJINIT_DECOMPRESS);
    if (tj3DecompressHeader(h, bytes, len) < 0) { tj3Destroy(h); return nil; }
    int w = tj3Get(h, TJPARAM_JPEGWIDTH), ht = tj3Get(h, TJPARAM_JPEGHEIGHT);
    tjscalingfactor sf = {1, 1};
    while ((w + sf.denom - 1) / sf.denom > MAX_TEX || (ht + sf.denom - 1) / sf.denom > MAX_TEX) sf.denom *= 2;
    tj3SetScalingFactor(h, sf);
    w = TJSCALED(w, sf);
    ht = TJSCALED(ht, sf);
    size_t pitch = align_up((size_t)w * 4, align);
    if (w <= 0 || ht <= 0) { tj3Destroy(h); return nil; }
    id<MTLBuffer> buf = [pool get:pitch * ht];
    if (!buf) { tj3Destroy(h); return nil; }
    tj3Set(h, TJPARAM_STOPONWARNING, 0);
    int rc = tj3Decompress8(h, bytes, len, buf.contents, (int)pitch, TJPF_BGRX);
    int fatal = rc < 0 && tj3GetErrorCode(h) == TJERR_FATAL;
    tj3Destroy(h);
    if (fatal) { [pool put:buf]; return nil; }
    if (buf.storageMode == MTLStorageModeManaged) [buf didModifyRange:NSMakeRange(0, pitch * ht)];
    d.buffer = buf;
    d.kind = KIND_BGRA;
    d.width = w;
    d.height = ht;
    d.pitch[0] = pitch;
    d.decode_ms = now_ms() - t1;
    d.mode = -1;
    d.bands = 1;
    return d;
#endif
}

// ---------------------------------------------------------------------------
// GPU

static NSString *const kShaders = @R"MSL(
#include <metal_stdlib>
using namespace metal;

struct ConvParams {
    uint2 size;        // output size
    float2 cscale;     // luma pixel -> chroma pixel scale (h_c/hmax, v_c/vmax)
    float2 csize;      // chroma plane size
    uint kind;         // 0 YCbCr, 1 gray, 2 BGRA copy
};

kernel void convert(texture2d<float, access::read> Y [[texture(0)]],
                    texture2d<float, access::sample> Cb [[texture(1)]],
                    texture2d<float, access::sample> Cr [[texture(2)]],
                    texture2d<float, access::write> out [[texture(3)]],
                    constant ConvParams &p [[buffer(0)]],
                    uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.size.x || gid.y >= p.size.y) return;
    float4 c;
    if (p.kind == 2) {
        c = float4(Y.read(gid).rgb, 1.0);
    } else {
        float y = Y.read(gid).r;
        if (p.kind == 1) {
            c = float4(y, y, y, 1.0);
        } else {
            constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
            // chroma sample centres: same "fancy upsampling" as libjpeg (linear, centred)
            float2 uv = (float2(gid) + 0.5) * p.cscale / p.csize;
            float cb = Cb.sample(s, uv).r - 128.0 / 255.0;
            float cr = Cr.sample(s, uv).r - 128.0 / 255.0;
            c = float4(y + 1.402 * cr, y - 0.344136 * cb - 0.714136 * cr, y + 1.772 * cb, 1.0);
        }
    }
    out.write(saturate(c), gid);
}

struct PlaneParams {
    uint2 size;
    float2 cscale;
    int2 cmax;         // chroma plane size - 1
    uint off[3];
    uint pitch[3];
    uint kind;
};

static inline float chroma(device const uchar *p, uint pitch, float2 c, int2 cmax) {
    float2 f = c - 0.5;
    int2 i0 = int2(floor(f));
    float2 w = f - float2(i0);
    int2 a = clamp(i0, int2(0), cmax), b = clamp(i0 + 1, int2(0), cmax);
    float v00 = p[a.y * pitch + a.x], v01 = p[a.y * pitch + b.x];
    float v10 = p[b.y * pitch + a.x], v11 = p[b.y * pitch + b.x];
    return mix(mix(v00, v01, w.x), mix(v10, v11, w.x), w.y) * (1.0 / 255.0);
}

kernel void convert_buf(device const uchar *src [[buffer(1)]],
                        texture2d<float, access::write> out [[texture(3)]],
                        constant PlaneParams &p [[buffer(0)]],
                        uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.size.x || gid.y >= p.size.y) return;
    float4 c;
    if (p.kind == 2) {
        device const uchar *q = src + gid.y * p.pitch[0] + gid.x * 4;
        c = float4(q[2], q[1], q[0], 255) * (1.0 / 255.0);
    } else {
        float y = src[p.off[0] + gid.y * p.pitch[0] + gid.x] * (1.0 / 255.0);
        if (p.kind == 1) {
            c = float4(y, y, y, 1.0);
        } else {
            float2 cc = (float2(gid) + 0.5) * p.cscale;
            float cb = chroma(src + p.off[1], p.pitch[1], cc, p.cmax) - 128.0 / 255.0;
            float cr = chroma(src + p.off[2], p.pitch[2], cc, p.cmax) - 128.0 / 255.0;
            c = float4(y + 1.402 * cr, y - 0.344136 * cb - 0.714136 * cr, y + 1.772 * cb, 1.0);
        }
    }
    out.write(saturate(c), gid);
}

struct VOut { float4 pos [[position]]; float2 uv; };
struct DrawParams { float2 origin; float2 size; uint orientation; };   // NDC top-left + extent

vertex VOut vmain(uint vid [[vertex_id]], constant DrawParams &p [[buffer(0)]]) {
    float2 q = float2(vid & 1, vid >> 1);            // 0..1 display space, y down
    VOut o;
    o.pos = float4(p.origin.x + q.x * p.size.x, p.origin.y - q.y * p.size.y, 0, 1);
    float u = q.x, v = q.y;
    float2 t;
    switch (p.orientation) {                          // EXIF orientation
        case 2: t = float2(1 - u, v); break;
        case 3: t = float2(1 - u, 1 - v); break;
        case 4: t = float2(u, 1 - v); break;
        case 5: t = float2(v, u); break;
        case 6: t = float2(v, 1 - u); break;
        case 7: t = float2(1 - v, 1 - u); break;
        case 8: t = float2(1 - v, u); break;
        default: t = float2(u, v); break;
    }
    o.uv = t;
    return o;
}

fragment float4 fmain(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {
    constexpr sampler s(filter::linear, mip_filter::linear, address::clamp_to_edge, max_anisotropy(4));
    return tex.sample(s, in.uv);
}
)MSL";

typedef struct {
    uint32_t size[2];
    float cscale[2];
    float csize[2];
    uint32_t kind;
} ConvParams;

typedef struct {
    float origin[2];     // NDC position of the image's top-left corner
    float size[2];       // NDC extent
    uint32_t orientation;
    uint32_t pad;
} DrawParams;

typedef struct {
    uint32_t size[2];
    float cscale[2];
    int32_t cmax[2];
    uint32_t off[3];
    uint32_t pitch[3];
    uint32_t kind;
} PlaneParams;


@interface GPU : NSObject
@property(nonatomic, readonly) id<MTLDevice> device;
@property(nonatomic, readonly) id<MTLCommandQueue> queue;
@property(nonatomic, readonly) id<MTLComputePipelineState> convert, convertBuf;
@property(nonatomic, readonly) id<MTLRenderPipelineState> draw;
@property(nonatomic, readonly) size_t align;
- (instancetype)initWithDevice:(id<MTLDevice>)dev;
- (id<MTLTexture>)textureFor:(Decoded *)d commandBuffer:(id<MTLCommandBuffer>)cb;
@property(nonatomic) BufferPool *pool;
@end

@implementation GPU
- (instancetype)initWithDevice:(id<MTLDevice>)dev {
    if (!(self = [super init])) return nil;
    _device = dev;
    _queue = [dev newCommandQueue];
    NSError *err = nil;
    MTLCompileOptions *opt = [MTLCompileOptions new];
    opt.mathMode = MTLMathModeFast;
    id<MTLLibrary> lib = [dev newLibraryWithSource:kShaders options:opt error:&err];
    if (!lib) { NSLog(@"shader: %@", err); exit(1); }
    _convert = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"convert"] error:&err];
    _convertBuf = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"convert_buf"] error:&err];
    MTLRenderPipelineDescriptor *rp = [MTLRenderPipelineDescriptor new];
    rp.vertexFunction = [lib newFunctionWithName:@"vmain"];
    rp.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    rp.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    _draw = [dev newRenderPipelineStateWithDescriptor:rp error:&err];
    if (!_convert || !_draw) { NSLog(@"pipeline: %@", err); exit(1); }
    _align = MAX([dev minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatR8Unorm],
                 [dev minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm]);
    _align = MAX(_align, (size_t)64);
    return self;
}

- (id<MTLTexture>)planeTexture:(Decoded *)d index:(int)c format:(MTLPixelFormat)fmt w:(int)w h:(int)h {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModeManaged;
    td.usage = MTLTextureUsageShaderRead;
    return [d.buffer newTextureWithDescriptor:td offset:d.offset[c] bytesPerRow:d.pitch[c]];
}

// Upload + convert + build mipmaps (all on the GPU, asynchronously).
- (id<MTLTexture>)textureFor:(Decoded *)d commandBuffer:(id<MTLCommandBuffer>)cb {
    if (d.texture) return d.texture;
    id<MTLTexture> out = [_pool textureWidth:d.width height:d.height];
    if (!g_use_textures) {
        PlaneParams pp = {{(uint32_t)d.width, (uint32_t)d.height}, {1, 1}, {0, 0}, {0}, {0}, 0};
        for (int c = 0; c < 3; c++) { pp.off[c] = (uint32_t)d.offset[c]; pp.pitch[c] = (uint32_t)d.pitch[c]; }
        if (d.kind == KIND_BGRA) pp.kind = 2;
        else if (d.ncomp == 1) pp.kind = 1;
        else {
            pp.cscale[0] = (float)d.h1 / d.hmax;
            pp.cscale[1] = (float)d.v1 / d.vmax;
            pp.cmax[0] = d.pw[1] - 1;
            pp.cmax[1] = d.ph[1] - 1;
        }
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:_convertBuf];
        [ce setBuffer:d.buffer offset:0 atIndex:1];
        [ce setTexture:out atIndex:3];
        [ce setBytes:&pp length:sizeof pp atIndex:0];
        [ce dispatchThreadgroups:MTLSizeMake((d.width + 15) / 16, (d.height + 15) / 16, 1)
            threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [ce endEncoding];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be generateMipmapsForTexture:out];
        [be endEncoding];
        d.texture = out;
        return out;
    }
    id<MTLTexture> t0, t1 = nil, t2 = nil;
    ConvParams p = {{(uint32_t)d.width, (uint32_t)d.height}, {1, 1}, {1, 1}, 0};
    if (d.kind == KIND_BGRA) {
        t0 = [self planeTexture:d index:0 format:MTLPixelFormatBGRA8Unorm w:d.width h:d.height];
        p.kind = 2;
    } else {
        t0 = [self planeTexture:d index:0 format:MTLPixelFormatR8Unorm w:d.pw[0] h:d.ph[0]];
        if (d.ncomp == 3) {
            t1 = [self planeTexture:d index:1 format:MTLPixelFormatR8Unorm w:d.pw[1] h:d.ph[1]];
            t2 = [self planeTexture:d index:2 format:MTLPixelFormatR8Unorm w:d.pw[2] h:d.ph[2]];
            p.cscale[0] = (float)d.h1 / d.hmax;
            p.cscale[1] = (float)d.v1 / d.vmax;
            p.csize[0] = d.pw[1];
            p.csize[1] = d.ph[1];
        } else {
            p.kind = 1;
        }
    }
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:_convert];
    [ce setTexture:t0 atIndex:0];
    [ce setTexture:t1 ?: t0 atIndex:1];
    [ce setTexture:t2 ?: t0 atIndex:2];
    [ce setTexture:out atIndex:3];
    [ce setBytes:&p length:sizeof p atIndex:0];
    MTLSize tg = MTLSizeMake(16, 16, 1);
    [ce dispatchThreadgroups:MTLSizeMake((d.width + 15) / 16, (d.height + 15) / 16, 1) threadsPerThreadgroup:tg];
    [ce endEncoding];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be generateMipmapsForTexture:out];
    [be endEncoding];
    d.texture = out;
    return out;
}
@end

// ---------------------------------------------------------------------------
// image cache + background decoder

@interface Loader : NSObject
- (instancetype)initWithFiles:(NSArray<NSString *> *)files pool:(BufferPool *)pool align:(size_t)align;
- (Decoded *)get:(NSInteger)i;               // nil if not decoded yet
- (BOOL)failed:(NSInteger)i;                 // could not be decoded at all
- (void)focus:(NSInteger)cur direction:(int)dir;
@property(nonatomic, copy) void (^onDecoded)(NSInteger index);
@end

@implementation Loader {
    NSArray<NSString *> *_files;
    BufferPool *_pool;
    size_t _align;
    NSMutableDictionary<NSNumber *, Decoded *> *_cache;
    NSMutableSet<NSNumber *> *_failed;
    NSInteger _cur;
    int _dir;
    NSCondition *_cond;
}
- (instancetype)initWithFiles:(NSArray<NSString *> *)files pool:(BufferPool *)pool align:(size_t)align {
    if (!(self = [super init])) return nil;
    _files = files;
    _pool = pool;
    _align = align;
    _cache = [NSMutableDictionary dictionary];
    _failed = [NSMutableSet set];
    _dir = 1;
    _cond = [NSCondition new];
    NSThread *th = [[NSThread alloc] initWithTarget:self selector:@selector(run) object:nil];
    th.qualityOfService = NSQualityOfServiceUserInteractive;
    [th start];
    return self;
}
- (BOOL)failed:(NSInteger)i {
    [_cond lock];
    BOOL f = [_failed containsObject:@(i)];
    [_cond unlock];
    return f;
}
- (Decoded *)get:(NSInteger)i {
    [_cond lock];
    Decoded *d = _cache[@(i)];
    [_cond unlock];
    return d;
}
- (void)focus:(NSInteger)cur direction:(int)dir {
    [_cond lock];
    _cur = cur;
    _dir = dir;
    [_cond signal];
    [_cond unlock];
}
// Priority order: current, then AHEAD in the paging direction, then BEHIND.
- (NSArray<NSNumber *> *)wantedLocked {
    NSMutableArray *w = [NSMutableArray array];
    NSInteger n = (NSInteger)_files.count;
    void (^add)(NSInteger) = ^(NSInteger i) {   // no wrap-around past the first/last image
        if (i >= 0 && i < n && ![w containsObject:@(i)]) [w addObject:@(i)];
    };
    add(_cur);
    for (int k = 1; k <= MAX(AHEAD, BEHIND); k++) {
        if (k <= AHEAD) add(_cur + _dir * k);
        if (k <= BEHIND) add(_cur - _dir * k);
    }
    return w;
}
- (void)run {
    for (;;) {
        [_cond lock];
        NSNumber *next = nil;
        NSArray *wanted;
        for (;;) {
            wanted = [self wantedLocked];
            for (NSNumber *i in wanted)
                if (!_cache[i] && ![_failed containsObject:i]) { next = i; break; }
            if (next) break;
            [_cond wait];
        }
        // evict everything outside the window
        for (NSNumber *k in _cache.allKeys)
            if (![wanted containsObject:k]) {
                Decoded *old = _cache[k];
                [_cache removeObjectForKey:k];
                id<MTLBuffer> b = old.buffer;
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self->_pool putTexture:old.texture];
                    old.texture = nil;
                    [self->_pool put:b];
                });
            }
        [_cond unlock];

        Decoded *d;
        @autoreleasepool { d = decode_file(_files[next.integerValue], _pool, _align); }

        [_cond lock];
        if (d) _cache[next] = d; else [_failed addObject:next];
        [_cond unlock];
        if (!d) fprintf(stderr, "cannot decode %s\n", _files[next.integerValue].fileSystemRepresentation);
        void (^cb)(NSInteger) = self.onDecoded;
        if (cb) dispatch_async(dispatch_get_main_queue(), ^{ cb(next.integerValue); });
    }
}
@end

// ---------------------------------------------------------------------------
// window

@interface ViewerView : MTKView <MTKViewDelegate>
@property(nonatomic) NSArray<NSString *> *files;
@property(nonatomic) GPU *gpu;
@property(nonatomic) Loader *loader;
@property(nonatomic) NSInteger index;
- (void)startSlideshow:(double)ms;
// used by --zoomtest
- (void)testKey:(char)k image:(Decoded *)d view:(CGSize)ds;
- (DrawParams)drawParams:(Decoded *)d view:(CGSize)ds;
- (double)testScale:(Decoded *)d view:(CGSize)ds;
- (BOOL)testZoomed;
@end

@implementation ViewerView {
    double _requestTime;      // when the current image was requested
    BOOL _reported;
    BOOL _wasReady;           // already decoded when requested
    BOOL _started;
    // slideshow
    double _slideMs;          // 0: off
    BOOL _paused;
    NSInteger _shownIndex;    // last image actually drawn
    double _shownTime;
    unsigned _slideGen;       // invalidates pending slideshow steps
    // zoom / pan: kept when paging, so a series can be compared at the same spot
    BOOL _zoomed;             // NO: fit to window
    double _scale;            // image pixels -> drawable (device) pixels
    double _cx, _cy;          // image point (display orientation, pixels) at the view centre
}
- (BOOL)acceptsFirstResponder { return YES; }

- (void)go:(NSInteger)i dir:(int)dir {
    NSInteger n = (NSInteger)_files.count;
    i = MAX(0, MIN(i, n - 1));              // stop at the first/last image, no rollover
    if (i == _index && _started) return;
    _started = YES;
    _index = i;
    _requestTime = now_ms();
    _reported = NO;
    _wasReady = [_loader get:i] != nil;
    [_loader focus:i direction:dir];
    [self updateTitle];
    self.needsDisplay = YES;
}

- (void)updateTitle {
    Decoded *d = [_loader get:_index];
    NSString *name = _files[_index].lastPathComponent;
    NSString *t = d ? [NSString stringWithFormat:@"%@  (%ld/%lu)  %dx%d  decode %.1f ms", name, _index + 1,
                                                 _files.count, d.width, d.height, d.decode_ms]
                    : [NSString stringWithFormat:@"%@  (%ld/%lu)  %@", name, _index + 1, _files.count,
                                                 [_loader failed:_index] ? @"CANNOT DECODE (not a JPEG or damaged)" : @"loading..."];
    if (d)
        t = [t stringByAppendingFormat:@"   %@ %.0f%%", _zoomed ? @"zoom" : @"fit", 100 * [self currentScale:d view:self.drawableSize]];
    if (_slideMs > 0)
        t = [t stringByAppendingFormat:@"   %@", _paused ? @"[slideshow paused: P]"
                                                     : [NSString stringWithFormat:@"[slideshow %g ms]", _slideMs]];
    self.window.title = t;
}

// Slideshow: the next image comes exactly _slideMs after the current one was
// drawn. If decoding takes longer than that, it waits: no image is skipped.
- (void)startSlideshow:(double)ms {
    _slideMs = ms;
    _shownIndex = -1;
    [self updateTitle];
}

- (void)scheduleSlide {
    if (_slideMs <= 0 || _paused || _index >= (NSInteger)_files.count - 1) return;   // stops at the last image
    unsigned gen = ++_slideGen;
    NSInteger idx = _index;
    double wait = _slideMs - (now_ms() - _shownTime);
    __weak ViewerView *ws = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(MAX(wait, 0) * 1e6)), dispatch_get_main_queue(), ^{
        ViewerView *v = ws;
        if (v && gen == v->_slideGen && idx == v->_index && !v->_paused) [v go:idx + 1 dir:1];
    });
}

- (void)imageDecoded:(NSInteger)i {
    Decoded *d = [_loader get:i];
    if (!d) {
        if (i == _index) {   // show the error, clear the old image; slideshow moves on
            [self updateTitle];
            self.needsDisplay = YES;
            if (_shownIndex != _index) { _shownIndex = _index; _shownTime = now_ms(); [self scheduleSlide]; }
        }
        return;
    }
    if (i == _index) {
        [self updateTitle];
        self.needsDisplay = YES;
    } else {
        // upload + convert neighbours ahead of time, so paging only has to draw
        NSInteger dist = labs(i - _index);
        if (dist <= 1 && !d.texture) {
            id<MTLCommandBuffer> cb = [_gpu.queue commandBuffer];
            [_gpu textureFor:d commandBuffer:cb];
            [cb commit];
        }
    }
}

// --- zoom / pan --- (view size is a parameter so --zoomtest can drive it offscreen)

static void display_size(Decoded *d, double *iw, double *ih) {   // after EXIF rotation
    BOOL swap = d.orientation >= 5;
    *iw = swap ? d.height : d.width;
    *ih = swap ? d.width : d.height;
}

- (BOOL)isFullScreen { return (self.window.styleMask & NSWindowStyleMaskFullScreen) != 0; }

- (double)fitScale:(Decoded *)d view:(CGSize)ds {
    double iw, ih;
    display_size(d, &iw, &ih);
    double s = MIN(ds.width / iw, ds.height / ih);
    if (s > 1 && self.window && !self.isFullScreen) s = 1;   // don't upscale in a window
    return s;
}
- (double)fitScale:(Decoded *)d { return [self fitScale:d view:self.drawableSize]; }
- (double)currentScale:(Decoded *)d view:(CGSize)ds { return _zoomed ? _scale : [self fitScale:d view:ds]; }

// Keeps the view centre inside the image (or centred where the image is smaller).
- (void)clampCenter:(Decoded *)d scale:(double)s view:(CGSize)ds {
    double iw, ih;
    display_size(d, &iw, &ih);
    double vw = ds.width / s, vh = ds.height / s;
    _cx = iw <= vw ? iw / 2 : MAX(vw / 2, MIN(_cx, iw - vw / 2));
    _cy = ih <= vh ? ih / 2 : MAX(vh / 2, MIN(_cy, ih - vh / 2));
}

- (void)zoomTo:(double)ns image:(Decoded *)d {
    if (!_zoomed) {   // leaving fit mode: start from the image centre
        double iw, ih;
        display_size(d, &iw, &ih);
        _cx = iw / 2;
        _cy = ih / 2;
    }
    _zoomed = YES;
    _scale = ns;
    [self updateTitle];
    self.needsDisplay = YES;
}

- (void)zoomFit {
    _zoomed = NO;
    [self updateTitle];
    self.needsDisplay = YES;
}

- (void)zoomBy:(double)f image:(Decoded *)d view:(CGSize)ds {
    double fit = [self fitScale:d view:ds];
    double cur = _zoomed ? _scale : fit;
    double ns = cur * f;
    // stop exactly at 100% and at "fit" when a step crosses them
    for (int k = 0; k < 2; k++) {
        double snap = k == 0 ? 1.0 : fit;
        if ((cur < snap - 1e-9 && ns > snap + 1e-9) || (cur > snap + 1e-9 && ns < snap - 1e-9)) {
            ns = snap;
            break;
        }
    }
    ns = MAX(MIN(fit, 1.0) / 4, MIN(ns, 32.0));
    if (fabs(ns - 1.0) < 1e-6) ns = 1.0;        // e.g. sqrt(2) * sqrt(1/2)
    if (fabs(ns - fit) < 1e-6 * fit) ns = fit;
    if (fabs(ns - fit) < 1e-9 && fabs(ns - 1.0) > 1e-9) { [self zoomFit]; return; }
    [self zoomTo:ns image:d];
}

- (void)panX:(double)fx y:(double)fy image:(Decoded *)d view:(CGSize)ds {   // fractions of the view size
    if (!_zoomed) return;
    _cx += fx * ds.width / _scale;
    _cy += fy * ds.height / _scale;
    [self clampCenter:d scale:_scale view:ds];
    self.needsDisplay = YES;
}

// Where to draw the image: NDC corner + extent (top-left snapped to device pixels).
- (DrawParams)drawParams:(Decoded *)d view:(CGSize)ds {
    double iw, ih;
    display_size(d, &iw, &ih);
    double s = [self currentScale:d view:ds];
    if (_zoomed) [self clampCenter:d scale:s view:ds];
    double cx = _zoomed ? _cx : iw / 2, cy = _zoomed ? _cy : ih / 2;
    double x0 = round(ds.width / 2 - cx * s), y0 = round(ds.height / 2 - cy * s);
    DrawParams p = {{(float)(x0 / ds.width * 2 - 1), (float)(1 - y0 / ds.height * 2)},
                    {(float)(2 * iw * s / ds.width), (float)(2 * ih * s / ds.height)},
                    (uint32_t)d.orientation, 0};
    return p;
}

// Test hook: apply one key action ("+", "-", "0", "1", "L", "R", "U", "D") without events.
- (void)testKey:(char)k image:(Decoded *)d view:(CGSize)ds {
    switch (k) {
    case '+': [self zoomBy:M_SQRT2 image:d view:ds]; break;
    case '-': [self zoomBy:M_SQRT1_2 image:d view:ds]; break;
    case '0': [self zoomFit]; break;
    case '1': [self zoomTo:1.0 image:d]; break;
    case 'L': [self panX:-0.125 y:0 image:d view:ds]; break;
    case 'R': [self panX:0.125 y:0 image:d view:ds]; break;
    case 'U': [self panX:0 y:-0.125 image:d view:ds]; break;
    case 'D': [self panX:0 y:0.125 image:d view:ds]; break;
    }
}
- (double)testScale:(Decoded *)d view:(CGSize)ds { return [self currentScale:d view:ds]; }
- (BOOL)testZoomed { return _zoomed; }

- (void)keyDown:(NSEvent *)e {
    // zoom keys by character, so they work on any keyboard layout and the keypad
    NSString *ch = e.charactersIgnoringModifiers;
    Decoded *cur = [_loader get:_index];
    if (ch.length == 1) {
        switch ([ch characterAtIndex:0]) {
        case '+': case '=': if (cur) [self zoomBy:M_SQRT2 image:cur view:self.drawableSize]; return;
        case '-': case '_': if (cur) [self zoomBy:M_SQRT1_2 image:cur view:self.drawableSize]; return;
        case '0': [self zoomFit]; return;
        case '1': if (cur) [self zoomTo:1.0 image:cur]; return;
        default: break;
        }
    }
    double step = (e.modifierFlags & NSEventModifierFlagShift) ? 0.5 : 0.125;
    CGSize ds = self.drawableSize;
    switch (e.keyCode) {
    case 123: if (cur) [self panX:-step y:0 image:cur view:ds]; break;             // Left
    case 124: if (cur) [self panX:step y:0 image:cur view:ds]; break;              // Right
    case 126: if (cur) [self panX:0 y:-step image:cur view:ds]; break;             // Up
    case 125: if (cur) [self panX:0 y:step image:cur view:ds]; break;              // Down
    case 121: case 49: [self go:_index + 1 dir:1]; break;                           // PgDn Space
    case 116: case 51: [self go:_index - 1 dir:-1]; break;                          // PgUp Backspace
    case 115: [self go:0 dir:1]; break;                                            // Home
    case 119: [self go:(NSInteger)_files.count - 1 dir:-1]; break;                // End
    case 3: case 36: [self.window toggleFullScreen:nil]; break;                    // F Return
    case 53:                                                                       // Esc
        if (self.window.styleMask & NSWindowStyleMaskFullScreen) [self.window toggleFullScreen:nil];
        else [NSApp terminate:nil];
        break;
    case 12: [NSApp terminate:nil]; break;                                         // Q
    case 35:                                                                       // P: pause slideshow
        if (_slideMs > 0) {
            _paused = !_paused;
            _slideGen++;
            _shownTime = now_ms();
            if (!_paused) [self scheduleSlide];
            [self updateTitle];
        }
        break;
    default: [super keyDown:e];
    }
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    view.needsDisplay = YES;
    [self updateTitle];   // fit percentage changes with the window size
}

- (void)drawInMTKView:(MTKView *)view {
    MTLRenderPassDescriptor *rpd = view.currentRenderPassDescriptor;
    if (!rpd) return;
    rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
    id<MTLCommandBuffer> cb = [_gpu.queue commandBuffer];
    Decoded *d = [_loader get:_index];
    id<MTLTexture> tex = d ? [_gpu textureFor:d commandBuffer:cb] : nil;
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rpd];
    if (tex) {
        if (_shownIndex != _index) {
            _shownIndex = _index;
            _shownTime = now_ms();
            [self scheduleSlide];
        }
        DrawParams p = [self drawParams:d view:view.drawableSize];
        [re setRenderPipelineState:_gpu.draw];
        [re setVertexBytes:&p length:sizeof p atIndex:0];
        [re setFragmentTexture:tex atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [re endEncoding];
    if (tex && !_reported) {
        _reported = YES;
        double req = _requestTime;
        NSInteger idx = _index;
        NSString *name = _files[idx].lastPathComponent;
        BOOL wasCached = _wasReady;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
            double total = now_ms() - req;
            printf("[%3ld] %-40s %5dx%-5d read %5.1f ms  decode %6.1f ms (%s, %2d bands)  key->on screen %6.1f ms%s\n",
                   idx + 1, name.UTF8String, d.width, d.height, d.read_ms, d.decode_ms,
                   d.mode == 2 ? "split " : d.mode == 1 ? "RST   " : d.mode == 0 ? "single" : "tj    ", d.bands, total,
                   wasCached ? "  (prefetched)" : "");
            fflush(stdout);
        }];
    }
    [cb presentDrawable:view.currentDrawable];
    [cb commit];
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic) NSWindow *window;
@end
@implementation AppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)s { return YES; }
@end

// ---------------------------------------------------------------------------

static NSArray<NSString *> *collect_files(NSArray<NSString *> *args) {
    NSMutableArray *out = [NSMutableArray array];
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *a in args) {
        BOOL dir = NO;
        if (![fm fileExistsAtPath:a isDirectory:&dir]) { fprintf(stderr, "no such file: %s\n", a.UTF8String); continue; }
        if (!dir) { [out addObject:a]; continue; }
        NSArray *items = [[fm contentsOfDirectoryAtPath:a error:nil] sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
        for (NSString *f in items) {
            NSString *ext = f.pathExtension.lowercaseString;
            if ([ext isEqualToString:@"jpg"] || [ext isEqualToString:@"jpeg"] || [ext isEqualToString:@"jpe"])
                [out addObject:[a stringByAppendingPathComponent:f]];
        }
    }
    return out;
}

static int run_bench(NSArray<NSString *> *files, GPU *gpu) {
    BufferPool *pool = gpu.pool;
    double tr = 0, td = 0, tg = 0, t0 = now_ms();
    for (NSString *f in files) {
        @autoreleasepool {
            Decoded *d = decode_file(f, pool, gpu.align);
            if (!d) { printf("FAILED %s\n", f.UTF8String); continue; }
            double g0 = now_ms();
            id<MTLCommandBuffer> cb = [gpu.queue commandBuffer];
            [gpu textureFor:d commandBuffer:cb];
            [cb commit];
            [cb waitUntilCompleted];
            double g = now_ms() - g0;
            printf("%-45s %5dx%-5d read %5.1f  decode %6.1f  gpu %5.1f ms  (%s)\n", f.lastPathComponent.UTF8String, d.width,
                   d.height, d.read_ms, d.decode_ms, g, d.mode == 2 ? "split" : d.mode == 1 ? "RST" : d.mode == 0 ? "single" : "tj");
            tr += d.read_ms; td += d.decode_ms; tg += g;
            [pool putTexture:d.texture];
            d.texture = nil;
            [pool put:d.buffer];
        }
    }
    double n = files.count;
    printf("\n%lu files: read %.1f ms (avg %.1f), decode %.1f ms (avg %.1f), gpu upload+convert %.1f ms (avg %.1f)\n"
           "total %.1f ms, avg %.1f ms/image\n",
           files.count, tr, tr / n, td, td / n, tg, tg / n, now_ms() - t0, (now_ms() - t0) / n);
    return 0;
}

#ifdef NV_TURBOJPEG
// Compare GPU conversion output with libjpeg-turbo's BGRX decode.
static int run_selftest(NSArray<NSString *> *files, GPU *gpu) {
    BufferPool *pool = gpu.pool;
    int bad = 0;
    for (NSString *f in files) {
        @autoreleasepool {
            Decoded *d = decode_file(f, pool, gpu.align);
            if (!d) { printf("FAILED %s\n", f.UTF8String); bad++; continue; }
            id<MTLCommandBuffer> cb = [gpu.queue commandBuffer];
            id<MTLTexture> tex = [gpu textureFor:d commandBuffer:cb];
            size_t rp = (size_t)d.width * 4;
            id<MTLBuffer> rb = [gpu.device newBufferWithLength:rp * d.height options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(d.width, d.height, 1) toBuffer:rb destinationOffset:0
            destinationBytesPerRow:rp destinationBytesPerImage:rp * d.height];
            [be endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            NSData *data = read_file(f);
            uint8_t *ref = malloc(rp * d.height);
            tjhandle h = tj3Init(TJINIT_DECOMPRESS);
            tj3Decompress8(h, data.bytes, data.length, ref, (int)rp, TJPF_RGBX);
            tj3Destroy(h);
            const uint8_t *g = rb.contents;
            long hist[256] = {0};
            int maxd = 0;
            double sum = 0;
            size_t cnt = 0;
            for (size_t k = 0; k < rp * d.height; k++) {
                if ((k & 3) == 3) continue;
                int e = abs((int)g[k] - ref[k]);
                hist[e]++;
                sum += e;
                cnt++;
                if (e > maxd) maxd = e;
            }
            printf("%-45s max diff %3d  mean %.3f  <=1: %.4f%%\n", f.lastPathComponent.UTF8String, maxd, sum / cnt,
                   100.0 * (hist[0] + hist[1]) / cnt);
            if (maxd > 8) bad++;
            free(ref);
            d.texture = nil;
            [pool put:d.buffer];
        }
    }
    printf(bad ? "SELFTEST: %d problem(s)\n" : "SELFTEST OK\n", bad);
    return bad != 0;
}
#else
static int run_selftest(NSArray<NSString *> *files, GPU *gpu) {
    fprintf(stderr, "--selftest needs libjpeg-turbo as reference: build with TURBOJPEG=1\n");
    return 2;
}
#endif

// Offscreen check of zoom/pan: replays key sequences through the viewer's own
// code, renders into a texture and, at 100%, compares every drawn pixel with
// the decoded image.
static int run_zoomtest(NSArray<NSString *> *files, GPU *gpu) {
    const CGSize ds = {1600, 1000};
    const int W = (int)ds.width, H = (int)ds.height;
    ViewerView *v = [[ViewerView alloc] initWithFrame:NSMakeRect(0, 0, 800, 500) device:gpu.device];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> target = [gpu.device newTextureWithDescriptor:td];
    id<MTLBuffer> out = [gpu.device newBufferWithLength:(size_t)W * H * 4 options:MTLResourceStorageModeShared];
    const char *scen[] = {"1", "1RRRDD", "1RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD",
                          "1UUUUUUUUUUUUUUUUUUUUUUUUUUUULLLLLLLLLLLLLLLLLLLLLLLLLLLLL", "1+-", "1-+", "+", "++--", "0", NULL};
    int bad = 0;
    for (NSString *f in files) {
        @autoreleasepool {
            Decoded *d = decode_file(f, gpu.pool, gpu.align);
            if (!d) continue;
            id<MTLCommandBuffer> cb = [gpu.queue commandBuffer];
            id<MTLTexture> tex = [gpu textureFor:d commandBuffer:cb];
            size_t rp = (size_t)d.width * 4;
            id<MTLBuffer> ref = [gpu.device newBufferWithLength:rp * d.height options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(d.width, d.height, 1) toBuffer:ref destinationOffset:0
            destinationBytesPerRow:rp destinationBytesPerImage:rp * d.height];
            [be endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            // scale sequence of repeated "+" from fit: must hit 100% exactly
            [v testKey:'0' image:d view:ds];
            NSMutableString *seq = [NSMutableString string];
            BOOL hit100 = NO;
            for (int k = 0; k < 16; k++) {
                [v testKey:'+' image:d view:ds];
                double sc = [v testScale:d view:ds];
                if (sc == 1.0) hit100 = YES;
                if (k < 8) [seq appendFormat:@"%.0f%% ", sc * 100];
            }
            printf("%-24s %5dx%-5d orient %d  '+' steps: %s... %s\n", f.lastPathComponent.UTF8String, d.width,
                   d.height, d.orientation, seq.UTF8String, hit100 ? "hits 100%" : "MISSES 100%");
            if (!hit100) bad++;
            for (int si = 0; scen[si]; si++) {
                [v testKey:'0' image:d view:ds];
                for (const char *k = scen[si]; *k; k++) [v testKey:*k image:d view:ds];
                DrawParams p = [v drawParams:d view:ds];
                double sc = [v testScale:d view:ds];
                id<MTLCommandBuffer> c2 = [gpu.queue commandBuffer];
                MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
                rpd.colorAttachments[0].texture = target;
                rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
                rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
                rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> re = [c2 renderCommandEncoderWithDescriptor:rpd];
                [re setRenderPipelineState:gpu.draw];
                [re setVertexBytes:&p length:sizeof p atIndex:0];
                [re setFragmentTexture:tex atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                [re endEncoding];
                id<MTLBlitCommandEncoder> b2 = [c2 blitCommandEncoder];
                [b2 copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                         sourceSize:MTLSizeMake(W, H, 1) toBuffer:out destinationOffset:0
                destinationBytesPerRow:(size_t)W * 4 destinationBytesPerImage:(size_t)W * H * 4];
                [b2 endEncoding];
                [c2 commit];
                [c2 waitUntilCompleted];
                // image rectangle in target pixels
                double x0 = (p.origin[0] + 1) / 2 * W, y0 = (1 - p.origin[1]) / 2 * H;
                double wpx = p.size[0] / 2 * W, hpx = p.size[1] / 2 * H;
                long ix0 = lround(x0), iy0 = lround(y0);
                const uint8_t *o = out.contents, *r = ref.contents;
                long checked = 0, diff = 0;
                if (sc == 1.0 && d.orientation == 1) {
                    for (int y = 0; y < H; y++)
                        for (int x = 0; x < W; x++) {
                            long sx = x - ix0, sy = y - iy0;
                            if (sx < 0 || sy < 0 || sx >= d.width || sy >= d.height) continue;
                            const uint8_t *a = o + ((size_t)y * W + x) * 4, *b = r + (size_t)sy * rp + (size_t)sx * 4;
                            checked++;
                            if (a[2] != b[0] || a[1] != b[1] || a[0] != b[2]) diff++;   // BGRA vs RGBA
                        }
                }
                BOOL inside = x0 <= 0.5 || x0 + wpx >= W - 0.5 || fabs(x0 - (W - wpx) / 2) < 1;   // fills or centred
                BOOL insidey = y0 <= 0.5 || y0 + hpx >= H - 0.5 || fabs(y0 - (H - hpx) / 2) < 1;
                BOOL covers = (wpx < W || (x0 <= 0.5 && x0 + wpx >= W - 0.5)) && (hpx < H || (y0 <= 0.5 && y0 + hpx >= H - 0.5));
                BOOL ok = inside && insidey && covers && diff == 0;
                printf("   %-10.10s%s scale %6.1f%%  image at (%6.0f,%6.0f) %6.0fx%-6.0f %s%s\n", scen[si],
                       strlen(scen[si]) > 10 ? "…" : " ", sc * 100, x0, y0, wpx, hpx,
                       checked ? [NSString stringWithFormat:@"1:1 check %ld px, %ld differ  ", checked, diff].UTF8String : "",
                       ok ? "OK" : "FAIL");
                if (!ok) bad++;
            }
            [gpu.pool putTexture:d.texture];
            d.texture = nil;
            [gpu.pool put:d.buffer];
        }
    }
    printf(bad ? "ZOOMTEST: %d problem(s)\n" : "ZOOMTEST OK\n", bad);
    return bad != 0;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        BOOL full = NO, bench = NO, selftest = NO, zoomtest = NO;
        double auto_ms = 0, slide_ms = 0;
        NSMutableArray *args = [NSMutableArray array];
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--fullscreen")) full = YES;
            else if (!strcmp(argv[i], "--bench")) bench = YES;
            else if (!strcmp(argv[i], "--selftest")) selftest = YES;
            else if (!strcmp(argv[i], "--zoomtest")) zoomtest = YES;
            else if (!strcmp(argv[i], "--auto") && i + 1 < argc) auto_ms = atof(argv[++i]);   // page every N ms, then quit
            else if (!strcmp(argv[i], "-j") && i + 1 < argc) nj_set_max_workers(atoi(argv[++i]));   // decoder threads
            else if ((!strcmp(argv[i], "-s") || !strcmp(argv[i], "--slideshow")) && i + 1 < argc) slide_ms = atof(argv[++i]);
            else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { args = nil; break; }
            else [args addObject:@(argv[i])];
        }
        NSArray *files = args ? collect_files(args) : @[];
        if (!files.count) {
            fprintf(stderr,
                "usage: nitroview [options] file.jpg|directory ...\n"
                "\n"
                "options:\n"
                "  -f, --fullscreen      start in full screen\n"
                "  -s, --slideshow MS    slideshow: next image after MS milliseconds on screen\n"
                "                        (waits for slow images, stops at the last one; P pauses)\n"
                "  -j N                  use N decoder threads (default: all logical CPUs)\n"
                "  --bench               load all files without a window, print timings\n"
                "  --selftest            compare GPU colour conversion with libjpeg-turbo%s\n"
                "  -h, --help            this help\n"
                "\n"
                "keys:\n"
                "  PgDn Space              next image         PgUp Backspace           previous\n"
                "  Home / End              first / last       F / Enter                full screen\n"
                "  + / -                   zoom in / out      0 fit to window          1 actual size (1:1)\n"
                "  arrows                  move a zoomed image (Shift: bigger steps)\n"
                "  P                       pause/resume slideshow\n"
                "  Esc                     leave full screen / quit                Q   quit\n",
#ifdef NV_TURBOJPEG
                ""
#else
                " (not in this build)"
#endif
            );
            return 1;
        }
        const char *stg = getenv("NV_STORAGE");
        if (stg && !strcmp(stg, "managed")) g_storage = MTLResourceStorageModeManaged;
        if (getenv("NV_TEXTURES")) g_use_textures = YES, g_storage = MTLResourceStorageModeManaged;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 1; }
        GPU *gpu = [[GPU alloc] initWithDevice:dev];
        gpu.pool = [[BufferPool alloc] initWithDevice:dev];
        if (bench) return run_bench(files, gpu);
        if (selftest) return run_selftest(files, gpu);
        if (zoomtest) return run_zoomtest(files, gpu);

        NSApplication *app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate *del = [AppDelegate new];
        app.delegate = del;

        NSMenu *bar = [NSMenu new], *appMenu = [NSMenu new];
        NSMenuItem *item = [NSMenuItem new];
        [bar addItem:item];
        [appMenu addItemWithTitle:@"Quit nitroview" action:@selector(terminate:) keyEquivalent:@"q"];
        item.submenu = appMenu;
        app.mainMenu = bar;

        NSRect scr = NSScreen.mainScreen.visibleFrame;
        NSRect frame = NSInsetRect(scr, scr.size.width * 0.08, scr.size.height * 0.08);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:frame
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                              NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        win.backgroundColor = NSColor.blackColor;
        del.window = win;

        ViewerView *v = [[ViewerView alloc] initWithFrame:frame device:dev];
        v.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        v.paused = YES;
        v.enableSetNeedsDisplay = YES;
        v.delegate = v;
        v.files = files;
        v.gpu = gpu;
        BufferPool *pool = gpu.pool;
        v.loader = [[Loader alloc] initWithFiles:files pool:pool align:gpu.align];
        __weak ViewerView *wv = v;
        v.loader.onDecoded = ^(NSInteger i) { [wv imageDecoded:i]; };
        win.contentView = v;
        [win makeFirstResponder:v];
        [win makeKeyAndOrderFront:nil];
        [v go:0 dir:1];
        if (full) [win toggleFullScreen:nil];
        if (slide_ms > 0) [v startSlideshow:slide_ms];
        if (auto_ms > 0) {
            __block NSInteger shown = 1;
            [NSTimer scheduledTimerWithTimeInterval:auto_ms / 1000.0 repeats:YES block:^(NSTimer *t) {
                if (shown++ >= (NSInteger)files.count) { [NSApp terminate:nil]; return; }
                [v go:v.index + 1 dir:1];
            }];
        }
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
