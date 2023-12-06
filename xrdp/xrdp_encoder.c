/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Laxmikant Rashinkar 2004-2014
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Encoder
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include "xrdp_encoder.h"
#include "xrdp.h"
#include "ms-rdpbcgr.h"
#include "thread_calls.h"
#include "fifo.h"
#include "xrdp_egfx.h"

#ifdef XRDP_RFXCODEC
#include "rfxcodec_encode.h"
#endif

#ifdef XRDP_X264
#include "xrdp_encoder_x264.h"
#endif

#define XRDP_SURCMD_PREFIX_BYTES 256

#ifdef XRDP_RFXCODEC
/* LH3 LL3, HH3 HL3, HL2 LH2, LH1 HH2, HH1 HL1 todo check this */
static const unsigned char g_rfx_quantization_values[] =
{
    0x66, 0x66, 0x77, 0x88, 0x98,
    0x76, 0x77, 0x88, 0x98, 0xA9
};
#endif

struct enc_rect
{
    short x1;
    short y1;
    short x2;
    short y2;
};

/*****************************************************************************/
static int
process_enc_jpg(struct xrdp_encoder *self, XRDP_ENC_DATA *enc);
#ifdef XRDP_RFXCODEC
static int
process_enc_rfx(struct xrdp_encoder *self, XRDP_ENC_DATA *enc);
#endif
static int
process_enc_h264(struct xrdp_encoder *self, XRDP_ENC_DATA *enc);
static int
process_enc_egfx(struct xrdp_encoder *self, XRDP_ENC_DATA *enc);

/*****************************************************************************/
struct xrdp_encoder *
xrdp_encoder_create(struct xrdp_mm *mm)
{
    LOG(LOG_LEVEL_INFO, "xrdp_encoder_create:");

    struct xrdp_encoder *self;
    struct xrdp_client_info *client_info;
    char buf[1024];
    int pid;

    client_info = mm->wm->client_info;

    /* RemoteFX 7.1 requires LAN but GFX does not */
    if (client_info->mcs_connection_type != CONNECTION_TYPE_LAN)
    {
        if ((mm->egfx_flags & 3) == 0)
        {
            return 0;
        }
    }
    if (client_info->bpp < 24)
    {
        return 0;
    }

    self = g_new0(struct xrdp_encoder, 1);
    if (self == NULL)
    {
        return NULL;
    }
    self->mm = mm;

    if (mm->egfx_flags & 1)
    {
        LOG(LOG_LEVEL_INFO,
            "xrdp_encoder_create: starting h264 codec session gfx");
        self->in_codec_mode = 1;
        client_info->capture_code = 5;
        client_info->capture_format =
            /* XRDP_yuv444_709fr */
            //(32 << 24) | (67 << 16) | (0 << 12) | (0 << 8) | (0 << 4) | 0;
            /* XRDP_nv12_709fr */
            (12 << 24) | (66 << 16) | (0 << 12) | (0 << 8) | (0 << 4) | 0;
        self->process_enc = process_enc_egfx;
        self->gfx = 1;
#if defined(XRDP_X264)
        self->codec_handle_gfx[1] = xrdp_encoder_x264_create();
#endif
    }
#ifdef XRDP_RFXCODEC
    else if (mm->egfx_flags & 2)
    {
        LOG(LOG_LEVEL_INFO,
            "xrdp_encoder_create: starting gfx rfx pro codec session");
        self->in_codec_mode = 1;
        client_info->capture_code = 4;
        self->process_enc = process_enc_egfx;
        self->gfx = 1;
        self->quants = (const char *) g_rfx_quantization_values;
        self->num_quants = 2;
        self->quant_idx_y = 0;
        self->quant_idx_u = 1;
        self->quant_idx_v = 1;
        self->codec_handle_gfx[0] = rfxcodec_encode_create(
                                        mm->wm->screen->width,
                                        mm->wm->screen->height,
                                        RFX_FORMAT_YUV,
                                        RFX_FLAGS_RLGR1 | RFX_FLAGS_PRO1);
    }

    else if (client_info->rfx_codec_id != 0)
    {
        LOG_DEVEL(LOG_LEVEL_INFO,
                  "xrdp_encoder_create: starting rfx codec session");
        self->codec_id = client_info->rfx_codec_id;
        self->in_codec_mode = 1;
        client_info->capture_code = 2;
        self->process_enc = process_enc_rfx;
        self->codec_handle_rfx = rfxcodec_encode_create(
                                     mm->wm->screen->width,
                                     mm->wm->screen->height,
                                     RFX_FORMAT_YUV, 0);
    }
#endif
    else if (client_info->jpeg_codec_id != 0)
    {
        LOG_DEVEL(LOG_LEVEL_INFO,
                  "xrdp_encoder_create: starting jpeg codec session");
        self->codec_id = client_info->jpeg_codec_id;
        self->in_codec_mode = 1;
        self->codec_quality = client_info->jpeg_prop[0];
        client_info->capture_code = 0;
        client_info->capture_format =
            /* XRDP_a8b8g8r8 */
            (32 << 24) | (3 << 16) | (8 << 12) | (8 << 8) | (8 << 4) | 8;
        self->process_enc = process_enc_jpg;
    }
    else if (client_info->h264_codec_id != 0)
    {
        LOG_DEVEL(LOG_LEVEL_INFO,
                  "xrdp_encoder_create: starting h264 codec session");
        self->codec_id = client_info->h264_codec_id;
        self->in_codec_mode = 1;
        client_info->capture_code = 3;
        client_info->capture_format =
            /* XRDP_nv12 */
            (12 << 24) | (64 << 16) | (0 << 12) | (0 << 8) | (0 << 4) | 0;
        /* XRDP_yuv444_709fr */
        //(32 << 24) | (67 << 16) | (0 << 12) | (0 << 8) | (0 << 4) | 0;
        self->process_enc = process_enc_h264;
#if defined(XRDP_X264)
        self->codec_handle_h264 = xrdp_encoder_x264_create();
#endif
    }
    else
    {
        g_free(self);
        return 0;
    }

    LOG_DEVEL(LOG_LEVEL_INFO,
              "init_xrdp_encoder: initializing encoder codec_id %d",
              self->codec_id);

    /* setup required FIFOs */
    self->fifo_to_proc = fifo_create();
    self->fifo_processed = fifo_create();
    self->mutex = tc_mutex_create();

    pid = g_getpid();
    /* setup wait objects for signalling */
    g_snprintf(buf, 1024, "xrdp_%8.8x_encoder_event_to_proc", pid);
    self->xrdp_encoder_event_to_proc = g_create_wait_obj(buf);
    g_snprintf(buf, 1024, "xrdp_%8.8x_encoder_event_processed", pid);
    self->xrdp_encoder_event_processed = g_create_wait_obj(buf);
    g_snprintf(buf, 1024, "xrdp_%8.8x_encoder_term", pid);
    self->xrdp_encoder_term = g_create_wait_obj(buf);
    if (client_info->gfx)
    {
        self->frames_in_flight = 2;
        self->max_compressed_bytes = 3145728;
    }
    else
    {
        self->frames_in_flight = client_info->max_unacknowledged_frame_count;
        self->max_compressed_bytes = client_info->max_fastpath_frag_bytes & ~15;
    }
    /* make sure frames_in_flight is at least 1 */
    self->frames_in_flight = MAX(self->frames_in_flight, 1);

    /* create thread to process messages */
    tc_thread_create(proc_enc_msg, self);

    return self;
}

