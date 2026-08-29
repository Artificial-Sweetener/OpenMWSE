#include <apps/openmw/diagnosticperformance.hpp>
#include <apps/openmw/diagnosticrenderingtiming.hpp>

#include <apps/openmw/mwrender/renderworkcounter.hpp>

#include <components/sceneutil/diagnosticgputiming.hpp>

#include <gtest/gtest.h>

#include <osg/Camera>
#include <osg/Stats>
#include <osgViewer/Viewer>

#include <array>
#include <limits>
#include <sstream>
#include <string>

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

TEST(DiagnosticPerformanceTest, ReportsNamedGpuPassesAndAggregatesMatchingCameras)
{
    constexpr unsigned frame = 42;
    osgViewer::Viewer viewer;
    viewer.setViewerStats(new osg::Stats("viewer"));
    viewer.getCamera()->setName(std::string(SceneUtil::sceneCameraName));
    SceneUtil::enableDiagnosticGpuPassStatistics(*viewer.getCamera());
    viewer.getCamera()->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticGpuPassTimeStatistic), 0.010);
    viewer.getCamera()->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassTimeStatistic), 0.008);
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

    osg::ref_ptr<osg::Camera> firstShadow = new osg::Camera;
    firstShadow->setName(std::string(SceneUtil::shadowCameraName));
    SceneUtil::enableDiagnosticGpuPassStatistics(*firstShadow);
    firstShadow->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticGpuPassTimeStatistic), 0.002);
    firstShadow->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassBeginStatistic), 9.996);
    firstShadow->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassEndStatistic), 9.998);

    osg::ref_ptr<osg::Camera> secondShadow = new osg::Camera;
    secondShadow->setName(std::string(SceneUtil::shadowCameraName));
    SceneUtil::enableDiagnosticGpuPassStatistics(*secondShadow);
    secondShadow->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticGpuPassTimeStatistic), 0.003);
    secondShadow->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassBeginStatistic), 9.998);
    secondShadow->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassEndStatistic), 9.999);
    secondShadow->getStats()->setAttribute(frame, std::string(MWRender::groundcoverInstanceRenderCounter), 300.0);

    osg::ref_ptr<osg::Camera> auxiliary = new osg::Camera;
    auxiliary->setName("AuxiliarySceneCamera");
    SceneUtil::enableDiagnosticGpuPassStatistics(*auxiliary);
    auxiliary->getStats()->setAttribute(frame, std::string(SceneUtil::diagnosticCpuPassTimeStatistic), 0.004);

    std::ostringstream output;
    OMW::reportDiagnosticPerformance(frame, frame, viewer, output);
    const std::string report = output.str();

    EXPECT_NE(report.find("provider: performance@5"), std::string::npos);
    EXPECT_NE(report.find("draw-breakdown:\n  camera-covered:\n    available: true\n    count: 1\n"
                          "    median: 0.010999"),
        std::string::npos)
        << report;
    EXPECT_NE(
        report.find("  outside-cameras:\n    available: true\n    count: 1\n    median: 0.006000"), std::string::npos)
        << report;
    EXPECT_NE(report.find("  main-scene:\n    available: true\n    count: 1\n    median: 0.01"), std::string::npos);
    EXPECT_NE(report.find("  shadow-maps:\n    available: true\n    count: 1\n    median: 0.005"), std::string::npos);
    EXPECT_NE(report.find("  water-reflection:\n    available: false"), std::string::npos);
    EXPECT_NE(report.find("cpu-passes:\n  main-scene:\n    available: true\n    count: 1"), std::string::npos);
    EXPECT_NE(report.find("unclassified-camera-details:\n  - index: 1\n    name: \"AuxiliarySceneCamera\"\n"
                          "    cpu-time:\n      available: true\n      count: 1\n      median: 0.004"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("groundcover-passes:\n  main-scene.instance-geometry-pairs-submitted:\n"
                          "    available: true\n    count: 1\n    median: 1200"),
        std::string::npos)
        << report;
    EXPECT_NE(report.find("  shadow-maps.instance-geometry-pairs-submitted:\n"
                          "    available: true\n    count: 1\n    median: 300"),
        std::string::npos)
        << report;
    EXPECT_NE(
        report.find("  water-reflection.instance-geometry-pairs-submitted:\n    available: false"), std::string::npos)
        << report;
    EXPECT_NE(report.find("gpu-render-bins:\n  early-sky:\n    available: false"), std::string::npos);
    EXPECT_NE(report.find("  scene-root:\n    available: true\n    count: 1"), std::string::npos) << report;
    EXPECT_NE(report.find("cpu-render-bins:\n  early-sky:\n    available: false"), std::string::npos);
}
