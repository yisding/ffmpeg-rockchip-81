/*
 * Copyright (c) 2023 NyanMisaka
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Rockchip RGA (2D Raster Graphic Acceleration) base function
 */

#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"

#include "filters.h"
#include "video.h"

#include "rkrga_common.h"

typedef struct RGAAsyncFrame {
    RGAFrame *src;
    RGAFrame *dst;
    RGAFrame *pat;
} RGAAsyncFrame;

typedef struct RGAFormatMap {
    enum AVPixelFormat    pix_fmt;
    enum _Rga_SURF_FORMAT rga_fmt;
} RGAFormatMap;

#define RK_FORMAT_YCbCr_444_SP (0x32 << 8)
#define RK_FORMAT_YCrCb_444_SP (0x33 << 8)

#define YUV_FORMATS \
    { AV_PIX_FMT_GRAY8,    RK_FORMAT_YCbCr_400 },        /* RGA2 only */ \
    { AV_PIX_FMT_YUV420P,  RK_FORMAT_YCbCr_420_P },      /* RGA2 only */ \
    { AV_PIX_FMT_YUVJ420P, RK_FORMAT_YCbCr_420_P },      /* RGA2 only */ \
    { AV_PIX_FMT_YUV422P,  RK_FORMAT_YCbCr_422_P },      /* RGA2 only */ \
    { AV_PIX_FMT_YUVJ422P, RK_FORMAT_YCbCr_422_P },      /* RGA2 only */ \
    { AV_PIX_FMT_NV12,     RK_FORMAT_YCbCr_420_SP }, \
    { AV_PIX_FMT_NV21,     RK_FORMAT_YCrCb_420_SP }, \
    { AV_PIX_FMT_NV16,     RK_FORMAT_YCbCr_422_SP }, \
    { AV_PIX_FMT_NV24,     RK_FORMAT_YCbCr_444_SP },     /* RGA2-Pro only */ \
    { AV_PIX_FMT_NV42,     RK_FORMAT_YCrCb_444_SP },     /* RGA2-Pro only */ \
    { AV_PIX_FMT_P010,     RK_FORMAT_YCbCr_420_SP_10B }, /* RGA3 only */ \
    { AV_PIX_FMT_P210,     RK_FORMAT_YCbCr_422_SP_10B }, /* RGA3 only */ \
    { AV_PIX_FMT_NV15,     RK_FORMAT_YCbCr_420_SP_10B }, /* RGA2 only input, aka P010 compact */ \
    { AV_PIX_FMT_NV20_PACKED, RK_FORMAT_YCbCr_422_SP_10B }, /* RGA2 only input, aka P210 compact */ \
    { AV_PIX_FMT_YUYV422,  RK_FORMAT_YUYV_422 }, \
    { AV_PIX_FMT_YVYU422,  RK_FORMAT_YVYU_422 }, \
    { AV_PIX_FMT_UYVY422,  RK_FORMAT_UYVY_422 },

#define RGB_FORMATS \
    { AV_PIX_FMT_RGB555LE, RK_FORMAT_BGRA_5551 },        /* RGA2 only */ \
    { AV_PIX_FMT_BGR555LE, RK_FORMAT_RGBA_5551 },        /* RGA2 only */ \
    { AV_PIX_FMT_RGB565LE, RK_FORMAT_BGR_565 }, \
    { AV_PIX_FMT_BGR565LE, RK_FORMAT_RGB_565 }, \
    { AV_PIX_FMT_RGB24,    RK_FORMAT_RGB_888 }, \
    { AV_PIX_FMT_BGR24,    RK_FORMAT_BGR_888 }, \
    { AV_PIX_FMT_RGBA,     RK_FORMAT_RGBA_8888 }, \
    { AV_PIX_FMT_RGB0,     RK_FORMAT_RGBA_8888 },        /* RK_FORMAT_RGBX_8888 triggers RGA2 on multicore RGA */ \
    { AV_PIX_FMT_BGRA,     RK_FORMAT_BGRA_8888 }, \
    { AV_PIX_FMT_BGR0,     RK_FORMAT_BGRA_8888 },        /* RK_FORMAT_BGRX_8888 triggers RGA2 on multicore RGA */ \
    { AV_PIX_FMT_ARGB,     RK_FORMAT_ARGB_8888 },        /* RGA3 only input */ \
    { AV_PIX_FMT_0RGB,     RK_FORMAT_ARGB_8888 },        /* RGA3 only input */ \
    { AV_PIX_FMT_ABGR,     RK_FORMAT_ABGR_8888 },        /* RGA3 only input */ \
    { AV_PIX_FMT_0BGR,     RK_FORMAT_ABGR_8888 },        /* RGA3 only input */

static const RGAFormatMap supported_formats_main[] = {
    YUV_FORMATS
    RGB_FORMATS
};

static const RGAFormatMap supported_formats_overlay[] = {
    RGB_FORMATS
};
#undef YUV_FORMATS
#undef RGB_FORMATS

static int map_av_to_rga_format(enum AVPixelFormat in_format,
                                enum _Rga_SURF_FORMAT *out_format, int is_overlay)
{
    int i;

    if (is_overlay)
        goto overlay;

    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats_main); i++) {
        if (supported_formats_main[i].pix_fmt == in_format) {
            if (out_format)
                *out_format = supported_formats_main[i].rga_fmt;
            return 1;
        }
    }
    return 0;

overlay:
    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats_overlay); i++) {
        if (supported_formats_overlay[i].pix_fmt == in_format) {
            if (out_format)
                *out_format = supported_formats_overlay[i].rga_fmt;
            return 1;
        }
    }
    return 0;
}

static int get_pixel_stride(const AVDRMObjectDescriptor *object,
                            const AVDRMLayerDescriptor *layer,
                            enum AVPixelFormat pix_fmt,
                            int is_rgb, int is_planar,
                            int width, int height,
                            int *ws, int *hs)
{
    const AVDRMPlaneDescriptor *plane0, *plane1;
    const int is_packed_fmt = is_rgb || (!is_rgb && !is_planar);
    uint64_t h_stride;

    if (!object || !layer || !ws || !hs || width <= 0 || height <= 0)
        return AVERROR(EINVAL);

    plane0 = &layer->planes[0];
    plane1 = &layer->planes[1];

    if (plane0->pitch <= 0 || plane0->pitch > INT_MAX)
        return AVERROR(EINVAL);

    if (is_packed_fmt) {
        int min_linesize = av_image_get_linesize(pix_fmt, width, 0);

        if (min_linesize < 0)
            return min_linesize;
        if (plane0->pitch < min_linesize)
            return AVERROR(EINVAL);
        switch (pix_fmt) {
        case AV_PIX_FMT_GRAY8:
            *ws = plane0->pitch;
            break;
        case AV_PIX_FMT_RGB555LE:
        case AV_PIX_FMT_BGR555LE:
        case AV_PIX_FMT_RGB565LE:
        case AV_PIX_FMT_BGR565LE:
        case AV_PIX_FMT_YUYV422:
        case AV_PIX_FMT_YVYU422:
        case AV_PIX_FMT_UYVY422:
            if (plane0->pitch % 2)
                return AVERROR(EINVAL);
            *ws = plane0->pitch / 2;
            break;
        case AV_PIX_FMT_RGB24:
        case AV_PIX_FMT_BGR24:
            if (plane0->pitch % 3)
                return AVERROR(EINVAL);
            *ws = plane0->pitch / 3;
            break;
        default:
            if (plane0->pitch % 4)
                return AVERROR(EINVAL);
            *ws = plane0->pitch / 4;
            break;
        }
    } else {
        switch (pix_fmt) {
        case AV_PIX_FMT_P010:
        case AV_PIX_FMT_P210:
            if (plane0->pitch % 2)
                return AVERROR(EINVAL);
            *ws = plane0->pitch / 2;
            break;
        case AV_PIX_FMT_NV15:
        case AV_PIX_FMT_NV20_PACKED:
            if ((int64_t)plane0->pitch * 8 % 10)
                return AVERROR(EINVAL);
            *ws = (int64_t)plane0->pitch * 8 / 10;
            break;
        default:
            *ws = plane0->pitch;
            break;
        }
    }
    if (is_packed_fmt) {
        h_stride = ALIGN_DOWN(object->size / plane0->pitch, is_rgb ? 1 : 2);
    } else {
        if (plane1->offset < 0)
            return AVERROR(EINVAL);
        h_stride = plane1->offset / plane0->pitch;
    }
    if (!is_packed_fmt && plane1->offset % plane0->pitch)
        return AVERROR(EINVAL);

    if (*ws <= 0 || !h_stride ||
        h_stride > INT_MAX ||
        *ws < width || h_stride < height)
        return AVERROR(EINVAL);

    *hs = h_stride;
    return 0;
}

static int get_single_drm_object_index(const AVDRMFrameDescriptor *desc,
                                       const AVDRMLayerDescriptor *layer)
{
    int object_index;

    if (!desc ||
        desc->nb_objects < 1 || desc->nb_objects > AV_DRM_MAX_PLANES ||
        !layer || layer->nb_planes < 1 || layer->nb_planes > AV_DRM_MAX_PLANES)
        return AVERROR(EINVAL);

    object_index = layer->planes[0].object_index;
    if (object_index < 0 || object_index >= desc->nb_objects)
        return AVERROR(EINVAL);

    for (int i = 1; i < layer->nb_planes; i++)
        if (layer->planes[i].object_index != object_index)
            return AVERROR(EINVAL);

    return object_index;
}

static const AVDRMObjectDescriptor *get_single_drm_object(const AVDRMFrameDescriptor *desc,
                                                          const AVDRMLayerDescriptor *layer)
{
    int object_index = get_single_drm_object_index(desc, layer);

    return object_index < 0 ? NULL : &desc->objects[object_index];
}

static int get_afbc_pixel_stride(const AVPixFmtDescriptor *desc,
                                 ptrdiff_t byte_stride,
                                 int *pixel_stride)
{
    int bpp;
    int64_t bits;

    if (!desc || !pixel_stride || byte_stride <= 0)
        return AVERROR(EINVAL);

    bpp = av_get_padded_bits_per_pixel(desc);
    bits = (int64_t)byte_stride * 8;
    if (bpp <= 0 || bits % bpp)
        return AVERROR(EINVAL);

    bits /= bpp;
    if (bits <= 0 || bits > INT_MAX)
        return AVERROR(EINVAL);

    *pixel_stride = bits;
    return 0;
}

static int get_afbc_byte_stride(const AVPixFmtDescriptor *desc,
                                int pixel_stride,
                                ptrdiff_t *byte_stride)
{
    int bpp;
    int64_t bits;

    if (!desc || !byte_stride || pixel_stride <= 0)
        return AVERROR(EINVAL);

    bpp = av_get_padded_bits_per_pixel(desc);
    bits = (int64_t)pixel_stride * bpp;
    if (bpp <= 0 || bits % 8)
        return AVERROR(EINVAL);

    bits /= 8;
    if (bits <= 0 || bits > PTRDIFF_MAX)
        return AVERROR(EINVAL);

    *byte_stride = bits;
    return 0;
}

static int get_afbc_min_size(const AVPixFmtDescriptor *desc,
                             int pixel_stride,
                             int height,
                             int afbc_offset_y,
                             uint64_t *min_size)
{
    uint64_t block_cols, block_rows, blocks;
    uint64_t header_size, block_payload_size;
    int bpp;

    if (!desc || !min_size || pixel_stride <= 0 || height <= 0 ||
        afbc_offset_y < 0)
        return AVERROR(EINVAL);

    bpp = av_get_padded_bits_per_pixel(desc);
    if (bpp <= 0)
        return AVERROR(EINVAL);

    block_cols = ((uint64_t)pixel_stride + 15) >> 4;
    block_rows = ((uint64_t)height + afbc_offset_y + 15) >> 4;
    if (!block_cols || !block_rows || block_cols > UINT64_MAX / block_rows)
        return AVERROR(EINVAL);

    blocks = block_cols * block_rows;
    if (blocks > (UINT64_MAX - 63) / 16)
        return AVERROR(EINVAL);

    header_size = (blocks * 16 + 63) & ~UINT64_C(63);
    block_payload_size = ((uint64_t)bpp * 256 / 8 + 127) & ~UINT64_C(127);
    if (!block_payload_size ||
        blocks > (UINT64_MAX - header_size) / block_payload_size)
        return AVERROR(EINVAL);

    *min_size = header_size + blocks * block_payload_size;
    return 0;
}