/*****************************************************************************/
void
xrdp_encoder_delete(struct xrdp_encoder *self)
{
    XRDP_ENC_DATA *enc;
    XRDP_ENC_DATA_DONE *enc_done;
    FIFO *fifo;

    LOG_DEVEL(LOG_LEVEL_INFO, "xrdp_encoder_delete:");
    if (self == 0)
    {
        return;
    }
    if (self->in_codec_mode == 0)
    {
        return;
    }
    /* tell worker thread to shut down */
    g_set_wait_obj(self->xrdp_encoder_term);
    g_sleep(1000);

#ifdef XRDP_RFXCODEC
    if (self->codec_handle_gfx[0] != NULL)
    {
        rfxcodec_encode_destroy(self->codec_handle_gfx[0]);
    }
    if (self->codec_handle_rfx != NULL)
    {
        rfxcodec_encode_destroy(self->codec_handle_rfx);
    }
#endif

#if defined(XRDP_X264)
    if (self->codec_handle_gfx[1] != NULL)
    {
        xrdp_encoder_x264_delete(self->codec_handle_gfx[1]);
    }
    if (self->codec_handle_h264 != NULL)
    {
        xrdp_encoder_x264_delete(self->codec_handle_h264);
    }
#endif
    /* destroy wait objects used for signalling */
    g_delete_wait_obj(self->xrdp_encoder_event_to_proc);
    g_delete_wait_obj(self->xrdp_encoder_event_processed);
    g_delete_wait_obj(self->xrdp_encoder_term);

    /* cleanup fifo_to_proc */
    fifo = self->fifo_to_proc;
    if (fifo)
    {
        while (!fifo_is_empty(fifo))
        {
            enc = (XRDP_ENC_DATA *) fifo_remove_item(fifo);
            if (enc == NULL)
            {
                continue;
            }
            if (ENC_IS_BIT_SET(enc->flags, ENC_FLAGS_GFX_BIT))
            {
                g_free(enc->u.gfx.cmd);
            }
            else
            {
                g_free(enc->u.sc.drects);
                g_free(enc->u.sc.crects);
            }
            g_free(enc);
        }
        fifo_delete(fifo);
    }

    /* cleanup fifo_processed */
    fifo = self->fifo_processed;
    if (fifo)
    {
        while (!fifo_is_empty(fifo))
        {
            enc_done = (XRDP_ENC_DATA_DONE *) fifo_remove_item(fifo);
            if (enc_done == NULL)
            {
                continue;
            }
            g_free(enc_done->comp_pad_data);
            g_free(enc_done);
        }
        fifo_delete(fifo);
    }
    tc_mutex_delete(self->mutex);
    g_free(self);
}

/*****************************************************************************/
/* called from encoder thread */
static int
process_enc_jpg(struct xrdp_encoder *self, XRDP_ENC_DATA *enc)
{
    int index;
    int x;
    int y;
    int cx;
    int cy;
    int quality;
    int error;
    int out_data_bytes;
    int count;
    char *out_data;
    XRDP_ENC_DATA_DONE *enc_done;
    FIFO *fifo_processed;
    tbus mutex;
    tbus event_processed;

    LOG_DEVEL(LOG_LEVEL_DEBUG, "process_enc_jpg:");
    quality = self->codec_quality;
    fifo_processed = self->fifo_processed;
    mutex = self->mutex;
    event_processed = self->xrdp_encoder_event_processed;
    count = enc->u.sc.num_crects;
    for (index = 0; index < count; index++)
    {
        x = enc->u.sc.crects[index * 4 + 0];
        y = enc->u.sc.crects[index * 4 + 1];
        cx = enc->u.sc.crects[index * 4 + 2];
        cy = enc->u.sc.crects[index * 4 + 3];
        if (cx < 1 || cy < 1)
        {
            LOG_DEVEL(LOG_LEVEL_WARNING, "process_enc_jpg: error 1");
            continue;
        }

        LOG_DEVEL(LOG_LEVEL_DEBUG, "process_enc_jpg: x %d y %d cx %d cy %d", x, y, cx, cy);

        out_data_bytes = MAX((cx + 4) * cy * 4, 8192);
        if ((out_data_bytes < 1) || (out_data_bytes > 16 * 1024 * 1024))
        {
            LOG_DEVEL(LOG_LEVEL_ERROR, "process_enc_jpg: error 2");
            return 1;
        }
        out_data = g_new(char, out_data_bytes + 256 + 2);
        if (out_data == NULL)
        {
            LOG_DEVEL(LOG_LEVEL_ERROR, "process_enc_jpg: error 3");
            return 1;
        }

        out_data[256] = 0; /* header bytes */
        out_data[257] = 0;
        error = libxrdp_codec_jpeg_compress(self->mm->wm->session, 0, enc->u.sc.data,
                                            enc->u.sc.width, enc->u.sc.height,
                                            enc->u.sc.width * 4, x, y, cx, cy,
                                            quality,
                                            out_data + 256 + 2,
                                            &out_data_bytes);
        if (error < 0)
        {
            LOG_DEVEL(LOG_LEVEL_ERROR,
                      "process_enc_jpg: jpeg error %d bytes %d",
                      error, out_data_bytes);
            g_free(out_data);
            return 1;
        }
        LOG_DEVEL(LOG_LEVEL_WARNING,
                  "jpeg error %d bytes %d", error, out_data_bytes);
        enc_done = g_new0(XRDP_ENC_DATA_DONE, 1);
        if (enc_done == NULL)
        {
            LOG(LOG_LEVEL_INFO, "process_enc_jpg: error 3");
            return 1;
        }
        enc_done->comp_bytes = out_data_bytes + 2;
        enc_done->pad_bytes = 256;
        enc_done->comp_pad_data = out_data;
        enc_done->enc = enc;
        enc_done->last = index == (enc->u.sc.num_crects - 1);
        enc_done->x = x;
        enc_done->y = y;
        enc_done->cx = cx;
        enc_done->cy = cy;
        enc_done->frame_id = enc->u.sc.frame_id;
        /* done with msg */
        /* inform main thread done */
        tc_mutex_lock(mutex);
        fifo_add_item(fifo_processed, enc_done);
        tc_mutex_unlock(mutex);
        /* signal completion for main thread */
        g_set_wait_obj(event_processed);
    }
    return 0;
}

