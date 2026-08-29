#include <apps/openmw/diagnosticperformance.hpp>

#include <gtest/gtest.h>

#include <array>
#include <limits>

TEST(DiagnosticPerformanceTest, CalculatesStableTailQuantiles)
{
    constexpr std::array samples{ 1.0, 2.0, 3.0, 4.0, 100.0 };
    const OMW::DiagnosticDistribution result = OMW::summarizeDiagnosticSamples(samples);

    EXPECT_EQ(result.mCount, samples.size());
    EXPECT_DOUBLE_EQ(result.mMedian, 3.0);
    EXPECT_DOUBLE_EQ(result.mP95, 80.8);
    EXPECT_DOUBLE_EQ(result.mP99, 96.16);
    EXPECT_DOUBLE_EQ(result.mWorst, 100.0);
}

TEST(DiagnosticPerformanceTest, RejectsMissingAndNonFiniteSamples)
{
    EXPECT_THROW(OMW::summarizeDiagnosticSamples({}), std::invalid_argument);
    constexpr std::array samples{ 1.0, std::numeric_limits<double>::infinity() };
    EXPECT_THROW(OMW::summarizeDiagnosticSamples(samples), std::invalid_argument);
}
