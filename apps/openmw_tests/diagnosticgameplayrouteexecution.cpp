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

/// Retain only the requested delayed interval while executing every preceding route state.
TEST(DiagnosticGameplayRouteExecutionTest, SelectsOnlySegmentsIntersectingTheCaptureTail)
{
    EXPECT_EQ(DiagnosticCtl::resolveGameplayRouteCaptureStart(std::nullopt, 1000), 0u);
    EXPECT_EQ(DiagnosticCtl::resolveGameplayRouteCaptureStart("900", 1000), 900u);
    EXPECT_THROW(DiagnosticCtl::resolveGameplayRouteCaptureStart("1000", 1000), std::invalid_argument);
    EXPECT_THROW(DiagnosticCtl::resolveGameplayRouteCaptureStart("-1", 1000), std::invalid_argument);
    EXPECT_THROW(DiagnosticCtl::resolveGameplayRouteCaptureStart("tail", 1000), std::invalid_argument);

    EXPECT_FALSE(DiagnosticCtl::shouldCaptureGameplayRouteSegment(0, 256, 900));
    EXPECT_FALSE(DiagnosticCtl::shouldCaptureGameplayRouteSegment(512, 256, 900));
    EXPECT_TRUE(DiagnosticCtl::shouldCaptureGameplayRouteSegment(768, 256, 900));
    EXPECT_TRUE(DiagnosticCtl::shouldCaptureGameplayRouteSegment(1024, 16, 900));
    EXPECT_FALSE(DiagnosticCtl::shouldCaptureGameplayRouteSegment(768, 0, 900));
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

TEST(DiagnosticGameplayRouteExecutionTest, ExtractsClearanceWithoutParsingRawPerformanceStatistics)
{
    const std::string response
        = "request-frame: 4\nroute-clearance:\n  provider: bullet-standing-actor@1\n"
          "  frames: []\nprovider: performance@16\nraw-statistics:\nStats Viewer FrameNumber 4\n";
    EXPECT_EQ(DiagnosticCtl::extractGameplayRouteClearanceReport(response),
        "route-clearance:\n  provider: bullet-standing-actor@1\n  frames: []");
    EXPECT_THROW(DiagnosticCtl::extractGameplayRouteClearanceReport("provider: performance@16\n"), std::runtime_error);
    EXPECT_THROW(
        DiagnosticCtl::extractGameplayRouteClearanceReport("route-clearance:\n  frames: []\n"), std::runtime_error);
}

/// Extract traversal evidence independently when clearance evidence precedes it in the same response.
TEST(DiagnosticGameplayRouteExecutionTest, ExtractsTraversalWithoutParsingOtherRouteEvidence)
{
    const std::string response
        = "route-clearance:\n  frames: []\nroute-traversal:\n  provider: bullet-player-movement@1\n"
          "  frames: []\nprovider: performance@6\n";
    EXPECT_EQ(DiagnosticCtl::extractGameplayRouteTraversalReport(response),
        "route-traversal:\n  provider: bullet-player-movement@1\n  frames: []");
    EXPECT_THROW(
        DiagnosticCtl::extractGameplayRouteTraversalReport("route-traversal:\n  frames: []\n"), std::runtime_error);
}
