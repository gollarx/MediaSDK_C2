/********************************************************************************

INTEL CORPORATION PROPRIETARY INFORMATION
This software is supplied under the terms of a license agreement or nondisclosure
agreement with Intel Corporation and may not be copied or disclosed except in
accordance with the terms of that agreement
Copyright(c) 2017 Intel Corporation. All Rights Reserved.

*********************************************************************************/

#include "mfx_gralloc_allocator.h"

#include "mfx_debug.h"
#include "mfx_c2_debug.h"
#include "mfx_c2_utils.h"

using namespace android;

#undef MFX_DEBUG_MODULE_NAME
#define MFX_DEBUG_MODULE_NAME "mfx_gralloc_allocator"

status_t MfxGrallocModule::Create(std::unique_ptr<MfxGrallocModule>* allocator)
{
    status_t res = OK;
    if (allocator) {
        std::unique_ptr<MfxGrallocModule> alloc(new (std::nothrow)MfxGrallocModule());
        if (alloc) {
            res = alloc->Init();
            if (res == OK) *allocator = std::move(alloc);
        } else {
            res = NO_MEMORY;
        }
    } else {
        res = UNEXPECTED_NULL;
    }
    return res;
}

MfxGrallocModule::~MfxGrallocModule()
{
    MFX_DEBUG_TRACE_FUNC;

#ifdef MFX_C2_USE_GRALLOC_1
    if (gralloc1_dev_) gralloc1_close(gralloc1_dev_);
#endif
}

status_t MfxGrallocModule::Init()
{
    status_t res = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &hw_module_);

#ifdef MFX_C2_USE_GRALLOC_1
    if (res == OK) {
        int32_t gr1_err = GRALLOC1_ERROR_NONE;
        do {
            gr1_err = gralloc1_open(hw_module_, &gralloc1_dev_);

            if (GRALLOC1_ERROR_NONE != gr1_err) {
                res = UNKNOWN_ERROR;
                break;
            }

            bool functions_acquired =
                gr_get_format_.Acquire(gralloc1_dev_) &&
                gr_get_dimensions_.Acquire(gralloc1_dev_) &&
                gr_get_stride_.Acquire(gralloc1_dev_);

            if (!functions_acquired) {
                res = UNKNOWN_ERROR;
                gralloc1_close(gralloc1_dev_);
                gralloc1_dev_ = nullptr;
            }
        } while (false);
    }
#else
    if (res == OK) {
        gralloc_module_ = (gralloc_module_t*)hw_module_;
    }
#endif
    return res;
}

android::status_t MfxGrallocModule::GetBufferDetails(const buffer_handle_t handle,
    MfxGrallocModule::BufferDetails* details)
{
    MFX_DEBUG_TRACE_FUNC;

    status_t res = OK;
#ifdef MFX_C2_USE_GRALLOC_1
        int32_t errGetFormat     = (*gr_get_format_)(gralloc1_dev_, handle, &(details->format));
        int32_t errGetStride     = (*gr_get_stride_)(gralloc1_dev_, handle, &(details->pitch));
        uint32_t width {};
        uint32_t height {};
        int32_t errGetDimensions = (*gr_get_dimensions_)(gralloc1_dev_, handle, &width, &height);

        if (GRALLOC1_ERROR_NONE == errGetFormat &&
            GRALLOC1_ERROR_NONE == errGetStride &&
            GRALLOC1_ERROR_NONE == errGetDimensions)
        {
            details->width = details->allocWidth = width;
            details->height = details->allocHeight = height;
        }
        else
        {
            res = UNKNOWN_ERROR;
        }
#else
    struct intel_ufo_buffer_details_t
    {
        // this structure mimics the same from ufo android o mr0
        uint32_t magic;         // [in] size of this struct

        int width;              // \see alloc_device_t::alloc
        int height;             // \see alloc_device_t::alloc
        int format;             // \see alloc_device_t::alloc \note resolved format (not flexible)

        uint32_t placeholder1[7];

        uint32_t pitch;         // buffer pitch (in bytes)
        uint32_t allocWidth;    // allocated buffer width in pixels.
        uint32_t allocHeight;   // allocated buffer height in lines.

        uint32_t placeholder2[10];
    };

    const int INTEL_UFO_GRALLOC_MODULE_PERFORM_GET_BO_INFO = 6;

    intel_ufo_buffer_details_t info {};
    info.magic = sizeof(info);
    int err = gralloc_module_->perform(gralloc_module_, INTEL_UFO_GRALLOC_MODULE_PERFORM_GET_BO_INFO, handle, &info);
    if (0 != err) {
        MFX_DEBUG_TRACE_MSG("Failed to get BO_INFO");
        res = UNKNOWN_ERROR;
    } else {
        details->width = info.width;
        details->height = info.height;
        details->format = info.format;
        details->pitch = info.pitch;
        details->allocWidth = info.allocWidth;
        details->allocHeight = info.allocHeight;
    }
#endif
    return res;
}

