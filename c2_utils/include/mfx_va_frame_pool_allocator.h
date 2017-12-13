/********************************************************************************

INTEL CORPORATION PROPRIETARY INFORMATION
This software is supplied under the terms of a license agreement or nondisclosure
agreement with Intel Corporation and may not be copied or disclosed except in
accordance with the terms of that agreement
Copyright(c) 2017 Intel Corporation. All Rights Reserved.

*********************************************************************************/

#pragma once

#if defined(LIBVA_SUPPORT)

#include "mfx_va_allocator.h"
#include "mfx_frame_converter.h"
#include "mfx_frame_pool_allocator.h"
#include "mfx_pool.h"
#include <mutex>
#include "mfx_debug.h"


// Complex allocator: allocates pool through MfxFrameAllocator interface.
// Allocates for every required item of mfxAllocRequest pair:
// (C2GraphicBlock, mfxMemId).
// Then alloc method gives pre-allocated frames from the pool.
class MfxVaFramePoolAllocator : public MfxVaFrameAllocator, public MfxFramePoolAllocator
{
public:
    MfxVaFramePoolAllocator(VADisplay dpy):
        MfxVaFrameAllocator(dpy)
    {
        pool_ = std::make_unique<MfxPool<android::C2GraphicBlock>>();
    }
    virtual ~MfxVaFramePoolAllocator() = default;
private:
    virtual void SetC2Allocator(std::shared_ptr<android::C2BlockAllocator> c2_allocator) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        c2_allocator_ = c2_allocator;
    }

    virtual std::shared_ptr<android::C2GraphicBlock> Alloc() override
    {
        MFX_DEBUG_TRACE_FUNC;
        std::shared_ptr<android::C2GraphicBlock> res = pool_->Alloc();
        MFX_DEBUG_TRACE_STREAM(res);
        return res;
    }
    // Forget about allocated resources.
    virtual void Reset() override
    {
        pool_ = std::make_unique<MfxPool<android::C2GraphicBlock>>();
    }
private:
    virtual mfxStatus AllocFrames(mfxFrameAllocRequest *request, mfxFrameAllocResponse *response) override;

    virtual mfxStatus FreeFrames(mfxFrameAllocResponse *response) override;

private:
    std::mutex mutex_;

    std::shared_ptr<android::C2BlockAllocator> c2_allocator_;

    std::unique_ptr<MfxPool<android::C2GraphicBlock>> pool_;

private:
    MFX_CLASS_NO_COPY(MfxVaFramePoolAllocator)
};

#endif //#if defined(LIBVA_SUPPORT)