/********************************************************************************

INTEL CORPORATION PROPRIETARY INFORMATION
This software is supplied under the terms of a license agreement or nondisclosure
agreement with Intel Corporation and may not be copied or disclosed except in
accordance with the terms of that agreement
Copyright(c) 2017 Intel Corporation. All Rights Reserved.

*********************************************************************************/

#include "mfx_c2_encoder_component.h"

#include "mfx_debug.h"
#include "mfx_msdk_debug.h"
#include "mfx_c2_debug.h"
#include "mfx_c2_components_registry.h"
#include "mfx_c2_utils.h"
#include "mfx_defaults.h"

#include <limits>
#include <thread>
#include <chrono>

using namespace android;

const nsecs_t TIMEOUT_NS = MFX_SECOND_NS;

MfxC2EncoderComponent::MfxC2EncoderComponent(const android::C2String name, int flags, EncoderType encoder_type) :
    MfxC2Component(name, flags),
    encoder_type_(encoder_type),
    synced_points_count_(0)
{
    MFX_DEBUG_TRACE_FUNC;
}

MfxC2EncoderComponent::~MfxC2EncoderComponent()
{
    MFX_DEBUG_TRACE_FUNC;

    FreeEncoder();

    session_.Close();
}

void MfxC2EncoderComponent::RegisterClass(MfxC2ComponentsRegistry& registry)
{
    MFX_DEBUG_TRACE_FUNC;

    registry.RegisterMfxC2Component("C2.h264ve",
        &MfxC2Component::Factory<MfxC2EncoderComponent, EncoderType>::Create<ENCODER_H264>);
}

android::status_t MfxC2EncoderComponent::Init()
{
    MFX_DEBUG_TRACE_FUNC;

    Reset();

    mfxStatus mfx_res = MfxDev::Create(&device_);
    if(mfx_res == MFX_ERR_NONE) {
        mfx_res = session_.Init(MFX_IMPLEMENTATION, &g_required_mfx_version);
        MFX_DEBUG_TRACE_I32(g_required_mfx_version.Major);
        MFX_DEBUG_TRACE_I32(g_required_mfx_version.Minor);

        if(mfx_res == MFX_ERR_NONE) {
            mfxIMPL mfx_impl {};
            mfxStatus sts = session_.QueryIMPL(&mfx_impl);
            MFX_DEBUG_TRACE_mfxStatus(sts);
            MFX_DEBUG_TRACE_I32(mfx_impl);

            mfx_res = device_->InitMfxSession(&session_);
        } else {
            MFX_DEBUG_TRACE_MSG("MFXVideoSession::Init failed");
            MFX_DEBUG_TRACE_mfxStatus(mfx_res);
        }
    }

    return MfxStatusToC2(mfx_res);
}

status_t MfxC2EncoderComponent::DoStart()
{
    MFX_DEBUG_TRACE_FUNC;

    working_queue_.Start();
    waiting_queue_.Start();

    return C2_OK;
}

status_t MfxC2EncoderComponent::DoStop()
{
    MFX_DEBUG_TRACE_FUNC;

    waiting_queue_.Stop();
    working_queue_.Stop();
    FreeEncoder();
    Reset();

    return C2_OK;
}

void MfxC2EncoderComponent::Reset()
{
    MFX_DEBUG_TRACE_FUNC;

    switch (encoder_type_)
    {
    case ENCODER_H264:
        video_params_.mfx.CodecId = MFX_CODEC_AVC;
        break;
    default:
        MFX_DEBUG_TRACE_MSG("unhandled codec type: BUG in plug-ins registration");
        break;
    }
    mfx_set_defaults_mfxVideoParam_enc(&video_params_);

    // default pattern
    video_params_.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
}

mfxStatus MfxC2EncoderComponent::InitEncoder(const mfxFrameInfo& frame_info)
{
    MFX_DEBUG_TRACE_FUNC;
    mfxStatus mfx_res = MFX_ERR_NONE;

    video_params_.mfx.FrameInfo = frame_info;
    {
        encoder_.reset(MFX_NEW_NO_THROW(MFXVideoENCODE(session_)));
        if (nullptr == encoder_) {
            mfx_res = MFX_ERR_MEMORY_ALLOC;
        }

        if (MFX_ERR_NONE == mfx_res) {
            mfx_res = encoder_->Init(&video_params_);

            if (MFX_WRN_PARTIAL_ACCELERATION == mfx_res) {
                MFX_DEBUG_TRACE_MSG("InitEncoder returns MFX_WRN_PARTIAL_ACCELERATION");
                mfx_res = MFX_ERR_NONE;
            }
        }

        if (MFX_ERR_NONE == mfx_res) {
            mfx_res = encoder_->GetVideoParam(&video_params_);
        }

        if (MFX_ERR_NONE != mfx_res) {
            FreeEncoder();
        }
    }
    MFX_DEBUG_TRACE_mfxStatus(mfx_res);
    return mfx_res;
}

void MfxC2EncoderComponent::FreeEncoder()
{
    MFX_DEBUG_TRACE_FUNC;

    if(nullptr != encoder_) {
        encoder_->Close();
        encoder_ = nullptr;
    }
}

