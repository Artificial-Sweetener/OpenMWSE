#include <apps/openmw/mwrender/waterscreencoverage.hpp>

#include <gtest/gtest.h>

namespace MWRender
{
    namespace
    {
        /// Publish complete query observations monotonically when GPU results retire out of order.
        TEST(WaterScreenCoverageTest, OlderFramesCannotReplaceTheLatestResult)
        {
            osg::ref_ptr<WaterScreenCoverage> coverage = new WaterScreenCoverage;
            EXPECT_FALSE(coverage->recentResult(12));

            coverage->publish(12, 4500);
            ASSERT_TRUE(coverage->recentResult(12));
            EXPECT_EQ(coverage->recentResult(12)->mFrame, 12u);
            EXPECT_EQ(coverage->recentResult(12)->mSamples, 4500u);

            coverage->publish(10, 9000);
            ASSERT_TRUE(coverage->recentResult(12));
            EXPECT_EQ(coverage->recentResult(12)->mSamples, 4500u);

            coverage->publish(13, 3200);
            ASSERT_TRUE(coverage->recentResult(13));
            EXPECT_EQ(coverage->recentResult(13)->mSamples, 3200u);
        }

        /// Reject a result after its drawable has stopped producing representative main-view queries.
        TEST(WaterScreenCoverageTest, StaleResultsCannotRemainDominant)
        {
            osg::ref_ptr<WaterScreenCoverage> coverage = new WaterScreenCoverage;
            coverage->publish(20, 9000);

            EXPECT_TRUE(coverage->recentResult(32));
            EXPECT_FALSE(coverage->recentResult(33));
        }
    }
}