#ifdef XRDP_RFXCODEC
/*****************************************************************************/
/* called from encoder thread */
static int
process_enc_rfx(struct xrdp_encoder *self, XRDP_ENC_DATA *enc)
{
    int index;
    int x;
    int y;
    int cx;
    int cy;
    int out_data_bytes;
    int count;
    int tiles_written;
    int all_tiles_written;
    int tiles_left;
    int finished;
    char *out_data;
    XRDP_ENC_DATA_DONE *enc_done;
    FIFO *fifo_processed;
    tbus mutex;
    tbus event_processed;
    struct rfx_tile *tiles;
    struct rfx_rect *rfxrects;
    int alloc_bytes;

    LOG_DEVEL(LOG_LEVEL_DEBUG, "process_enc_rfx:");
    LOG_DEVEL(LOG_LEVEL_DEBUG, "process_enc_rfx: num_crects %d num_drects %d",
              enc->u.sc.num_crects, enc->u.sc.num_drects);
    fifo_processed = self->fifo_processed;
    mutex = self->mutex;
    event_processed = self->xrdp_encoder_event_processed;

    all_tiles_written = 0;
    do
    {
        tiles_written = 0;
        tiles_left = enc->u.sc.num_crects - all_tiles_written;
        out_data = NULL;
        out_data_bytes = 0;

        if ((tiles_left > 0) && (enc->u.sc.num_drects > 0))
        {
            alloc_bytes = XRDP_SURCMD_PREFIX_BYTES;
            alloc_bytes += self->max_compressed_bytes;
            alloc_bytes += sizeof(struct rfx_tile) * tiles_left +
                           sizeof(struct rfx_rect) * enc->u.sc.num_drects;
            out_data = g_new(char, alloc_bytes);
            if (out_data != NULL)
            {
                tiles = (struct rfx_tile *)
                        (out_data + XRDP_SURCMD_PREFIX_BYTES +
                         self->max_compressed_bytes);
                rfxrects = (struct rfx_rect *) (tiles + tiles_left);

                count = tiles_left;
                for (index = 0; index < count; index++)
                {
                    x = enc->u.sc.crects[(index + all_tiles_written) * 4 + 0];
                    y = enc->u.sc.crects[(index + all_tiles_written) * 4 + 1];
                    cx = enc->u.sc.crects[(index + all_tiles_written) * 4 + 2];
                    cy = enc->u.sc.crects[(index + all_tiles_written) * 4 + 3];
                    tiles[index].x = x;
                    tiles[index].y = y;
                    tiles[index].cx = cx;
                    tiles[index].cy = cy;
                    tiles[index].quant_y = self->quant_idx_y;
                    tiles[index].quant_cb = self->quant_idx_u;
                    tiles[index].quant_cr = self->quant_idx_v;
                }

                count = enc->u.sc.num_drects;
                for (index = 0; index < count; index++)
                {
                    x = enc->u.sc.drects[index * 4 + 0];
                    y = enc->u.sc.drects[index * 4 + 1];
                    cx = enc->u.sc.drects[index * 4 + 2];
                    cy = enc->u.sc.drects[index * 4 + 3];
                    rfxrects[index].x = x;
                    rfxrects[index].y = y;
                    rfxrects[index].cx = cx;
                    rfxrects[index].cy = cy;
                }

                out_data_bytes = self->max_compressed_bytes;
                tiles_written = rfxcodec_encode(self->codec_handle_rfx,
                                                out_data + XRDP_SURCMD_PREFIX_BYTES,
                                                &out_data_bytes,
                                                enc->u.sc.data,
                                                enc->u.sc.twidth, enc->u.sc.theight,
                                                enc->u.sc.twidth * 4,
                                                rfxrects, enc->u.sc.num_drects,
                                                tiles, enc->u.sc.num_crects,
                                                self->quants, self->num_quants);
            }
        }

        LOG_DEVEL(LOG_LEVEL_DEBUG,
                  "process_enc_rfx: rfxcodec_encode tiles_written %d",
                  tiles_written);
        /* only if enc_done->comp_bytes is not zero is something sent
           to the client but you must always send something back even
           on error so Xorg can get ack */
        enc_done = g_new0(XRDP_ENC_DATA_DONE, 1);
        if (enc_done == NULL)
        {
            return 1;
        }
        enc_done->comp_bytes = tiles_written > 0 ? out_data_bytes : 0;
        enc_done->pad_bytes = XRDP_SURCMD_PREFIX_BYTES;
        enc_done->comp_pad_data = out_data;
        enc_done->enc = enc;
        enc_done->cx = enc->u.sc.twidth;
        enc_done->cy = enc->u.sc.theight;

        enc_done->continuation = all_tiles_written > 0;
        if (tiles_written > 0)
        {
            all_tiles_written += tiles_written;
        }
        finished =
            (all_tiles_written == enc->u.sc.num_crects) || (tiles_written < 0);
        enc_done->last = finished;
        enc_done->frame_id = enc->u.sc.frame_id;

        /* done with msg */
        /* inform main thread done */
        tc_mutex_lock(mutex);
        fifo_add_item(fifo_processed, enc_done);
        tc_mutex_unlock(mutex);
        /* signal completion for main thread */
        g_set_wait_obj(event_processed);

    }
    while (!finished);

    return 0;
}
#endif

#define SAVE_VIDEO 0