void MfxC2EncoderComponent::RetainLockedFrame(MfxFrameWrapper input)
{
    MFX_DEBUG_TRACE_FUNC;

    if(input.GetMfxFrameSurface()->Data.Locked) {
        locked_frames_.emplace_back(std::move(input));
    }
}

mfxStatus MfxC2EncoderComponent::EncodeFrameAsync(
    mfxEncodeCtrl *ctrl, mfxFrameSurface1 *surface, mfxBitstream *bs,
    mfxSyncPoint *syncp)
{
    MFX_DEBUG_TRACE_FUNC;

    mfxStatus sts = MFX_ERR_NONE;

    int trying_count = 0;
    const int MAX_TRYING_COUNT = 200;
    const auto timeout = std::chrono::milliseconds(5);

    do {
      sts = encoder_->EncodeFrameAsync(ctrl, surface, bs, syncp);
      ++trying_count;

      if (MFX_WRN_DEVICE_BUSY == sts) {

        if(trying_count >= MAX_TRYING_COUNT) {
            MFX_DEBUG_TRACE_MSG("Too many MFX_WRN_DEVICE_BUSY from EncodeFrameAsync");
            sts = MFX_ERR_DEVICE_FAILED;
            break;
        }

        std::unique_lock<std::mutex> lock(dev_busy_mutex_);
        dev_busy_cond_.wait_for(lock, timeout, [this] { return synced_points_count_ < video_params_.AsyncDepth; } );
      }
    } while (MFX_WRN_DEVICE_BUSY == sts);

    return sts;
}

status_t MfxC2EncoderComponent::AllocateBitstream(const std::unique_ptr<android::C2Work>& work,
    MfxBitstreamWrapper* mfx_bitstream)
{
    // TODO: allocation pool is required here
    MFX_DEBUG_TRACE_FUNC;

    status_t res = C2_OK;

    do {
        MFX_DEBUG_TRACE_I32(video_params_.mfx.BufferSizeInKB);
        MFX_DEBUG_TRACE_I32(video_params_.mfx.BRCParamMultiplier);
        mfxU32 required_size = video_params_.mfx.BufferSizeInKB * 1000 * video_params_.mfx.BRCParamMultiplier;
        MFX_DEBUG_TRACE_I32(required_size);

        if(work->worklets.size() != 1) {
            MFX_DEBUG_TRACE_MSG("Cannot handle multiple worklets");
            res = C2_BAD_VALUE;
            break;
        }

        std::unique_ptr<C2Worklet>& worklet = work->worklets.front();
        C2BufferPack& output = worklet->output;

        if(worklet->allocators.size() != 1 || worklet->output.buffers.size() != 1) {
            MFX_DEBUG_TRACE_MSG("Cannot handle multiple outputs");
            res = C2_BAD_VALUE;
            break;
        }

        std::shared_ptr<C2BlockAllocator> allocator = worklet->allocators.front();
        C2MemoryUsage mem_usage = { C2MemoryUsage::kSoftwareRead, C2MemoryUsage::kSoftwareWrite };
        std::shared_ptr<C2LinearBlock> out_block;

        res = allocator->allocateLinearBlock(required_size, mem_usage, &out_block);
        if(C2_OK != res) break;

        res = MfxBitstreamWrapper::Create(out_block, TIMEOUT_NS, mfx_bitstream);

    } while(false);

    return res;
}

void MfxC2EncoderComponent::DoWork(std::unique_ptr<android::C2Work>&& work)
{
    MFX_DEBUG_TRACE_FUNC;

    MFX_DEBUG_TRACE_P(work.get());

    status_t res = C2_OK;

    do {
        C2BufferPack& input = work->input;

        MfxFrameWrapper mfx_frame;
        res = MfxFrameWrapper::Create(input, TIMEOUT_NS, &mfx_frame);
        if(C2_OK != res) break;

        if(nullptr == encoder_) {
            // get frame format and size for encoder init from the first frame
            // should be got from slot descriptor
            mfxStatus mfx_sts = InitEncoder(mfx_frame.GetMfxFrameSurface()->Info);
            if(MFX_ERR_NONE != mfx_sts) {
                MFX_DEBUG_TRACE_mfxStatus(mfx_sts);
                res = MfxStatusToC2(mfx_sts);
                break;
            }
        }

        MfxBitstreamWrapper mfx_bitstream;
        res = AllocateBitstream(work, &mfx_bitstream);
        if(C2_OK != res) break;

        mfxSyncPoint sync_point;

        mfxStatus mfx_sts = EncodeFrameAsync(nullptr/*encodeCtrl*/,
            mfx_frame.GetMfxFrameSurface(), mfx_bitstream.GetMfxBitstream(), &sync_point);

        if (MFX_WRN_INCOMPATIBLE_VIDEO_PARAM == mfx_sts) mfx_sts = MFX_ERR_NONE;

        if( (MFX_ERR_NONE != mfx_sts) && (MFX_ERR_MORE_DATA != mfx_sts) ) {
            MFX_DEBUG_TRACE_mfxStatus(mfx_sts);
            res = MfxStatusToC2(mfx_sts);
            break;
        }

        waiting_queue_.Push( [ input = std::move(mfx_frame), this ] () mutable {
            RetainLockedFrame(std::move(input));
        } );

        pending_works_.push(std::move(work));

        if(MFX_ERR_NONE == mfx_sts) {

            std::unique_ptr<android::C2Work> work = std::move(pending_works_.front());

            pending_works_.pop();

            waiting_queue_.Push(
                [ work = std::move(work), bs = std::move(mfx_bitstream), sync_point, this ] () mutable {
                WaitWork(std::move(work), std::move(bs), sync_point);
            } );

            {
                std::unique_lock<std::mutex> lock(dev_busy_mutex_);
                ++synced_points_count_;
            }
        }

        if(MFX_ERR_MORE_DATA == mfx_sts) mfx_sts = MFX_ERR_NONE;

    } while(false); // fake loop to have a cleanup point there

    if(C2_OK != res) { // notify listener in case of failure only
        NotifyWorkDone(std::move(work), res);
    }
}

