#include <apps/openmw/diagnosticcollection.hpp>
#include <apps/openmw/diagnosticperformance.hpp>
#include <apps/openmw/diagnosticrenderingtiming.hpp>

#include <apps/openmw/mwrender/renderworkcounter.hpp>

#include <components/sceneutil/diagnosticcamerawork.hpp>
#include <components/sceneutil/diagnosticgputiming.hpp>
#include <components/sceneutil/diagnosticshadowwork.hpp>
#include <components/sdlutil/diagnosticpresentationtiming.hpp>
#include <components/terrain/diagnosticcompositemapwork.hpp>
#include <components/terrain/diagnosticviewselection.hpp>

#include <gtest/gtest.h>

#include <osg/Camera>
#include <osg/Stats>
#include <osgViewer/Viewer>

#include <array>
#include <limits>
#include <sstream>
#include <string>

/// Preserve explicit route-to-renderer identities without excluding nested loading draws from their intervals.
TEST(DiagnosticPerformanceTest, ReportsRouteFramesWithNestedRendererIntervals)
{
    const std::array<OMW::DiagnosticRouteFrame, 3> frames{ { { 100, 100 }, { 101, 105 }, { 106, 106 } } };
    std::ostringstream output;
    OMW::reportDiagnosticRouteFrames(frames, output);
    EXPECT_EQ(output.str(),
        "route-sampled-frames: 3\nroute-frame-map:\n"
        "  - route-ordinal: 0\n    first-render-frame: 100\n    scene-render-frame: 100\n"
        "  - route-ordinal: 1\n    first-render-frame: 101\n    scene-render-frame: 105\n"
        "  - route-ordinal: 2\n    first-render-frame: 106\n    scene-render-frame: 106\n");
    std::ostringstream ordinary;
    OMW::reportDiagnosticRouteFrames({}, ordinary);
    EXPECT_TRUE(ordinary.str().empty());
}

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

/// Attribute reflected sky to its dependent cameras without borrowing primary-camera timing or unrelated passes.
TEST(DiagnosticPerformanceTest, ReportsReflectedSkyFromItsOwningCameras)
{
    constexpr unsigned frame = 42;
    osgViewer::Viewer viewer;
    viewer.setViewerStats(new osg::Stats("viewer"));
    SceneUtil::enableDiagnosticGpuPassStatistics(*viewer.getCamera());
    const std::string gpuStatistic = "OpenMW diagnostic GPU render bin reflected sky";
    const std::string cpuStatistic = "OpenMW diagnostic CPU render bin reflected sky";
    viewer.getCamera()->getStats()->setAttribute(frame, gpuStatistic, .9);
    viewer.getCamera()->getStats()->setAttribute(frame, cpuStatistic, .8);
    viewer.getCamera()->getStats()->setAttribute(frame, "OpenMW diagnostic GPU render bin scene root", .012);

    std::array<osg::ref_ptr<osg::Camera>, 3> cameras;
    for (std::size_t index = 0; index < cameras.size(); ++index)
    {
        cameras[index] = new osg::Camera;
        cameras[index]->setName(index < 2 ? std::string(SceneUtil::reflectionCameraName) : "UnrelatedCamera");
        SceneUtil::enableDiagnosticGpuPassStatistics(*cameras[index]);
        cameras[index]->getStats()->setAttribute(frame, gpuStatistic, index == 0 ? .003 : .002);
        cameras[index]->getStats()->setAttribute(frame, cpuStatistic, index == 0 ? .0002 : .0003);
    }

    std::ostringstream output;
    OMW::reportDiagnosticPerformance(frame, frame, viewer, output);
    const std::string report = output.str();
    const auto gpuStart = report.find("gpu-render-bins:\n");
    const auto cpuStart = report.find("cpu-render-bins:\n");
    ASSERT_NE(gpuStart, std::string::npos);
    ASSERT_NE(cpuStart, std::string::npos);
    const std::string gpu = report.substr(gpuStart, cpuStart - gpuStart);
    const std::string cpu = report.substr(cpuStart);
    EXPECT_NE(gpu.find("  reflected-sky:\n    available: true\n    count: 1\n    median: 0.005"), std::string::npos)
        << gpu;
    EXPECT_NE(cpu.find("  reflected-sky:\n    available: true\n    count: 1\n    median: 0.0005"), std::string::npos)
        << cpu;
    EXPECT_NE(gpu.find("  scene-root:\n    available: true\n    count: 1\n    median: 0.012"), std::string::npos)
        << gpu;
}

