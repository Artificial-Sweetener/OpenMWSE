#ifndef OPENMW_COMPONENTS_DIAGNOSTICS_FIXEDSTEPSCHEDULER_HPP
#define OPENMW_COMPONENTS_DIAGNOSTICS_FIXEDSTEPSCHEDULER_HPP

#include <cstdint>

namespace Diagnostics
{
    /// Schedules bounded deterministic frames without consuming work queued during the current frame.
    class FixedStepScheduler
    {
    public:
        /// Identifies whether one frame began with scheduled deterministic work.
        struct Frame
        {
            bool mScheduled = false;
            double mDuration = 0.0;
        };

        /// Set the deterministic frame duration within the supported simulation interval.
        void setFrameDuration(double duration);

        /// Replace pending work with a bounded positive frame count.
        void schedule(std::uint32_t count);

        /// Snapshot the work state that applies to the frame about to execute.
        Frame beginFrame() const noexcept;

        /// Consume a frame only when it began with scheduled work.
        void completeFrame(const Frame& frame) noexcept;

        /// Return the number of deterministic frames that have not begun yet.
        std::uint32_t remainingFrames() const noexcept;

        /// Return the cumulative number of scheduled frames completed by this scheduler.
        std::uint64_t completedFrames() const noexcept;

    private:
        double mFrameDuration = 1.0 / 60.0;
        std::uint32_t mRemainingFrames = 0;
        std::uint64_t mCompletedFrames = 0;
    };
}

#endif
