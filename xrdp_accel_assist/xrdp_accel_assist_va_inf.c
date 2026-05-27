/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg 2026
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
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <epoxy/gl.h>
#include <epoxy/egl.h>

#include "encoder_headers/va_inf.h"

#include "arch.h"
#include "os_calls.h"
#include "string_calls.h"
#include "xrdp_accel_assist.h"
#include "xrdp_accel_assist_x11.h"
#include "xrdp_accel_assist_va_inf.h"
#include "log.h"

extern EGLDisplay g_egl_display; /* in xrdp_accel_assist_egl.c */
extern EGLContext g_egl_context; /* in xrdp_accel_assist_egl.c */

#if 1
static char g_lib_name[] = "/opt/yami/lib/libva_inf.so";
static char g_lib_name1[] = "libva_inf.so";
static char g_func_name[] = "va_inf_get_funcs";
#else
static char g_lib_name[] = "/opt/yami/lib/libyami_inf.so";
static char g_lib_name1[] = "libyami_inf.so";
static char g_func_name[] = "yami_get_funcs";
#endif

/* if VA_DRM_DEVICE is not defined */
static char g_drm_name[] = "/dev/dri/renderD128";

static va_inf_get_funcs_proc g_va_inf_get_funcs = NULL;

static struct va_inf_funcs g_enc_funcs;

static long g_lib = 0;

static int g_fd = -1;

struct enc_info
{
    int width;
    int height;
    int frameCount;
    int pad0;
    void *enc;
};

/*****************************************************************************/
int
xrdp_accel_assist_va_inf_init(void)
{
    int error;
    int version;
    char *drm_dev;

    LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_va_inf_init:");
    g_lib = g_load_library(g_lib_name);
    if (g_lib == 0)
    {
        g_lib = g_load_library(g_lib_name1);
        if (g_lib == 0)
        {
            LOG(LOG_LEVEL_ERROR, "load library for %s/%s failed",
                g_lib_name, g_lib_name1);
            return 1;
        }
    }
    else
    {
        LOG(LOG_LEVEL_INFO, "loaded library %s", g_lib_name);
    }
    g_va_inf_get_funcs = g_get_proc_address(g_lib, g_func_name);
    if (g_va_inf_get_funcs == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "get proc address for %s failed", g_func_name);
        return 1;
    }
    g_memset(&g_enc_funcs, 0, sizeof(g_enc_funcs));
    error = g_va_inf_get_funcs(&g_enc_funcs, VI_VERSION_INT(VI_MAJOR, VI_MINOR));
    LOG(LOG_LEVEL_INFO, "g_va_inf_get_funcs rv %d", error);
    if (error != VI_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "g_va_inf_get_funcs failed");
        return 1;
    }
    error = g_enc_funcs.get_version(&version);
    if (error != VI_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "get_version failed");
        return 1;
    }
    if (version < VI_VERSION_INT(VI_MAJOR, VI_MINOR))
    {
        LOG(LOG_LEVEL_ERROR, "va_inf version too old 0x%8.8x",
            version);
        return 1;
    }
    LOG(LOG_LEVEL_INFO, "va_inf version 0x%8.8x ok", version);
    drm_dev = getenv("VA_DRM_DEVICE");
    if (drm_dev == NULL)
    {
        drm_dev = g_drm_name;
    }
    g_fd = g_file_open_ex(drm_dev, 1, 1, 0, 0);
    if (g_fd == -1)
    {
        LOG(LOG_LEVEL_ERROR, "open %s failed", g_drm_name);
        return 1;
    }
    LOG(LOG_LEVEL_INFO, "open %s ok, fd %d", g_drm_name, g_fd);
    error = g_enc_funcs.init(VI_TYPE_DRM, (void *) (size_t) g_fd);
    if (error != 0)
    {
        LOG(LOG_LEVEL_ERROR, "init failed");
        return 1;
    }
    return 0;
}

/*****************************************************************************/
int
xrdp_accel_assist_va_inf_create_encoder(int width, int height, int tex,
                                        int tex_format, struct enc_info **ei)
{
    int error;
    struct enc_info *lei;

    LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_va_inf_create_encoder:");
    lei = g_new0(struct enc_info, 1);
    if (lei == NULL)
    {
        return 1;
    }
    error = g_enc_funcs.encoder_create(&(lei->enc), width, height,
                                       VI_TYPE_H264,
                                       //0);
                                       VI_H264_ENC_FLAGS_PROFILE_MAIN);
    if (error != VI_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "encoder_create failed %d", error);
        g_free(lei);
        return 1;
    }
    LOG(LOG_LEVEL_INFO, "encoder_create ok");
    lei->width = width;
    lei->height = height;
    *ei = lei;
    return 0;
}