/* Canonical formats: https://dri.freedesktop.org/docs/drm/gpu/afbc.html */
static uint32_t get_drm_afbc_format(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_NV12:     return DRM_FORMAT_YUV420_8BIT;
    case AV_PIX_FMT_NV15:     return DRM_FORMAT_YUV420_10BIT;
    case AV_PIX_FMT_NV16:     return DRM_FORMAT_YUYV;
    case AV_PIX_FMT_NV20_PACKED: return DRM_FORMAT_Y210;
    case AV_PIX_FMT_NV24:     return DRM_FORMAT_VUY888;
    case AV_PIX_FMT_RGB565LE: return DRM_FORMAT_RGB565;
    case AV_PIX_FMT_BGR565LE: return DRM_FORMAT_BGR565;
    case AV_PIX_FMT_RGB24:    return DRM_FORMAT_RGB888;
    case AV_PIX_FMT_BGR24:    return DRM_FORMAT_BGR888;
    case AV_PIX_FMT_RGBA:     return DRM_FORMAT_ABGR8888;
    case AV_PIX_FMT_RGB0:     return DRM_FORMAT_XBGR8888;
    case AV_PIX_FMT_BGRA:     return DRM_FORMAT_ARGB8888;
    case AV_PIX_FMT_BGR0:     return DRM_FORMAT_XRGB8888;
    default:                  return DRM_FORMAT_INVALID;
    }
}

static int rga_rotates_dimensions(int rotate_mode)
{
    return (rotate_mode & 0x04) == 0x04 || /* HAL_TRANSFORM_ROT_90 */
           (rotate_mode & 0x07) == 0x07;  /* HAL_TRANSFORM_ROT_270 */
}

static uint32_t get_drm_linear_format(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_GRAY8:    return DRM_FORMAT_R8;
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P: return DRM_FORMAT_YUV420;
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P: return DRM_FORMAT_YUV422;
    case AV_PIX_FMT_NV12:     return DRM_FORMAT_NV12;
    case AV_PIX_FMT_NV21:     return DRM_FORMAT_NV21;
    case AV_PIX_FMT_NV16:     return DRM_FORMAT_NV16;
    case AV_PIX_FMT_NV24:     return DRM_FORMAT_NV24;
    case AV_PIX_FMT_NV42:     return DRM_FORMAT_NV42;
    case AV_PIX_FMT_P010:     return DRM_FORMAT_P010;
    case AV_PIX_FMT_P210:     return DRM_FORMAT_P210;
    case AV_PIX_FMT_NV15:     return DRM_FORMAT_NV15;
    case AV_PIX_FMT_NV20_PACKED: return DRM_FORMAT_NV20;
    case AV_PIX_FMT_YUYV422:  return DRM_FORMAT_YUYV;
    case AV_PIX_FMT_YVYU422:  return DRM_FORMAT_YVYU;
    case AV_PIX_FMT_UYVY422:  return DRM_FORMAT_UYVY;
    case AV_PIX_FMT_RGB555LE: return DRM_FORMAT_XRGB1555;
    case AV_PIX_FMT_BGR555LE: return DRM_FORMAT_XBGR1555;
    case AV_PIX_FMT_RGB565LE: return DRM_FORMAT_RGB565;
    case AV_PIX_FMT_BGR565LE: return DRM_FORMAT_BGR565;
    case AV_PIX_FMT_RGB24:    return DRM_FORMAT_RGB888;
    case AV_PIX_FMT_BGR24:    return DRM_FORMAT_BGR888;
    case AV_PIX_FMT_RGBA:     return DRM_FORMAT_ABGR8888;
    case AV_PIX_FMT_RGB0:     return DRM_FORMAT_XBGR8888;
    case AV_PIX_FMT_BGRA:     return DRM_FORMAT_ARGB8888;
    case AV_PIX_FMT_BGR0:     return DRM_FORMAT_XRGB8888;
    case AV_PIX_FMT_ARGB:     return DRM_FORMAT_BGRA8888;
    case AV_PIX_FMT_0RGB:     return DRM_FORMAT_BGRX8888;
    case AV_PIX_FMT_ABGR:     return DRM_FORMAT_RGBA8888;
    case AV_PIX_FMT_0BGR:     return DRM_FORMAT_RGBX8888;
    default:                  return DRM_FORMAT_INVALID;
    }
}

static int get_aligned_linesize(enum AVPixelFormat pix_fmt, int width, int plane)
{
    const AVPixFmtDescriptor *pixdesc = av_pix_fmt_desc_get(pix_fmt);
    int is_rgb, is_yuv, is_planar, is_packed_fmt, is_fully_planar;
    int linesize;

    if (!pixdesc)
        return AVERROR(EINVAL);

    is_rgb = pixdesc->flags & AV_PIX_FMT_FLAG_RGB;
    is_yuv = !is_rgb && pixdesc->nb_components >= 2;
    is_planar = pixdesc->flags & AV_PIX_FMT_FLAG_PLANAR;
    is_packed_fmt = is_rgb || (!is_rgb && !is_planar);
    is_fully_planar = is_planar &&
                      pixdesc->comp[1].plane != pixdesc->comp[2].plane;

    if (pix_fmt == AV_PIX_FMT_NV15 ||
        pix_fmt == AV_PIX_FMT_NV20_PACKED) {
        int width_align_256_odds = FFALIGN(width, 256) | 256;
        return FFALIGN(width_align_256_odds * 10 / 8, 64);
    }
    if (pix_fmt == AV_PIX_FMT_P010 ||
        pix_fmt == AV_PIX_FMT_P210)
        return FFALIGN(width, 64) * 2;
    if ((pix_fmt == AV_PIX_FMT_NV24 ||
         pix_fmt == AV_PIX_FMT_NV42) && plane)
        return FFALIGN(width, 64) * 2;

    linesize = av_image_get_linesize(pix_fmt, width, plane);
    if (linesize < 0)
        return linesize;

    if (is_packed_fmt) {
        int pixel_width = av_get_padded_bits_per_pixel(pixdesc) / 8;

        if (pixel_width <= 0)
            return AVERROR(EINVAL);
        linesize = FFALIGN(linesize / pixel_width, 16) * pixel_width;
    } else if (is_yuv && is_fully_planar) {
        linesize = FFALIGN(linesize, 16 >> (plane ? pixdesc->log2_chroma_w : 0));
    } else {
        linesize = FFALIGN(linesize, 64);
    }

    return linesize;
}

static int reset_linear_drm_desc(AVFilterContext *avctx,
                                 AVDRMFrameDescriptor *desc,
                                 enum AVPixelFormat pix_fmt,
                                 int width, int height)
{
    const AVPixFmtDescriptor *pixdesc = av_pix_fmt_desc_get(pix_fmt);
    AVDRMLayerDescriptor *layer;
    uint32_t drm_fmt = get_drm_linear_format(pix_fmt);
    int nb_planes = av_pix_fmt_count_planes(pix_fmt);

    if (!desc || !pixdesc || width <= 0 || height <= 0 ||
        desc->nb_objects < 1 || desc->objects[0].fd < 0 ||
        desc->objects[0].size == 0 ||
        drm_fmt == DRM_FORMAT_INVALID ||
        nb_planes < 1 || nb_planes > AV_DRM_MAX_PLANES) {
        av_log(avctx, AV_LOG_ERROR, "Invalid output DRM frame descriptor\n");
        return AVERROR(EINVAL);
    }

    desc->nb_objects = 1;
    desc->nb_layers = 1;
    desc->objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;

    layer = &desc->layers[0];
    memset(layer, 0, sizeof(*layer));
    layer->format = drm_fmt;
    layer->nb_planes = nb_planes;

    for (int i = 0; i < nb_planes; i++) {
        int linesize = get_aligned_linesize(pix_fmt, width, i);

        if (linesize < 0)
            return linesize;

        layer->planes[i].object_index = 0;
        layer->planes[i].pitch = linesize;
        if (!i) {
            layer->planes[i].offset = 0;
        } else {
            const AVDRMPlaneDescriptor *prev = &layer->planes[i - 1];
            int plane_height = FFALIGN(height, 2) >>
                               (i > 1 ? pixdesc->log2_chroma_h : 0);

            layer->planes[i].offset = prev->offset + prev->pitch * plane_height;
        }
    }

    return 0;
}

static int is_pixel_stride_rga3_compat(int ws, int hs,
                                       enum _Rga_SURF_FORMAT fmt)
{
    switch (fmt) {
    case RK_FORMAT_YCbCr_420_SP:
    case RK_FORMAT_YCrCb_420_SP:
    case RK_FORMAT_YCbCr_422_SP:     return !(ws % 16) && !(hs % 2);
    case RK_FORMAT_YCbCr_420_SP_10B:
    case RK_FORMAT_YCbCr_422_SP_10B: return !(ws % 64) && !(hs % 2);
    case RK_FORMAT_YUYV_422:
    case RK_FORMAT_YVYU_422:
    case RK_FORMAT_UYVY_422:         return !(ws % 8) && !(hs % 2);
    case RK_FORMAT_RGB_565:
    case RK_FORMAT_BGR_565:          return !(ws % 8);
    case RK_FORMAT_RGB_888:
    case RK_FORMAT_BGR_888:          return !(ws % 16);
    case RK_FORMAT_RGBA_8888:
    case RK_FORMAT_BGRA_8888:
    case RK_FORMAT_ARGB_8888:
    case RK_FORMAT_ABGR_8888:        return !(ws % 4);
    default:                         return 0;
    }
}

static int is_rga2_core_mask(int scheduler_core)
{
    return scheduler_core && scheduler_core == (scheduler_core & 0xc);
}

static int is_rga3_core_mask(int scheduler_core)
{
    return scheduler_core && scheduler_core == (scheduler_core & 0x3);
}

static int has_mixed_rga_core_mask(int scheduler_core)
{
    return (scheduler_core & 0x3) && (scheduler_core & 0xc);
}

static int rga3_core_can_be_used(const RKRGAContext *r, int scheduler_core)
{
    return !r->is_rga2_used && r->has_rga3 &&
           (!scheduler_core || is_rga3_core_mask(scheduler_core));
}

static void select_rga2_core(RKRGAContext *r, RGAFrameInfo *out)
{
    r->is_rga2_used = 1;
    if (out && (r->has_rga3 || r->has_rga2p)) {
        out->scheduler_core = 0x4;
        if (r->has_rga2p)
            out->scheduler_core |= 0x8;
    }
}

static int validate_rga3_pixel_stride(AVFilterContext *avctx,
                                      RGAFrameInfo *info, RGAFrameInfo *out,
                                      int ws, int hs, const char *role)
{
    RKRGAContext *r = avctx->priv;

    if (!rga3_core_can_be_used(r, out ? out->scheduler_core : r->scheduler_core) ||
        is_pixel_stride_rga3_compat(ws, hs, info->rga_fmt))
        return 0;

    if (r->has_rga2) {
        select_rga2_core(r, out);
        av_log(avctx, AV_LOG_WARNING,
               "%s pixel stride (%dx%d) format '%s' is not supported by RGA3, falling back to RGA2\n",
               role, ws, hs, av_get_pix_fmt_name(info->pix_fmt));
        return 1;
    }

    av_log(avctx, AV_LOG_ERROR,
           "%s pixel stride (%dx%d) format '%s' is not supported by RGA3\n",
           role, ws, hs, av_get_pix_fmt_name(info->pix_fmt));
    return AVERROR(EINVAL);
}

static int validate_active_rect_stride(AVFilterContext *avctx,
                                       const RGAFrameInfo *info,
                                       int ws, int hs, const char *role)
{
    if (!info || ws <= 0 || hs <= 0 ||
        info->act_x < 0 || info->act_y < 0 ||
        info->act_w <= 0 || info->act_h <= 0 ||
        info->act_x > ws - info->act_w ||
        info->act_y > hs - info->act_h) {
        av_log(avctx, AV_LOG_ERROR,
               "%s active rectangle %dx%d@%d,%d exceeds stride %dx%d\n",
               role, info ? info->act_w : 0, info ? info->act_h : 0,
               info ? info->act_x : 0, info ? info->act_y : 0, ws, hs);
        return AVERROR(EINVAL);
    }

    return 0;
}

static int is_yuv_rect_aligned(const RGAFrameInfo *info, int x, int y, int w, int h)
{
    return (info->pix_desc->flags & AV_PIX_FMT_FLAG_RGB) ||
           !((x | y | w | h) & (RK_RGA_YUV_ALIGN - 1));
}

static int is_rga2_only_input_format(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_GRAY8:
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_RGB555LE:
    case AV_PIX_FMT_BGR555LE:
        return 1;
    default:
        return 0;
    }
}

static int is_linear_rga2_only_input_format(enum AVPixelFormat pix_fmt)
{
    return pix_fmt == AV_PIX_FMT_NV15 ||
           pix_fmt == AV_PIX_FMT_NV20_PACKED;
}

