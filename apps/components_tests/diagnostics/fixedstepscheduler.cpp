#include <components/diagnostics/fixedstepscheduler.hpp>

#include <gtest/gtest.h>

TEST(FixedStepSchedulerTest, DoesNotConsumeWorkScheduledDuringAnIdleFrame)
{
    Diagnostics::FixedStepScheduler scheduler;
    const Diagnostics::FixedStepScheduler::Frame idleFrame = scheduler.beginFrame();

    scheduler.schedule(2);
    scheduler.completeFrame(idleFrame);

    EXPECT_EQ(scheduler.remainingFrames(), 2u);
    EXPECT_EQ(scheduler.completedFrames(), 0u);
}

TEST(FixedStepSchedulerTest, ConsumesExactlyOneScheduledFrameAfterItBegins)
{
    Diagnostics::FixedStepScheduler scheduler;
    scheduler.setFrameDuration(0.025);
    EXPECT_DOUBLE_EQ(scheduler.frameDuration(), 0.025);
    scheduler.schedule(2);

    const Diagnostics::FixedStepScheduler::Frame first = scheduler.beginFrame();
    EXPECT_TRUE(first.mScheduled);
    EXPECT_DOUBLE_EQ(first.mDuration, 0.025);
    scheduler.completeFrame(first);
    EXPECT_EQ(scheduler.remainingFrames(), 1u);

    const Diagnostics::FixedStepScheduler::Frame second = scheduler.beginFrame();
    scheduler.completeFrame(second);
    EXPECT_EQ(scheduler.remainingFrames(), 0u);
    EXPECT_EQ(scheduler.completedFrames(), 2u);
    EXPECT_FALSE(scheduler.beginFrame().mScheduled);
}

TEST(FixedStepSchedulerTest, RejectsUnboundedDurationsAndCounts)
{
    Diagnostics::FixedStepScheduler scheduler;
    EXPECT_THROW(scheduler.setFrameDuration(0.0), std::invalid_argument);
    EXPECT_THROW(scheduler.setFrameDuration(0.21), std::invalid_argument);
    EXPECT_THROW(scheduler.schedule(0), std::invalid_argument);
    EXPECT_THROW(scheduler.schedule(10001), std::invalid_argument);
}
