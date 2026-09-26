// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// JPEG decoder benchmark: decodes every file given on the command line with
// several decoders and reports per-decoder total / average time.
// All files are read into memory first, so only decode time is measured.
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#import <VideoToolbox/VideoToolbox.h>
#include <turbojpeg.h>
#include <mach/mach_time.h>
#include <dispatch/dispatch.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include "../third_party/stb_image.h"
#include "../src/nitrojpeg.h"

#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__JPEG
#include "../third_party/wuffs.c"

typedef struct { const char *name; NSData *data; } File;

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static volatile uint64_t g_sink;   // prevents the optimizer from dropping work
static void touch(const uint8_t *p, size_t n) { g_sink += p[0] + p[n / 2] + p[n - 1]; }

// ---------- libjpeg-turbo ----------
static int tj_decode(NSData *d, int flags, int scale_denom, int yuv) {
    tjhandle h = tj3Init(TJINIT_DECOMPRESS);
    tj3Set(h, TJPARAM_FASTDCT, (flags & 1) != 0);
    tj3Set(h, TJPARAM_FASTUPSAMPLE, (flags & 2) != 0);
    if (tj3DecompressHeader(h, d.bytes, d.length) < 0) { tj3Destroy(h); return -1; }
    int w = tj3Get(h, TJPARAM_JPEGWIDTH), ht = tj3Get(h, TJPARAM_JPEGHEIGHT);
    int rc;
    if (yuv) {
        size_t sz = tj3YUVBufSize(w, 1, ht, tj3Get(h, TJPARAM_SUBSAMP));
        uint8_t *buf = malloc(sz);
        rc = tj3DecompressToYUV8(h, d.bytes, d.length, buf, 1);
        touch(buf, sz); free(buf);
    } else {
        tjscalingfactor sf = { 1, scale_denom };
        tj3SetScalingFactor(h, sf);
        int sw = TJSCALED(w, sf), sh = TJSCALED(ht, sf);
        size_t sz = (size_t)sw * sh * 4;
        uint8_t *buf = malloc(sz);
        rc = tj3Decompress8(h, d.bytes, d.length, buf, sw * 4, TJPF_BGRX);
        touch(buf, sz); free(buf);
    }
    tj3Destroy(h);
    return rc;
}
static int tj_full(NSData *d)  { return tj_decode(d, 0, 1, 0); }
static int tj_fast(NSData *d)  { return tj_decode(d, 3, 1, 0); }
static int tj_yuv(NSData *d)   { return tj_decode(d, 0, 1, 1); }
static int tj_half(NSData *d)  { return tj_decode(d, 0, 2, 0); }
static int tj_quarter(NSData *d) { return tj_decode(d, 0, 4, 0); }

// ---------- stb_image ----------
static int stb(NSData *d) {
    int w, h, c;
    uint8_t *p = stbi_load_from_memory(d.bytes, (int)d.length, &w, &h, &c, 4);
    if (!p) return -1;
    touch(p, (size_t)w * h * 4); stbi_image_free(p);
    return 0;
}

// ---------- Wuffs ----------
static int wuffs(NSData *d) {
    wuffs_jpeg__decoder *dec = wuffs_jpeg__decoder__alloc();
    wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader((uint8_t *)d.bytes, d.length, true);
    wuffs_base__image_config ic = {0};
    if (wuffs_jpeg__decoder__decode_image_config(dec, &ic, &src).repr) { free(dec); return -1; }
    uint32_t w = wuffs_base__pixel_config__width(&ic.pixcfg), h = wuffs_base__pixel_config__height(&ic.pixcfg);
    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL,
                                  WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, w, h);
    size_t sz = (size_t)w * h * 4;
    uint8_t *pix = malloc(sz);
    wuffs_base__range_ii_u64 wr = wuffs_jpeg__decoder__workbuf_len(dec);
    uint8_t *work = malloc(wr.max_incl ? wr.max_incl : 1);
    wuffs_base__pixel_buffer pb = {0};
    wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(pix, sz));
    wuffs_base__status st = wuffs_jpeg__decoder__decode_frame(dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC,
                                                              wuffs_base__make_slice_u8(work, wr.max_incl), NULL);
    touch(pix, sz);
    free(work); free(pix); free(dec);
    return st.repr ? -1 : 0;
}