static int is_rga3_only_input_format(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_P010:
    case AV_PIX_FMT_P210:
    case AV_PIX_FMT_ARGB:
    case AV_PIX_FMT_0RGB:
    case AV_PIX_FMT_ABGR:
    case AV_PIX_FMT_0BGR:
        return 1;
    default:
        return 0;
    }
}

static int is_supported_afbc_modifier(uint64_t modifier)
{
    return modifier == DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_SPARSE |
                                               AFBC_FORMAT_MOD_BLOCK_SIZE_16x16);
}

static int validate_drm_modifier(AVFilterContext *avctx, uint64_t modifier,
                                 int *is_afbc)
{
    *is_afbc = 0;

    if (modifier == DRM_FORMAT_MOD_LINEAR)
        return 0;
    if (is_supported_afbc_modifier(modifier)) {
        *is_afbc = 1;
        return 0;
    }

    av_log(avctx, AV_LOG_ERROR, "Unsupported DRM modifier 0x%016"PRIx64"\n",
           modifier);
    return AVERROR(ENOSYS);
}

static int validate_drm_desc_shape(AVFilterContext *avctx,
                                   const AVDRMFrameDescriptor *desc)
{
    if (!desc ||
        desc->nb_objects < 1 || desc->nb_objects > AV_DRM_MAX_PLANES ||
        desc->nb_layers != 1) {
        av_log(avctx, AV_LOG_ERROR, "Invalid DRM frame descriptor\n");
        return AVERROR(EINVAL);
    }

    for (int i = 0; i < desc->nb_objects; i++) {
        const AVDRMObjectDescriptor *object = &desc->objects[i];

        if (object->fd < 0 || object->size == 0) {
            av_log(avctx, AV_LOG_ERROR, "Invalid DRM object %d\n", i);
            return AVERROR(EINVAL);
        }
    }

    for (int i = 0; i < desc->nb_layers; i++) {
        const AVDRMLayerDescriptor *layer = &desc->layers[i];

        if (layer->nb_planes < 1 || layer->nb_planes > AV_DRM_MAX_PLANES) {
            av_log(avctx, AV_LOG_ERROR, "Invalid DRM layer %d descriptor\n", i);
            return AVERROR(EINVAL);
        }

        for (int j = 0; j < layer->nb_planes; j++) {
            const AVDRMPlaneDescriptor *plane = &layer->planes[j];
            int object_index = plane->object_index;

            if (object_index < 0 || object_index >= desc->nb_objects ||
                plane->offset < 0 || plane->pitch <= 0 ||
                (size_t)plane->offset >= desc->objects[object_index].size) {
                av_log(avctx, AV_LOG_ERROR,
                       "Invalid DRM plane descriptor in layer %d plane %d\n", i, j);
                return AVERROR(EINVAL);
            }
        }
    }

    return 0;
}

static int get_expected_chroma_pitch(enum AVPixelFormat pix_fmt,
                                     const AVPixFmtDescriptor *pix_desc,
                                     ptrdiff_t luma_pitch,
                                     ptrdiff_t *expected_pitch)
{
    int is_fully_planar;

    if (!pix_desc || !expected_pitch || luma_pitch <= 0)
        return AVERROR(EINVAL);

    is_fully_planar = (pix_desc->flags & AV_PIX_FMT_FLAG_PLANAR) &&
                      pix_desc->nb_components >= 3 &&
                      pix_desc->comp[1].plane != pix_desc->comp[2].plane;

    if (is_fully_planar) {
        if (luma_pitch % (1 << pix_desc->log2_chroma_w))
            return AVERROR(EINVAL);
        *expected_pitch = luma_pitch >> pix_desc->log2_chroma_w;
    } else if (pix_fmt == AV_PIX_FMT_NV24 ||
               pix_fmt == AV_PIX_FMT_NV42) {
        if (luma_pitch > INT_MAX / 2)
            return AVERROR(EINVAL);
        *expected_pitch = luma_pitch * 2;
    } else {
        *expected_pitch = luma_pitch;
    }

    return 0;
}

static int validate_linear_drm_layer(AVFilterContext *avctx,
                                     const AVDRMObjectDescriptor *object,
                                     const AVDRMLayerDescriptor *layer,
                                     enum AVPixelFormat pix_fmt,
                                     int width, int height)
{
    uint32_t drm_fmt = get_drm_linear_format(pix_fmt);
    ptrdiff_t linesizes[4] = { 0 };
    size_t plane_sizes[4] = { 0 };
    size_t expected_offset = 0;
    int nb_planes = av_pix_fmt_count_planes(pix_fmt);
    int layout_height, ret;
    int64_t layout_height64;

    if (!object || width <= 0 || height <= 0 ||
        drm_fmt == DRM_FORMAT_INVALID ||
        nb_planes < 1 || nb_planes > AV_DRM_MAX_PLANES) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported DRM format for pixel format '%s'\n",
               av_get_pix_fmt_name(pix_fmt));
        return AVERROR(ENOSYS);
    }

    if (!layer || layer->format != drm_fmt || layer->nb_planes != nb_planes) {
        av_log(avctx, AV_LOG_ERROR,
               "Input DRM format 0x%08"PRIx32" with %d planes does not match pixel format '%s'\n",
               layer ? layer->format : 0, layer ? layer->nb_planes : 0,
               av_get_pix_fmt_name(pix_fmt));
        return AVERROR(EINVAL);
    }

    for (int i = 0; i < nb_planes; i++) {
        const AVDRMPlaneDescriptor *plane = &layer->planes[i];
        int min_linesize;

        if (plane->object_index < 0 || plane->offset < 0 ||
            plane->pitch <= 0 || plane->pitch > INT_MAX) {
            av_log(avctx, AV_LOG_ERROR, "Invalid DRM plane %d descriptor\n", i);
            return AVERROR(EINVAL);
        }
        min_linesize = av_image_get_linesize(pix_fmt, width, i);
        if (min_linesize < 0)
            return min_linesize;
        if (plane->pitch < min_linesize) {
            av_log(avctx, AV_LOG_ERROR,
                   "DRM plane %d pitch %td is smaller than required %d\n",
                   i, plane->pitch, min_linesize);
            return AVERROR(EINVAL);
        }
        linesizes[i] = plane->pitch;
    }

    if (layer->planes[0].offset != 0) {
        av_log(avctx, AV_LOG_ERROR,
               "Only zero-based DRM plane layouts are supported\n");
        return AVERROR(ENOSYS);
    }

    layout_height = height;
    if (nb_planes > 1) {
        const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(pix_fmt);

        if (layer->planes[1].offset % layer->planes[0].pitch) {
            av_log(avctx, AV_LOG_ERROR,
                   "DRM chroma plane offset is not aligned to luma pitch\n");
            return AVERROR(EINVAL);
        }
        layout_height64 = layer->planes[1].offset / layer->planes[0].pitch;
        if (layout_height64 < height || layout_height64 > INT_MAX) {
            av_log(avctx, AV_LOG_ERROR,
                   "DRM luma stride height %"PRId64" is outside valid range for frame height %d\n",
                   layout_height64, height);
            return AVERROR(EINVAL);
        }
        layout_height = layout_height64;

        for (int i = 1; i < nb_planes; i++) {
            ptrdiff_t expected_pitch;

            ret = get_expected_chroma_pitch(pix_fmt, pix_desc,
                                            layer->planes[0].pitch,
                                            &expected_pitch);
            if (ret < 0)
                return ret;

            if (layer->planes[i].pitch != expected_pitch) {
                av_log(avctx, AV_LOG_ERROR,
                       "DRM plane %d pitch %td is not representable by RGA stride %td\n",
                       i, layer->planes[i].pitch, layer->planes[0].pitch);
                return AVERROR(ENOSYS);
            }
        }
    }

    ret = av_image_fill_plane_sizes(plane_sizes, pix_fmt, layout_height, linesizes);
    if (ret < 0)
        return ret;

    for (int i = 0; i < nb_planes; i++) {
        const AVDRMPlaneDescriptor *plane = &layer->planes[i];

        if ((size_t)plane->offset > object->size ||
            plane_sizes[i] > object->size - (size_t)plane->offset) {
            av_log(avctx, AV_LOG_ERROR,
                   "DRM plane %d extends past object size\n", i);
            return AVERROR(EINVAL);
        }
        if ((size_t)plane->offset != expected_offset) {
            av_log(avctx, AV_LOG_ERROR,
                   "Only zero-based contiguous DRM plane layouts are supported\n");
            return AVERROR(ENOSYS);
        }
        expected_offset += plane_sizes[i];
    }

    return 0;
}

static int validate_afbc_drm_layer(AVFilterContext *avctx,
                                   const AVDRMObjectDescriptor *object,
                                   const AVDRMLayerDescriptor *layer,
                                   const AVPixFmtDescriptor *pix_desc,
                                   enum AVPixelFormat pix_fmt,
                                   int height,
                                   int afbc_offset_y)
{
    const AVDRMPlaneDescriptor *plane;
    uint32_t drm_fbc_fmt = get_drm_afbc_format(pix_fmt);
    uint64_t min_size;
    int pixel_stride;
    int ret;

    if (!object || !layer || !object->size || layer->nb_planes != 1) {
        av_log(avctx, AV_LOG_ERROR, "AFBC input must contain exactly one plane\n");
        return AVERROR(EINVAL);
    }

    plane = &layer->planes[0];
    if (plane->object_index < 0 || plane->offset != 0 || plane->pitch <= 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid AFBC DRM plane descriptor\n");
        return AVERROR(EINVAL);
    }

    if (drm_fbc_fmt == DRM_FORMAT_INVALID || drm_fbc_fmt != layer->format) {
        av_log(avctx, AV_LOG_ERROR, "Input format '%s' with AFBC modifier is not supported\n",
               av_get_pix_fmt_name(pix_fmt));
        return AVERROR(ENOSYS);
    }

    ret = get_afbc_pixel_stride(pix_desc, plane->pitch, &pixel_stride);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid AFBC DRM plane pitch: %td\n",
               plane->pitch);
        return ret;
    }

    ret = get_afbc_min_size(pix_desc, pixel_stride, height,
                            afbc_offset_y, &min_size);
    if (ret < 0)
        return ret;

    if ((uint64_t)object->size < min_size) {
        av_log(avctx, AV_LOG_ERROR,
               "AFBC DRM object is too small: size=%"PRIu64" min=%"PRIu64"\n",
               (uint64_t)object->size, min_size);
        return AVERROR(EINVAL);
    }

    return 0;
}

static void clear_unused_frames(RGAFrame *list)
{
    while (list) {
        if (list->queued == 1 && !list->locked) {
            av_frame_free(&list->frame);
            list->queued = 0;
        }
        list = list->next;
    }
}

static void clear_frame_list(RGAFrame **list)
{
    while (*list) {
        RGAFrame *frame = NULL;

        frame = *list;
        *list = (*list)->next;
        av_frame_free(&frame->frame);
        av_freep(&frame);
    }
}

static void release_frame(RGAFrame *frame)
{
    if (!frame)
        return;

    av_frame_free(&frame->frame);
    frame->queued = 0;
    frame->locked = 0;
}

static const AVRKMPPDRMFrameDescriptor *get_rkmpp_drm_desc(const AVFrame *frame)
{
    const AVHWFramesContext *hwfc;

    if (!frame || !frame->data[0])
        return NULL;

    /* only RKMPP hwcontext frames carry the extended descriptor: a foreign
     * DRM_PRIME frame whose descriptor buffer merely happens to be large
     * enough must not have its trailing bytes read as afbc_offset_y */
    if (!frame->hw_frames_ctx)
        return NULL;
    hwfc = (AVHWFramesContext *)frame->hw_frames_ctx->data;
    if (hwfc->device_ctx->type != AV_HWDEVICE_TYPE_RKMPP)
        return NULL;

    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
        if (frame->buf[i] &&
            frame->buf[i]->data == frame->data[0] &&
            frame->buf[i]->size >= sizeof(AVRKMPPDRMFrameDescriptor))
            return (const AVRKMPPDRMFrameDescriptor *)frame->data[0];
    }

    return NULL;
}

static RGAFrame *get_free_frame(RGAFrame **list)
{
    RGAFrame *out = *list;

    for (; out; out = out->next) {
        if (!out->queued) {
            out->queued = 1;
            break;
        }
    }

    if (!out) {
        out = av_mallocz(sizeof(*out));
        if (!out) {
            av_log(NULL, AV_LOG_ERROR, "Cannot alloc new output frame\n");
            return NULL;
        }
        out->queued = 1;
        out->next   = *list;
        *list       = out;
    }

    return out;
}

