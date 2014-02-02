/*
 * Copyright (c) 2011-2012, The Linux Foundation. All rights reserved.

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of The Linux Foundation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <cutils/log.h>
#include <fcntl.h>
#include <dlfcn.h>
#include "gralloc_priv.h"
#include "alloc_controller.h"
#include "memalloc.h"
#include "ionalloc.h"
#include "gr.h"
#include "comptype.h"
#include "mdp_version.h"

#ifdef VENUS_COLOR_FORMAT
#include <media/msm_media_info.h>
#else
#define VENUS_Y_STRIDE(args...) 0
#define VENUS_Y_SCANLINES(args...) 0
#define VENUS_BUFFER_SIZE(args...) 0
#endif

#define ASTC_BLOCK_SIZE 16
#define ASTC_IN_UNITS(n, unit_size)  (((n) + (unit_size) -1) / (unit_size))

using namespace gralloc;
using namespace qdutils;

ANDROID_SINGLETON_STATIC_INSTANCE(AdrenoMemInfo);

//Common functions
static bool canFallback(int usage, bool triedSystem)
{
    // Fallback to system heap when alloc fails unless
    // 1. Composition type is MDP
    // 2. Alloc from system heap was already tried
    // 3. The heap type is requsted explicitly
    // 4. The heap type is protected
    // 5. The buffer is meant for external display only

    if(QCCompositionType::getInstance().getCompositionType() &
       COMPOSITION_TYPE_MDP)
        return false;
    if(triedSystem)
        return false;
    if(usage & (GRALLOC_HEAP_MASK | GRALLOC_USAGE_PROTECTED))
        return false;
    if(usage & (GRALLOC_HEAP_MASK | GRALLOC_USAGE_PRIVATE_EXTERNAL_ONLY))
        return false;
    //Return true by default
    return true;
}

static bool useUncached(int usage)
{
    if (usage & GRALLOC_USAGE_PRIVATE_UNCACHED)
        return true;
    if(((usage & GRALLOC_USAGE_SW_WRITE_MASK) == GRALLOC_USAGE_SW_WRITE_RARELY)
       ||((usage & GRALLOC_USAGE_SW_READ_MASK) == GRALLOC_USAGE_SW_READ_RARELY))
        return true;
    return false;
}

//-------------- AdrenoMemInfo-----------------------//
AdrenoMemInfo::AdrenoMemInfo()
{
    LINK_adreno_compute_aligned_width_and_height = NULL;
    LINK_adreno_compute_padding = NULL;
    LINK_adreno_isMacroTilingSupportedByGpu = NULL;

    libadreno_utils = ::dlopen("libadreno_utils.so", RTLD_NOW);
    if (libadreno_utils) {
        *(void **)&LINK_adreno_compute_aligned_width_and_height =
                ::dlsym(libadreno_utils, "compute_aligned_width_and_height");
        *(void **)&LINK_adreno_compute_padding =
                ::dlsym(libadreno_utils, "compute_surface_padding");
        *(void **)&LINK_adreno_isMacroTilingSupportedByGpu =
                ::dlsym(libadreno_utils, "isMacroTilingSupportedByGpu");
    }
}

AdrenoMemInfo::~AdrenoMemInfo()
{
    if (libadreno_utils) {
        ::dlclose(libadreno_utils);
    }
}

int AdrenoMemInfo::isMacroTilingSupportedByGPU()
{
    if ((libadreno_utils)) {
        if(LINK_adreno_isMacroTilingSupportedByGpu) {
            return LINK_adreno_isMacroTilingSupportedByGpu();
        }
    }
    return 0;
}


void AdrenoMemInfo::getAlignedWidthAndHeight(int width, int height, int format,
                            int tile_enabled, int& aligned_w, int& aligned_h)
{
    aligned_w = ALIGN(width, 32);
    aligned_h = ALIGN(height, 32);
    // Currently surface padding is only computed for RGB* surfaces.
    if (format <= HAL_PIXEL_FORMAT_sRGB_X_8888) {
        // Don't add any additional padding if debug.gralloc.map_fb_memory
        // is enabled
        char property[PROPERTY_VALUE_MAX];
        if((property_get("debug.gralloc.map_fb_memory", property, NULL) > 0) &&
           (!strncmp(property, "1", PROPERTY_VALUE_MAX ) ||
           (!strncasecmp(property,"true", PROPERTY_VALUE_MAX )))) {
              return;
        }

        int bpp = 4;
        switch(format)
        {
            case HAL_PIXEL_FORMAT_RGB_888:
                bpp = 3;
                break;
            case HAL_PIXEL_FORMAT_RGB_565:
                bpp = 2;
                break;
            default: break;
        }
        if (libadreno_utils) {
            int raster_mode         = 0;   // Adreno unknown raster mode.
            int padding_threshold   = 512; // Threshold for padding surfaces.
            // the function below computes aligned width and aligned height
            // based on linear or macro tile mode selected.
            if(LINK_adreno_compute_aligned_width_and_height) {
                LINK_adreno_compute_aligned_width_and_height(width,
                                     height, bpp, tile_enabled,
                                     raster_mode, padding_threshold,
                                     &aligned_w, &aligned_h);

            } else if(LINK_adreno_compute_padding) {
                int surface_tile_height = 1;   // Linear surface
                aligned_w = LINK_adreno_compute_padding(width, bpp,
                                     surface_tile_height, raster_mode,
                                     padding_threshold);
                ALOGW("%s: Warning!! Old GFX API is used to calculate stride",
                                                            __FUNCTION__);
            } else {
                ALOGW("%s: Warning!! Symbols compute_surface_padding and " \
                    "compute_aligned_width_and_height not found", __FUNCTION__);
            }
        }
    } else {
        switch (format)
        {
            case HAL_PIXEL_FORMAT_YCrCb_420_SP_ADRENO:
            case HAL_PIXEL_FORMAT_RAW_SENSOR:
                aligned_w = ALIGN(width, 32);
                break;
            case HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED:
                aligned_w = ALIGN(width, 128);
                break;
            case HAL_PIXEL_FORMAT_YCbCr_420_SP:
            case HAL_PIXEL_FORMAT_YCrCb_420_SP:
            case HAL_PIXEL_FORMAT_YV12:
            case HAL_PIXEL_FORMAT_YCbCr_422_SP:
            case HAL_PIXEL_FORMAT_YCrCb_422_SP:
            case HAL_PIXEL_FORMAT_YCbCr_422_I:
            case HAL_PIXEL_FORMAT_YCrCb_422_I:
                aligned_w = ALIGN(width, 16);
                break;
            case HAL_PIXEL_FORMAT_YCbCr_420_SP_VENUS:
            case HAL_PIXEL_FORMAT_NV12_ENCODEABLE:
                aligned_w = VENUS_Y_STRIDE(COLOR_FMT_NV12, width);
                break;
            case HAL_PIXEL_FORMAT_BLOB:
                aligned_w = width;
                break;
            case HAL_PIXEL_FORMAT_NV21_ZSL:
                aligned_w = ALIGN(width, 64);
                break;
            case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_4x4_KHR:
            case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR:
                aligned_w = ASTC_IN_UNITS(width, 4);
                aligned_h = ASTC_IN_UNITS(height, 4);
                break;
            case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_5x4_KHR:
            case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR:
                aligned_w = ASTC_IN_UNITS(width, 5);
                aligned_h = ASTC_IN_UNITS(height, 4);
                break;
            case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_5x5_KHR:
            case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR:
                aligned_w = ASTC_IN_UNITS(width, 5);
                aligned_h = ASTC_IN_UNITS(height, 5);
                break;
            case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_6x5_KHR:
            case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR:
                aligned_w = ASTC_IN_UNITS(width, 6);
                aligned_h = ASTC_IN_UNITS(height, 5);
                break;
            case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_6x6_KHR:
            case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR:
                aligned_w = ASTC_IN_UNITS(width, 6);
                aligned_h = ASTC_IN_UNITS(height, 6);
                break;
            case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_8x5_KHR:
            case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR:
                aligned_w = ASTC_IN_UNITS(width, 8);
                aligned_h = ASTC_IN_UNITS(height, 5);
                break;
            case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_8x6_KHR:
            case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR:
                aligned_w = ASTC_IN_UNITS(width, 8);
                aligned_h = ASTC_IN_UNITS(height, 6);
                break;
            case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_8x8_KHR:
            case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR:
                aligned_w = ASTC_IN_UNITS(width, 8);
                aligned_h = ASTC_IN_UNITS(height, 8);
                break;
            case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_10x5_KHR:
            case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_10x5_KHR:
                aligned_w = ASTC_IN_UNITS(width, 10);
                aligned_h = ASTC_IN_UNITS(height, 5);
                break;
            case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_10x6_KHR:
            case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_10x6_KHR:
                aligned_w = ASTC_IN_UNITS(width, 10);
                aligned_h = ASTC_IN_UNITS(height, 6);
                break;
            case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_10x8_KHR:
            case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_10x8_KHR:
                aligned_w = ASTC_IN_UNITS(width, 10);
                aligned_h = ASTC_IN_UNITS(height, 8);
                break;
            case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_10x10_KHR:
            case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10_KHR:
                aligned_w = ASTC_IN_UNITS(width, 10);
                aligned_h = ASTC_IN_UNITS(height, 10);
                break;
            case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_12x10_KHR:
            case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_12x10_KHR:
                aligned_w = ASTC_IN_UNITS(width, 12);
                aligned_h = ASTC_IN_UNITS(height, 10);
                break;
            case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_12x12_KHR:
            case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_12x12_KHR:
                aligned_w = ASTC_IN_UNITS(width, 12);
                aligned_h = ASTC_IN_UNITS(height, 12);
                break;
            default: break;
        }
    }
}

//-------------- IAllocController-----------------------//
IAllocController* IAllocController::sController = NULL;
IAllocController* IAllocController::getInstance(void)
{
    if(sController == NULL) {
        sController = new IonController();
    }
    return sController;
}


//-------------- IonController-----------------------//
IonController::IonController()
{
    mIonAlloc = new IonAlloc();
}

int IonController::allocate(alloc_data& data, int usage)
{
    int ionFlags = 0;
    int ret;

    data.uncached = useUncached(usage);
    data.allocType = 0;

    if(usage & GRALLOC_USAGE_PRIVATE_UI_CONTIG_HEAP)
        ionFlags |= ION_HEAP(ION_SF_HEAP_ID);

    if(usage & GRALLOC_USAGE_PRIVATE_SYSTEM_HEAP)
        ionFlags |= ION_HEAP(ION_SYSTEM_HEAP_ID);

    if(usage & GRALLOC_USAGE_PRIVATE_IOMMU_HEAP)
        ionFlags |= ION_HEAP(ION_IOMMU_HEAP_ID);

    if(usage & GRALLOC_USAGE_PROTECTED) {
        if (usage & GRALLOC_USAGE_PRIVATE_MM_HEAP) {
            ionFlags |= ION_HEAP(ION_CP_MM_HEAP_ID);
            ionFlags |= ION_SECURE;
        } else {
            // for targets/OEMs which do not need HW level protection
            // do not set ion secure flag & MM heap. Fallback to IOMMU heap.
            ionFlags |= ION_HEAP(ION_IOMMU_HEAP_ID);
        }
    } else if(usage & GRALLOC_USAGE_PRIVATE_MM_HEAP) {
        //MM Heap is exclusively a secure heap.
        //If it is used for non secure cases, fallback to IOMMU heap
        ALOGW("GRALLOC_USAGE_PRIVATE_MM_HEAP \
                                cannot be used as an insecure heap!\
                                trying to use IOMMU instead !!");
        ionFlags |= ION_HEAP(ION_IOMMU_HEAP_ID);
    }

    if(usage & GRALLOC_USAGE_PRIVATE_CAMERA_HEAP)
        ionFlags |= ION_HEAP(ION_CAMERA_HEAP_ID);

    if(usage & GRALLOC_USAGE_PRIVATE_ADSP_HEAP)
        ionFlags |= ION_HEAP(ION_ADSP_HEAP_ID);

    if(ionFlags & ION_SECURE)
         data.allocType |= private_handle_t::PRIV_FLAGS_SECURE_BUFFER;

    // if no flags are set, default to
    // SF + IOMMU heaps, so that bypass can work
    // we can fall back to system heap if
    // we run out.
    if(!ionFlags)
        ionFlags = ION_HEAP(ION_SF_HEAP_ID) | ION_HEAP(ION_IOMMU_HEAP_ID);

    data.flags = ionFlags;
    ret = mIonAlloc->alloc_buffer(data);

    // Fallback
    if(ret < 0 && canFallback(usage,
                              (ionFlags & ION_SYSTEM_HEAP_ID)))
    {
        ALOGW("Falling back to system heap");
        data.flags = ION_HEAP(ION_SYSTEM_HEAP_ID);
        ret = mIonAlloc->alloc_buffer(data);
    }

    if(ret >= 0 ) {
        data.allocType |= private_handle_t::PRIV_FLAGS_USES_ION;
    }

    return ret;
}

IMemAlloc* IonController::getAllocator(int flags)
{
    IMemAlloc* memalloc = NULL;
    if (flags & private_handle_t::PRIV_FLAGS_USES_ION) {
        memalloc = mIonAlloc;
    } else {
        ALOGE("%s: Invalid flags passed: 0x%x", __FUNCTION__, flags);
    }

    return memalloc;
}

bool isMacroTileEnabled(int format, int usage)
{
    bool tileEnabled = false;

    // Check whether GPU & MDSS supports MacroTiling feature
    if(AdrenoMemInfo::getInstance().isMacroTilingSupportedByGPU() &&
            qdutils::MDPVersion::getInstance().supportsMacroTile())
    {
        // check the format
        switch(format)
        {
            case  HAL_PIXEL_FORMAT_RGBA_8888:
            case  HAL_PIXEL_FORMAT_RGBX_8888:
            case  HAL_PIXEL_FORMAT_BGRA_8888:
                {
                    tileEnabled = true;
                    // check the usage flags
                    if (usage & (GRALLOC_USAGE_SW_READ_MASK |
                                GRALLOC_USAGE_SW_WRITE_MASK)) {
                        // Application intends to use CPU for rendering
                        tileEnabled = false;
                    }
                    break;
                }
            default:
                break;
        }
    }
    return tileEnabled;
}

// helper function
size_t getSize(int format, int width, int height, int alignedw, int alignedh)
{
    size_t size;

    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
        case HAL_PIXEL_FORMAT_sRGB_A_8888:
        case HAL_PIXEL_FORMAT_sRGB_X_8888:
            size = alignedw * alignedh * 4;
            break;
        case HAL_PIXEL_FORMAT_RGB_888:
            size = alignedw * alignedh * 3;
            break;
        case HAL_PIXEL_FORMAT_RGB_565:
        case HAL_PIXEL_FORMAT_RAW_SENSOR:
            size = alignedw * alignedh * 2;
            break;

            // adreno formats
        case HAL_PIXEL_FORMAT_YCrCb_420_SP_ADRENO:  // NV21
            size  = ALIGN(alignedw*alignedh, 4096);
            size += ALIGN(2 * ALIGN(width/2, 32) * ALIGN(height/2, 32), 4096);
            break;
        case HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED:   // NV12
            // The chroma plane is subsampled,
            // but the pitch in bytes is unchanged
            // The GPU needs 4K alignment, but the video decoder needs 8K
            size  = ALIGN( alignedw * alignedh, 8192);
            size += ALIGN( alignedw * ALIGN(height/2, 32), 8192);
            break;
        case HAL_PIXEL_FORMAT_YV12:
            if ((format == HAL_PIXEL_FORMAT_YV12) && ((width&1) || (height&1))) {
                ALOGE("w or h is odd for the YV12 format");
                return -EINVAL;
            }
            alignedh = height;
            size = alignedw*alignedh +
                    (ALIGN(alignedw/2, 16) * (alignedh/2))*2;
            size = ALIGN(size, 4096);
            break;
        case HAL_PIXEL_FORMAT_YCbCr_420_SP:
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
            alignedh = height;
            size = ALIGN((alignedw*alignedh) + (alignedw* alignedh)/2 + 1, 4096);
            break;
        case HAL_PIXEL_FORMAT_YCbCr_422_SP:
        case HAL_PIXEL_FORMAT_YCrCb_422_SP:
        case HAL_PIXEL_FORMAT_YCbCr_422_I:
        case HAL_PIXEL_FORMAT_YCrCb_422_I:
            if(width & 1) {
                ALOGE("width is odd for the YUV422_SP format");
                return -EINVAL;
            }
            alignedh = height;
            size = ALIGN(alignedw * alignedh * 2, 4096);
            break;
        case HAL_PIXEL_FORMAT_YCbCr_420_SP_VENUS:
        case HAL_PIXEL_FORMAT_NV12_ENCODEABLE:
            alignedh = VENUS_Y_SCANLINES(COLOR_FMT_NV12, height);
            size = VENUS_BUFFER_SIZE(COLOR_FMT_NV12, width, height);
            break;
        case HAL_PIXEL_FORMAT_BLOB:
            if(height != 1) {
                ALOGE("%s: Buffers with format HAL_PIXEL_FORMAT_BLOB \
                      must have height==1 ", __FUNCTION__);
                return -EINVAL;
            }
            alignedh = height;
            alignedw = width;
            size = width;
            break;
        case HAL_PIXEL_FORMAT_NV21_ZSL:
            alignedh = ALIGN(height, 64);
            size = ALIGN((alignedw*alignedh) + (alignedw* alignedh)/2, 4096);
            break;
        case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_4x4_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_5x4_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_5x5_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_6x5_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_6x6_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_8x5_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_8x6_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_8x8_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_10x5_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_10x6_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_10x8_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_10x10_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_12x10_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_RGBA_ASTC_12x12_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_10x5_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_10x6_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_10x8_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10_KHR:
        case HAL_PIXEL_FORMAT_COMPRESSED_SRGB8_ALPHA8_ASTC_12x10_KHR:
            size = alignedw * alignedh * ASTC_BLOCK_SIZE;
            break;
        default:
            ALOGE("unrecognized pixel format: 0x%x", format);
            return -EINVAL;
    }
    return size;
}

size_t getBufferSizeAndDimensions(int width, int height, int format,
        int& alignedw, int &alignedh)
{
    size_t size;

    AdrenoMemInfo::getInstance().getAlignedWidthAndHeight(width,
            height,
            format,
            false,
            alignedw,
            alignedh);

    size = getSize(format, width, height, alignedw, alignedh);

    return size;
}


size_t getBufferSizeAndDimensions(int width, int height, int format, int usage,
        int& alignedw, int &alignedh)
{
    size_t size;
    int tileEnabled = isMacroTileEnabled(format, usage);

    AdrenoMemInfo::getInstance().getAlignedWidthAndHeight(width,
            height,
            format,
            tileEnabled,
            alignedw,
            alignedh);

    size = getSize(format, width, height, alignedw, alignedh);

    return size;
}


void getBufferAttributes(int width, int height, int format, int usage,
        int& alignedw, int &alignedh, int& tileEnabled, size_t& size)
{
    tileEnabled = isMacroTileEnabled(format, usage);

    AdrenoMemInfo::getInstance().getAlignedWidthAndHeight(width,
            height,
            format,
            tileEnabled,
            alignedw,
            alignedh);

    if(size)
        size = getSize(format, width, height, alignedw, alignedh);
}



// Allocate buffer from width, height and format into a
// private_handle_t. It is the responsibility of the caller
// to free the buffer using the free_buffer function
int alloc_buffer(private_handle_t **pHnd, int w, int h, int format, int usage)
{
    alloc_data data;
    int alignedw, alignedh;
    gralloc::IAllocController* sAlloc =
        gralloc::IAllocController::getInstance();
    data.base = 0;
    data.fd = -1;
    data.offset = 0;
    data.size = getBufferSizeAndDimensions(w, h, format, usage, alignedw,
                                            alignedh);

    data.align = getpagesize();
    data.uncached = useUncached(usage);
    int allocFlags = usage;

    int err = sAlloc->allocate(data, allocFlags);
    if (0 != err) {
        ALOGE("%s: allocate failed", __FUNCTION__);
        return -ENOMEM;
    }

    private_handle_t* hnd = new private_handle_t(data.fd, data.size,
                                                 data.allocType, 0, format,
                                                 alignedw, alignedh);
    hnd->base = (int) data.base;
    hnd->offset = data.offset;
    hnd->gpuaddr = 0;
    *pHnd = hnd;
    return 0;
}

void free_buffer(private_handle_t *hnd)
{
    gralloc::IAllocController* sAlloc =
        gralloc::IAllocController::getInstance();
    if (hnd && hnd->fd > 0) {
        IMemAlloc* memalloc = sAlloc->getAllocator(hnd->flags);
        memalloc->free_buffer((void*)hnd->base, hnd->size, hnd->offset, hnd->fd);
    }
    if(hnd)
        delete hnd;

}