void MfxC2EncoderComponent::Drain()
{
    MFX_DEBUG_TRACE_FUNC;

    status_t res = C2_OK;

    while (!pending_works_.empty()) {

        MfxBitstreamWrapper mfx_bitstream;
        res = AllocateBitstream(pending_works_.front(), &mfx_bitstream);
        if(C2_OK != res) break;

        mfxSyncPoint sync_point;

        mfxStatus mfx_sts = EncodeFrameAsync(nullptr/*encodeCtrl*/,
            nullptr/*input surface*/, mfx_bitstream.GetMfxBitstream(), &sync_point);

        if (MFX_ERR_NONE == mfx_sts) {

            std::unique_ptr<android::C2Work> work = std::move(pending_works_.front());

            pending_works_.pop();

            waiting_queue_.Push(
                [ work = std::move(work), bs = std::move(mfx_bitstream), sync_point, this ] () mutable {
                WaitWork(std::move(work), std::move(bs), sync_point);
            } );

        } else {
            // MFX_ERR_MORE_DATA is an error here too -
            // we are calling EncodeFrameAsync times exactly how many outputs should be fetched
            res = MfxStatusToC2(mfx_sts);
            break;
        }
    }

    if(C2_OK != res) {
        while(!pending_works_.empty()) {
            NotifyWorkDone(std::move(pending_works_.front()), res);
            pending_works_.pop();
        }
    }
}

void MfxC2EncoderComponent::WaitWork(std::unique_ptr<C2Work>&& work,
    MfxBitstreamWrapper&& bit_stream, mfxSyncPoint sync_point)
{
    MFX_DEBUG_TRACE_FUNC;

    mfxStatus mfx_res = MFX_ERR_NONE;

    mfx_res = session_.SyncOperation(sync_point, MFX_INFINITE);

    // checking for unlocked surfaces and releasing them
    locked_frames_.erase(
        std::remove_if(
            locked_frames_.begin(),
            locked_frames_.end(),
            [] (const MfxFrameWrapper& mfx_frame)->bool { return !mfx_frame.GetMfxFrameSurface()->Data.Locked; } ),
        locked_frames_.end());

    if(MFX_ERR_NONE == mfx_res) {

        C2Event event;
        event.fire(); // pre-fire event as output buffer is ready to use
        mfxBitstream* mfx_bitstream = bit_stream.GetMfxBitstream();

        MFX_DEBUG_TRACE_U32(mfx_bitstream->DataOffset);
        MFX_DEBUG_TRACE_U32(mfx_bitstream->DataLength);

        C2ConstLinearBlock const_linear = bit_stream.GetC2LinearBlock()->share(
            mfx_bitstream->DataOffset,
            mfx_bitstream->DataLength, event.fence());
        C2BufferData out_buffer_data = const_linear;

        std::unique_ptr<C2Worklet>& worklet = work->worklets.front();

        worklet->output.ordinal.timestamp = work->input.ordinal.timestamp;
        worklet->output.ordinal.frame_index = work->input.ordinal.frame_index;
        worklet->output.ordinal.custom_ordinal = work->input.ordinal.custom_ordinal;

        worklet->output.buffers.front() = std::make_shared<C2Buffer>(out_buffer_data);
    }

    NotifyWorkDone(std::move(work), MfxStatusToC2(mfx_res));

    {
      std::unique_lock<std::mutex> lock(dev_busy_mutex_);
      --synced_points_count_;
    }
    dev_busy_cond_.notify_one();
}

status_t MfxC2EncoderComponent::queue_nb(std::list<std::unique_ptr<android::C2Work>>* const items)
{
    MFX_DEBUG_TRACE_FUNC;

    for(auto& item : *items) {

        bool eos = (item->input.flags & BUFFERFLAG_END_OF_STREAM);

        working_queue_.Push( [ work = std::move(item), this ] () mutable {

            DoWork(std::move(work));

        } );

        if(eos) {
            working_queue_.Push( [this] () { Drain(); } );
        }
    }

    return C2_OK;
}