static int set_colorspace_info(AVFilterContext *avctx,
                               RGAFrameInfo *in_info,
                               enum AVColorSpace in_spc,
                               enum AVColorRange in_rng,
                               RGAFrameInfo *out_info,
                               enum AVColorSpace *out_spc,
                               enum AVColorRange *out_rng,
                               int *color_space_mode,
                               int is_rga2_used)
{
    int rgb_in, rgb_out, out_mode = 0;

    if (!in_info || !out_info || !color_space_mode)
        return AVERROR(EINVAL);

    rgb_in  = in_info->pix_desc->flags & AV_PIX_FMT_FLAG_RGB;
    rgb_out = out_info->pix_desc->flags & AV_PIX_FMT_FLAG_RGB;

    /* rgb2yuv */
    if (rgb_in && !rgb_out) {
        if (in_rng == AVCOL_RANGE_UNSPECIFIED)
            in_rng = AVCOL_RANGE_JPEG;
        /* rgb full -> yuv full/limit */
        if (in_rng == AVCOL_RANGE_JPEG) {
            if ((out_rng && *out_rng == AVCOL_RANGE_JPEG) ||
                (out_spc && *out_spc == AVCOL_SPC_BT470BG)) {
                if (out_spc)
                    *out_spc = AVCOL_SPC_BT470BG;

                if (out_rng && *out_rng == AVCOL_RANGE_JPEG)
                    out_mode = 1 << 2; /* IM_RGB_TO_YUV_BT601_FULL */
                else {
                    if (out_rng)
                        *out_rng = AVCOL_RANGE_MPEG;
                    out_mode = 2 << 2; /* IM_RGB_TO_YUV_BT601_LIMIT */
                }
            } else {
                if (out_spc)
                    *out_spc = AVCOL_SPC_BT709;
                if (out_rng)
                    *out_rng = AVCOL_RANGE_MPEG;
                out_mode = is_rga2_used ? (0xb << 8) /* rgb2yuv_709_limit */
                                        : (3 << 2);  /* IM_RGB_TO_YUV_BT709_LIMIT */
            }
        }
        if (out_mode)
            *color_space_mode |= out_mode;
    }
    /* yuv2rgb */
    else if (!rgb_in && rgb_out) {
        /* yuv full/limit -> rgb full */
        switch (in_rng) {
        case AVCOL_RANGE_UNSPECIFIED:
        case AVCOL_RANGE_MPEG:
            if (in_spc == AVCOL_SPC_BT709)
                out_mode = 3 << 0; /* IM_YUV_TO_RGB_BT709_LIMIT */
            if (in_spc == AVCOL_SPC_BT470BG ||
                in_spc == AVCOL_SPC_UNSPECIFIED)
                out_mode = 1 << 0; /* IM_YUV_TO_RGB_BT601_LIMIT */
            break;
        case AVCOL_RANGE_JPEG:
            if (in_spc == AVCOL_SPC_BT709) {
                av_log(avctx, AV_LOG_ERROR,
                       "Full-range BT.709 YUV to RGB is not supported by RGA\n");
                return AVERROR(ENOSYS);
            }
            if (in_spc == AVCOL_SPC_BT470BG ||
                in_spc == AVCOL_SPC_UNSPECIFIED)
                out_mode = 2 << 0; /* IM_YUV_TO_RGB_BT601_FULL */
            break;
        }
        if (!out_mode) {
            av_log(avctx, AV_LOG_ERROR,
                   "YUV to RGB color space/range conversion is not supported\n");
            return AVERROR(ENOSYS);
        }
        if (out_spc)
            *out_spc = AVCOL_SPC_RGB;
        if (out_rng)
            *out_rng = AVCOL_RANGE_JPEG;
        if (out_mode)
            *color_space_mode |= out_mode;
    }
    /* passthrough */
    else {
        if (out_spc)
            *out_spc = in_spc;
        if (out_rng)
            *out_rng = in_rng;
    }

    /* yuvj2yuv */
    if ((in_info->pix_fmt == AV_PIX_FMT_YUVJ420P ||
         in_info->pix_fmt == AV_PIX_FMT_YUVJ422P) && !rgb_out) {
        if (out_rng)
            *out_rng = AVCOL_RANGE_JPEG;
    }

    return 0;
}

