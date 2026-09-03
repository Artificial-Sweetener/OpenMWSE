#include <apps/openmw/diagnosticcollection.hpp>

#include <gtest/gtest.h>

#include <osg/Group>
#include <osg/Stats>
#include <osgViewer/Viewer>

#include <components/sceneutil/diagnosticgputiming.hpp>
#include <components/sdlutil/diagnosticpresentationtiming.hpp>

#include <limits>
#include <optional>
#include <stdexcept>

TEST(DiagnosticCollectionTest, EnablesOnlyOneBoundedFutureIntervalAndRestoresPriorCategories)
{
    osgViewer::Viewer viewer;
    viewer.setViewerStats(new osg::Stats("viewer"));
    viewer.setSceneData(new osg::Group);
    viewer.getViewerStats()->collectStats("frame_rate", true);

    OMW::DiagnosticCollection collection(viewer);
    EXPECT_FALSE(collection.active());
    EXPECT_FALSE(SDLUtil::diagnosticPresentationTimingCollectionEnabled());
    EXPECT_FALSE(viewer.getViewerStats()->collectStats("scene"));
    EXPECT_FALSE(viewer.getCamera()->getStats()->collectStats("rendering"));

    EXPECT_TRUE(collection.start(17, 4, 100));
    EXPECT_TRUE(collection.active());
    EXPECT_TRUE(SDLUtil::diagnosticPresentationTimingCollectionEnabled());
    SDLUtil::recordDiagnosticSwapBufferTime(100, 0.004);
    double swapDuration = 0.0;
    EXPECT_TRUE(viewer.getViewerStats()->getAttribute(
        100, std::string(SDLUtil::diagnosticSwapBufferTimeStatistic), swapDuration));
    EXPECT_DOUBLE_EQ(swapDuration, 0.004);
    EXPECT_FALSE(viewer.getViewerStats()->collectStats("scene"));
    EXPECT_TRUE(viewer.getCamera()->getStats()->collectStats("rendering"));
    EXPECT_TRUE(viewer.getCamera()->getStats()->collectStats("scene"));
    EXPECT_FALSE(collection.start(18, 1, 100));
    EXPECT_FALSE(collection.update(110).has_value());
    EXPECT_FALSE(collection.update(111).has_value());
    EXPECT_FALSE(collection.update(112).has_value());

    const std::optional<OMW::CompletedDiagnosticCollection> completed = collection.update(113);
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->mRequestId, 17);
    EXPECT_EQ(completed->mFirstFrame, 100);
    EXPECT_EQ(completed->mLastFrame, 103);
    EXPECT_FALSE(collection.active());
    EXPECT_FALSE(SDLUtil::diagnosticPresentationTimingCollectionEnabled());
    EXPECT_FALSE(viewer.getViewerStats()->collectStats("scene"));
    EXPECT_FALSE(viewer.getCamera()->getStats()->collectStats("rendering"));
    EXPECT_TRUE(viewer.getViewerStats()->collectStats("frame_rate"));
}

TEST(DiagnosticCollectionTest, RejectsInvalidAndOverflowingIntervals)
{
    osgViewer::Viewer viewer;
    viewer.setViewerStats(new osg::Stats("viewer"));
    OMW::DiagnosticCollection collection(viewer);

    EXPECT_THROW(collection.start(1, 0, 10), std::invalid_argument);
    EXPECT_THROW(collection.start(1, 601, 10), std::invalid_argument);
    EXPECT_THROW(collection.start(1, 2, std::numeric_limits<unsigned>::max()), std::overflow_error);
}

TEST(DiagnosticCollectionTest, WaitsForPendingQueriesWithoutExtendingTheMeasuredInterval)
{
    osgViewer::Viewer viewer;
    viewer.setViewerStats(new osg::Stats("viewer"));
    OMW::DiagnosticCollection collection(viewer);
    ASSERT_TRUE(collection.start(19, 4, 100));
    osg::Stats& stats = *viewer.getCamera()->getStats();
    const std::string statistic(SceneUtil::diagnosticGpuQueryStateStatistic);
    stats.setAttribute(100, statistic, static_cast<int>(SceneUtil::DiagnosticGpuQueryState::Pending));
    EXPECT_FALSE(collection.update(113).has_value());
    EXPECT_TRUE(collection.active());
    stats.setAttribute(100, statistic, static_cast<int>(SceneUtil::DiagnosticGpuQueryState::Complete));
    stats.setAttribute(104, statistic, static_cast<int>(SceneUtil::DiagnosticGpuQueryState::Pending));
    const auto completed = collection.update(114);
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->mFirstFrame, 100);
    EXPECT_EQ(completed->mLastFrame, 103);
    EXPECT_FALSE(collection.active());
}

TEST(DiagnosticCollectionTest, ReportsUnretiredQueriesBeforeTheirSampleHistoryExpires)
{
    osgViewer::Viewer viewer;
    viewer.setViewerStats(new osg::Stats("viewer"));
    OMW::DiagnosticCollection collection(viewer);
    ASSERT_TRUE(collection.start(20, 600, 100));
    osg::Stats& stats = *viewer.getCamera()->getStats();
    const std::string statistic(SceneUtil::diagnosticGpuQueryStateStatistic);
    stats.setAttribute(100, statistic, static_cast<int>(SceneUtil::DiagnosticGpuQueryState::Pending));
    EXPECT_FALSE(collection.update(709).has_value());
    EXPECT_FALSE(collection.update(1121).has_value());
    const auto completed = collection.update(1122);
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->mFirstFrame, 100);
    EXPECT_EQ(completed->mLastFrame, 699);
    double queryState = 0;
    ASSERT_TRUE(stats.getAttribute(100, statistic, queryState));
    EXPECT_EQ(queryState, static_cast<int>(SceneUtil::DiagnosticGpuQueryState::Pending));
    EXPECT_FALSE(collection.active());
}

TEST(DiagnosticCollectionTest, DoesNotWaitForUnavailableOrNonRenderingCameras)
{
    osgViewer::Viewer viewer;
    viewer.setViewerStats(new osg::Stats("viewer"));
    OMW::DiagnosticCollection collection(viewer);
    ASSERT_TRUE(collection.start(21, 4, 100));
    osg::Stats& stats = *viewer.getCamera()->getStats();
    const std::string statistic(SceneUtil::diagnosticGpuQueryStateStatistic);
    stats.setAttribute(100, statistic, static_cast<int>(SceneUtil::DiagnosticGpuQueryState::Unsupported));
    stats.setAttribute(101, statistic, static_cast<int>(SceneUtil::DiagnosticGpuQueryState::Invalid));
    stats.setAttribute(102, statistic, static_cast<int>(SceneUtil::DiagnosticGpuQueryState::Dropped));
    EXPECT_TRUE(collection.update(113).has_value());
}
