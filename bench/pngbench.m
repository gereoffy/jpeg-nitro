// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// PNG decoder comparison: Apple ImageIO vs Wuffs, files preloaded into memory.
//   pngbench files.png...
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#include <mach/mach_time.h>

#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__ZLIB
#define WUFFS_CONFIG__MODULE__PNG
#include "../third_party/wuffs.c"

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}
static volatile uint64_t g_sink;
static void touch(const uint8_t *p, size_t n) { g_sink += p[0] + p[n / 2] + p[n - 1]; }

static int imageio_raw(NSData *d) {
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)d, NULL);
    NSDictionary *o = @{(id)kCGImageSourceShouldCacheImmediately: @YES};
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, (__bridge CFDictionaryRef)o);
    if (!img) { CFRelease(src); return -1; }
    CFDataRef px = CGDataProviderCopyData(CGImageGetDataProvider(img));
    touch(CFDataGetBytePtr(px), CFDataGetLength(px));
    CFRelease(px); CGImageRelease(img); CFRelease(src);
    return 0;
}

static int imageio_bgra(NSData *d) {
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)d, NULL);
    NSDictionary *o = @{(id)kCGImageSourceShouldCacheImmediately: @YES};
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, (__bridge CFDictionaryRef)o);
    if (!img) { CFRelease(src); return -1; }
    size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img), sz = w * h * 4;
    uint8_t *buf = malloc(sz);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(buf, w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGContextSetBlendMode(ctx, kCGBlendModeCopy);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
    touch(buf, sz);
    CGContextRelease(ctx); CGColorSpaceRelease(cs); CGImageRelease(img); CFRelease(src); free(buf);
    return 0;
}

static int wuffs_png(NSData *d) {
    wuffs_png__decoder *dec = wuffs_png__decoder__alloc();
    wuffs_png__decoder__set_quirk(dec, WUFFS_BASE__QUIRK_IGNORE_CHECKSUM, 1);   // like libpng's fast path
    wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader((uint8_t *)d.bytes, d.length, true);
    wuffs_base__image_config ic = {0};
    if (wuffs_png__decoder__decode_image_config(dec, &ic, &src).repr) { free(dec); return -1; }
    uint32_t w = wuffs_base__pixel_config__width(&ic.pixcfg), h = wuffs_base__pixel_config__height(&ic.pixcfg);
    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL,
                                  WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, w, h);
    size_t sz = (size_t)w * h * 4;
    uint8_t *pix = malloc(sz);
    wuffs_base__range_ii_u64 wr = wuffs_png__decoder__workbuf_len(dec);
    uint8_t *work = malloc(wr.max_incl ? wr.max_incl : 1);
    wuffs_base__pixel_buffer pb = {0};
    wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(pix, sz));
    wuffs_base__status st = wuffs_png__decoder__decode_frame(dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC,
                                                             wuffs_base__make_slice_u8(work, wr.max_incl), NULL);
    touch(pix, sz);
    free(work); free(pix); free(dec);
    return st.repr ? -1 : 0;
}

typedef struct { const char *name; int (*fn)(NSData *); } Decoder;
static Decoder decoders[] = {
    {"ImageIO   native buffer", imageio_raw},
    {"ImageIO   -> BGRA ctx",   imageio_bgra},
    {"Wuffs     -> BGRA",       wuffs_png},
};

int main(int argc, const char **argv) {
    @autoreleasepool {
        NSMutableArray *files = [NSMutableArray array];
        size_t bytes = 0, pixels = 0;
        for (int i = 1; i < argc; i++) {
            NSData *d = [NSData dataWithContentsOfFile:@(argv[i])];
            if (!d) continue;
            [files addObject:d];
            bytes += d.length;
            CGImageSourceRef s = CGImageSourceCreateWithData((__bridge CFDataRef)d, NULL);
            NSDictionary *p = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(s, 0, NULL));
            pixels += [p[(id)kCGImagePropertyPixelWidth] longValue] * [p[(id)kCGImagePropertyPixelHeight] longValue];
            CFRelease(s);
        }
        int n = (int)files.count;
        printf("%d PNG files, %.1f MB, %.1f Mpixel\n\n", n, bytes / 1e6, pixels / 1e6);
        for (size_t k = 0; k < sizeof decoders / sizeof *decoders; k++) {
            decoders[k].fn(files[0]);   // warm-up
            int fails = 0;
            double t0 = now_ms();
            for (int i = 0; i < n; i++) @autoreleasepool { if (decoders[k].fn(files[i])) fails++; }
            double t = now_ms() - t0;
            printf("%-26s avg %7.1f ms/img  (%.0f MB/s compressed, %.0f Mpix/s)%s\n", decoders[k].name, t / n,
                   bytes / 1e3 / t, pixels / 1e3 / t, fails ? "  FAILED" : "");
            if (getenv("PER")) {   // per-file times (3 runs, best)
                for (int i = 0; i < n; i++) {
                    double best = 1e9;
                    for (int r = 0; r < 3; r++) {
                        double a = now_ms();
                        @autoreleasepool { decoders[k].fn(files[i]); }
                        best = MIN(best, now_ms() - a);
                    }
                    printf("    %-34.34s %8.1f ms\n", argv[i + 1] + (strrchr(argv[i + 1], '/') ? strrchr(argv[i + 1], '/') - argv[i + 1] + 1 : 0), best);
                }
            }
        }
    }
    return 0;
}