static int verify_rga_frame_info_io_dynamic(AVFilterContext *avctx,
                                            RGAFrameInfo *in, RGAFrameInfo *out)
{
    RKRGAContext *r = avctx->priv;

    if (!in || !out)
        return AVERROR(EINVAL);

    if (out->pix_fmt == AV_PIX_FMT_NV15 ||
        out->pix_fmt == AV_PIX_FMT_NV20_PACKED) {
        av_log(avctx, AV_LOG_ERROR, "'%s' as output is not supported\n",
               av_get_pix_fmt_name(out->pix_fmt));
        return AVERROR(ENOSYS);
    }
    if (r->is_rga2_used && !r->has_rga2) {
        av_log(avctx, AV_LOG_ERROR, "RGA2 is requested but not available\n");
        return AVERROR(ENOSYS);
    }
    if (r->is_rga2_used &&
        (in->pix_fmt == AV_PIX_FMT_P010 ||
         out->pix_fmt == AV_PIX_FMT_P010)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' is not supported if RGA2 is requested\n",
               av_get_pix_fmt_name(AV_PIX_FMT_P010));
        return AVERROR(ENOSYS);
    }
    if (r->is_rga2_used &&
        (in->pix_fmt == AV_PIX_FMT_P210 ||
         out->pix_fmt == AV_PIX_FMT_P210)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' is not supported if RGA2 is requested\n",
               av_get_pix_fmt_name(AV_PIX_FMT_P210));
        return AVERROR(ENOSYS);
    }
    if (r->is_rga2_used && is_rga3_only_input_format(in->pix_fmt)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' input is not supported if RGA2 is requested\n",
               av_get_pix_fmt_name(in->pix_fmt));
        return AVERROR(ENOSYS);
    }
    if (!r->has_rga2p && r->is_rga2_used && in->crop && in->pix_desc->comp[0].depth >= 10) {
        av_log(avctx, AV_LOG_ERROR, "Cropping 10-bit '%s' input is not supported if RGA2 (non-Pro) is requested\n",
               av_get_pix_fmt_name(in->pix_fmt));
        return AVERROR(ENOSYS);
    }
    if (r->is_rga2_used && !r->has_rga2p &&
        (out->act_w > 4096 || out->act_h > 4096)) {
        av_log(avctx, AV_LOG_ERROR, "Max supported output size of RGA2 (non-Pro) is 4096x4096\n");
        return AVERROR(EINVAL);
    }
    if (!r->is_rga2_used &&
        (in->act_w < 68 || in->act_h < 2)) {
        av_log(avctx, AV_LOG_ERROR, "Min supported input size of RGA3 is 68x2\n");
        return AVERROR(EINVAL);
    }
    if (!r->is_rga2_used &&
        (out->act_w > 8128 || out->act_h > 8128)) {
        av_log(avctx, AV_LOG_ERROR, "Max supported output size of RGA3 is 8128x8128\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static RGAFrame *submit_frame(RKRGAContext *r, AVFilterLink *inlink,
                              const AVFrame *picref, int do_overlay, int pat_preproc)
{
    RGAFrame        *rga_frame;
    AVFilterContext *ctx = inlink->dst;
    rga_info_t info = { .mmuFlag = 1, };
    int nb_link = FF_INLINK_IDX(inlink);
    RGAFrameInfo *in_info = &r->in_rga_frame_infos[nb_link];
    RGAFrameInfo *out_info = &r->out_rga_frame_info;
    int w_stride = 0, h_stride = 0;
    const AVRKMPPDRMFrameDescriptor *rkmpp_desc = NULL;
    const AVDRMFrameDescriptor *desc;
    const AVDRMObjectDescriptor *object;
    const AVDRMLayerDescriptor *layer;
    const AVDRMPlaneDescriptor *plane0;
    RGAFrame **frame_list = NULL;
    int is_afbc = 0;
    int ret, is_fbc = 0;

    if (pat_preproc && !nb_link)
        return NULL;

    frame_list = nb_link ?
        (pat_preproc ? &r->pat_preproc_frame_list : &r->pat_frame_list) : &r->src_frame_list;

    clear_unused_frames(*frame_list);

    rga_frame = get_free_frame(frame_list);
    if (!rga_frame)
        return NULL;

    if (picref->format != AV_PIX_FMT_DRM_PRIME) {
        av_log(ctx, AV_LOG_ERROR, "RGA gets a wrong frame\n");
        goto fail;
    }
    rga_frame->frame = av_frame_clone(picref);
    if (!rga_frame->frame)
        goto fail;

    rkmpp_desc = get_rkmpp_drm_desc(rga_frame->frame);
    desc = rkmpp_desc ? &rkmpp_desc->drm_desc :
                        (AVDRMFrameDescriptor *)rga_frame->frame->data[0];
    ret = validate_drm_desc_shape(ctx, desc);
    if (ret < 0)
        goto fail;

    layer = &desc->layers[0];
    object = get_single_drm_object(desc, layer);
    if (!object || object->fd < 0) {
        av_log(ctx, AV_LOG_ERROR, "RGA requires all DRM planes to share one valid object\n");
        goto fail;
    }

    ret = validate_drm_modifier(ctx, object->format_modifier, &is_afbc);
    if (ret < 0)
        goto fail;
    is_fbc = is_afbc;
    if (!is_fbc) {
        ret = validate_linear_drm_layer(ctx, object, layer, in_info->pix_fmt,
                                        inlink->w, inlink->h);
        if (ret < 0)
            goto fail;

        ret = get_pixel_stride(object,
                               layer,
                               in_info->pix_fmt,
                               (in_info->pix_desc->flags & AV_PIX_FMT_FLAG_RGB),
                               (in_info->pix_desc->flags & AV_PIX_FMT_FLAG_PLANAR),
                               inlink->w, inlink->h, &w_stride, &h_stride);
        if (ret < 0 || !w_stride || !h_stride) {
            av_log(ctx, AV_LOG_ERROR, "Failed to get frame strides\n");
            goto fail;
        }
    }

    info.fd           = object->fd;
    info.format       = in_info->rga_fmt;
    info.in_fence_fd  = -1;
    info.out_fence_fd = -1;

    if (in_info->uncompact_10b_msb)
        info.is_10b_compact = info.is_10b_endian = 1;

    if (!nb_link) {
        info.rotation = in_info->rotate_mode;
        info.blend    = (do_overlay && !pat_preproc) ? in_info->blend_mode : 0;
    }

    if (!is_fbc && is_linear_rga2_only_input_format(in_info->pix_fmt) &&
        !r->is_rga2_used) {
        if (!r->has_rga2) {
            av_log(ctx, AV_LOG_ERROR,
                   "Linear input format '%s' is only supported by RGA2\n",
                   av_get_pix_fmt_name(in_info->pix_fmt));
            goto fail;
        }
        select_rga2_core(r, out_info);
    }

    if (is_fbc && !r->has_rga2p &&
        (r->is_rga2_used || is_rga2_core_mask(out_info->scheduler_core))) {
        av_log(ctx, AV_LOG_ERROR, "Input format '%s' with AFBC modifier is not supported by RGA2 (non-Pro)\n",
               av_get_pix_fmt_name(in_info->pix_fmt));
        goto fail;
    }

    if (!is_fbc) {
        ret = validate_active_rect_stride(ctx, in_info,
                                          w_stride, h_stride, "Input");
        if (ret < 0)
            goto fail;
        ret = validate_rga3_pixel_stride(ctx, in_info, out_info,
                                         w_stride, h_stride, "Input");
        if (ret < 0)
            goto fail;
        if ((ret = verify_rga_frame_info_io_dynamic(ctx, in_info, out_info)) < 0)
            goto fail;
    }

    if (pat_preproc) {
        RGAFrameInfo *in0_info = &r->in_rga_frame_infos[0];
        rga_set_rect(&info.rect, 0, 0,
                     FFMIN((in0_info->act_w - in_info->overlay_x), in_info->act_w),
                     FFMIN((in0_info->act_h - in_info->overlay_y), in_info->act_h),
                     w_stride, h_stride, in_info->rga_fmt);
    } else
        rga_set_rect(&info.rect, in_info->act_x, in_info->act_y,
                     in_info->act_w, in_info->act_h,
                     w_stride, h_stride, in_info->rga_fmt);

    if (is_fbc) {
        int afbc_offset_y = 0;

        if (rkmpp_desc && rkmpp_desc->afbc_offset_y > 0)
            afbc_offset_y = rkmpp_desc->afbc_offset_y;

        ret = validate_afbc_drm_layer(ctx, object, layer,
                                      in_info->pix_desc, in_info->pix_fmt,
                                      inlink->h, afbc_offset_y);
        if (ret < 0)
            goto fail;

        if (afbc_offset_y > 0)
            info.rect.yoffset += afbc_offset_y;

        plane0 = &layer->planes[0];
        info.rect.wstride = plane0->pitch;
        if ((ret = get_afbc_pixel_stride(in_info->pix_desc, plane0->pitch,
                                         &info.rect.wstride)) < 0)
            goto fail;

        if (info.rect.wstride % RK_RGA_AFBC_16x16_STRIDE_ALIGN)
            goto fail;

        info.rect.hstride = FFALIGN(inlink->h + afbc_offset_y,
                                    RK_RGA_AFBC_16x16_STRIDE_ALIGN);

        ret = validate_active_rect_stride(ctx, in_info,
                                          info.rect.wstride,
                                          info.rect.hstride, "Input");
        if (ret < 0)
            goto fail;
        ret = validate_rga3_pixel_stride(ctx, in_info, out_info,
                                         info.rect.wstride,
                                         info.rect.hstride, "Input");
        if (ret < 0)
            goto fail;
        if (ret > 0 && !r->has_rga2p) {
            av_log(ctx, AV_LOG_ERROR, "Input format '%s' with AFBC modifier is not supported by RGA2 (non-Pro)\n",
                   av_get_pix_fmt_name(in_info->pix_fmt));
            goto fail;
        }
        if ((ret = verify_rga_frame_info_io_dynamic(ctx, in_info, out_info)) < 0)
            goto fail;

        info.rd_mode = 1 << 1; /* IM_AFBC16x16_MODE */
    }

    rga_frame->info = info;

    return rga_frame;

fail:
    release_frame(rga_frame);
    return NULL;
}

static RGAFrame *query_frame(RKRGAContext *r, AVFilterLink *outlink,
                             const AVFrame *in, const AVFrame *picref_pat, int pat_preproc)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    RGAFrame        *out_frame;
    rga_info_t info = { .mmuFlag = 1, };
    RGAFrameInfo *in0_info = &r->in_rga_frame_infos[0];
    RGAFrameInfo *in1_info = ctx->nb_inputs > 1 ? &r->in_rga_frame_infos[1] : NULL;
    RGAFrameInfo *out_info = pat_preproc ? in1_info : &r->out_rga_frame_info;
    RGAFrameInfo validate_in_info, validate_out_info;
    AVBufferRef *hw_frame_ctx = pat_preproc ? r->pat_preproc_hwframes_ctx :
                                              ff_filter_link(outlink)->hw_frames_ctx;
    int w_stride = 0, h_stride = 0;
    int act_w, act_h;
    int frame_w, frame_h;
    AVDRMFrameDescriptor *desc;
    AVDRMLayerDescriptor *layer;
    AVDRMObjectDescriptor *object;
    RGAFrame **frame_list = NULL;
    int ret, is_afbc = 0, object_index;

    if (!out_info || !hw_frame_ctx)
        return NULL;

    validate_in_info = pat_preproc ? *in1_info : *in0_info;
    validate_out_info = *out_info;
    if (pat_preproc) {
        act_w = FFMIN(in0_info->act_w - in1_info->overlay_x, in1_info->act_w);
        act_h = FFMIN(in0_info->act_h - in1_info->overlay_y, in1_info->act_h);
        validate_in_info.act_x = 0;
        validate_in_info.act_y = 0;
        validate_in_info.act_w = act_w;
        validate_in_info.act_h = act_h;
        validate_out_info.act_x = in1_info->overlay_x;
        validate_out_info.act_y = in1_info->overlay_y;
        validate_out_info.act_w = act_w;
        validate_out_info.act_h = act_h;
    } else {
        act_w = out_info->act_w;
        act_h = out_info->act_h;
    }

    frame_list = pat_preproc ? &r->pat_frame_list : &r->dst_frame_list;

    clear_unused_frames(*frame_list);

    out_frame = get_free_frame(frame_list);
    if (!out_frame)
        return NULL;

    out_frame->frame = av_frame_alloc();
    if (!out_frame->frame)
        goto fail;

    if (in && (ret = av_frame_copy_props(out_frame->frame, in)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to copy metadata fields from in to out: %d\n", ret);
        goto fail;
    }
    out_frame->frame->crop_top    = 0;
    out_frame->frame->crop_bottom = 0;
    out_frame->frame->crop_left   = 0;
    out_frame->frame->crop_right  = 0;
    /* an unknown input SAR (0/den) must stay as copied from the input:
     * scaling it would turn 0/1 into the invalid 1/0 in the rotated case */
    if (in->sample_aspect_ratio.num) {
        if ((in0_info->rotate_mode & 0x04) == 0x04 /* HAL_TRANSFORM_ROT_90 */ ||
            (in0_info->rotate_mode & 0x07) == 0x07 /* HAL_TRANSFORM_ROT_270 */) {
            av_reduce(&out_frame->frame->sample_aspect_ratio.den,
                      &out_frame->frame->sample_aspect_ratio.num,
                      (int64_t)in->sample_aspect_ratio.num * outlink->w * in0_info->act_w,
                      (int64_t)in->sample_aspect_ratio.den * outlink->h * in0_info->act_h,
                      INT_MAX);
        } else {
            av_reduce(&out_frame->frame->sample_aspect_ratio.num,
                      &out_frame->frame->sample_aspect_ratio.den,
                      (int64_t)in->sample_aspect_ratio.num * outlink->h * in0_info->act_w,
                      (int64_t)in->sample_aspect_ratio.den * outlink->w * in0_info->act_h,
                      INT_MAX);
        }
    }

    if ((ret = av_hwframe_get_buffer(hw_frame_ctx, out_frame->frame, 0)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Cannot allocate an internal frame: %d\n", ret);
        goto fail;
    }

    frame_w = out_frame->frame->width;
    frame_h = out_frame->frame->height;
    desc = (AVDRMFrameDescriptor *)out_frame->frame->data[0];
    ret = reset_linear_drm_desc(ctx, desc, out_info->pix_fmt, frame_w, frame_h);
    if (ret < 0)
        goto fail;
    ret = validate_drm_desc_shape(ctx, desc);
    if (ret < 0)
        goto fail;

    layer = &desc->layers[0];
    object_index = get_single_drm_object_index(desc, layer);
    if (object_index < 0)
        goto fail;

    object = &desc->objects[object_index];
    if (object->fd < 0)
        goto fail;

    if (r->is_rga2_used || is_rga2_core_mask(out_info->scheduler_core)) {
        if (!r->has_rga2p && pat_preproc && (act_w > 4096 || act_h > 4096)) {
            av_log(ctx, AV_LOG_ERROR, "Max supported output size of RGA2 (non-Pro) is 4096x4096\n");
            goto fail;
        }
        if (r->afbc_out && !pat_preproc) {
            av_log(ctx, AV_LOG_WARNING, "Output format '%s' with AFBC modifier is not supported by RGA2\n",
                   av_get_pix_fmt_name(out_info->pix_fmt));
            r->afbc_out = 0;
        }
    }

    is_afbc = r->afbc_out && !pat_preproc;
    if (is_afbc) {
        uint32_t drm_afbc_fmt = get_drm_afbc_format(out_info->pix_fmt);
        int afbc_w_stride = FFALIGN(pat_preproc ? inlink->w : outlink->w,
                                    RK_RGA_AFBC_16x16_STRIDE_ALIGN);
        int afbc_h_stride = FFALIGN(pat_preproc ? inlink->h : outlink->h,
                                    RK_RGA_AFBC_16x16_STRIDE_ALIGN);

        if (drm_afbc_fmt == DRM_FORMAT_INVALID) {
            av_log(ctx, AV_LOG_WARNING, "Output format '%s' with AFBC modifier is not supported\n",
                   av_get_pix_fmt_name(out_info->pix_fmt));
            is_afbc = r->afbc_out = 0;
        } else if ((out_info->rga_fmt == RK_FORMAT_YCbCr_420_SP_10B ||
                    out_info->rga_fmt == RK_FORMAT_YCbCr_422_SP_10B) &&
                   (afbc_w_stride % 64)) {
            av_log(ctx, AV_LOG_WARNING, "Output pixel wstride '%d' format '%s' is not supported by RGA3 AFBC\n",
                   afbc_w_stride, av_get_pix_fmt_name(out_info->pix_fmt));
            is_afbc = r->afbc_out = 0;
        } else {
            w_stride = afbc_w_stride;
            h_stride = afbc_h_stride;
        }
    }

    ret = validate_linear_drm_layer(ctx, object, layer, out_info->pix_fmt,
                                    frame_w, frame_h);
    if (ret < 0)
        goto fail;
    if (!is_afbc) {
        ret = get_pixel_stride(object,
                               layer,
                               out_info->pix_fmt,
                               (out_info->pix_desc->flags & AV_PIX_FMT_FLAG_RGB),
                               (out_info->pix_desc->flags & AV_PIX_FMT_FLAG_PLANAR),
                               frame_w, frame_h, &w_stride, &h_stride);
        if (ret < 0 || !w_stride || !h_stride) {
            av_log(ctx, AV_LOG_ERROR, "Failed to get frame strides\n");
            goto fail;
        }
    }

    ret = validate_active_rect_stride(ctx, &validate_out_info,
                                      w_stride, h_stride, "Output");
    if (ret < 0)
        goto fail;
    ret = validate_rga3_pixel_stride(ctx, &validate_out_info, out_info,
                                     w_stride, h_stride, "Output");
    if (ret < 0)
        goto fail;
    if (ret > 0 && is_afbc) {
        is_afbc = r->afbc_out = 0;
        ret = get_pixel_stride(object,
                               layer,
                               out_info->pix_fmt,
                               (out_info->pix_desc->flags & AV_PIX_FMT_FLAG_RGB),
                               (out_info->pix_desc->flags & AV_PIX_FMT_FLAG_PLANAR),
                               frame_w, frame_h, &w_stride, &h_stride);
        if (ret < 0 || !w_stride || !h_stride) {
            av_log(ctx, AV_LOG_ERROR, "Failed to get frame strides\n");
            goto fail;
        }
        ret = validate_active_rect_stride(ctx, &validate_out_info,
                                          w_stride, h_stride, "Output");
        if (ret < 0)
            goto fail;
        ret = validate_rga3_pixel_stride(ctx, &validate_out_info, out_info,
                                         w_stride, h_stride, "Output");
        if (ret < 0)
            goto fail;
    }
    if ((ret = verify_rga_frame_info_io_dynamic(ctx,
                                                pat_preproc ? &validate_in_info : in0_info,
                                                &validate_out_info)) < 0)
        goto fail;

    info.fd           = object->fd;
    info.format       = out_info->rga_fmt;
    info.core         = out_info->scheduler_core;
    info.in_fence_fd  = -1;
    info.out_fence_fd = -1;
    info.sync_mode    = RGA_BLIT_ASYNC;

    if (out_info->uncompact_10b_msb)
        info.is_10b_compact = info.is_10b_endian = 1;

    if (!pat_preproc) {
        int is_rga2_used = r->is_rga2_used ||
                           is_rga2_core_mask(out_info->scheduler_core);

#ifdef RGA_NORMAL_DST_FULL_CSC_FIXUP
        if (in1_info && picref_pat) {
            enum AVColorSpace pat_colorspace = picref_pat->colorspace;
            enum AVColorRange pat_color_range = picref_pat->color_range;
            /* yuv2rgb src->pat */
            ret = set_colorspace_info(ctx, in0_info, in->colorspace, in->color_range,
                                      in1_info, &pat_colorspace, &pat_color_range,
                                      &info.color_space_mode, is_rga2_used);
            if (ret < 0)
                goto fail;
            /* rgb2yuv pat->dst */
            ret = set_colorspace_info(ctx, in1_info, pat_colorspace, pat_color_range,
                                      out_info, &out_frame->frame->colorspace, &out_frame->frame->color_range,
                                      &info.color_space_mode, is_rga2_used);
            if (ret < 0)
                goto fail;
        } else
#endif
        {
            ret = set_colorspace_info(ctx, in0_info, in->colorspace, in->color_range,
                                      out_info, &out_frame->frame->colorspace, &out_frame->frame->color_range,
                                      &info.color_space_mode, is_rga2_used);
            if (ret < 0)
                goto fail;
        }
    }

    if (pat_preproc)
        rga_set_rect(&info.rect, in1_info->overlay_x, in1_info->overlay_y,
                     act_w, act_h,
                     w_stride, h_stride, in1_info->rga_fmt);
    else
        rga_set_rect(&info.rect, out_info->act_x, out_info->act_y,
                     act_w, act_h,
                     w_stride, h_stride, out_info->rga_fmt);

    if (is_afbc) {
        uint32_t drm_afbc_fmt = get_drm_afbc_format(out_info->pix_fmt);

#ifndef RGA_NORMAL_FBCE_RGB_BGR_FIXUP
        /* Inverted RGB/BGR order in FBCE */
        switch (info.rect.format) {
        case RK_FORMAT_RGBA_8888:
            info.rect.format = RK_FORMAT_BGRA_8888;
            break;
        case RK_FORMAT_BGRA_8888:
            info.rect.format = RK_FORMAT_RGBA_8888;
            break;
        }
#endif

        info.rect.wstride = w_stride;
        info.rect.hstride = h_stride;
        info.rd_mode = 1 << 1; /* IM_AFBC16x16_MODE */

        object->format_modifier =
            DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_SPARSE | AFBC_FORMAT_MOD_BLOCK_SIZE_16x16);

        layer->format = drm_afbc_fmt;
        layer->nb_planes = 1;

        layer->planes[0].offset = 0;
        layer->planes[0].pitch  = info.rect.wstride;

        if ((ret = get_afbc_byte_stride(out_info->pix_desc, info.rect.wstride,
                                        &layer->planes[0].pitch)) < 0)
            goto fail;
    }

    out_frame->info = info;

    return out_frame;

fail:
    release_frame(out_frame);

    return NULL;
}

static av_cold int init_hwframes_ctx(AVFilterContext *avctx)
{
    RKRGAContext      *r       = avctx->priv;
    AVFilterLink      *inlink  = avctx->inputs[0];
    AVFilterLink      *outlink = avctx->outputs[0];
    FilterLink        *inl     = ff_filter_link(inlink);
    FilterLink        *outl    = ff_filter_link(outlink);
    AVHWFramesContext *hwfc_in;
    AVHWFramesContext *hwfc_out;
    AVBufferRef       *hwfc_out_ref;
    AVHWDeviceContext *device_ctx;
    AVBufferRef       *device_ref;
    AVRKMPPFramesContext *rkmpp_fc;
    int                ret;

    if (!inl->hw_frames_ctx)
        return AVERROR(EINVAL);

    hwfc_in = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    device_ref = hwfc_in->device_ref;
    device_ctx = (AVHWDeviceContext *)device_ref->data;

    if (!device_ctx || device_ctx->type != AV_HWDEVICE_TYPE_RKMPP) {
        if (avctx->hw_device_ctx) {
            device_ref = avctx->hw_device_ctx;
            device_ctx = (AVHWDeviceContext *)device_ref->data;
        }
        if (!device_ctx || device_ctx->type != AV_HWDEVICE_TYPE_RKMPP) {
            av_log(avctx, AV_LOG_ERROR, "No RKMPP hardware context provided\n");
            return AVERROR(EINVAL);
        }
    }

    hwfc_out_ref = av_hwframe_ctx_alloc(device_ref);
    if (!hwfc_out_ref)
        return AVERROR(ENOMEM);

    hwfc_out = (AVHWFramesContext *)hwfc_out_ref->data;
    hwfc_out->format    = AV_PIX_FMT_DRM_PRIME;
    hwfc_out->sw_format = r->out_sw_format;
    hwfc_out->width     = outlink->w;
    hwfc_out->height    = outlink->h;

    rkmpp_fc = hwfc_out->hwctx;
    rkmpp_fc->flags |= MPP_BUFFER_FLAGS_CACHABLE;

    ret = av_hwframe_ctx_init(hwfc_out_ref);
    if (ret < 0) {
        av_buffer_unref(&hwfc_out_ref);
        av_log(avctx, AV_LOG_ERROR, "Error creating frames_ctx for output pad: %d\n", ret);
        return ret;
    }

    av_buffer_unref(&outl->hw_frames_ctx);
    outl->hw_frames_ctx = hwfc_out_ref;

    return 0;
}

static av_cold int init_pat_preproc_hwframes_ctx(AVFilterContext *avctx)
{
    RKRGAContext      *r = avctx->priv;
    AVFilterLink      *inlink0 = avctx->inputs[0];
    AVFilterLink      *inlink1 = avctx->inputs[1];
    FilterLink        *inl0 = ff_filter_link(inlink0);
    FilterLink        *inl1 = ff_filter_link(inlink1);
    AVHWFramesContext *hwfc_in0, *hwfc_in1;
    AVHWFramesContext *hwfc_pat;
    AVBufferRef       *hwfc_pat_ref;
    AVHWDeviceContext *device_ctx0;
    AVBufferRef       *device_ref0;
    int                ret;

    if (!inl0->hw_frames_ctx || !inl1->hw_frames_ctx)
        return AVERROR(EINVAL);

    hwfc_in0 = (AVHWFramesContext *)inl0->hw_frames_ctx->data;
    hwfc_in1 = (AVHWFramesContext *)inl1->hw_frames_ctx->data;
    device_ref0 = hwfc_in0->device_ref;
    device_ctx0 = (AVHWDeviceContext *)device_ref0->data;

    if (!device_ctx0 || device_ctx0->type != AV_HWDEVICE_TYPE_RKMPP) {
        if (avctx->hw_device_ctx) {
            device_ref0 = avctx->hw_device_ctx;
            device_ctx0 = (AVHWDeviceContext *)device_ref0->data;
        }
        if (!device_ctx0 || device_ctx0->type != AV_HWDEVICE_TYPE_RKMPP) {
            av_log(avctx, AV_LOG_ERROR, "No RKMPP hardware context provided\n");
            return AVERROR(EINVAL);
        }
    }

    hwfc_pat_ref = av_hwframe_ctx_alloc(device_ref0);
    if (!hwfc_pat_ref)
        return AVERROR(ENOMEM);

    hwfc_pat = (AVHWFramesContext *)hwfc_pat_ref->data;
    hwfc_pat->format    = AV_PIX_FMT_DRM_PRIME;
    hwfc_pat->sw_format = hwfc_in1->sw_format;
    hwfc_pat->width     = inlink0->w;
    hwfc_pat->height    = inlink0->h;

    ret = av_hwframe_ctx_init(hwfc_pat_ref);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error creating frames_ctx for pat preproc: %d\n", ret);
        av_buffer_unref(&hwfc_pat_ref);
        return ret;
    }

    av_buffer_unref(&r->pat_preproc_hwframes_ctx);
    r->pat_preproc_hwframes_ctx = hwfc_pat_ref;

    return 0;
}

