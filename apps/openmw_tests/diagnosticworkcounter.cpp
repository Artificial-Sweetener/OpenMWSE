#include <apps/openmw/diagnosticworkcounter.hpp>
#include <apps/openmw/mwrender/renderworkcounter.hpp>

#include <components/sceneutil/deformationstats.hpp>

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

TEST(DiagnosticWorkCounterTest, CollectsRenderWorkOnlyWhenRequested)
{
    osg::ref_ptr<osg::Stats> stats = new osg::Stats("test");
    constexpr unsigned frameNumber = 42;

    MWRender::resetDiagnosticRenderWorkCounters(*stats, frameNumber);
    double value = -1.0;
    EXPECT_FALSE(stats->getAttribute(frameNumber, std::string(MWRender::colorCopyCounter), value));

    stats->collectStats("scene", true);
    MWRender::resetDiagnosticRenderWorkCounters(*stats, frameNumber);
    ASSERT_TRUE(stats->getAttribute(frameNumber, std::string(MWRender::colorCopyCounter), value));
    EXPECT_DOUBLE_EQ(value, 0.0);

    MWRender::addDiagnosticRenderWorkCounter(*stats, frameNumber, MWRender::colorCopyCounter, 2.0);
    ASSERT_TRUE(stats->getAttribute(frameNumber, std::string(MWRender::colorCopyCounter), value));
    EXPECT_DOUBLE_EQ(value, 2.0);

    ASSERT_TRUE(stats->getAttribute(
        frameNumber, std::string(MWRender::transparentClassifiedLeafCounter), value));
    EXPECT_DOUBLE_EQ(value, 0.0);

    MWRender::addDiagnosticRenderWorkCounter(
        *stats, frameNumber, MWRender::groundcoverInstanceTestCounter, 12.0);
    ASSERT_TRUE(stats->getAttribute(
        frameNumber, std::string(MWRender::groundcoverInstanceTestCounter), value));
    EXPECT_DOUBLE_EQ(value, 12.0);

    ASSERT_TRUE(
        stats->getAttribute(frameNumber, std::string(MWRender::groundcoverConservativeInstanceRenderCounter), value));
    EXPECT_DOUBLE_EQ(value, 0.0);
}

TEST(DiagnosticWorkCounterTest, ResetsDeformationWorkOnlyWhenRequested)
{
    osg::ref_ptr<osg::Stats> stats = new osg::Stats("test");
    constexpr unsigned frameNumber = 43;

    SceneUtil::resetDiagnosticDeformationCounters(*stats, frameNumber);
    double value = -1.0;
    EXPECT_FALSE(stats->getAttribute(frameNumber, std::string(SceneUtil::skinningVertexCounter), value));

    stats->collectStats("scene", true);
    SceneUtil::resetDiagnosticDeformationCounters(*stats, frameNumber);
    ASSERT_TRUE(stats->getAttribute(frameNumber, std::string(SceneUtil::skinningVertexCounter), value));
    EXPECT_DOUBLE_EQ(value, 0.0);
    ASSERT_TRUE(stats->getAttribute(frameNumber, std::string(SceneUtil::deformationCpuMillisecondCounter), value));
    EXPECT_DOUBLE_EQ(value, 0.0);
}