// ---------- Apple ImageIO ----------
static int imageio_full(NSData *d) {
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)d, NULL);
    NSDictionary *o = @{(id)kCGImageSourceShouldCacheImmediately: @YES};
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, (__bridge CFDictionaryRef)o);
    size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
    size_t sz = w * h * 4;
    uint8_t *buf = malloc(sz);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(buf, w, h, 8, w * 4, cs,
                                             kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGContextSetBlendMode(ctx, kCGBlendModeCopy);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
    touch(buf, sz);
    CGContextRelease(ctx); CGColorSpaceRelease(cs); CGImageRelease(img); CFRelease(src); free(buf);
    return 0;
}
// ImageIO decode only (its own internal buffer, whatever format it likes)
static int imageio_raw(NSData *d) {
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)d, NULL);
    NSDictionary *o = @{(id)kCGImageSourceShouldCacheImmediately: @YES};
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, (__bridge CFDictionaryRef)o);
    CFDataRef px = CGDataProviderCopyData(CGImageGetDataProvider(img));
    touch(CFDataGetBytePtr(px), CFDataGetLength(px));
    CFRelease(px); CGImageRelease(img); CFRelease(src);
    return 0;
}
static int imageio_thumb(NSData *d) {
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)d, NULL);
    NSDictionary *o = @{(id)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
                        (id)kCGImageSourceThumbnailMaxPixelSize: @3000,
                        (id)kCGImageSourceShouldCacheImmediately: @YES};
    CGImageRef img = CGImageSourceCreateThumbnailAtIndex(src, 0, (__bridge CFDictionaryRef)o);
    CFDataRef px = CGDataProviderCopyData(CGImageGetDataProvider(img));
    touch(CFDataGetBytePtr(px), CFDataGetLength(px));
    CFRelease(px); CGImageRelease(img); CFRelease(src);
    return 0;
}

// ---------- VideoToolbox (hardware JPEG if available) ----------
static int vt(NSData *d) {
    tjhandle h = tj3Init(TJINIT_DECOMPRESS);
    tj3DecompressHeader(h, d.bytes, d.length);
    int w = tj3Get(h, TJPARAM_JPEGWIDTH), ht = tj3Get(h, TJPARAM_JPEGHEIGHT);
    tj3Destroy(h);
    CMVideoFormatDescriptionRef fmt = NULL;
    CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_JPEG, w, ht, NULL, &fmt);
    NSDictionary *spec = @{(id)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder: @YES};
    NSDictionary *attrs = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
    VTDecompressionSessionRef s = NULL;
    OSStatus e = VTDecompressionSessionCreate(NULL, fmt, (__bridge CFDictionaryRef)spec,
                                              (__bridge CFDictionaryRef)attrs, NULL, &s);
    if (e) { static int once; if (!once++) fprintf(stderr, "VT create err %d\n", (int)e); CFRelease(fmt); return e; }
    CMBlockBufferRef bb = NULL;
    CMBlockBufferCreateWithMemoryBlock(NULL, (void *)d.bytes, d.length, kCFAllocatorNull, NULL, 0, d.length, 0, &bb);
    CMSampleBufferRef sb = NULL;
    size_t ssz = d.length;
    CMSampleBufferCreateReady(NULL, bb, fmt, 1, 0, NULL, 1, &ssz, &sb);
    __block int rc = 0;
    e = VTDecompressionSessionDecodeFrameWithOutputHandler(s, sb, 0, NULL,
        ^(OSStatus st, VTDecodeInfoFlags f, CVImageBufferRef ib, CMTime pts, CMTime dur) {
            if (st || !ib) { rc = st ? st : -1; return; }
            CVPixelBufferLockBaseAddress(ib, kCVPixelBufferLock_ReadOnly);
            touch(CVPixelBufferGetBaseAddress(ib), CVPixelBufferGetDataSize(ib));
            CVPixelBufferUnlockBaseAddress(ib, kCVPixelBufferLock_ReadOnly);
        });
    VTDecompressionSessionWaitForAsynchronousFrames(s);
    if (e) rc = e; { static int o2; if (!o2++) fprintf(stderr, "VT decode err %d rc %d\n", (int)e, rc); }
    CFRelease(sb); CFRelease(bb);
    VTDecompressionSessionInvalidate(s); CFRelease(s); CFRelease(fmt);
    return rc;
}

// ---------- nitrojpeg (parallel single-image decode) ----------
static int nj_planes_n(NSData *d, int nt) {
    nj_info fi;
    if (nj_read_info(d.bytes, d.length, &fi) || !fi.supported) return -1;
    uint8_t *pl[3] = {0}; size_t pitch[3] = {0}, tot = 0;
    for (int c = 0; c < fi.ncomp; c++) { pitch[c] = (fi.plane_w[c] + 255) & ~255; tot += pitch[c] * fi.plane_h[c]; }
    uint8_t *buf = malloc(tot), *p = buf;
    for (int c = 0; c < fi.ncomp; c++) { pl[c] = p; p += pitch[c] * fi.plane_h[c]; }
    int rc = nj_decode_planes(d.bytes, d.length, &fi, pl, pitch, nt, NULL);
    touch(buf, tot); free(buf);
    return rc;
}
static int nj_nt(void) { const char *e = getenv("NJ_THREADS"); return e ? atoi(e) : 0; }
static int nj_planes(NSData *d)  { return nj_planes_n(d, nj_nt()); }
static int nj_planes1(NSData *d) { return nj_planes_n(d, 1); }
static int nj_bgrx(NSData *d) {
    nj_info fi;
    if (nj_read_info(d.bytes, d.length, &fi) || !fi.supported) return -1;
    size_t sz = (size_t)fi.width * fi.height * 4;
    uint8_t *buf = malloc(sz);
    int rc = nj_decode_bgrx(d.bytes, d.length, &fi, buf, fi.width * 4, nj_nt(), NULL);
    touch(buf, sz); free(buf);
    return rc;
}