status_t MfxGrallocAllocator::Create(std::unique_ptr<MfxGrallocAllocator>* allocator)
{
    status_t res = OK;
    if (allocator) {
        std::unique_ptr<MfxGrallocAllocator> alloc(new (std::nothrow)MfxGrallocAllocator());
        if (alloc) {
            res = alloc->Init();
            if (res == OK) *allocator = std::move(alloc);
        } else {
            res = NO_MEMORY;
        }
    } else {
        res = UNEXPECTED_NULL;
    }
    return res;
}

status_t MfxGrallocAllocator::Init()
{
    status_t res = MfxGrallocModule::Init();

#ifdef MFX_C2_USE_GRALLOC_1
    if (OK == res) {
        bool functions_acquired =
            gr_allocate_.Acquire(gralloc1_dev_) &&
            gr_release_.Acquire(gralloc1_dev_) &&
            gr_lock_.Acquire(gralloc1_dev_) &&
            gr_unlock_.Acquire(gralloc1_dev_) &&
            gr_create_descriptor_.Acquire(gralloc1_dev_) &&
            gr_set_consumer_usage_.Acquire(gralloc1_dev_) &&
            gr_set_producer_usage_.Acquire(gralloc1_dev_) &&
            gr_set_dimensions_.Acquire(gralloc1_dev_) &&
            gr_set_format_.Acquire(gralloc1_dev_) &&
            gr_destroy_descriptor_.Acquire(gralloc1_dev_);

        if (!functions_acquired) {
            res = UNKNOWN_ERROR;
            // if MfxGrallocModule::Init allocated some resources
            // its destructor is responsible to free them.
        }
    }
#else
    if (OK == res) {
        res = gralloc_open(hw_module_, &alloc_dev_);
    }
#endif
    return res;
}

MfxGrallocAllocator::~MfxGrallocAllocator()
{
#ifndef MFX_C2_USE_GRALLOC_1
    gralloc_close(alloc_dev_);
#endif
}

status_t MfxGrallocAllocator::Alloc(const uint16_t width, const uint16_t height, buffer_handle_t* handle)
{
    MFX_DEBUG_TRACE_FUNC;
    status_t res = OK;

    MFX_DEBUG_TRACE_I32(width);
    MFX_DEBUG_TRACE_I32(height);

#ifdef MFX_C2_USE_GRALLOC_1

    int32_t gr1_err = GRALLOC1_ERROR_NONE;
    gralloc1_buffer_descriptor_t descriptor = 0;

    do {
        gr1_err = (*gr_create_descriptor_)(gralloc1_dev_, &descriptor);
        if (GRALLOC1_ERROR_NONE != gr1_err) break;

        gr1_err = (*gr_set_consumer_usage_)(gralloc1_dev_, descriptor, GRALLOC1_CONSUMER_USAGE_VIDEO_ENCODER);
        if (GRALLOC1_ERROR_NONE != gr1_err) break;

        gr1_err = (*gr_set_producer_usage_)(gralloc1_dev_, descriptor, GRALLOC1_PRODUCER_USAGE_CPU_WRITE);
        if (GRALLOC1_ERROR_NONE != gr1_err) break;

        gr1_err = (*gr_set_dimensions_)(gralloc1_dev_, descriptor, width, height);
        if (GRALLOC1_ERROR_NONE != gr1_err) break;

        gr1_err = (*gr_set_format_)(gralloc1_dev_, descriptor, HAL_PIXEL_FORMAT_NV12_Y_TILED_INTEL);
        if (GRALLOC1_ERROR_NONE != gr1_err) break;

        gr1_err = (*gr_allocate_)(gralloc1_dev_, 1, &descriptor, handle);
        if (GRALLOC1_ERROR_NONE != gr1_err) break;

    } while(false);

    if (0 != descriptor) {
        (*gr_destroy_descriptor_)(gralloc1_dev_, descriptor);
    }

    if (GRALLOC1_ERROR_NONE != gr1_err)
    {
        MFX_DEBUG_TRACE_I32(gr1_err);
        res = NO_MEMORY;
    }
#else
    int stride;
    res = alloc_dev_->alloc(alloc_dev_, width, height, HAL_PIXEL_FORMAT_NV12_TILED_INTEL,
        GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE,
        (buffer_handle_t *)handle, &stride);

#endif

    MFX_DEBUG_TRACE_P(*handle);
    MFX_DEBUG_TRACE__android_status_t(res);
    return res;
}