#if SAVE_VIDEO
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static int n_save_data(const char *data, int data_size, int width, int height)
{
    int fd;
    struct _header
    {
        char tag[4];
        int width;
        int height;
        int bytes_follow;
    } header;

    fd = open("video.bin", O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    lseek(fd, 0, SEEK_END);
    header.tag[0] = 'B';
    header.tag[1] = 'E';
    header.tag[2] = 'E';
    header.tag[3] = 'F';
    header.width = width;
    header.height = height;
    header.bytes_follow = data_size;
    if (write(fd, &header, 16) != 16)
    {
        g_printf("save_data: write failed\n");
    }

    if (write(fd, data, data_size) != data_size)
    {
        g_printf("save_data: write failed\n");
    }
    close(fd);
    return 0;
}
#endif

#if defined(XRDP_X264)

/*****************************************************************************/
static int
out_RFX_AVC420_METABLOCK(struct xrdp_egfx_rect *dst_rect,
                         struct stream *s,
                         struct xrdp_egfx_rect *rects,
                         int num_rects)
{
    struct xrdp_region *reg;
    struct xrdp_rect rect;
    int index;
    int count;

    /* RFX_AVC420_METABLOCK */
    s_push_layer(s, iso_hdr, 4); /* numRegionRects, set later */
    reg = xrdp_region_create(NULL);
    if (reg == NULL)
    {
        return 1;
    }
    for (index = 0; index < num_rects; index++)
    {
        rect.left = MAX(0, rects[index].x1 - dst_rect->x1 - 1);
        rect.top = MAX(0, rects[index].y1 - dst_rect->y1 - 1);
        rect.right = MIN(dst_rect->x2 - dst_rect->x1,
                         rects[index].x2 - dst_rect->x1 + 1);
        rect.bottom = MIN(dst_rect->y2 - dst_rect->y1,
                          rects[index].y2 - dst_rect->y1 + 1);
        xrdp_region_add_rect(reg, &rect);
    }
    index = 0;
    while (xrdp_region_get_rect(reg, index, &rect) == 0)
    {
        out_uint16_le(s, rect.left);
        out_uint16_le(s, rect.top);
        out_uint16_le(s, rect.right);
        out_uint16_le(s, rect.bottom);
        index++;
    }
    xrdp_region_delete(reg);
    count = index;
    while (index > 0)
    {
        out_uint8(s, 23); /* qp */
        out_uint8(s, 100); /* quality level 0..100 */
        index--;
    }
    s_push_layer(s, mcs_hdr, 0);
    s_pop_layer(s, iso_hdr);
    out_uint32_le(s, count); /* numRegionRects */
    s_pop_layer(s, mcs_hdr);
    return 0;
}

/*****************************************************************************/
/* called from encoder thread */
static int
process_enc_h264(struct xrdp_encoder *self, XRDP_ENC_DATA *enc)
{
    int index;
    int x;
    int y;
    int cx;
    int cy;
    int out_data_bytes;
    int rcount;
    short *rrects;
    int error;
    char *out_data;
    XRDP_ENC_DATA_DONE *enc_done;
    FIFO *fifo_processed;
    tbus mutex;
    tbus event_processed;
    struct stream ls;
    struct stream *s;
    int comp_bytes_pre;
    int session_id;
    int codec_flags;

    LOG(LOG_LEVEL_DEBUG, "process_enc_x264:");
    LOG(LOG_LEVEL_DEBUG, "process_enc_x264: num_crects %d num_drects %d",
        enc->u.sc.num_crects, enc->u.sc.num_drects);

    fifo_processed = self->fifo_processed;
    mutex = self->mutex;
    event_processed = self->xrdp_encoder_event_processed;

    rcount = enc->u.sc.num_drects;
    rrects = enc->u.sc.drects;
    if (rcount > 15)
    {
        rcount = enc->u.sc.num_crects;
        rrects = enc->u.sc.crects;
    }

    out_data_bytes = 16 * 1024 * 1024;
    index = 256 + 16 + 2 + enc->u.sc.num_drects * 8;
    out_data = g_new(char, out_data_bytes + index);
    if (out_data == NULL)
    {
        return 1;
    }

    s = &ls;
    g_memset(s, 0, sizeof(struct stream));
    ls.data = out_data + 256;
    ls.p = ls.data;

    session_id = (enc->u.sc.flags >> 28) & 0xF;

    codec_flags = 0;
    s_push_layer(s, mcs_hdr, 0);
    out_uint32_le(s, 0); /* flags, updated later */
    out_uint32_le(s, session_id);
    out_uint16_le(s, enc->u.sc.width); /* src_width */
    out_uint16_le(s, enc->u.sc.height); /* src_height */
    out_uint16_le(s, enc->u.sc.width); /* dst_width */
    out_uint16_le(s, enc->u.sc.height); /* dst_height */
    out_uint16_le(s, rcount);
    for (index = 0; index < rcount; index++)
    {
        x = rrects[index * 4 + 0];
        y = rrects[index * 4 + 1];
        cx = rrects[index * 4 + 2];
        cy = rrects[index * 4 + 3];
        x -= enc->u.sc.left;
        y -= enc->u.sc.top;
        out_uint16_le(s, x);
        out_uint16_le(s, y);
        out_uint16_le(s, cx);
        out_uint16_le(s, cy);
    }
    s_push_layer(s, iso_hdr, 4);
    comp_bytes_pre = 4 + 4 + 2 + 2 + 2 + 2 + 2 + rcount * 8 + 4;

    error = 0;
    if (enc->u.sc.flags & 1)
    {
        /* already compressed */
        uint8_t *ud = (uint8_t *) (enc->u.sc.data);
        int cbytes = ud[0] | (ud[1] << 8) | (ud[2] << 16) | (ud[3] << 24);
        if ((cbytes < 1) || (cbytes > out_data_bytes))
        {
            LOG(LOG_LEVEL_INFO, "process_enc_h264: bad h264 bytes %d", cbytes);
            g_free(out_data);
            return 1;
        }
        LOG(LOG_LEVEL_DEBUG,
            "process_enc_h264: already compressed and size is %d", cbytes);
        out_data_bytes = cbytes;
        g_memcpy(s->p, enc->u.sc.data + 4, out_data_bytes);
    }
    else
    {
#if defined(XRDP_X264)
        error = xrdp_encoder_x264_encode(self->codec_handle_h264, session_id,
                                         enc->u.sc.left, enc->u.sc.top,
                                         enc->u.sc.width, enc->u.sc.height,
                                         enc->u.sc.twidth, enc->u.sc.theight,
                                         0, enc->u.sc.data, rrects, rcount,
                                         s->p, &out_data_bytes, &codec_flags);
#endif
    }
    s_push_layer(s, sec_hdr, 0);
    s_pop_layer(s, mcs_hdr);
    out_uint32_le(s, codec_flags);
    s_pop_layer(s, sec_hdr);
    LOG_DEVEL(LOG_LEVEL_TRACE,
              "process_enc_h264: xrdp_encoder_x264_encode rv %d "
              "out_data_bytes %d width %d height %d",
              error, out_data_bytes, enc->width, enc->height);
    if (error != 0)
    {
        LOG_DEVEL(LOG_LEVEL_TRACE,
                  "process_enc_h264: xrdp_encoder_x264_encode failed rv %d",
                  error);
        g_free(out_data);
        return 1;
    }
    s->end = s->p + out_data_bytes;

    s_pop_layer(s, iso_hdr);
    out_uint32_le(s, out_data_bytes);

#if SAVE_VIDEO
    n_save_data(s->p, out_data_bytes, enc->width, enc->height);
#endif

    enc_done = g_new0(XRDP_ENC_DATA_DONE, 1);
    if (enc_done == NULL)
    {
        return 1;
    }
    enc_done->comp_bytes = comp_bytes_pre + out_data_bytes;
    enc_done->pad_bytes = 256;
    enc_done->comp_pad_data = out_data;
    enc_done->enc = enc;
    enc_done->last = 1;
    enc_done->x = enc->u.sc.left;
    enc_done->y = enc->u.sc.top;
    enc_done->cx = enc->u.sc.width;
    enc_done->cy = enc->u.sc.height;
    enc_done->frame_id = enc->u.sc.frame_id;

    /* done with msg */
    /* inform main thread done */
    tc_mutex_lock(mutex);
    fifo_add_item(fifo_processed, enc_done);
    tc_mutex_unlock(mutex);
    /* signal completion for main thread */
    g_set_wait_obj(event_processed);

    return 0;
}

#else

/*****************************************************************************/
/* called from encoder thread */
static int
process_enc_h264(struct xrdp_encoder *self, XRDP_ENC_DATA *enc)
{
    LOG_DEVEL(LOG_LEVEL_INFO, "process_enc_h264: dummy func");
    return 0;
}

#endif

/*****************************************************************************/
static struct stream *
gfx_wiretosurface1(struct xrdp_encoder *self,
                   struct xrdp_egfx_bulk *bulk, struct stream *in_s,
                   struct xrdp_enc_gfx_cmd *enc_gfx_cmd)
{
#ifdef XRDP_X264
    int index;
    int surface_id;
    int codec_id;
    int pixel_format;
    int num_rects_d;
    int num_rects_c;
    struct stream *rv;
    short left;
    short top;
    short width;
    short height;
    short twidth;
    short theight;
    int bitmap_data_length;
    int flags;
    struct xrdp_egfx_rect *d_rects;
    struct xrdp_egfx_rect *c_rects;
    struct xrdp_egfx_rect dst_rect;
    int error;
    struct stream ls;
    struct stream *s;
    short *crects;

    if (self->codec_handle_gfx[1] == NULL)
    {
        return NULL;
    }
    s = &ls;
    g_memset(s, 0, sizeof(struct stream));
    s->size = self->max_compressed_bytes;
    s->data = g_new(char, s->size);
    if (s->data == NULL)
    {
        return NULL;
    }
    s->p = s->data;
    if (!s_check_rem(in_s, 11))
    {
        g_free(s->data);
        return NULL;
    }
    in_uint16_le(in_s, surface_id);
    in_uint16_le(in_s, codec_id);
    in_uint8(in_s, pixel_format);
    in_uint32_le(in_s, flags);
    in_uint16_le(in_s, num_rects_d);
    if ((num_rects_d < 1) || (num_rects_d > 16 * 1024) ||
            (!s_check_rem(in_s, num_rects_d * 8)))
    {
        g_free(s->data);
        return NULL;
    }
    d_rects = g_new0(struct xrdp_egfx_rect, num_rects_d);
    if (d_rects == NULL)
    {
        g_free(s->data);
        return NULL;
    }
    for (index = 0; index < num_rects_d; index++)
    {
        in_uint16_le(in_s, left);
        in_uint16_le(in_s, top);
        in_uint16_le(in_s, width);
        in_uint16_le(in_s, height);
        d_rects[index].x1 = left;
        d_rects[index].y1 = top;
        d_rects[index].x2 = left + width;
        d_rects[index].y2 = top + height;

    }
    if (!s_check_rem(in_s, 2))
    {
        g_free(s->data);
        g_free(d_rects);
        return NULL;
    }
    in_uint16_le(in_s, num_rects_c);
    if ((num_rects_c < 1) || (num_rects_c > 16 * 1024) ||
            (!s_check_rem(in_s, num_rects_c * 8)))
    {
        g_free(s->data);
        g_free(d_rects);
        return NULL;
    }
    c_rects = g_new0(struct xrdp_egfx_rect, num_rects_c);
    if (c_rects == NULL)
    {
        g_free(s->data);
        g_free(d_rects);
        return NULL;
    }
    crects = g_new(short, num_rects_c * 4);
    if (crects == NULL)
    {
        g_free(s->data);
        g_free(c_rects);
        g_free(d_rects);
        return NULL;
    }
    g_memcpy(crects, in_s->p, num_rects_c * 2 * 4);
    for (index = 0; index < num_rects_c; index++)
    {
        in_uint16_le(in_s, left);
        in_uint16_le(in_s, top);
        in_uint16_le(in_s, width);
        in_uint16_le(in_s, height);
        c_rects[index].x1 = left;
        c_rects[index].y1 = top;
        c_rects[index].x2 = left + width;
        c_rects[index].y2 = top + height;
    }
    if (!s_check_rem(in_s, 12))
    {
        g_free(s->data);
        g_free(c_rects);
        g_free(d_rects);
        g_free(crects);
        return NULL;
    }
    in_uint16_le(in_s, left);
    in_uint16_le(in_s, top);
    in_uint16_le(in_s, width);
    in_uint16_le(in_s, height);
    in_uint16_le(in_s, twidth);
    in_uint16_le(in_s, theight);
    dst_rect.x1 = left;
    dst_rect.y1 = top;
    dst_rect.x2 = left + width;
    dst_rect.y2 = top + height;

    /* RFX_AVC420_METABLOCK */
    if (out_RFX_AVC420_METABLOCK(&dst_rect, s, d_rects, num_rects_d) != 0)
    {
        g_free(s->data);
        g_free(c_rects);
        g_free(d_rects);
        g_free(crects);
        return NULL;
    }

    g_free(c_rects);
    g_free(d_rects);

    if (ENC_IS_BIT_SET(flags, 0))
    {
        /* already compressed */
        out_uint8a(s, enc_gfx_cmd->data, enc_gfx_cmd->data_bytes);
    }
    else
    {
        /* assume NV12 format */
        if (twidth * theight * 3 / 2 > enc_gfx_cmd->data_bytes)
        {
            g_free(s->data);
            g_free(crects);
            return NULL;
        }
        bitmap_data_length = s_rem_out(s);
        error = xrdp_encoder_x264_encode(self->codec_handle_gfx[1], 0,
                                         left, top,
                                         width, height, twidth, theight,  0,
                                         enc_gfx_cmd->data,
                                         crects, num_rects_c,
                                         s->p, &bitmap_data_length, NULL);
        if (error == 0)
        {
            xstream_seek(s, bitmap_data_length);
        }
        else
        {
            g_free(s->data);
            g_free(crects);
            return NULL;
        }
    }
    s_mark_end(s);
    bitmap_data_length = (int) (s->end - s->data);
    rv = xrdp_egfx_wire_to_surface1(bulk, surface_id,
                                    codec_id,
                                    pixel_format, &dst_rect,
                                    s->data, bitmap_data_length);
    g_free(s->data);
    g_free(crects);
    return rv;
#else
    (void)self;
    (void)bulk;
    (void)in_s;
    (void)enc_gfx_cmd;
    return NULL;
#endif
}

/*****************************************************************************/
static struct stream *
gfx_wiretosurface2(struct xrdp_encoder *self,
                   struct xrdp_egfx_bulk *bulk, struct stream *in_s,
                   struct xrdp_enc_gfx_cmd *enc_gfx_cmd)
{
#ifdef XRDP_RFXCODEC
    int index;
    int surface_id;
    int codec_id;
    int codec_context_id;
    int pixel_format;
    int num_rects_d;
    int num_rects_c;
    struct stream *rv;
    short left;
    short top;
    short width;
    short height;
    short twidth;
    short theight;
    char *bitmap_data;
    int bitmap_data_length;
    struct rfx_tile *tiles;
    struct rfx_rect *rfxrects;
    int flags;
    int tiles_written;
    int do_free;
    int do_send;

    if (self->codec_handle_gfx[0] == NULL)
    {
        return NULL;
    }
    if (!s_check_rem(in_s, 15))
    {
        return NULL;
    }
    in_uint16_le(in_s, surface_id);
    in_uint16_le(in_s, codec_id);
    in_uint32_le(in_s, codec_context_id);
    in_uint8(in_s, pixel_format);
    in_uint32_le(in_s, flags);
    in_uint16_le(in_s, num_rects_d);
    if ((num_rects_d < 1) || (num_rects_d > 16 * 1024) ||
            (!s_check_rem(in_s, num_rects_d * 8)))
    {
        return NULL;
    }
    rfxrects = g_new0(struct rfx_rect, num_rects_d);
    if (rfxrects == NULL)
    {
        return NULL;
    }
    for (index = 0; index < num_rects_d; index++)
    {
        in_uint16_le(in_s, left);
        in_uint16_le(in_s, top);
        in_uint16_le(in_s, width);
        in_uint16_le(in_s, height);
        rfxrects[index].x = left;
        rfxrects[index].y = top;
        rfxrects[index].cx = width;
        rfxrects[index].cy = height;
    }
    if (!s_check_rem(in_s, 2))
    {
        g_free(rfxrects);
        return NULL;
    }
    in_uint16_le(in_s, num_rects_c);
    if ((num_rects_c < 1) || (num_rects_c > 16 * 1024) ||
            (!s_check_rem(in_s, num_rects_c * 8)))
    {
        g_free(rfxrects);
        return NULL;
    }
    tiles = g_new0(struct rfx_tile, num_rects_c);
    if (tiles == NULL)
    {
        g_free(rfxrects);
        return NULL;
    }
    for (index = 0; index < num_rects_c; index++)
    {
        in_uint16_le(in_s, left);
        in_uint16_le(in_s, top);
        in_uint16_le(in_s, width);
        in_uint16_le(in_s, height);
        tiles[index].x = left;
        tiles[index].y = top;
        tiles[index].cx = width;
        tiles[index].cy = height;
        tiles[index].quant_y = self->quant_idx_y;
        tiles[index].quant_cb = self->quant_idx_u;
        tiles[index].quant_cr = self->quant_idx_v;
    }
    if (!s_check_rem(in_s, 12))
    {
        g_free(tiles);
        g_free(rfxrects);
        return NULL;
    }
    in_uint16_le(in_s, left);
    in_uint16_le(in_s, top);
    in_uint16_le(in_s, width);
    in_uint16_le(in_s, height);
    in_uint16_le(in_s, twidth);
    in_uint16_le(in_s, theight);
    do_free = 0;
    do_send = 0;
    if (ENC_IS_BIT_SET(flags, 0))
    {
        /* already compressed */
        bitmap_data_length = enc_gfx_cmd->data_bytes;
        bitmap_data = enc_gfx_cmd->data;
        do_send = 1;
    }
    else
    {
        bitmap_data_length = self->max_compressed_bytes;
        bitmap_data = g_new(char, bitmap_data_length);
        if (bitmap_data == NULL)
        {
            g_free(tiles);
            g_free(rfxrects);
            return NULL;
        }
        do_free = 1;
        tiles_written = rfxcodec_encode(self->codec_handle_gfx[0],
                                        bitmap_data,
                                        &bitmap_data_length,
                                        enc_gfx_cmd->data,
                                        twidth, theight,
                                        twidth * 4,
                                        rfxrects, num_rects_d,
                                        tiles, num_rects_c,
                                        self->quants, self->num_quants);
        if (tiles_written > 0)
        {
            do_send = 1;
        }
    }
    g_free(tiles);
    g_free(rfxrects);
    rv = NULL;
    if (do_send)
    {
        rv = xrdp_egfx_wire_to_surface2(bulk, surface_id,
                                        codec_id, codec_context_id,
                                        pixel_format,
                                        bitmap_data, bitmap_data_length);
    }
    if (do_free)
    {
        g_free(bitmap_data);
    }
    return rv;
#else
    (void)self;
    (void)bulk;
    (void)in_s;
    (void)enc_gfx_cmd;
    return NULL;
#endif
}

/*****************************************************************************/
static struct stream *
gfx_solidfill(struct xrdp_encoder *self,
              struct xrdp_egfx_bulk *bulk, struct stream *in_s)
{
    int surface_id;
    int pixel;
    int num_rects;
    char *ptr8;
    struct xrdp_egfx_rect *rects;

    if (!s_check_rem(in_s, 8))
    {
        return NULL;
    }
    in_uint16_le(in_s, surface_id);
    in_uint32_le(in_s, pixel);
    in_uint16_le(in_s, num_rects);
    if (!s_check_rem(in_s, num_rects * 8))
    {
        return NULL;
    }
    in_uint8p(in_s, ptr8, num_rects * 8);
    rects = (struct xrdp_egfx_rect *) ptr8;
    return xrdp_egfx_fill_surface(bulk, surface_id, pixel, num_rects, rects);
}

/*****************************************************************************/
static struct stream *
gfx_surfacetosurface(struct xrdp_encoder *self,
                     struct xrdp_egfx_bulk *bulk, struct stream *in_s)
{
    int surface_id_src;
    int surface_id_dst;
    char *ptr8;
    int num_pts;
    struct xrdp_egfx_rect *rects;
    struct xrdp_egfx_point *pts;

    if (!s_check_rem(in_s, 14))
    {
        return NULL;
    }
    in_uint16_le(in_s, surface_id_src);
    in_uint16_le(in_s, surface_id_dst);
    in_uint8p(in_s, ptr8, 8);
    rects = (struct xrdp_egfx_rect *) ptr8;
    in_uint16_le(in_s, num_pts);
    if (!s_check_rem(in_s, num_pts * 4))
    {
        return NULL;
    }
    in_uint8p(in_s, ptr8, num_pts * 4);
    pts = (struct xrdp_egfx_point *) ptr8;
    return xrdp_egfx_surface_to_surface(bulk, surface_id_src, surface_id_dst,
                                        rects, num_pts, pts);
}

/*****************************************************************************/
static struct stream *
gfx_createsurface(struct xrdp_encoder *self,
                  struct xrdp_egfx_bulk *bulk, struct stream *in_s)
{
    int surface_id;
    int width;
    int height;
    int pixel_format;

    if (!s_check_rem(in_s, 7))
    {
        return NULL;
    }
    in_uint16_le(in_s, surface_id);
    in_uint16_le(in_s, width);
    in_uint16_le(in_s, height);
    in_uint8(in_s, pixel_format);
    return xrdp_egfx_create_surface(bulk, surface_id,
                                    width, height, pixel_format);
}

/*****************************************************************************/
static struct stream *
gfx_deletesurface(struct xrdp_encoder *self,
                  struct xrdp_egfx_bulk *bulk, struct stream *in_s)
{
    int surface_id;

    if (!s_check_rem(in_s, 2))
    {
        return NULL;
    }
    in_uint16_le(in_s, surface_id);
    return xrdp_egfx_delete_surface(bulk, surface_id);
}

/*****************************************************************************/
static struct stream *
gfx_startframe(struct xrdp_encoder *self,
               struct xrdp_egfx_bulk *bulk, struct stream *in_s)
{
    int frame_id;
    int time_stamp;

    if (!s_check_rem(in_s, 8))
    {
        return NULL;
    }
    in_uint32_le(in_s, frame_id);
    in_uint32_le(in_s, time_stamp);
    return xrdp_egfx_frame_start(bulk, frame_id, time_stamp);
}

/*****************************************************************************/
static struct stream *
gfx_endframe(struct xrdp_encoder *self,
             struct xrdp_egfx_bulk *bulk, struct stream *in_s, int *aframe_id)
{
    int frame_id;

    if (!s_check_rem(in_s, 4))
    {
        return NULL;
    }
    in_uint32_le(in_s, frame_id);
    *aframe_id = frame_id;
    return xrdp_egfx_frame_end(bulk, frame_id);
}

/*****************************************************************************/
static struct stream *
gfx_resetgraphics(struct xrdp_encoder *self,
                  struct xrdp_egfx_bulk *bulk, struct stream *in_s)
{
    int width;
    int height;
    int monitor_count;
    int index;
    struct monitor_info *mi;
    struct stream *rv;

    if (!s_check_rem(in_s, 12))
    {
        return NULL;
    }
    in_uint32_le(in_s, width);
    in_uint32_le(in_s, height);
    in_uint32_le(in_s, monitor_count);
    if ((monitor_count < 1) || (monitor_count > 16) ||
            !s_check_rem(in_s, monitor_count * 20))
    {
        return NULL;
    }
    mi = g_new0(struct monitor_info, monitor_count);
    if (mi == NULL)
    {
        return NULL;
    }
    for (index = 0; index < monitor_count; index++)
    {
        in_uint32_le(in_s, mi[index].left);
        in_uint32_le(in_s, mi[index].top);
        in_uint32_le(in_s, mi[index].right);
        in_uint32_le(in_s, mi[index].bottom);
        in_uint32_le(in_s, mi[index].is_primary);
    }
    rv = xrdp_egfx_reset_graphics(bulk, width, height, monitor_count, mi);
    g_free(mi);
    return rv;
}

/*****************************************************************************/
static struct stream *
gfx_mapsurfacetooutput(struct xrdp_encoder *self,
                       struct xrdp_egfx_bulk *bulk, struct stream *in_s)
{
    int surface_id;
    int x;
    int y;

    if (!s_check_rem(in_s, 10))
    {
        return NULL;
    }
    in_uint16_le(in_s, surface_id);
    in_uint32_le(in_s, x);
    in_uint32_le(in_s, y);
    return xrdp_egfx_map_surface(bulk, surface_id, x, y);
}

/*****************************************************************************/
/* called from encoder thread */
static int
process_enc_egfx(struct xrdp_encoder *self, XRDP_ENC_DATA *enc)
{
    struct stream *s;
    struct stream in_s;
    struct xrdp_egfx_bulk *bulk;
    XRDP_ENC_DATA_DONE *enc_done;
    FIFO *fifo_processed;
    tbus mutex;
    tbus event_processed;
    int cmd_id;
    int cmd_bytes;
    int frame_id;
    int got_frame_id;
    char *holdp;
    char *holdend;

    fifo_processed = self->fifo_processed;
    mutex = self->mutex;
    event_processed = self->xrdp_encoder_event_processed;
    bulk = self->mm->egfx->bulk;
    g_memset(&in_s, 0, sizeof(in_s));
    in_s.data = enc->u.gfx.cmd;
    in_s.size = enc->u.gfx.cmd_bytes;
    in_s.p = in_s.data;
    in_s.end = in_s.data + in_s.size;
    while (s_check_rem(&in_s, 8))
    {
        s = NULL;
        frame_id = 0;
        got_frame_id = 0;
        holdp = in_s.p;
        in_uint16_le(&in_s, cmd_id);
        in_uint8s(&in_s, 2); /* flags */
        in_uint32_le(&in_s, cmd_bytes);
        if ((cmd_bytes < 8) || (cmd_bytes > 32 * 1024))
        {
            return 1;
        }
        holdend = in_s.end;
        in_s.end = holdp + cmd_bytes;
        switch (cmd_id)
        {
            case XR_RDPGFX_CMDID_WIRETOSURFACE_1:       /* 0x0001 */
                s = gfx_wiretosurface1(self, bulk, &in_s, &(enc->u.gfx));
                break;
            case XR_RDPGFX_CMDID_WIRETOSURFACE_2:       /* 0x0002 */
                s = gfx_wiretosurface2(self, bulk, &in_s, &(enc->u.gfx));
                break;
            case XR_RDPGFX_CMDID_SOLIDFILL:             /* 0x0004 */
                s = gfx_solidfill(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_SURFACETOSURFACE:      /* 0x0005 */
                s = gfx_surfacetosurface(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_CREATESURFACE:         /* 0x0009 */
                s = gfx_createsurface(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_DELETESURFACE:         /* 0x000A */
                s = gfx_deletesurface(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_STARTFRAME:            /* 0x000B */
                s = gfx_startframe(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_ENDFRAME:              /* 0x000C */
                s = gfx_endframe(self, bulk, &in_s, &frame_id);
                got_frame_id = 1;
                break;
            case XR_RDPGFX_CMDID_RESETGRAPHICS:         /* 0x000E */
                s = gfx_resetgraphics(self, bulk, &in_s);
                break;
            case XR_RDPGFX_CMDID_MAPSURFACETOOUTPUT:    /* 0x000F */
                s = gfx_mapsurfacetooutput(self, bulk, &in_s);
                break;
            default:
                break;
        }
        if (s == NULL)
        {
            LOG(LOG_LEVEL_ERROR, "process_enc_egfx: cmd_id %d s = nil", cmd_id);
            return 1;
        }
        /* setup for next cmd */
        in_s.p = holdp + cmd_bytes;
        in_s.end = holdend;
        /* setup enc_done struct */
        enc_done = g_new0(XRDP_ENC_DATA_DONE, 1);
        if (enc_done == NULL)
        {
            free_stream(s);
            return 1;
        }
        ENC_SET_BIT(enc_done->flags, ENC_DONE_FLAGS_GFX_BIT);
        enc_done->enc = enc;
        enc_done->last = !s_check_rem(&in_s, 8);
        enc_done->comp_bytes = (int) (s->end - s->data);
        enc_done->comp_pad_data = s->data;
        if (got_frame_id)
        {
            ENC_SET_BIT(enc_done->flags, ENC_DONE_FLAGS_FRAME_ID_BIT);
            enc_done->frame_id = frame_id;
        }
        g_free(s); /* don't call free_stream() here so s->data is valid */
        /* inform main thread done */
        tc_mutex_lock(mutex);
        fifo_add_item(fifo_processed, enc_done);
        tc_mutex_unlock(mutex);
        /* signal completion for main thread */
        g_set_wait_obj(event_processed);
    }
    return 0;
}

/**
 * Encoder thread main loop
 *****************************************************************************/
THREAD_RV THREAD_CC
proc_enc_msg(void *arg)
{
    XRDP_ENC_DATA *enc;
    FIFO *fifo_to_proc;
    tbus mutex;
    tbus event_to_proc;
    tbus term_obj;
    tbus lterm_obj;
    int robjs_count;
    int wobjs_count;
    int cont;
    int timeout;
    tbus robjs[32];
    tbus wobjs[32];
    struct xrdp_encoder *self;

    LOG_DEVEL(LOG_LEVEL_INFO, "proc_enc_msg: thread is running");

    self = (struct xrdp_encoder *) arg;
    if (self == NULL)
    {
        LOG_DEVEL(LOG_LEVEL_DEBUG, "proc_enc_msg: self nil");
        return 0;
    }

    fifo_to_proc = self->fifo_to_proc;
    mutex = self->mutex;
    event_to_proc = self->xrdp_encoder_event_to_proc;

    term_obj = g_get_term_event();
    lterm_obj = self->xrdp_encoder_term;

    cont = 1;
    while (cont)
    {
        timeout = -1;
        robjs_count = 0;
        wobjs_count = 0;
        robjs[robjs_count++] = term_obj;
        robjs[robjs_count++] = lterm_obj;
        robjs[robjs_count++] = event_to_proc;

        if (g_obj_wait(robjs, robjs_count, wobjs, wobjs_count, timeout) != 0)
        {
            /* error, should not get here */
            g_sleep(100);
        }

        if (g_is_wait_obj_set(term_obj)) /* global term */
        {
            LOG(LOG_LEVEL_DEBUG,
                "Received termination signal, stopping the encoder thread");
            break;
        }

        if (g_is_wait_obj_set(lterm_obj)) /* xrdp_mm term */
        {
            LOG(LOG_LEVEL_INFO, "proc_enc_msg: xrdp_mm term");
            break;
        }

        if (g_is_wait_obj_set(event_to_proc))
        {
            /* clear it right away */
            g_reset_wait_obj(event_to_proc);
            /* get first msg */
            tc_mutex_lock(mutex);
            enc = (XRDP_ENC_DATA *) fifo_remove_item(fifo_to_proc);
            tc_mutex_unlock(mutex);
            while (enc != NULL)
            {
                /* do work */
                if (self->process_enc(self, enc) != 0)
                {
                    LOG(LOG_LEVEL_ERROR, "proc_enc_msg: process_enc failed");
                    if (ENC_IS_BIT_SET(enc->flags, ENC_FLAGS_GFX_BIT))
                    {
                        g_free(enc->u.gfx.cmd);
                    }
                    else
                    {
                        g_free(enc->u.sc.drects);
                        g_free(enc->u.sc.crects);
                    }
                    if (enc->shmem_ptr != NULL)
                    {
                        g_munmap(enc->shmem_ptr, enc->shmem_bytes);
                    }
                }
                /* get next msg */
                tc_mutex_lock(mutex);
                enc = (XRDP_ENC_DATA *) fifo_remove_item(fifo_to_proc);
                tc_mutex_unlock(mutex);
            }
        }

    } /* end while (cont) */
    LOG_DEVEL(LOG_LEVEL_DEBUG, "proc_enc_msg: thread exit");
    return 0;
}
