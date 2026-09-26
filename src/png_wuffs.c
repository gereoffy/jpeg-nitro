// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Optional PNG decoding with Wuffs (https://github.com/google/wuffs, Apache-2.0),
// compiled separately so the viewer builds fast. Used by nitroview when built
// with NV_WUFFS; without it the viewer decodes PNG with Apple ImageIO.
#include "png_wuffs.h"

#include <stdlib.h>
#include <string.h>

#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__ZLIB
#define WUFFS_CONFIG__MODULE__PNG
#include "../third_party/wuffs.c"

int nv_png_size(const uint8_t *data, size_t len, int *w, int *h) {
    wuffs_png__decoder *dec = wuffs_png__decoder__alloc();
    if (!dec) return -1;
    wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader((uint8_t *)data, len, true);
    wuffs_base__image_config ic = {0};
    wuffs_base__status st = wuffs_png__decoder__decode_image_config(dec, &ic, &src);
    free(dec);
    if (st.repr) return -1;
    *w = (int)wuffs_base__pixel_config__width(&ic.pixcfg);
    *h = (int)wuffs_base__pixel_config__height(&ic.pixcfg);
    return 0;
}

int nv_png_decode_bgra(const uint8_t *data, size_t len, uint8_t *dst, size_t pitch, int w, int h) {
    wuffs_png__decoder *dec = wuffs_png__decoder__alloc();
    if (!dec) return -1;
    // checksums are not verified (like most fast decoders); damaged data is still detected
    wuffs_png__decoder__set_quirk(dec, WUFFS_BASE__QUIRK_IGNORE_CHECKSUM, 1);
    wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader((uint8_t *)data, len, true);
    wuffs_base__image_config ic = {0};
    int rc = -1;
    uint8_t *work = NULL;
    if (wuffs_png__decoder__decode_image_config(dec, &ic, &src).repr) goto done;
    if ((int)wuffs_base__pixel_config__width(&ic.pixcfg) != w || (int)wuffs_base__pixel_config__height(&ic.pixcfg) != h)
        goto done;
    // premultiplied alpha: the viewer draws on black, so this is exactly "over black"
    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL,
                                  WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, (uint32_t)w, (uint32_t)h);
    wuffs_base__pixel_buffer pb = {0};
    wuffs_base__table_u8 tab = wuffs_base__make_table_u8(dst, (size_t)w * 4, (size_t)h, pitch);
    if (wuffs_base__pixel_buffer__set_interleaved(&pb, &ic.pixcfg, tab, wuffs_base__empty_slice_u8()).repr) goto done;
    wuffs_base__range_ii_u64 wr = wuffs_png__decoder__workbuf_len(dec);
    work = malloc(wr.max_incl ? wr.max_incl : 1);
    if (!work) goto done;
    wuffs_base__status st = wuffs_png__decoder__decode_frame(dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC,
                                                             wuffs_base__make_slice_u8(work, wr.max_incl), NULL);
    rc = st.repr ? -1 : 0;
done:
    free(work);
    free(dec);
    return rc;
}
