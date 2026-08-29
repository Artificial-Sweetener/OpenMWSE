#include "fixedstepscheduler.hpp"

#include <stdexcept>

namespace Diagnostics
{
    void FixedStepScheduler::setFrameDuration(double duration)
    {
        if (!(duration > 0.0 && duration <= 0.2))
            throw std::invalid_argument("diagnostic fixed frame duration must be within (0, 0.2] seconds");
        mFrameDuration = duration;
    }

    void FixedStepScheduler::schedule(std::uint32_t count)
    {
        if (count == 0 || count > 10000)
            throw std::invalid_argument("diagnostic fixed frame count must be within [1, 10000]");
        mRemainingFrames = count;
    }

    FixedStepScheduler::Frame FixedStepScheduler::beginFrame() const noexcept
    {
        return { .mScheduled = mRemainingFrames > 0, .mDuration = mRemainingFrames > 0 ? mFrameDuration : 0.0 };
    }

    void FixedStepScheduler::completeFrame(const Frame& frame) noexcept
    {
        if (frame.mScheduled && mRemainingFrames > 0)
        {
            --mRemainingFrames;
            ++mCompletedFrames;
        }
    }

    std::uint32_t FixedStepScheduler::remainingFrames() const noexcept
    {
        return mRemainingFrames;
    }

    std::uint64_t FixedStepScheduler::completedFrames() const noexcept
    {
        return mCompletedFrames;
    }

    double FixedStepScheduler::frameDuration() const noexcept
    {
        return mFrameDuration;
    }
}