/// Preserve missing reflected-sky measurements even when a primary-camera statistic has the same name.
TEST(DiagnosticPerformanceTest, ReportsMissingReflectedSkyWithoutPrimaryCameraFallback)
{
    constexpr unsigned frame = 42;
    osgViewer::Viewer viewer;
    viewer.setViewerStats(new osg::Stats("viewer"));
    SceneUtil::enableDiagnosticGpuPassStatistics(*viewer.getCamera());
    viewer.getCamera()->getStats()->setAttribute(frame, "OpenMW diagnostic GPU render bin reflected sky", .9);
    viewer.getCamera()->getStats()->setAttribute(frame, "OpenMW diagnostic CPU render bin reflected sky", .8);
    std::ostringstream output;
    OMW::reportDiagnosticPerformance(frame, frame, viewer, output);
    const std::string report = output.str();
    const auto gpuStart = report.find("gpu-render-bins:\n");
    const auto cpuStart = report.find("cpu-render-bins:\n");
    ASSERT_NE(gpuStart, std::string::npos);
    ASSERT_NE(cpuStart, std::string::npos);
    EXPECT_NE(
        report.substr(gpuStart, cpuStart - gpuStart).find("  reflected-sky:\n    available: false"), std::string::npos)
        << report;
    EXPECT_NE(report.substr(cpuStart).find("  reflected-sky:\n    available: false"), std::string::npos) << report;
}

TEST(DiagnosticPerformanceTest, RejectsMissingAndNonFiniteSamples)
{
    EXPECT_THROW(OMW::summarizeDiagnosticSamples({}), std::invalid_argument);
    constexpr std::array samples{ 1.0, std::numeric_limits<double>::infinity() };
    EXPECT_THROW(OMW::summarizeDiagnosticSamples(samples), std::invalid_argument);
}

TEST(DiagnosticPerformanceTest, TerrainSelectionWorkFollowsDiagnosticCollectionLifetime)
{
    osg::ref_ptr<osg::Camera> camera = new osg::Camera;
    SceneUtil::enableDiagnosticGpuPassStatistics(*camera);
    EXPECT_FALSE(Terrain::diagnosticViewSelectionCollectionEnabled(*camera));

    SceneUtil::setDiagnosticGpuPassStatisticsCollection(true);
    EXPECT_TRUE(Terrain::diagnosticViewSelectionCollectionEnabled(*camera));
    SceneUtil::setDiagnosticGpuPassStatisticsCollection(false);

    EXPECT_FALSE(Terrain::diagnosticViewSelectionCollectionEnabled(*camera));
}

/// Keep absent, unfinished, unsupported, and invalid query results distinct from completed GPU measurements.
TEST(DiagnosticPerformanceTest, ReportsQueryLifecycleWithoutInventingDurations)
{
    osgViewer::Viewer viewer;
    osg::Camera* camera = viewer.getCamera();
    camera->setName("QueryLifecycleCamera");
    SceneUtil::enableDiagnosticGpuPassStatistics(*camera);
    constexpr std::array states{ SceneUtil::DiagnosticGpuQueryState::Unsupported,
        SceneUtil::DiagnosticGpuQueryState::Pending, SceneUtil::DiagnosticGpuQueryState::Complete,
        SceneUtil::DiagnosticGpuQueryState::Invalid, SceneUtil::DiagnosticGpuQueryState::Dropped };
    for (unsigned index = 0; index < states.size(); ++index)
        camera->getStats()->setAttribute(
            100 + index, std::string(SceneUtil::diagnosticGpuQueryStateStatistic), static_cast<int>(states[index]));
    camera->getStats()->setAttribute(102, std::string(SceneUtil::diagnosticGpuPassTimeStatistic), 0.003);
    camera->getStats()->setAttribute(
        106, std::string(SceneUtil::diagnosticGpuQueryStateStatistic), std::numeric_limits<double>::quiet_NaN());

    std::ostringstream output;
    OMW::reportDiagnosticPerformance(100, 106, viewer, output);
    const std::string report = output.str();
    EXPECT_NE(report.find("  - camera: \"QueryLifecycleCamera\"\n    unsupported: 1\n    pending: 1\n"
                          "    complete: 1\n    invalid: 2\n    dropped: 1\n    not-recorded: 1\n"),
        std::string::npos)
        << report;
    EXPECT_NE(
        report.find("  unclassified-cameras:\n    available: true\n    count: 1\n    median: 0.003"), std::string::npos)
        << report;
}

