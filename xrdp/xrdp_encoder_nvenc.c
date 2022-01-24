/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg 2021
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
 * NVIDIA Codec SDK Encoder
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <cuda.h>
#include <nvEncodeAPI.h>

#include "arch.h"
#include "os_calls.h"
#include "xrdp_encoder_nvenc.h"

#if NVENCAPI_MAJOR_VERSION < 11
#warning NVENCAPI_MAJOR_VERSION too old
#endif

#define LLOG_LEVEL 1
#define LLOGLN(_level, _args) \
  do \
  { \
    if (_level < LLOG_LEVEL) \
    { \
        g_write("xrdp:xrdp_encoder_nvenc [%10.10u]: ", g_time3()); \
        g_writeln _args ; \
    } \
  } \
  while (0)

struct nvenc_encoder
{
    int width;
    int height;
    int frameCount;
    int pad0;
    NV_ENC_INPUT_PTR inputBuffers[4];
    NV_ENC_OUTPUT_PTR bitstreamBuffers[4];
    void *enc;
};

struct nvenc_global
{
    struct nvenc_encoder encoders[16];
    int cu_dev_count;
    int pad0;
    CUcontext cu_con[8];
    NV_ENCODE_API_FUNCTION_LIST enc_funcs;
};

/*****************************************************************************/
void *
xrdp_encoder_nvenc_create(void)
{
    int index;
    struct nvenc_global *ng;

    LLOGLN(0, ("xrdp_encoder_nvenc_create:"));
    cuInit(0);
    ng = (struct nvenc_global *) g_malloc(sizeof(struct nvenc_global), 1);
    if (ng == NULL)
    {
        return NULL;
    }
    cuDeviceGetCount(&(ng->cu_dev_count));
    for (index = 0; index < ng->cu_dev_count; index++)
    {
        if (index > 7)
        {
            break;
        }
        cuCtxCreate(ng->cu_con + index, 0, index);
    }
    ng->enc_funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NvEncodeAPICreateInstance(&(ng->enc_funcs));
    return ng;
}

/*****************************************************************************/
static int
xrdp_encoder_cleanup_nvenc_encoder(struct nvenc_global *ng, struct nvenc_encoder *ne)
{
    int index;

    if (ne->enc == NULL)
    {
        return 0;
    }
    for (index = 0; index < 4; index++)
    {
        ng->enc_funcs.nvEncDestroyInputBuffer(ne->enc, ne->inputBuffers[index]);
    }
    for (index = 0; index < 4; index++)
    {
        ng->enc_funcs.nvEncDestroyBitstreamBuffer(ne->enc, ne->bitstreamBuffers[index]);
    }
    ng->enc_funcs.nvEncDestroyEncoder(ne->enc);
    ne->enc = NULL;
    ne->frameCount = 0;
    return 0;
}

/*****************************************************************************/
int
xrdp_encoder_nvenc_delete(void *handle)
{
    struct nvenc_global *ng;
    struct nvenc_encoder *ne;
    int index;

    LLOGLN(0, ("xrdp_encoder_nvenc_delete:"));
    if (handle == NULL)
    {
        return 0;
    }
    ng = (struct nvenc_global *) handle;
    for (index = 0; index < 16; index++)
    {
        ne = ng->encoders + index;
        xrdp_encoder_cleanup_nvenc_encoder(ng, ne);
    }
    for (index = 0; index < ng->cu_dev_count; index++)
    {
        cuCtxDestroy(ng->cu_con[index]);
    }
    g_free(ng);
    return 0;
}

