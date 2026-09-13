#include <components/resource/gpustatsoverlay.hpp>
#include <components/resource/stats.hpp>
#include <components/testing/graphics.hpp>
#include <components/vfs/manager.hpp>

#include <gtest/gtest.h>

#include <osg/Camera>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/RenderInfo>
#include <osg/Stats>
#include <osg/Switch>
#include <osgText/Text>
#include <osgViewer/Viewer>

namespace
{
    /// Provide an observable callback for overlay replacement tests without issuing a draw.
    class EmptyDrawCallback : public osg::Drawable::DrawCallback
    {
    public:
        /// Leave the drawable untouched because the test inspects callback ownership only.
        void drawImplementation(osg::RenderInfo&, const osg::Drawable*) const override {}
    };

    TEST(GpuStatsOverlayTest, ExtendsCameraStatisticsAndTimelineCallbacks)
    {
        osg::ref_ptr<osg::Switch> overlay = new osg::Switch;
        osg::ref_ptr<osg::Geode> cameraScene = new osg::Geode;
        osg::ref_ptr<osgText::Text> labels = new osgText::Text;
        osg::ref_ptr<osgText::Text> values = new osgText::Text;
        osg::ref_ptr<EmptyDrawCallback> originalValuesCallback = new EmptyDrawCallback;
        values->setDrawCallback(originalValuesCallback);

        osg::ref_ptr<osg::Geometry> background = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> backgroundVertices = new osg::Vec3Array;
        backgroundVertices->push_back({ 0.f, 30.f, 0.f });
        backgroundVertices->push_back({ 10.f, 30.f, 0.f });
        backgroundVertices->push_back({ 10.f, 20.f, 0.f });
        backgroundVertices->push_back({ 0.f, 20.f, 0.f });
        background->setVertexArray(backgroundVertices);
        cameraScene->addDrawable(labels);
        cameraScene->addDrawable(values);
        cameraScene->addDrawable(background);
        overlay->addChild(cameraScene);

        osg::ref_ptr<osg::Geode> timelineBars = new osg::Geode;
        osg::ref_ptr<osg::Geometry> timeline = new osg::Geometry;
        osg::ref_ptr<EmptyDrawCallback> originalTimelineCallback = new EmptyDrawCallback;
        timeline->setDrawCallback(originalTimelineCallback);
        timelineBars->addDrawable(timeline);

        osg::ref_ptr<osg::Camera> sceneCamera = new osg::Camera;
        sceneCamera->setName("MainCamera");
        sceneCamera->setStats(new osg::Stats("camera"));
        osgViewer::ViewerBase::Cameras cameras{ sceneCamera.get() };
        osg::ref_ptr<osg::Stats> viewerStats = new osg::Stats("viewer");
        osg::ref_ptr<osg::Camera> overlayCamera = new osg::Camera;
        double blockMultiplier = 1.0;

        Resource::configureGpuStatsOverlay(*overlay, 0, *timelineBars, *overlayCamera, cameras, viewerStats, nullptr,
            blockMultiplier, 10, 500.f, 14.f);

        const std::string labelText = labels->getText().createUTF8EncodedString();
        EXPECT_NE(labelText.find("Point lights"), std::string::npos);
        EXPECT_NE(labelText.find("GPU triangles"), std::string::npos);
        EXPECT_NE(values->getDrawCallback(), originalValuesCallback.get());
        EXPECT_NE(timeline->getDrawCallback(), originalTimelineCallback.get());
        EXPECT_NE(overlayCamera->getPreDrawCallback(), nullptr);
        EXPECT_FLOAT_EQ((*backgroundVertices)[1].y(), -134.f);
        EXPECT_FLOAT_EQ((*backgroundVertices)[2].y(), -134.f);
    }

    TEST(GpuStatsOverlayTest, ProfilerConfiguresOverlayFromItsRuntimeEvent)
    {
        const osg::ref_ptr<osg::GraphicsContext> context = TestingOpenMW::createGraphicsContext();
        if (context == nullptr)
            GTEST_SKIP() << "Offscreen GL context unavailable";

        VFS::Manager vfs;
        Resource::Profiler profiler(false, vfs);
        osgViewer::Viewer viewer;
        viewer.getCamera()->setGraphicsContext(context);
        viewer.getCamera()->setStats(new osg::Stats("camera"));

        for (int page = 0; page < 3; ++page)
        {
            osg::ref_ptr<osgGA::GUIEventAdapter> event = new osgGA::GUIEventAdapter;
            event->setEventType(osgGA::GUIEventAdapter::KEYDOWN);
            event->setKey(osgGA::GUIEventAdapter::KEY_F3);
            profiler.handle(*event, viewer);
        }

        EXPECT_NE(profiler.getCamera()->getPreDrawCallback(), nullptr);
        EXPECT_TRUE(viewer.getViewerStats()->collectStats("gpu_scene"));
        context->releaseContext();
    }
}