TEST(DiagnosticPerformanceTest, ReportsNamedGpuPassesAndAggregatesMatchingCameras)
{
    constexpr unsigned frame = 42;
    osgViewer::Viewer viewer;
    viewer.setViewerStats(new osg::Stats("viewer"));
    viewer.getViewerStats()->setAttribute(frame, std::string(SDLUtil::diagnosticSwapBufferTimeStatistic), 0.005);
    viewer.getCamera()->setName(std::string(SceneUtil::sceneCameraName));
    SceneUtil::enableDiagnosticGpuPassStatistics(*viewer.getCamera());
    viewer.getCamera()->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticGpuPassTimeStatistic), 0.010);
    viewer.getCamera()->getStats()->setAttribute(
        frame, std::string(SceneUtil::diagnosticGpuPassBeginTimestampStatistic), 100.020);
    viewer.getCamera()->getStats()->setAttribute(
        frame, std::string(SceneUtil::diagnosticGpuPassEndTimestampStatistic), 100.030);
    viewer.getCamera()->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassTimeStatistic), 0.008);
    viewer.getCamera()->getStats()->setAttribute(
        frame, std::string(SceneUtil::diagnosticCpuPassThreadCycleStatistic), 1000.0);
    viewer.getCamera()->getStats()->setAttribute(
        frame, std::string(SceneUtil::diagnosticCpuPassBeginStatistic), 10.000);
    viewer.getCamera()->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassEndStatistic), 10.008);
    viewer.getCamera()->getStats()->setAttribute(frame, "Draw traversal begin time", 9.995);
    viewer.getCamera()->getStats()->setAttribute(frame, "Draw traversal end time", 10.012);
    viewer.getCamera()->getStats()->setAttribute(
        frame, std::string(OMW::diagnosticRenderBinPasses()[1].mGpuStatistic), 0.007);
    viewer.getCamera()->getStats()->setAttribute(
        frame, std::string(OMW::diagnosticRenderBinPasses()[1].mCpuStatistic), 0.006);
    viewer.getCamera()->getStats()->setAttribute(
        frame, std::string(MWRender::groundcoverInstanceRenderCounter), 1200.0);
    viewer.getCamera()->getStats()->setAttribute(frame, std::string(MWRender::groundcoverInstanceTestCounter), 0.0);
    viewer.getCamera()->getStats()->setAttribute(frame, std::string(MWRender::groundcoverBatchRenderCounter), 12.0);
    viewer.getCamera()->getStats()->setAttribute(frame, std::string(MWRender::groundcoverDrawableRenderCounter), 24.0);
    viewer.getCamera()->getStats()->setAttribute(
        frame, std::string(MWRender::groundcoverConservativeInstanceTestCounter), 1000.0);
    viewer.getCamera()->getStats()->setAttribute(
        frame, std::string(MWRender::groundcoverConservativeInstanceRenderCounter), 600.0);

    osg::ref_ptr<osg::Camera> firstShadow = new osg::Camera;
    firstShadow->setName(std::string(SceneUtil::shadowCameraName));
    SceneUtil::setDiagnosticGpuPassInstance(*firstShadow, 0);
    SceneUtil::enableDiagnosticGpuPassStatistics(*firstShadow);
    firstShadow->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticGpuPassTimeStatistic), 0.002);
    firstShadow->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassTimeStatistic), 0.0015);
    firstShadow->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassBeginStatistic), 9.996);
    firstShadow->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassEndStatistic), 9.998);
    firstShadow->getStats()->setAttribute(
        frame, std::string(SceneUtil::diagnosticCpuPassBeginOverheadStatistic), 0.0001);
    firstShadow->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassEndOverheadStatistic), 0.0002);
    firstShadow->getStats()->setAttribute(
        frame, std::string(SceneUtil::diagnosticShadowRenderLeafCountStatistic), 200.0);
    firstShadow->getStats()->setAttribute(
        frame, std::string(SceneUtil::diagnosticShadowStateGraphCountStatistic), 40.0);
    SceneUtil::setDiagnosticGpuPassStatisticsCollection(true);
    SceneUtil::setDiagnosticShadowLightState(
        *firstShadow, osg::Vec3d(0.0, 0.8, 0.6), osg::Vec4f(0.5f, 0.4f, 0.3f, 1.f));
    SceneUtil::setDiagnosticShadowCasterCullState(*firstShadow, -4000.0, -500.0);
    SceneUtil::setDiagnosticShadowWorkFrame(*firstShadow, frame);
    Terrain::recordDiagnosticViewSelection(*firstShadow, frame,
        { .mSelectedNodes = 192,
            .mActiveGridNodes = 81,
            .mPendingRenderingNodes = 2,
            .mWorldRevision = 49,
            .mSelectionUpdated = true,
            .mSelectionChanged = false });
    SceneUtil::setDiagnosticGpuPassStatisticsCollection(false);

    osg::ref_ptr<osg::Camera> secondShadow = new osg::Camera;
    secondShadow->setName(std::string(SceneUtil::shadowCameraName));
    SceneUtil::setDiagnosticGpuPassInstance(*secondShadow, 1);
    SceneUtil::enableDiagnosticGpuPassStatistics(*secondShadow);
    secondShadow->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticGpuPassTimeStatistic), 0.003);
    secondShadow->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassBeginStatistic), 9.998);
    secondShadow->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassEndStatistic), 9.999);
    secondShadow->getStats()->setAttribute(frame, std::string(MWRender::groundcoverInstanceRenderCounter), 300.0);

    osg::ref_ptr<osg::Camera> alternateFirstShadow = new osg::Camera;
    alternateFirstShadow->setName(std::string(SceneUtil::shadowCameraName));
    SceneUtil::setDiagnosticGpuPassInstance(*alternateFirstShadow, 0);
    SceneUtil::enableDiagnosticGpuPassStatistics(*alternateFirstShadow);
    alternateFirstShadow->getStats()->setAttribute(
        frame, std::string(SceneUtil::diagnosticGpuPassTimeStatistic), 0.004);
    alternateFirstShadow->getStats()->setAttribute(
        frame, std::string(SceneUtil::diagnosticCpuPassTimeStatistic), 0.002);

    osg::ref_ptr<osg::Camera> auxiliary = new osg::Camera;
    auxiliary->setName("AuxiliarySceneCamera");
    SceneUtil::enableDiagnosticGpuPassStatistics(*auxiliary);
    auxiliary->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticGpuPassTimeStatistic), 0.003);
    auxiliary->getStats()->setAttribute(
        frame, std::string(SceneUtil::diagnosticGpuPassBeginTimestampStatistic), 100.031);
    auxiliary->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticGpuPassEndTimestampStatistic), 100.034);
    auxiliary->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassTimeStatistic), 0.004);

    osg::ref_ptr<osg::Camera> terrainComposite = new osg::Camera;
    terrainComposite->setName(std::string(SceneUtil::terrainCompositeCameraName));
    SceneUtil::enableDiagnosticGpuPassStatistics(*terrainComposite);
    terrainComposite->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticGpuPassTimeStatistic), 0.006);
    terrainComposite->getStats()->setAttribute(
        frame, std::string(SceneUtil::diagnosticGpuPassBeginTimestampStatistic), 100.000);
    terrainComposite->getStats()->setAttribute(
        frame, std::string(SceneUtil::diagnosticGpuPassEndTimestampStatistic), 100.006);
    terrainComposite->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassTimeStatistic), 0.010);
    terrainComposite->getStats()->setAttribute(frame, std::string(Terrain::diagnosticCompositeMapBudgetStatistic), 2.5);
    terrainComposite->getStats()->setAttribute(
        frame, std::string(Terrain::diagnosticCompositeMapQueuedBeforeStatistic), 12.0);
    terrainComposite->getStats()->setAttribute(
        frame, std::string(Terrain::diagnosticCompositeMapImmediateBeforeStatistic), 0.0);
    terrainComposite->getStats()->setAttribute(
        frame, std::string(Terrain::diagnosticCompositeMapProcessedStatistic), 1.0);
    terrainComposite->getStats()->setAttribute(
        frame, std::string(Terrain::diagnosticCompositeMapSubmittedDrawablesStatistic), 24.0);
    terrainComposite->getStats()->setAttribute(
        frame, std::string(Terrain::diagnosticCompositeMapCompletedStatistic), 1.0);
    terrainComposite->getStats()->setAttribute(
        frame, std::string(Terrain::diagnosticCompositeMapQueuedAfterStatistic), 11.0);
    terrainComposite->getStats()->setAttribute(
        frame, std::string(Terrain::diagnosticCompositeMapDrawableCpuStatistic), 0.15);

    std::ostringstream output;
    OMW::reportDiagnosticPerformance(frame, frame, viewer, output);
    const std::string report = output.str();

    EXPECT_NE(report.find("provider: performance@17"), std::string::npos);
    EXPECT_NE(report.find("threading-model: automatic-selection"), std::string::npos);
    EXPECT_NE(report.find("presentation:\n  swap-buffers:\n    available: true\n    count: 1"), std::string::npos)
        << report;
    EXPECT_NE(report.find("draw-breakdown:\n  camera-covered:\n    available: true\n    count: 1\n"
                          "    median: 0.010999"),
        std::string::npos)
        << report;
    EXPECT_NE(
        report.find("  outside-cameras:\n    available: true\n    count: 1\n    median: 0.006000"), std::string::npos)
        << report;
    EXPECT_NE(report.find("camera-transition-gaps:\n  - from: \"<draw-begin>\"\n"
                          "    to: \"ShadowCamera\"\n    gap-time:\n      available: true\n      count: 1"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("  - from: \"SceneCam\"\n    to: \"<draw-end>\"\n"
                          "    gap-time:\n      available: true\n      count: 1"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("  - from: \"ShadowCamera\"\n    to: \"SceneCam\"\n"
                          "    gap-time:\n      available: true\n      count: 1"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("diagnostic-camera-callback-overhead:\n  before-interval:\n"
                          "    available: true\n    count: 1"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("  after-interval:\n    available: true\n    count: 1"), std::string::npos) << report;
    EXPECT_NE(report.find("gpu-timeline:\n  camera-span:\n    available: true\n    count: 1"), std::string::npos)
        << report;
    EXPECT_NE(report.find("  between-cameras:\n    available: true\n    count: 1"), std::string::npos) << report;
    EXPECT_NE(report.find("    - from: \"Terrain Composite Map Camera\"\n      to: \"SceneCam\""), std::string::npos)
        << report;
    EXPECT_NE(report.find("  main-scene:\n    available: true\n    count: 1\n    median: 0.01"), std::string::npos);
    EXPECT_NE(report.find("  shadow-maps:\n    available: true\n    count: 1\n    median: 0.009"), std::string::npos);
    EXPECT_NE(report.find("  water-reflection:\n    available: false"), std::string::npos);
    EXPECT_NE(report.find("  shared-planar-reflection-source:\n    available: false"), std::string::npos);
    EXPECT_NE(
        report.find("  unclassified-cameras:\n    available: true\n    count: 1\n    median: 0.003"), std::string::npos)
        << report;
    EXPECT_NE(report.find("cpu-passes:\n  main-scene:\n    available: true\n    count: 1"), std::string::npos);
    EXPECT_NE(report.find("render-thread-cycles:\n  main-scene:\n    available: true\n    count: 1\n"
                          "    median: 1000"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("  terrain-composite:\n    available: true\n    count: 1"), std::string::npos) << report;
    EXPECT_NE(report.find("shadow-map-details:\n  - index: 1\n    physical-camera-count: 2\n"
                          "    gpu-time:\n      available: true\n      count: 1\n      median: 0.006"),
        std::string::npos)
        << report;
    EXPECT_NE(
        report.find("    cpu-time:\n      available: true\n      count: 1\n      median: 0.0035"), std::string::npos)
        << report;
    EXPECT_NE(report.find("    render-leaves:\n      available: true\n      count: 1\n"
                          "      median: 200"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("    state-graphs:\n      available: true\n      count: 1\n"
                          "      median: 40"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("    light-vertical-magnitude:\n      available: true\n      count: 1\n"
                          "      median: 0.599"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("    light-luminance:\n      available: true\n      count: 1\n"
                          "      median: 0.414"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("    caster-cull-minimum:\n      available: true\n      count: 1\n"
                          "      median: -4000"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("    receiver-depth-span:\n      available: true\n      count: 1\n"
                          "      median: 3500"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("    terrain-selected-nodes:\n      available: true\n      count: 1\n"
                          "      median: 192"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("    terrain-active-grid-nodes:\n      available: true\n      count: 1\n"
                          "      median: 81"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("    terrain-pending-rendering-nodes:\n      available: true\n      count: 1\n"
                          "      median: 2"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("    terrain-world-revision:\n      available: true\n      count: 1\n"
                          "      median: 49"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("    terrain-selection-updated:\n      available: true\n      count: 1\n"
                          "      median: 1"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("    terrain-selection-changed:\n      available: true\n      count: 1\n"
                          "      median: 0"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("  - index: 2\n    physical-camera-count: 1\n"
                          "    gpu-time:\n      available: true\n      count: 1\n"
                          "      median: 0.003"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("terrain-composite-details:\n  budget-milliseconds:\n"
                          "    available: true\n    count: 1\n    median: 2.5"),
        std::string::npos)
        << report;
    EXPECT_NE(
        report.find("  submitted-drawables:\n    available: true\n    count: 1\n    median: 24"), std::string::npos)
        << report;
    EXPECT_NE(report.find("  queued-maps-after:\n    available: true\n    count: 1\n    median: 11"), std::string::npos)
        << report;
    EXPECT_NE(report.find("  drawable-cpu-milliseconds:\n    available: true\n    count: 1"), std::string::npos)
        << report;
    EXPECT_NE(report.find("unclassified-camera-details:\n  - index: 1\n    name: \"AuxiliarySceneCamera\"\n"
                          "    gpu-time:\n      available: true\n      count: 1\n      median:"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("    cpu-time:\n      available: true\n      count: 1\n      median:"), std::string::npos)
        << report;
    EXPECT_NE(report.find("groundcover-passes:\n  main-scene.instance-geometry-pairs-submitted:\n"
                          "    available: true\n    count: 1\n    median: 1200"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("  shadow-maps.instance-geometry-pairs-submitted:\n"
                          "    available: true\n    count: 1\n    median: 300"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("  main-scene.model-batches-submitted:\n"
                          "    available: true\n    count: 1\n    median: 12"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("  main-scene.drawables-submitted:\n"
                          "    available: true\n    count: 1\n    median: 24"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("  main-scene.conservative-instances-tested:\n"
                          "    available: true\n    count: 1\n    median: 1000"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("  main-scene.conservative-instances-visible:\n"
                          "    available: true\n    count: 1\n    median: 600"),
        std::string::npos)
        << report;
    EXPECT_NE(
        report.find("  water-reflection.instance-geometry-pairs-submitted:\n    available: false"), std::string::npos)
        << report;
    EXPECT_NE(report.find("gpu-render-bins:\n  early-sky:\n    available: false"), std::string::npos);
    EXPECT_NE(report.find("  scene-root:\n    available: true\n    count: 1"), std::string::npos) << report;
    EXPECT_NE(report.find("cpu-render-bins:\n  early-sky:\n    available: false"), std::string::npos);
}