static av_cold int verify_rga_frame_info(AVFilterContext *avctx,
                                         RGAFrameInfo *src, RGAFrameInfo *dst, RGAFrameInfo *pat)
{
    RKRGAContext *r = avctx->priv;
    float scale_ratio_min, scale_ratio_max;
    float scale_ratio_w, scale_ratio_h;
    int scale_src_w, scale_src_h;
    int ret;

    if (!src || !dst)
        return AVERROR(EINVAL);

    if (dst->pix_fmt == AV_PIX_FMT_NV15 ||
        dst->pix_fmt == AV_PIX_FMT_NV20_PACKED) {
        av_log(avctx, AV_LOG_ERROR, "'%s' as output is not supported\n",
               av_get_pix_fmt_name(dst->pix_fmt));
        return AVERROR(ENOSYS);
    }

    scale_src_w = src->act_w;
    scale_src_h = src->act_h;
    if (rga_rotates_dimensions(src->rotate_mode))
        FFSWAP(int, scale_src_w, scale_src_h);

    scale_ratio_w = (float)dst->act_w / (float)scale_src_w;
    scale_ratio_h = (float)dst->act_h / (float)scale_src_h;

    /* P010 requires RGA3 */
    if (!r->has_rga3 &&
        (src->pix_fmt == AV_PIX_FMT_P010 ||
         dst->pix_fmt == AV_PIX_FMT_P010)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' is only supported by RGA3\n",
               av_get_pix_fmt_name(AV_PIX_FMT_P010));
        return AVERROR(ENOSYS);
    }
    /* P210 requires RGA3 */
    if (!r->has_rga3 &&
        (src->pix_fmt == AV_PIX_FMT_P210 ||
         dst->pix_fmt == AV_PIX_FMT_P210)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' is only supported by RGA3\n",
               av_get_pix_fmt_name(AV_PIX_FMT_P210));
        return AVERROR(ENOSYS);
    }
    /* NV24/NV42 requires RGA2-Pro */
    if (!r->has_rga2p &&
        (src->pix_fmt == AV_PIX_FMT_NV24 ||
         src->pix_fmt == AV_PIX_FMT_NV42 ||
         dst->pix_fmt == AV_PIX_FMT_NV24 ||
         dst->pix_fmt == AV_PIX_FMT_NV42)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' and '%s' are only supported by RGA2-Pro\n",
               av_get_pix_fmt_name(AV_PIX_FMT_NV24),
               av_get_pix_fmt_name(AV_PIX_FMT_NV42));
        return AVERROR(ENOSYS);
    }
    /* Input formats that requires RGA2 */
    if (!r->has_rga2 &&
        (is_rga2_only_input_format(src->pix_fmt) ||
         (pat && is_rga2_only_input_format(pat->pix_fmt)))) {
        av_log(avctx, AV_LOG_ERROR, "'%s' as input is only supported by RGA2\n",
               av_get_pix_fmt_name(is_rga2_only_input_format(src->pix_fmt) ?
                                   src->pix_fmt : pat->pix_fmt));
        return AVERROR(ENOSYS);
    }
    /* Output formats that requires RGA2 */
    if (!r->has_rga2 &&
        (dst->pix_fmt == AV_PIX_FMT_GRAY8 ||
         dst->pix_fmt == AV_PIX_FMT_YUV420P ||
         dst->pix_fmt == AV_PIX_FMT_YUVJ420P ||
         dst->pix_fmt == AV_PIX_FMT_YUV422P ||
         dst->pix_fmt == AV_PIX_FMT_YUVJ422P ||
         dst->pix_fmt == AV_PIX_FMT_RGB555LE ||
         dst->pix_fmt == AV_PIX_FMT_BGR555LE ||
         dst->pix_fmt == AV_PIX_FMT_ARGB ||
         dst->pix_fmt == AV_PIX_FMT_0RGB ||
         dst->pix_fmt == AV_PIX_FMT_ABGR ||
         dst->pix_fmt == AV_PIX_FMT_0BGR)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' as output is only supported by RGA2\n",
               av_get_pix_fmt_name(dst->pix_fmt));
        return AVERROR(ENOSYS);
    }
    /* non-YUVJ format to YUVJ format is not supported */
    if ((dst->pix_fmt == AV_PIX_FMT_YUVJ420P ||
         dst->pix_fmt == AV_PIX_FMT_YUVJ422P) &&
         (src->pix_fmt != AV_PIX_FMT_YUVJ420P &&
          src->pix_fmt != AV_PIX_FMT_YUVJ422P)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' to '%s' is not supported\n",
               av_get_pix_fmt_name(src->pix_fmt),
               av_get_pix_fmt_name(dst->pix_fmt));
        return AVERROR(ENOSYS);
    }
    /* P010/P210 requires RGA3 but it can't handle certain formats */
    if ((src->pix_fmt == AV_PIX_FMT_P010 ||
         src->pix_fmt == AV_PIX_FMT_P210) &&
         (dst->pix_fmt == AV_PIX_FMT_GRAY8 ||
          dst->pix_fmt == AV_PIX_FMT_YUV420P ||
          dst->pix_fmt == AV_PIX_FMT_YUVJ420P ||
          dst->pix_fmt == AV_PIX_FMT_YUV422P ||
          dst->pix_fmt == AV_PIX_FMT_YUVJ422P ||
          dst->pix_fmt == AV_PIX_FMT_RGB555LE ||
          dst->pix_fmt == AV_PIX_FMT_BGR555LE ||
          dst->pix_fmt == AV_PIX_FMT_ARGB ||
          dst->pix_fmt == AV_PIX_FMT_0RGB ||
          dst->pix_fmt == AV_PIX_FMT_ABGR ||
          dst->pix_fmt == AV_PIX_FMT_0BGR)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' to '%s' is not supported\n",
               av_get_pix_fmt_name(src->pix_fmt),
               av_get_pix_fmt_name(dst->pix_fmt));
        return AVERROR(ENOSYS);
    }
    /* RGA3 only format to RGA2 only format is not supported */
    if ((dst->pix_fmt == AV_PIX_FMT_P010 ||
         dst->pix_fmt == AV_PIX_FMT_P210) &&
         (src->pix_fmt == AV_PIX_FMT_GRAY8 ||
          src->pix_fmt == AV_PIX_FMT_YUV420P ||
          src->pix_fmt == AV_PIX_FMT_YUVJ420P ||
          src->pix_fmt == AV_PIX_FMT_YUV422P ||
          src->pix_fmt == AV_PIX_FMT_YUVJ422P ||
          src->pix_fmt == AV_PIX_FMT_RGB555LE ||
          src->pix_fmt == AV_PIX_FMT_BGR555LE)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' to '%s' is not supported\n",
               av_get_pix_fmt_name(src->pix_fmt),
               av_get_pix_fmt_name(dst->pix_fmt));
        return AVERROR(ENOSYS);
    }

    if (is_rga2_only_input_format(src->pix_fmt) ||
        src->pix_fmt == AV_PIX_FMT_NV24 ||
        src->pix_fmt == AV_PIX_FMT_NV42 ||
        dst->pix_fmt == AV_PIX_FMT_GRAY8 ||
        dst->pix_fmt == AV_PIX_FMT_YUV420P ||
        dst->pix_fmt == AV_PIX_FMT_YUVJ420P ||
        dst->pix_fmt == AV_PIX_FMT_YUV422P ||
        dst->pix_fmt == AV_PIX_FMT_YUVJ422P ||
        dst->pix_fmt == AV_PIX_FMT_NV24 ||
        dst->pix_fmt == AV_PIX_FMT_NV42 ||
        dst->pix_fmt == AV_PIX_FMT_RGB555LE ||
        dst->pix_fmt == AV_PIX_FMT_BGR555LE ||
        dst->pix_fmt == AV_PIX_FMT_ARGB ||
        dst->pix_fmt == AV_PIX_FMT_0RGB ||
        dst->pix_fmt == AV_PIX_FMT_ABGR ||
        dst->pix_fmt == AV_PIX_FMT_0BGR ||
        (pat && is_rga2_only_input_format(pat->pix_fmt))) {
        r->is_rga2_used = 1;
    }

    r->is_rga2_used = r->is_rga2_used || !r->has_rga3;
    if (r->is_rga2_used && is_rga3_only_input_format(src->pix_fmt)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' input is not supported if RGA2 is requested\n",
               av_get_pix_fmt_name(src->pix_fmt));
        return AVERROR(ENOSYS);
    }
    if (pat && r->is_rga2_used && is_rga3_only_input_format(pat->pix_fmt)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' overlay input is not supported if RGA2 is requested\n",
               av_get_pix_fmt_name(pat->pix_fmt));
        return AVERROR(ENOSYS);
    }
    if (r->has_rga3) {
        if (scale_ratio_w < 0.125f ||
            scale_ratio_w > 8.0f ||
            scale_ratio_h < 0.125f ||
            scale_ratio_h > 8.0f) {
            r->is_rga2_used = 1;
        }
        if (src->act_w < 68 ||
            src->act_w > 8176 ||
            src->act_h < 2 ||
            src->act_h > 8176 ||
            dst->act_w < 68 ||
            dst->act_w > 8128 ||
            dst->act_h > 8128) {
            r->is_rga2_used = 1;
        }
        if (pat && (pat->act_w < 68 ||
             pat->act_w > 8176 ||
             pat->act_h < 2 ||
             pat->act_h > 8176)) {
            r->is_rga2_used = 1;
        }
    }
    if (pat && r->is_rga2_used && is_rga3_only_input_format(pat->pix_fmt)) {
        av_log(avctx, AV_LOG_ERROR, "'%s' overlay input is not supported if RGA2 is requested\n",
               av_get_pix_fmt_name(pat->pix_fmt));
        return AVERROR(ENOSYS);
    }

    if ((ret = verify_rga_frame_info_io_dynamic(avctx, src, dst)) < 0)
        return ret;

    if (r->is_rga2_used) {
        if (!is_rga2_core_mask(r->scheduler_core) &&
            (r->has_rga3 || r->has_rga2p)) {
            r->scheduler_core = 0x4;
            if (r->has_rga2p)
                r->scheduler_core |= 0x8;
        } else if (!(r->has_rga3 || r->has_rga2p)) {
            r->scheduler_core = 0;
        }
    }

    /* Prioritize RGA3 on multicore RGA hw to avoid dma32 & algorithm quirks as much as possible */
    if (r->has_rga3 && r->has_rga2e && !r->is_rga2_used &&
        (r->scheduler_core == 0 || avctx->nb_inputs > 1 ||
         scale_ratio_w != 1.0f || scale_ratio_h != 1.0f ||
         src->crop || src->uncompact_10b_msb || dst->uncompact_10b_msb)) {
        r->scheduler_core = 0x3;
    }

    scale_ratio_max = 16.0f;
    if ((r->is_rga2_used && r->has_rga2l) ||
        (!r->is_rga2_used && r->has_rga3 && !r->has_rga2) ||
        is_rga3_core_mask(r->scheduler_core)) {
        scale_ratio_max = 8.0f;
    }
    scale_ratio_min = 1.0f / scale_ratio_max;

    if (scale_ratio_w < scale_ratio_min || scale_ratio_w > scale_ratio_max ||
        scale_ratio_h < scale_ratio_min || scale_ratio_h > scale_ratio_max) {
        av_log(avctx, AV_LOG_ERROR, "RGA scale ratio (%.04fx%.04f) exceeds %.04f ~ %.04f.\n",
               scale_ratio_w, scale_ratio_h, scale_ratio_min, scale_ratio_max);
        return AVERROR(EINVAL);
    }

    return 0;
}

