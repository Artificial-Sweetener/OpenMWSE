#include <apps/openmw/mwrender/renderbin.hpp>
#include <apps/openmw/mwrender/skyutil.hpp>

#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/diagnosticgputiming.hpp>
#include <components/sceneutil/offscreencontext.hpp>

#include <osg/BlendFunc>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Image>
#include <osgViewer/Viewer>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    /// Record actual draw order while preserving each geometry's normal rendering implementation.
    class RecordDraw final : public osg::Drawable::DrawCallback
    {
    public:
        /// Retain the test's draw sequence until its viewer has finished rendering.
        RecordDraw(std::vector<std::string>& order, std::string name)
            : mOrder(order)
            , mName(std::move(name))
        {
        }

        /// Record this draw before invoking the unmodified geometry.
        void drawImplementation(osg::RenderInfo& info, const osg::Drawable* drawable) const override
        {
            mOrder.push_back(mName);
            drawable->drawImplementation(info);
        }

    private:
        std::vector<std::string>& mOrder;
        std::string mName;
    };

    /// Read the completed color target before the offscreen frame is submitted.
    class ReadColor final : public osg::Camera::DrawCallback
    {
    public:
        osg::ref_ptr<osg::Image> mImage = new osg::Image;

        /// Preserve every test pixel for foreground, sky, and transparency comparisons.
        void operator()(osg::RenderInfo&) const override
        {
            mImage->readPixels(0, 0, 32, 16, GL_RGBA, GL_UNSIGNED_BYTE);
        }
    };

    /// Carry rendered pixels and the actual draw sequence from one isolated camera.
    struct SkyRender
    {
        osg::ref_ptr<osg::Image> image;
        std::vector<std::string> order;
    };

    /// Render opaque geometry, background, and a translucent overlay through the real reflection cull callback.
    SkyRender renderSky(bool reflection, bool useCallback)
    {
        SkyRender result;
        osg::ref_ptr<osg::GraphicsContext> context = SceneUtil::createOffscreenContext({ 32, 16, 0 });
        if (!context)
            throw std::runtime_error("Reflection sky test requires an offscreen context");
        osgViewer::Viewer viewer;
        viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
        osg::Camera* camera = viewer.getCamera();
        camera->setGraphicsContext(context);
        camera->setName(std::string(reflection ? SceneUtil::reflectionCameraName : SceneUtil::sceneCameraName));
        camera->setViewport(0, 0, 32, 16);
        camera->setProjectionMatrix(osg::Matrix::scale(1, 1, SceneUtil::AutoDepth::isReversed() ? -1 : 1));
        camera->setViewMatrix(osg::Matrix::identity());
        camera->setClearColor(osg::Vec4(0, 0, 0, 1));
        SceneUtil::setCameraClearDepth(camera);
        camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
        camera->getOrCreateStateSet()->addUniform(new osg::Uniform("isReflection", reflection));
        osg::ref_ptr<osg::Group> root = new osg::Group;
        root->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);

        const auto addQuad = [&](osg::Group& parent, const std::string& name, float left, float right, float depth,
                                 const osg::Vec4& color, int bin, bool writeDepth) {
            osg::ref_ptr<osg::Geometry> geometry = osg::createTexturedQuadGeometry(
                osg::Vec3(left, -1, depth), osg::Vec3(right - left, 0, 0), osg::Vec3(0, 2, 0));
            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            colors->push_back(color);
            geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
            geometry->setUseDisplayList(false);
            geometry->setUseVertexBufferObjects(true);
            geometry->setDrawCallback(new RecordDraw(result.order, name));
            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            geode->addDrawable(geometry);
            osg::StateSet* state = geode->getOrCreateStateSet();
            state->setRenderBinDetails(bin, "RenderBin");
            state->setAttributeAndModes(new SceneUtil::AutoDepth(osg::Depth::LEQUAL, 0, 1, writeDepth));
            if (color.a() < 1.f)
                state->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            parent.addChild(geode);
        };
        osg::ref_ptr<osg::Group> sky = new osg::Group;
        sky->getOrCreateStateSet()->setRenderBinDetails(MWRender::RenderBin_Sky, "RenderBin");
        if (useCallback)
            sky->addCullCallback(new MWRender::ReflectionSkyCullCallback);
        root->addChild(sky);
        addQuad(*sky, "sky", -1, 1, 0.9f, osg::Vec4(0, 0, 1, 1), 0, false);
        addQuad(*root, "opaque", -1, 0, 0, osg::Vec4(1, 0, 0, 1), MWRender::RenderBin_Default, true);
        addQuad(
            *root, "transparent", -0.5f, 0.5f, -0.5f, osg::Vec4(0, 1, 0, 0.5f), MWRender::RenderBin_DepthSorted, false);
        osg::ref_ptr<ReadColor> capture = new ReadColor;
        camera->setFinalDrawCallback(capture);
        viewer.setSceneData(root);
        viewer.realize();
        viewer.frame();
        result.image = capture->mImage;
        viewer.setDone(true);
        return result;
    }
}