typedef struct { const char *name; int (*fn)(NSData *); } Decoder;
static Decoder decoders[] = {
    {"libjpeg-turbo  full BGRX",          tj_full},
    {"libjpeg-turbo  full, fastDCT+ups",  tj_fast},
    {"libjpeg-turbo  full -> YUV planes", tj_yuv},
    {"libjpeg-turbo  1/2 scale BGRX",     tj_half},
    {"libjpeg-turbo  1/4 scale BGRX",     tj_quarter},
    {"stb_image      full RGBA",          stb},
    {"wuffs          full BGRA",          wuffs},
    {"ImageIO        full -> BGRA ctx",   imageio_full},
    {"ImageIO        full native buf",    imageio_raw},
    {"ImageIO        thumb 3000px",       imageio_thumb},
    {"VideoToolbox   JPEG -> BGRA",       vt},
    {"nitrojpeg       1 thread -> YUV",    nj_planes1},
    {"nitrojpeg       parallel -> YUV",    nj_planes},
    {"nitrojpeg       parallel -> BGRX",   nj_bgrx},
};

int main(int argc, const char **argv) {
    @autoreleasepool {
        const char *only = getenv("ONLY");
        int nthreads = getenv("THREADS") ? atoi(getenv("THREADS")) : 0;
        if (getenv("NJ_WORKERS")) nj_set_max_workers(atoi(getenv("NJ_WORKERS")));
        if (getenv("NJ_BANDS")) nj_set_bands(atoi(getenv("NJ_BANDS")));
        if (getenv("NJ_SEQ")) nj_set_sequential_scan(atoi(getenv("NJ_SEQ")));
        if (getenv("NJ_ENGINE")) nj_set_engine(atoi(getenv("NJ_ENGINE")));
        if (getenv("NJ_SCALAR")) nj_set_scalar_idct(1);
        if (getenv("NJ_CHUNKS")) nj_set_chunks(atoi(getenv("NJ_CHUNKS")));
        NSMutableArray *files = [NSMutableArray array];
        double t0 = now_ms(); size_t bytes = 0;
        for (int i = 1; i < argc; i++) {
            NSData *d = [NSData dataWithContentsOfFile:@(argv[i])];
            if (d) { [files addObject:d]; bytes += d.length; }
        }
        printf("%lu files, %.1f MB, read in %.1f ms\n\n", files.count, bytes / 1e6, now_ms() - t0);
        int n = (int)files.count;
        int repeat = getenv("REPEAT") ? atoi(getenv("REPEAT")) : 1;
        for (int rep = 0; rep < repeat; rep++)
        for (size_t k = 0; k < sizeof decoders / sizeof *decoders; k++) {
            Decoder dc = decoders[k];
            if (only && !strstr(dc.name, only)) continue;
            @autoreleasepool { dc.fn(files[0]); }  // warm-up
            __block int fails = 0;
            __block double worst = 0;
            double start = now_ms();
            if (nthreads > 0) {
                dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
                dispatch_semaphore_t sem = dispatch_semaphore_create(nthreads);
                dispatch_group_t g = dispatch_group_create();
                for (int i = 0; i < n; i++) {
                    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
                    dispatch_group_async(g, q, ^{
                        @autoreleasepool { if (dc.fn(files[i])) __sync_fetch_and_add(&fails, 1); }
                        dispatch_semaphore_signal(sem);
                    });
                }
                dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
            } else {
                for (int i = 0; i < n; i++) {
                    double a = now_ms();
                    @autoreleasepool { if (dc.fn(files[i])) fails++; }
                    double t = now_ms() - a; if (t > worst) worst = t;
                }
            }
            double total = now_ms() - start;
            printf("%-36s total %8.1f ms   avg %6.1f ms/img   worst %6.1f ms%s\n", dc.name, total, total / n,
                   worst, fails ? [NSString stringWithFormat:@"   FAILED: %d", fails].UTF8String : "");
            fflush(stdout);
        }
    }
    return 0;
}
