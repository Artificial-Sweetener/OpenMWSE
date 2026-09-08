#include <apps/openmw/diagnosticcollection.hpp>

#include <gtest/gtest.h>

#include <osg/FrameStamp>
#include <osg/Group>
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/Stats>
#include <osgUtil/RenderBin>
#include <osgViewer/Viewer>

#include <components/sceneutil/diagnosticgputiming.hpp>
#include <components/sceneutil/rtt.hpp>
#include <components/sdlutil/diagnosticpresentationtiming.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>

namespace
{
    /// Create a reflection-style camera lazily outside the traversable scene graph.
    class DiagnosticReflectionTarget final : public SceneUtil::RTTNode
    {
    public:
        /// Use a single small target without requiring a rendering context.
        DiagnosticReflectionTarget()
            : RTTNode(16, 16, 0, false, 0, StereoAwareness::Unaware, false)
        {
        }

        /// Identify the lazily created camera as a water-reflection pass.
        void setDefaults(osg::Camera* camera) override
        {
            camera->setName(std::string(SceneUtil::reflectionCameraName));
        }
    };

    /// Count delegated draws without requiring a graphics context for CPU instrumentation coverage.
    struct CountDiagnosticDraw final : osgUtil::RenderBin::DrawCallback
    {
        unsigned mDraws = 0;

        /// Record each draw forwarded by the diagnostic wrapper.
        void drawImplementation(osgUtil::RenderBin*, osg::RenderInfo&, osgUtil::RenderLeaf*&) override { ++mDraws; }
    };
}

/// Include cameras created during culling in the current and subsequent bounded measurement windows.
TEST(DiagnosticCollectionTest, CollectsLazyRenderTargetsAndStopsAtTheBoundary)
{
    osgViewer::Viewer viewer;
    viewer.setViewerStats(new osg::Stats("viewer"));
    osg::ref_ptr<DiagnosticReflectionTarget> target = new DiagnosticReflectionTarget;
    viewer.setSceneData(target);
    OMW::DiagnosticCollection collection(viewer);
    ASSERT_TRUE(collection.start(22, 1, 100));
    osg::Camera* camera = target->getCamera(nullptr);
    ASSERT_NE(camera, nullptr);
    EXPECT_TRUE(SceneUtil::diagnosticGpuPassStatisticsCollectionEnabled(*camera));
    const auto cameras = SceneUtil::diagnosticGpuPassCameras();
    EXPECT_EQ(std::count_if(cameras.begin(), cameras.end(),
                  [camera](const auto& registered) { return registered.get() == camera; }),
        1);
    EXPECT_EQ(target->getCamera(nullptr), camera);
    ASSERT_NE(camera->getInitialDrawCallback(), nullptr);
    ASSERT_NE(camera->getFinalDrawCallback(), nullptr);
    ASSERT_TRUE(collection.update(110).has_value());
    EXPECT_FALSE(SceneUtil::diagnosticGpuPassStatisticsCollectionEnabled(*camera));
    ASSERT_TRUE(collection.start(23, 1, 120));
    EXPECT_TRUE(SceneUtil::diagnosticGpuPassStatisticsCollectionEnabled(*camera));
    ASSERT_TRUE(collection.update(130).has_value());
    EXPECT_FALSE(SceneUtil::diagnosticGpuPassStatisticsCollectionEnabled(*camera));
}

/// Publish cumulative callback overhead independently of delegated work, including without GPU timer support.
TEST(DiagnosticCollectionTest, SeparatesRenderBinInstrumentationAndStopsCollectingAtTheBoundary)
{
    osgViewer::Viewer viewer;
    viewer.setViewerStats(new osg::Stats("viewer"));
    osg::ref_ptr<osg::State> state = new osg::State;
    state->setFrameStamp(new osg::FrameStamp);
    state->getFrameStamp()->setFrameNumber(100);
    osg::RenderInfo renderInfo;
    renderInfo.setState(state);
    renderInfo.pushCamera(viewer.getCamera());
    osg::ref_ptr<osgUtil::RenderBin> bin = new osgUtil::RenderBin;
    osg::ref_ptr<CountDiagnosticDraw> delegate = new CountDiagnosticDraw;
    bin->setDrawCallback(delegate);
    const std::string cpuStatistic = "test CPU bin";
    const std::string overheadStatistic = cpuStatistic + " callback overhead";
    SceneUtil::enableDiagnosticGpuRenderBinStatistics(*bin, bin->getBinNum(), "test GPU bin", cpuStatistic);
    osgUtil::RenderLeaf* previous = nullptr;
    double firstOverhead = 0.0;
    double cumulativeOverhead = 0.0;
    double cpuSeconds = 0.0;
    {
        OMW::DiagnosticCollection collection(viewer);
        ASSERT_TRUE(collection.start(1, 1, 100));
        bin->draw(renderInfo, previous);
        ASSERT_TRUE(viewer.getCamera()->getStats()->getAttribute(100, overheadStatistic, firstOverhead));
        EXPECT_GE(firstOverhead, 0.0);
        bin->draw(renderInfo, previous);
        EXPECT_EQ(delegate->mDraws, 2u);
        ASSERT_TRUE(viewer.getCamera()->getStats()->getAttribute(100, overheadStatistic, cumulativeOverhead));
        EXPECT_GE(cumulativeOverhead, firstOverhead);
        EXPECT_TRUE(viewer.getCamera()->getStats()->getAttribute(100, cpuStatistic, cpuSeconds));
        EXPECT_GE(cpuSeconds, 0.0);
        EXPECT_FALSE(viewer.getCamera()->getStats()->getAttribute(100, "test GPU bin", cpuSeconds));
    }
    state->getFrameStamp()->setFrameNumber(101);
    bin->draw(renderInfo, previous);
    EXPECT_EQ(delegate->mDraws, 3u);
    EXPECT_FALSE(viewer.getCamera()->getStats()->getAttribute(101, overheadStatistic, cumulativeOverhead));
    EXPECT_FALSE(viewer.getCamera()->getStats()->getAttribute(101, cpuStatistic, cpuSeconds));
}

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
