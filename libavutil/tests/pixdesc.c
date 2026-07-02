/*
 * pixel format descriptor
 * Copyright (c) 2009 Michael Niedermayer <michaelni@gmx.at>
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

#include <string.h>

#include "libavutil/log.h"
#include "libavutil/pixdesc.c"

static int check_bytes(const char *name, const uint8_t *got,
                       const uint8_t *expected, size_t size)
{
    if (!memcmp(got, expected, size))
        return 0;

    av_log(NULL, AV_LOG_ERROR, "%s byte mismatch\n", name);
    return 1;
}

static int check_values(const char *name, const uint32_t *got,
                        const uint32_t *expected, size_t nb_values)
{
    if (!memcmp(got, expected, nb_values * sizeof(*got)))
        return 0;

    av_log(NULL, AV_LOG_ERROR, "%s value mismatch\n", name);
    return 1;
}

static int check_compact_10bit(enum AVPixelFormat pix_fmt)
{
    static const uint32_t y_values[4] = { 0x001, 0x155, 0x2aa, 0x3ff };
    static const uint32_t u_values[2] = { 0x123, 0x321 };
    static const uint32_t v_values[2] = { 0x234, 0x3ab };
    static const uint8_t y_bytes[5]   = { 0x01, 0x54, 0xa5, 0xea, 0xff };
    static const uint8_t y_x1_bytes[5] = { 0x00, 0x54, 0xa5, 0x2a, 0x00 };
    static const uint8_t uv_bytes[5]  = { 0x23, 0xd1, 0x18, 0xf2, 0xea };
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);
    uint8_t y[8] = { 0 };
    uint8_t uv[8] = { 0 };
    uint8_t *data[4] = { y, uv, NULL, NULL };
    const uint8_t *const_data[4] = { y, uv, NULL, NULL };
    const int linesize[4] = { sizeof(y), sizeof(uv), 0, 0 };
    uint32_t values[4] = { 0 };
    int err = 0;

    av_write_image_line2(y_values, data, linesize, desc, 0, 0, 0, 4, 4);
    err |= check_bytes(desc->name, y, y_bytes, sizeof(y_bytes));

    av_read_image_line2(values, const_data, linesize, desc,
                        0, 0, 0, 4, 0, 4);
    err |= check_values(desc->name, values, y_values, FF_ARRAY_ELEMS(y_values));

    memset(y, 0, sizeof(y));
    memset(values, 0, sizeof(values));
    av_write_image_line2(y_values + 1, data, linesize, desc, 1, 0, 0, 2, 4);
    err |= check_bytes(desc->name, y, y_x1_bytes, sizeof(y_x1_bytes));

    av_read_image_line2(values, const_data, linesize, desc,
                        1, 0, 0, 2, 0, 4);
    err |= check_values(desc->name, values, y_values + 1, 2);

    av_write_image_line2(u_values, data, linesize, desc, 0, 0, 1, 2, 4);
    av_write_image_line2(v_values, data, linesize, desc, 0, 0, 2, 2, 4);
    err |= check_bytes(desc->name, uv, uv_bytes, sizeof(uv_bytes));

    memset(values, 0, sizeof(values));
    av_read_image_line2(values, const_data, linesize, desc,
                        0, 0, 1, 2, 0, 4);
    err |= check_values(desc->name, values, u_values, FF_ARRAY_ELEMS(u_values));

    memset(values, 0, sizeof(values));
    av_read_image_line2(values, const_data, linesize, desc,
                        0, 0, 2, 2, 0, 4);
    err |= check_values(desc->name, values, v_values, FF_ARRAY_ELEMS(v_values));

    return err;
}

int main(void){
    int i;
    int err=0;
    int skip = 0;

    for (i=0; i<AV_PIX_FMT_NB*2; i++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(i);
        if(!desc || !desc->name) {
            skip ++;
            continue;
        }
        if (skip) {
            av_log(NULL, AV_LOG_INFO, "%3d unused pixel format values\n", skip);
            skip = 0;
        }
        av_log(NULL, AV_LOG_INFO, "pix fmt %s avg_bpp:%d colortype:%d\n", desc->name, av_get_padded_bits_per_pixel(desc), get_color_type(desc));
    }

    err |= check_compact_10bit(AV_PIX_FMT_NV15);
    err |= check_compact_10bit(AV_PIX_FMT_NV20_PACKED);

    return err;
}