/*****************************************************************************/
int
xrdp_accel_assist_va_inf_delete_encoder(struct enc_info *ei)
{
    LOG(LOG_LEVEL_INFO, "xrdp_accel_assist_va_inf_delete_encoder:");
    g_enc_funcs.encoder_delete(ei->enc);
    g_free(ei);
    return 0;
}

static const EGLint g_create_image_attr[] =
{
    EGL_NONE
};

/*****************************************************************************/
enum encoder_result
xrdp_accel_assist_va_inf_encode(struct enc_info *ei, int tex,
                                void *cdata, int *cdata_bytes,
                                int flags)
{
    EGLClientBuffer cb;
    EGLImageKHR image;
    EGLint stride;
    EGLint offset;
    int fd;
    int error;
    int fourcc;
    int num_planes;
    int va_inf_flags;
    EGLuint64KHR modifiers;

    LOG_DEVEL(LOG_LEVEL_INFO, "tex %d", tex);
    LOG_DEVEL(LOG_LEVEL_INFO, "g_egl_display %p", g_egl_display);
    LOG_DEVEL(LOG_LEVEL_INFO, "g_egl_context %p", g_egl_context);
    cb = (EGLClientBuffer) (size_t) tex;
    image = eglCreateImageKHR(g_egl_display, g_egl_context,
                              EGL_GL_TEXTURE_2D_KHR,
                              cb, g_create_image_attr);
    LOG_DEVEL(LOG_LEVEL_INFO, "image %p", image);
    if (image == EGL_NO_IMAGE_KHR)
    {
        LOG(LOG_LEVEL_ERROR, "eglCreateImageKHR failed");
        return 1;
    }
    if (!eglExportDMABUFImageQueryMESA(g_egl_display, image,
                                       &fourcc, &num_planes,
                                       &modifiers))
    {
        LOG(LOG_LEVEL_ERROR, "eglExportDMABUFImageQueryMESA failed");
        eglDestroyImageKHR(g_egl_display, image);
        return 1;
    }
    LOG_DEVEL(LOG_LEVEL_INFO, "fourcc 0x%8.8X num_planes %d modifiers %d",
              fourcc, num_planes, (int) modifiers);
    if (num_planes != 1)
    {
        LOG(LOG_LEVEL_ERROR, "eglExportDMABUFImageQueryMESA return "
            "bad num_planes %d", num_planes);
        eglDestroyImageKHR(g_egl_display, image);
        return 1;
    }
    if (!eglExportDMABUFImageMESA(g_egl_display, image, &fd,
                                  &stride, &offset))
    {
        LOG(LOG_LEVEL_ERROR, "eglExportDMABUFImageMESA failed");
        eglDestroyImageKHR(g_egl_display, image);
        return 1;
    }
    /* glFinish() so fd is valid */
    glFinish();
    LOG_DEVEL(LOG_LEVEL_INFO, "fd %d stride %d offset %d", fd,
              stride, offset);
    LOG_DEVEL(LOG_LEVEL_INFO, "width %d height %d", ei->width, ei->height);
    error = g_enc_funcs.encoder_set_fd_src(ei->enc, fd,
                                           ei->width, ei->height,
                                           stride,
                                           stride * ei->height,
                                           VI_YUY2);
    LOG_DEVEL(LOG_LEVEL_INFO, "encoder_set_fd_src rv %d", error);
    if (error != VI_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "encoder_set_fd_src failed");
        g_file_close(fd);
        eglDestroyImageKHR(g_egl_display, image);
        return 1;
    }
    va_inf_flags = 0;
    if ((flags & XH_ENC_FLAGS_FORCEIDR) || (ei->frameCount < 1))
    {
        va_inf_flags = VI_H264_ENC_FLAG_KEYFRAME;
    }
    error = g_enc_funcs.encoder_encode_flags(ei->enc, cdata, cdata_bytes,
                       va_inf_flags);
    LOG_DEVEL(LOG_LEVEL_INFO, "encoder_encode rv %d cdata_bytes %d",
              error, *cdata_bytes);
    if (error != VI_SUCCESS)
    {
        LOG(LOG_LEVEL_ERROR, "encoder_encode failed rv %d", error);
        g_file_close(fd);
        eglDestroyImageKHR(g_egl_display, image);
        return 1;
    }
    ei->frameCount++;
    g_file_close(fd);
    eglDestroyImageKHR(g_egl_display, image);
    if (va_inf_flags == VI_H264_ENC_FLAG_KEYFRAME)
    {
        return KEY_FRAME_ENCODED;
    }
    return INCREMENTAL_FRAME_ENCODED;
}