static av_cold int fill_rga_frame_info_by_link(AVFilterContext *avctx,
                                               RGAFrameInfo *info,
                                               AVFilterLink *link,
                                               int nb_link, int is_inlink,
                                               int validate_format)
{
    AVHWFramesContext *hwfc;
    FilterLink *l = ff_filter_link(link);
    RKRGAContext *r = avctx->priv;

    if (!l->hw_frames_ctx || link->format != AV_PIX_FMT_DRM_PRIME)
        return AVERROR(EINVAL);

    hwfc = (AVHWFramesContext *)l->hw_frames_ctx->data;

    if (validate_format &&
        !map_av_to_rga_format(hwfc->sw_format, &info->rga_fmt, (is_inlink && nb_link > 0))) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported '%s' pad %d format: '%s'\n",
               (is_inlink ? "input" : "output"), nb_link,
               av_get_pix_fmt_name(hwfc->sw_format));
        return AVERROR(ENOSYS);
    }

    info->pix_fmt  = hwfc->sw_format;
    info->pix_desc = av_pix_fmt_desc_get(info->pix_fmt);
    if (!info->pix_desc)
        return AVERROR(EINVAL);
    info->act_x    = 0;
    info->act_y    = 0;
    info->act_w    = link->w;
    info->act_h    = link->h;

    /* The w/h of RGA YUV image needs to be 2 aligned. */
    if (validate_format &&
        !(info->pix_desc->flags & AV_PIX_FMT_FLAG_RGB) &&
        ((info->act_w | info->act_h) & (RK_RGA_YUV_ALIGN - 1))) {
        av_log(avctx, AV_LOG_ERROR, "'%s' pad %d format '%s' requires even dimensions\n",
               is_inlink ? "input" : "output", nb_link,
               av_get_pix_fmt_name(info->pix_fmt));
        return AVERROR(EINVAL);
    }

    info->uncompact_10b_msb = info->pix_fmt == AV_PIX_FMT_P010 ||
                              info->pix_fmt == AV_PIX_FMT_P210;

    if (link->w * link->h > (3840 * 2160 * 3))
        r->async_depth = FFMIN(r->async_depth, 1);

    return 0;
}

av_cold int ff_rkrga_init(AVFilterContext *avctx, RKRGAParam *param)
{
    RKRGAContext *r = avctx->priv;
    RGAFrameInfo clipped_pat_info;
    RGAFrameInfo *pat_info_for_verify = NULL;
    int i, ret;
    int rga_core_mask = 0x7;
    const char *rga_ver = querystring(RGA_VERSION);

    r->got_frame = 0;

    r->has_rga2  = !!strstr(rga_ver, "RGA_2");
    r->has_rga2l = !!strstr(rga_ver, "RGA_2_lite");
    r->has_rga2e = !!strstr(rga_ver, "RGA_2_Enhance");
    r->has_rga2p = !!strstr(rga_ver, "RGA_2_PRO");
    r->has_rga3  = !!strstr(rga_ver, "RGA_3");

    if (!(r->has_rga2 || r->has_rga3)) {
        av_log(avctx, AV_LOG_ERROR, "No RGA2/RGA3 hw available\n");
        return AVERROR(ENOSYS);
    }

    if (r->has_rga2p)
        rga_core_mask = 0xf;

    /* RGA core */
    if (r->scheduler_core && !(r->has_rga2 && r->has_rga3) && !r->has_rga2p) {
        av_log(avctx, AV_LOG_WARNING, "Scheduler core cannot be set on non-multicore RGA hw, ignoring\n");
        r->scheduler_core = 0;
    }
    if (r->scheduler_core &&
        is_rga3_core_mask(r->scheduler_core) && !r->has_rga3) {
        av_log(avctx, AV_LOG_WARNING, "Invalid scheduler core set, ignoring\n");
        r->scheduler_core = 0;
    }
    if (r->scheduler_core && r->scheduler_core != (r->scheduler_core & rga_core_mask)) {
        av_log(avctx, AV_LOG_WARNING, "Invalid scheduler core set, ignoring\n");
        r->scheduler_core = 0;
    }
    if (has_mixed_rga_core_mask(r->scheduler_core)) {
        av_log(avctx, AV_LOG_ERROR, "Mixed RGA2/RGA3 scheduler core masks are not supported\n");
        return AVERROR(EINVAL);
    }
    if (is_rga3_core_mask(r->scheduler_core))
        r->has_rga2 = r->has_rga2l = r->has_rga2e = r->has_rga2p = 0;
    if (is_rga2_core_mask(r->scheduler_core))
        r->has_rga3 = 0;

    r->filter_frame = param->filter_frame;
    if (!r->filter_frame)
         r->filter_frame = ff_filter_frame;
    r->out_sw_format = param->out_sw_format;

    /* OUT hwfc */
    ret = init_hwframes_ctx(avctx);
    if (ret < 0)
        goto fail;

    /* IN RGAFrameInfo */
    r->in_rga_frame_infos = av_calloc(avctx->nb_inputs, sizeof(*r->in_rga_frame_infos));
    if (!r->in_rga_frame_infos) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ret = fill_rga_frame_info_by_link(avctx, &r->in_rga_frame_infos[0],
                                      avctx->inputs[0], 0, 1, 1);
    if (ret < 0)
        goto fail;
    if (avctx->nb_inputs > 1) {
        if (param->overlay_x < 0 || param->overlay_y < 0) {
            av_log(avctx, AV_LOG_ERROR, "Negative overlay offsets are not supported\n");
            ret = AVERROR(EINVAL);
            goto fail;
        }

        r->is_overlay_offset_valid =
            param->overlay_x <= r->in_rga_frame_infos[0].act_w - 2 &&
            param->overlay_y <= r->in_rga_frame_infos[0].act_h - 2;

        ret = fill_rga_frame_info_by_link(avctx, &r->in_rga_frame_infos[1],
                                          avctx->inputs[1], 1, 1,
                                          r->is_overlay_offset_valid);
        if (ret < 0)
            goto fail;
    }
    for (i = 2; i < avctx->nb_inputs; i++) {
        ret = fill_rga_frame_info_by_link(avctx, &r->in_rga_frame_infos[i],
                                          avctx->inputs[i], i, 1, 1);
        if (ret < 0)
            goto fail;
    }
    if (avctx->nb_inputs == 1) {
        r->in_rga_frame_infos[0].rotate_mode = param->in_rotate_mode;

        if (param->in_crop) {
            /* The x/y/w/h of RGA YUV image needs to be 2 aligned */
            if (!(r->in_rga_frame_infos[0].pix_desc->flags & AV_PIX_FMT_FLAG_RGB) &&
                ((param->in_crop_x | param->in_crop_y |
                  param->in_crop_w | param->in_crop_h) & (RK_RGA_YUV_ALIGN - 1))) {
                av_log(avctx, AV_LOG_ERROR,
                       "YUV crop rectangles require even x/y/width/height\n");
                ret = AVERROR(EINVAL);
                goto fail;
            }
            r->in_rga_frame_infos[0].crop = 1;
            r->in_rga_frame_infos[0].act_x = param->in_crop_x;
            r->in_rga_frame_infos[0].act_y = param->in_crop_y;
            r->in_rga_frame_infos[0].act_w = param->in_crop_w;
            r->in_rga_frame_infos[0].act_h = param->in_crop_h;
        }
    }
    if (avctx->nb_inputs > 1) {
        int need_premultiply = 0;

        if (r->is_overlay_offset_valid &&
            r->in_rga_frame_infos[1].pix_desc->flags & AV_PIX_FMT_FLAG_ALPHA)
            need_premultiply = param->in_alpha_format == 0;

        /* IM_ALPHA_BLEND_DST_OVER */
        if (param->in_global_alpha >= 0 && param->in_global_alpha < 0xff) {
            r->in_rga_frame_infos[0].blend_mode = need_premultiply ? (0x4 | (1 << 12)) : 0x4;
            r->in_rga_frame_infos[0].blend_mode |= (param->in_global_alpha & 0xff) << 16; /* fg_global_alpha */
            r->in_rga_frame_infos[0].blend_mode |= 0xff << 24;                            /* bg_global_alpha */
        } else
            r->in_rga_frame_infos[0].blend_mode = need_premultiply ? 0x504 : 0x501;

        r->in_rga_frame_infos[1].overlay_x = param->overlay_x;
        r->in_rga_frame_infos[1].overlay_y = param->overlay_y;

        if (r->is_overlay_offset_valid) {
            ret = init_pat_preproc_hwframes_ctx(avctx);
            if (ret < 0)
                goto fail;
        }
    }

    /* OUT RGAFrameInfo */
    ret = fill_rga_frame_info_by_link(avctx, &r->out_rga_frame_info,
                                      avctx->outputs[0], 0, 0, 1);
    if (ret < 0)
        goto fail;

    if (avctx->nb_inputs > 1 && r->is_overlay_offset_valid) {
        RGAFrameInfo *in0_info = &r->in_rga_frame_infos[0];
        RGAFrameInfo *in1_info = &r->in_rga_frame_infos[1];
        RGAFrameInfo *out_info = &r->out_rga_frame_info;
        int overlay_w = FFMIN(in0_info->act_w - in1_info->overlay_x, in1_info->act_w);
        int overlay_h = FFMIN(in0_info->act_h - in1_info->overlay_y, in1_info->act_h);

        if (!is_yuv_rect_aligned(in0_info, in1_info->overlay_x, in1_info->overlay_y,
                                 overlay_w, overlay_h) ||
            !is_yuv_rect_aligned(out_info, in1_info->overlay_x, in1_info->overlay_y,
                                 overlay_w, overlay_h)) {
            av_log(avctx, AV_LOG_ERROR,
                   "YUV overlay rectangles require even x/y/width/height after clipping\n");
            ret = AVERROR(EINVAL);
            goto fail;
        }

        clipped_pat_info = *in1_info;
        clipped_pat_info.act_w = overlay_w;
        clipped_pat_info.act_h = overlay_h;
        pat_info_for_verify = &clipped_pat_info;
    }

    /* Pre-check RGAFrameInfo */
    ret = verify_rga_frame_info(avctx, &r->in_rga_frame_infos[0],
                                &r->out_rga_frame_info, pat_info_for_verify);
    if (ret < 0)
        goto fail;

    r->out_rga_frame_info.scheduler_core = r->scheduler_core;

    /* keep fifo size at least 1. Even when async_depth is 0, fifo is used. */
    r->async_fifo  = av_fifo_alloc2(r->async_depth + 1, sizeof(RGAAsyncFrame), 0);
    if (!r->async_fifo) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    return 0;

fail:
    ff_rkrga_close(avctx);
    return ret;
}

