#include <apps/openmw/diagnosticworkcounter.hpp>
#include <apps/openmw/mwrender/renderworkcounter.hpp>

#include <gtest/gtest.h>

#include <osg/Stats>
#include <osg/ref_ptr>

#include <set>
#include <string>

TEST(DiagnosticWorkCounterTest, DefinesBoundedUniqueStableNames)
{
    const std::span counters = OMW::diagnosticWorkCounters();
    ASSERT_FALSE(counters.empty());
    ASSERT_LE(counters.size(), 64);

    std::set<std::string> names;
    for (const OMW::DiagnosticWorkCounter& counter : counters)
    {
        EXPECT_FALSE(counter.mName.empty());
        EXPECT_FALSE(counter.mStatistic.empty());
        EXPECT_TRUE(names.emplace(counter.mName).second) << counter.mName;
    }
}

TEST(DiagnosticWorkCounterTest, CollectsPostProcessingWorkOnlyWhenRequested)
{
    osg::ref_ptr<osg::Stats> stats = new osg::Stats("test");
    constexpr unsigned frameNumber = 42;

    MWRender::resetPostProcessingWorkCounters(*stats, frameNumber);
    double value = -1.0;
    EXPECT_FALSE(stats->getAttribute(frameNumber, std::string(MWRender::colorCopyCounter), value));

    stats->collectStats("scene", true);
    MWRender::resetPostProcessingWorkCounters(*stats, frameNumber);
    ASSERT_TRUE(stats->getAttribute(frameNumber, std::string(MWRender::colorCopyCounter), value));
    EXPECT_DOUBLE_EQ(value, 0.0);

    MWRender::addPostProcessingWorkCounter(*stats, frameNumber, MWRender::colorCopyCounter, 2.0);
    ASSERT_TRUE(stats->getAttribute(frameNumber, std::string(MWRender::colorCopyCounter), value));
    EXPECT_DOUBLE_EQ(value, 2.0);
}