/// Preserve foreground and translucent pixels while drawing reflected sky after opaque surfaces.
TEST(ReflectionSkyTest, PreservesPixelsAndDrawsBetweenOpaqueAndTransparentGeometry)
{
    const SkyRender reference = renderSky(true, false);
    const SkyRender reflected = renderSky(true, true);
    EXPECT_EQ(reference.order, (std::vector<std::string>{ "sky", "opaque", "transparent" }));
    EXPECT_EQ(reflected.order, (std::vector<std::string>{ "opaque", "sky", "transparent" }));
    ASSERT_EQ(reference.image->getTotalSizeInBytes(), reflected.image->getTotalSizeInBytes());
    for (unsigned i = 0; i < reference.image->getTotalSizeInBytes(); ++i)
        EXPECT_EQ(reference.image->data()[i], reflected.image->data()[i]) << "byte " << i;
    EXPECT_GT(reflected.image->getColor(4, 8).r(), 0.9f);
    EXPECT_GT(reflected.image->getColor(28, 8).b(), 0.9f);
    EXPECT_GT(reflected.image->getColor(20, 8).g(), 0.4f);
    EXPECT_GT(reflected.image->getColor(20, 8).b(), 0.4f);
}

/// Leave the ordinary camera's established sky ordering intact.
TEST(ReflectionSkyTest, PreservesMainCameraOrdering)
{
    const SkyRender main = renderSky(false, true);
    EXPECT_EQ(main.order, (std::vector<std::string>{ "sky", "opaque", "transparent" }));
    EXPECT_GT(main.image->getColor(4, 8).r(), 0.9f);
    EXPECT_GT(main.image->getColor(28, 8).b(), 0.9f);
}

/// Exercise reversed depth in a subprocess because the renderer's depth convention is immutable process state.
TEST(ReflectionSkyDeathTest, PreservesReversedDepthPixelsAndOrdering)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            SceneUtil::AutoDepth::setReversed(true);
            const SkyRender reference = renderSky(true, false);
            const SkyRender reflected = renderSky(true, true);
            const auto expectedOrder = (std::vector<std::string>{ "opaque", "sky", "transparent" });
            const bool identical = reference.image->getTotalSizeInBytes() == reflected.image->getTotalSizeInBytes()
                && std::equal(reference.image->data(), reference.image->data() + reference.image->getTotalSizeInBytes(),
                    reflected.image->data());
            const bool populated = reflected.image->getColor(4, 8).r() > 0.9f
                && reflected.image->getColor(28, 8).b() > 0.9f && reflected.image->getColor(20, 8).g() > 0.4f;
            std::exit(identical && populated && reflected.order == expectedOrder ? EXIT_SUCCESS : EXIT_FAILURE);
        },
        testing::ExitedWithCode(EXIT_SUCCESS), "");
}