static void set_rga_async_frame_lock_status(RGAAsyncFrame *frame, int lock)
{
    int status = !!lock;

    if (!frame)
        return;

    if (frame->src)
        frame->src->locked = status;
    if (frame->dst)
        frame->dst->locked = status;
    if (frame->pat)
        frame->pat->locked = status;
}

static void rga_drain_fifo(RKRGAContext *r)
{
    RGAAsyncFrame aframe;

    while (r->async_fifo && av_fifo_read(r->async_fifo, &aframe, 1) >= 0) {
        if (imsync(aframe.dst->info.out_fence_fd) != IM_STATUS_SUCCESS)
            av_log(NULL, AV_LOG_WARNING, "RGA sync failed\n");

        set_rga_async_frame_lock_status(&aframe, 0);
    }
}

av_cold int ff_rkrga_close(AVFilterContext *avctx)
{
    RKRGAContext *r = avctx->priv;

    /* Drain the fifo during filter reset */
    rga_drain_fifo(r);

    clear_frame_list(&r->src_frame_list);
    clear_frame_list(&r->dst_frame_list);
    clear_frame_list(&r->pat_frame_list);

    clear_frame_list(&r->pat_preproc_frame_list);

    if (r->in_rga_frame_infos)
        av_freep(&r->in_rga_frame_infos);

    av_fifo_freep2(&r->async_fifo);

    av_buffer_unref(&r->pat_preproc_hwframes_ctx);

    return 0;
}

static int call_rkrga_blit(AVFilterContext *avctx,
                          rga_info_t *src_info,
                          rga_info_t *dst_info,
                          rga_info_t *pat_info)
{
    int ret;

    if (!src_info || !dst_info)
        return AVERROR(EINVAL);

#define PRINT_RGA_INFO(ctx, info, name) do { \
    if (info && name) \
        av_log(ctx, AV_LOG_DEBUG, "RGA %s | fd:%d mmu:%d rd:%d csc:%d | x:%d y:%d w:%d h:%d ws:%d hs:%d fmt:0x%x\n", \
               name, info->fd, info->mmuFlag, (info->rd_mode >> 1), info->color_space_mode, info->rect.xoffset, info->rect.yoffset, \
               info->rect.width, info->rect.height, info->rect.wstride, info->rect.hstride, (info->rect.format >> 8)); \
} while (0)

    PRINT_RGA_INFO(avctx, src_info, "src");
    PRINT_RGA_INFO(avctx, dst_info, "dst");
    PRINT_RGA_INFO(avctx, pat_info, "pat");
#undef PRINT_RGA_INFO

    if ((ret = c_RkRgaBlit(src_info, dst_info, pat_info)) != 0) {
        av_log(avctx, AV_LOG_ERROR, "RGA blit failed: %d\n", ret);
        return AVERROR_EXTERNAL;
    }
    if (dst_info->sync_mode == RGA_BLIT_ASYNC &&
        dst_info->out_fence_fd < 0) {
        av_log(avctx, AV_LOG_ERROR, "RGA async blit returned invalid fence_fd: %d\n",
               dst_info->out_fence_fd);
        return AVERROR_EXTERNAL;
    }

    return 0;
}

int ff_rkrga_filter_frame(RKRGAContext *r,
                          AVFilterLink *inlink_src, AVFrame *picref_src,
                          AVFilterLink *inlink_pat, AVFrame *picref_pat)
{
    AVFilterContext  *ctx = inlink_src->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    RGAAsyncFrame aframe;
    RGAFrame *src_frame = NULL;
    RGAFrame *dst_frame = NULL;
    RGAFrame *pat_frame = NULL;
    RGAFrame *pat_in = NULL;
    RGAFrame *pat_out = NULL;
    int ret, filter_ret;
    int do_overlay = ctx->nb_inputs > 1 &&
                     r->is_overlay_offset_valid &&
                     inlink_pat && picref_pat;

    /* Sync & Drain */
    while (r->eof && av_fifo_read(r->async_fifo, &aframe, 1) >= 0) {
        if (imsync(aframe.dst->info.out_fence_fd) != IM_STATUS_SUCCESS)
            av_log(ctx, AV_LOG_WARNING, "RGA sync failed\n");

        set_rga_async_frame_lock_status(&aframe, 0);

        filter_ret = r->filter_frame(outlink, aframe.dst->frame);
        /* ff_filter_frame consumes the frame even on failure */
        aframe.dst->frame = NULL;
        aframe.dst->queued--;
        if (filter_ret < 0) {
            release_frame(aframe.src);
            release_frame(aframe.dst);
            release_frame(aframe.pat);
            return filter_ret;
        }
        r->got_frame = 1;
    }

    if (!picref_src)
        return 0;

    /* SRC */
    if (!(src_frame = submit_frame(r, inlink_src, picref_src, do_overlay, 0))) {
        av_log(ctx, AV_LOG_ERROR, "Failed to submit frame on input: %d\n",
               FF_INLINK_IDX(inlink_src));
        return AVERROR(ENOMEM);
    }

    /* DST */
    if (!(dst_frame = query_frame(r, outlink, src_frame->frame, picref_pat, 0))) {
        av_log(ctx, AV_LOG_ERROR, "Failed to query an output frame\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    /* PAT */
    if (do_overlay) {
        RGAFrameInfo *in0_info = &r->in_rga_frame_infos[0];
        RGAFrameInfo *in1_info = &r->in_rga_frame_infos[1];
        RGAFrameInfo *out_info = &r->out_rga_frame_info;
        int pat_preprocessed = 0;

        /* translate PAT from top-left to (x,y) on a new image with the same size of SRC */
        if (in1_info->act_w != in0_info->act_w ||
            in1_info->act_h != in0_info->act_h ||
            in1_info->overlay_x > 0 ||
            in1_info->overlay_y > 0) {
            if (!(pat_in = submit_frame(r, inlink_pat, picref_pat, 0, 1))) {
                av_log(ctx, AV_LOG_ERROR, "Failed to submit frame on input: %d\n",
                       FF_INLINK_IDX(inlink_pat));
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            if (!(pat_out = query_frame(r, outlink, picref_pat, NULL, 1))) {
                av_log(ctx, AV_LOG_ERROR, "Failed to query an output frame\n");
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            dst_frame->info.core = out_info->scheduler_core;

            pat_out->info.priority = 1;
            pat_out->info.core = dst_frame->info.core;
            pat_out->info.sync_mode = RGA_BLIT_SYNC;

            /* Sync Blit Pre-Proc */
            ret = call_rkrga_blit(ctx, &pat_in->info, &pat_out->info, NULL);
            if (ret < 0)
                goto fail;

            pat_frame = pat_out;
            pat_preprocessed = 1;
        }

        if (!pat_frame && !(pat_frame = submit_frame(r, inlink_pat, picref_pat, 0, 0))) {
            av_log(ctx, AV_LOG_ERROR, "Failed to submit frame on input: %d\n",
                   FF_INLINK_IDX(inlink_pat));
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        dst_frame->info.core = out_info->scheduler_core;

        if (pat_preprocessed) {
            rga_info_t src_copy_info = src_frame->info;
            rga_info_t dst_copy_info = dst_frame->info;

            src_copy_info.blend = 0;
            dst_copy_info.sync_mode = RGA_BLIT_SYNC;
            dst_copy_info.out_fence_fd = -1;

            ret = call_rkrga_blit(ctx, &src_copy_info, &dst_copy_info, NULL);
            if (ret < 0)
                goto fail;

            src_frame->info.rect.xoffset += in1_info->overlay_x;
            src_frame->info.rect.yoffset += in1_info->overlay_y;
            src_frame->info.rect.width    = pat_frame->info.rect.width;
            src_frame->info.rect.height   = pat_frame->info.rect.height;

            dst_frame->info.rect.xoffset += in1_info->overlay_x;
            dst_frame->info.rect.yoffset += in1_info->overlay_y;
            dst_frame->info.rect.width    = pat_frame->info.rect.width;
            dst_frame->info.rect.height   = pat_frame->info.rect.height;
        }
    }

    /* Async Blit */
    ret = call_rkrga_blit(ctx,
                          &src_frame->info,
                          &dst_frame->info,
                          pat_frame ? &pat_frame->info : NULL);
    if (ret < 0)
        goto fail;

    dst_frame->queued++;
    aframe = (RGAAsyncFrame){ src_frame, dst_frame, pat_frame };
    set_rga_async_frame_lock_status(&aframe, 1);
    ret = av_fifo_write(r->async_fifo, &aframe, 1);
    if (ret < 0) {
        if (imsync(dst_frame->info.out_fence_fd) != IM_STATUS_SUCCESS)
            av_log(ctx, AV_LOG_WARNING, "RGA sync failed\n");
        set_rga_async_frame_lock_status(&aframe, 0);
        goto fail;
    }

    /* Sync & Retrieve */
    if (av_fifo_can_read(r->async_fifo) > r->async_depth) {
        av_fifo_read(r->async_fifo, &aframe, 1);
        if (imsync(aframe.dst->info.out_fence_fd) != IM_STATUS_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "RGA sync failed\n");
            set_rga_async_frame_lock_status(&aframe, 0);
            release_frame(aframe.src);
            release_frame(aframe.dst);
            release_frame(aframe.pat);
            return AVERROR_EXTERNAL;
        }
        set_rga_async_frame_lock_status(&aframe, 0);

        filter_ret = r->filter_frame(outlink, aframe.dst->frame);
        /* ff_filter_frame consumes the frame even on failure */
        aframe.dst->frame = NULL;
        aframe.dst->queued--;
        if (filter_ret < 0) {
            release_frame(aframe.src);
            release_frame(aframe.dst);
            release_frame(aframe.pat);
            return filter_ret;
        }
        r->got_frame = 1;
    }

    return 0;

fail:
    release_frame(src_frame);
    release_frame(dst_frame);
    release_frame(pat_in);
    if (pat_out != pat_frame)
        release_frame(pat_out);
    release_frame(pat_frame);
    return ret;
}