/*****************************************************************************/
int
xrdp_encoder_nvenc_encode(void *handle, int session,
                          int width, int height, int format, const char *data,
                          char *cdata, int *cdata_bytes)
{
    struct nvenc_global *ng;
    struct nvenc_encoder *ne;
    const char *src8;
    char *dst8;
    int index;

    NVENCSTATUS nv_error;
    NV_ENC_CONFIG presetCfg;
    NV_ENC_INITIALIZE_PARAMS createEncodeParams;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params;
    NV_ENC_CREATE_INPUT_BUFFER inputParams;
    NV_ENC_CREATE_BITSTREAM_BUFFER bitstreamParams;
    NV_ENC_LOCK_INPUT_BUFFER lockInput;
    NV_ENC_LOCK_BITSTREAM lockBitstream;
    NV_ENC_PIC_PARAMS picParams;
    NV_ENC_INPUT_PTR inputBuffer;
    NV_ENC_OUTPUT_PTR bitstreamBuffer;

    LLOGLN(10, ("xrdp_encoder_nvenc_encode:"));
    ng = (struct nvenc_global *) handle;
    ne = ng->encoders + (session & 0xF);
    if ((ne->enc == NULL) || (ne->width != width) || (ne->height != height))
    {
        xrdp_encoder_cleanup_nvenc_encoder(ng, ne);
        if ((width > 0) && (height > 0))
        {
            g_memset(&params, 0, sizeof(params));
            params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
            params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
            params.device = ng->cu_con[0];
            params.apiVersion = NVENCAPI_VERSION;
            nv_error = ng->enc_funcs.nvEncOpenEncodeSessionEx(&params, &(ne->enc));
            if (nv_error != NV_ENC_SUCCESS)
            {
                return 1;
            }
            g_memset(&presetCfg, 0, sizeof(presetCfg));
            presetCfg.version = NV_ENC_CONFIG_VER;
            presetCfg.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
            presetCfg.gopLength = NVENC_INFINITE_GOPLENGTH;
            presetCfg.frameIntervalP = 1;  /* 1 + B_Frame_Count */
            presetCfg.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
            presetCfg.mvPrecision = NV_ENC_MV_PRECISION_QUARTER_PEL;
            presetCfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
            presetCfg.rcParams.averageBitRate = 5000000;
            presetCfg.rcParams.constQP.qpInterP = 28;
            presetCfg.rcParams.constQP.qpInterB = 28;
            presetCfg.rcParams.constQP.qpIntra = 28;
            presetCfg.encodeCodecConfig.h264Config.chromaFormatIDC = 1;
            presetCfg.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
            g_memset(&createEncodeParams, 0, sizeof(createEncodeParams));
            createEncodeParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
            createEncodeParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
            createEncodeParams.encodeWidth = width;
            createEncodeParams.encodeHeight = height;
            createEncodeParams.darWidth = width;
            createEncodeParams.darHeight = height;
            createEncodeParams.frameRateNum = 30;
            createEncodeParams.frameRateDen = 1;
            createEncodeParams.enablePTD = 1;
            createEncodeParams.encodeConfig = &presetCfg;
            nv_error = ng->enc_funcs.nvEncInitializeEncoder(ne->enc, &createEncodeParams);
            if (nv_error != NV_ENC_SUCCESS)
            {
                ng->enc_funcs.nvEncDestroyEncoder(ne->enc);
                ne->enc = 0;
                return 1;
            }
            for (index = 0; index < 4; index++)
            {
                g_memset(&inputParams, 0, sizeof(inputParams));
                inputParams.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
                inputParams.width = width;
                inputParams.height = height;
                inputParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12_PL;
                ng->enc_funcs.nvEncCreateInputBuffer(ne->enc, &inputParams);
                ne->inputBuffers[index] = inputParams.inputBuffer;
            }
            for (index = 0; index < 4; index++)
            {
                g_memset(&bitstreamParams, 0, sizeof(bitstreamParams));
                bitstreamParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
                ng->enc_funcs.nvEncCreateBitstreamBuffer(ne->enc, &bitstreamParams);
                ne->bitstreamBuffers[index] = bitstreamParams.bitstreamBuffer;
            }
        }
        ne->width = width;
        ne->height = height;
    }

    if ((data != NULL) && (ne->enc != NULL))
    {
        inputBuffer = ne->inputBuffers[ne->frameCount % 4];
        bitstreamBuffer = ne->bitstreamBuffers[ne->frameCount % 4];
        g_memset(&lockInput, 0, sizeof(lockInput));
        lockInput.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
        lockInput.inputBuffer = inputBuffer;
        ng->enc_funcs.nvEncLockInputBuffer(ne->enc, &lockInput);
        src8 = data;
        dst8 = lockInput.bufferDataPtr;
        for (index = 0; index < height; index++)
        {
            g_memcpy(dst8, src8, width);
            src8 += width;
            dst8 += lockInput.pitch;
        }
        src8 = data;
        src8 += width * height;
        dst8 = lockInput.bufferDataPtr;
        dst8 += height * lockInput.pitch;
        for (index = 0; index < height; index += 2)
        {
            g_memcpy(dst8, src8, width);
            src8 += width;
            dst8 += lockInput.pitch;
        }
        ng->enc_funcs.nvEncUnlockInputBuffer(ne->enc, inputBuffer);
        g_memset(&picParams, 0, sizeof(picParams));
        picParams.version = NV_ENC_PIC_PARAMS_VER;
        picParams.inputBuffer = inputBuffer;
        picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12_PL;
        picParams.inputWidth = width;
        picParams.inputHeight = height;
        picParams.outputBitstream = bitstreamBuffer;
        picParams.inputTimeStamp = ne->frameCount;
        picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        ng->enc_funcs.nvEncEncodePicture(ne->enc, &picParams);
        g_memset(&lockBitstream, 0, sizeof(lockBitstream));
        lockBitstream.version = NV_ENC_LOCK_BITSTREAM_VER;
        lockBitstream.outputBitstream = bitstreamBuffer;
        ng->enc_funcs.nvEncLockBitstream(ne->enc, &lockBitstream);
        g_memcpy(cdata, lockBitstream.bitstreamBufferPtr, lockBitstream.bitstreamSizeInBytes);
        *cdata_bytes = lockBitstream.bitstreamSizeInBytes;
        ng->enc_funcs.nvEncUnlockBitstream(ne->enc, bitstreamBuffer);
        ne->frameCount++;
    }
    return 0; 
}

