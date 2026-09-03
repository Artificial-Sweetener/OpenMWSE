#include <apps/openmw-diagnosticctl/gameplayrouteexecution.hpp>

#include <gtest/gtest.h>

#include <stdexcept>

TEST(DiagnosticGameplayRouteExecutionTest, UsesTheWholeRouteInProtocolSizedSegmentsByDefault)
{
    const DiagnosticCtl::GameplayRouteExecutionPlan plan
        = DiagnosticCtl::makeGameplayRouteExecutionPlan(1400, std::nullopt, std::nullopt);

    EXPECT_EQ(plan.mFrameCount, 1400);
    EXPECT_EQ(plan.mSegmentFrameCount, 600u);
}

TEST(DiagnosticGameplayRouteExecutionTest, LimitsExecutionAndCheckpointCadence)
{
    const DiagnosticCtl::GameplayRouteExecutionPlan plan
        = DiagnosticCtl::makeGameplayRouteExecutionPlan(452, "180", "60");

    EXPECT_EQ(plan.mFrameCount, 180);
    EXPECT_EQ(plan.mSegmentFrameCount, 60u);
}

TEST(DiagnosticGameplayRouteExecutionTest, RejectsInvalidAndOutOfRangeLimits)
{
    EXPECT_THROW(DiagnosticCtl::makeGameplayRouteExecutionPlan(0, std::nullopt, std::nullopt), std::invalid_argument);
    EXPECT_THROW(DiagnosticCtl::makeGameplayRouteExecutionPlan(100, "0", std::nullopt), std::invalid_argument);
    EXPECT_THROW(DiagnosticCtl::makeGameplayRouteExecutionPlan(100, "101", std::nullopt), std::invalid_argument);
    EXPECT_THROW(DiagnosticCtl::makeGameplayRouteExecutionPlan(100, "20x", std::nullopt), std::invalid_argument);
    EXPECT_THROW(DiagnosticCtl::makeGameplayRouteExecutionPlan(1000, std::nullopt, "601"), std::invalid_argument);
    EXPECT_THROW(DiagnosticCtl::makeGameplayRouteExecutionPlan(100, "20", "21"), std::invalid_argument);
}

TEST(DiagnosticGameplayRouteExecutionTest, RequiresExactRouteProgressRatherThanRenderedFrameCount)
{
    EXPECT_NO_THROW(DiagnosticCtl::validateGameplayRouteProgress(100, 40, 60));
    EXPECT_NO_THROW(DiagnosticCtl::validateGameplayRouteProgress(40, 40, 0));
    EXPECT_THROW(DiagnosticCtl::validateGameplayRouteProgress(100, 40, 100), std::runtime_error);
    EXPECT_THROW(DiagnosticCtl::validateGameplayRouteProgress(100, 40, 61), std::runtime_error);
    EXPECT_THROW(DiagnosticCtl::validateGameplayRouteProgress(100, 40, 59), std::runtime_error);
    EXPECT_THROW(DiagnosticCtl::validateGameplayRouteProgress(39, 40, 0), std::runtime_error);
}