status_t MfxGrallocAllocator::Free(const buffer_handle_t handle)
{
    MFX_DEBUG_TRACE_FUNC;
    status_t res = OK;

    MFX_DEBUG_TRACE_P(handle);

    if (handle) {
#ifdef MFX_C2_USE_GRALLOC_1
        int32_t gr1_err = (*gr_release_)(gralloc1_dev_, handle);
        if (GRALLOC1_ERROR_NONE != gr1_err)
        {
            MFX_DEBUG_TRACE_I32(gr1_err);
            res = BAD_VALUE;
        }
#else
        res = alloc_dev_->free(alloc_dev_, handle);
#endif
    }

    MFX_DEBUG_TRACE__android_status_t(res);
    return res;
}

status_t MfxGrallocAllocator::LockFrame(buffer_handle_t handle, uint8_t** data, C2PlaneLayout *layout)
{
    MFX_DEBUG_TRACE_FUNC;
    MFX_DEBUG_TRACE_P(handle);

    status_t res = OK;


    if (!layout) res = UNEXPECTED_NULL;

    BufferDetails details {};
    if (OK == res) {
        res = GetBufferDetails(handle, &details);
    }

    mfxU8 *img = NULL;
    if (OK == res) {
#ifdef MFX_C2_USE_GRALLOC_1
        gralloc1_rect_t rect;
        rect.left   = 0;
        rect.top    = 0;
        rect.width  = details.width;
        rect.height = details.height;

        int32_t err = (*gr_lock_)(gralloc1_dev_,
                                   (buffer_handle_t)handle,
                                   GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK,
                                   GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK,
                                   &rect,
                                   (void**)&img,
                                   -1);

        if (GRALLOC1_ERROR_NONE != err || !img)
        {
            res = MFX_ERR_LOCK_MEMORY;
        }
#else
        res = gralloc_module_->lock(gralloc_module_, handle, GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK,
                                                        0, 0, details.width, details.height, (void**)&img);
#endif
    }

    if (OK == res) {
        InitNV12PlaneLayout(details.pitch, details.allocHeight, layout);
        *data = img;
    }

    MFX_DEBUG_TRACE__android_status_t(res);
    return res;
}

status_t MfxGrallocAllocator::UnlockFrame(buffer_handle_t handle)
{
    MFX_DEBUG_TRACE_FUNC;
    MFX_DEBUG_TRACE_P(handle);
    status_t res = OK;

#ifdef MFX_C2_USE_GRALLOC_1
    int32_t releaseFence = -1;
    int32_t gr1_res = (*gr_unlock_)(gralloc1_dev_, (buffer_handle_t)handle, &releaseFence);
    if (GRALLOC1_ERROR_NONE != gr1_res)
    {
        MFX_DEBUG_TRACE_I32(gr1_res);
        res = INVALID_OPERATION;
    }
#else
    res = gralloc_module_->unlock(gralloc_module_, handle);
#endif
    MFX_DEBUG_TRACE__android_status_t(res);
    return res;
}