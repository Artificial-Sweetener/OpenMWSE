#include <apps/openmw/mwrender/reflectedclouds.hpp>
#include <apps/openmw/mwrender/renderbin.hpp>
#include <apps/openmw/mwrender/skyutil.hpp>
#include <apps/openmw/mwrender/vismask.hpp>

#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/diagnosticgputiming.hpp>
#include <components/sceneutil/offscreencontext.hpp>

#include <osg/BlendFunc>
#include <osg/FrameStamp>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Texture2D>
#include <osgViewer/Viewer>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    /// Preserve completed scene pixels and the actual cloud-camera viewport for each draw.
    struct CloudBufferEvidence
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        std::vector<osg::Vec2f> cloudViewports;
        std::vector<unsigned> cloudDrawFrames;
        std::vector<unsigned> sceneFrames;
    };

    /// Describe the scene and successive cover states without changing the analytic transport oracle.
    struct CloudBufferScene
    {
        bool mBuffered = true;
        std::array<bool, 4> mEnabled{ true, true, true, true };
        bool mReflection = true;
        int mWidth = 32;
        int mHeight = 16;
        bool mCameraRelative = false;
        bool mForeground = false;
        bool mResize = false;
        bool mSkyMask = false;
        std::array<float, 4> mMainCoverage{ 1.f, 1.f, 1.f, 1.f };
        std::array<float, 4> mLowCoverage{};
    };

    /// Record the rendering context that actually executes the shared atmosphere geometry.
    class ObserveCloudDraw final : public osg::Drawable::DrawCallback
    {
    public:
        /// Keep evidence alive for the duration of this viewer's render loop.
        explicit ObserveCloudDraw(CloudBufferEvidence& evidence)
            : mEvidence(evidence)
        {
        }

        /// Observe dependent cloud renders without replacing the real draw implementation.
        void drawImplementation(osg::RenderInfo& info, const osg::Drawable* drawable) const override
        {
            if (info.getCurrentCamera()->getName() == "ReflectedCloudCamera")
            {
                const osg::Viewport* viewport = info.getCurrentCamera()->getViewport();
                mEvidence.cloudViewports.emplace_back(viewport->width(), viewport->height());
                mEvidence.cloudDrawFrames.push_back(info.getState()->getFrameStamp()->getFrameNumber());
            }
            drawable->drawImplementation(info);
        }

    private:
        CloudBufferEvidence& mEvidence;
    };

    /// Read the final composite before the offscreen target is released.
    class ReadCloudComposite final : public osg::Camera::DrawCallback
    {
    public:
        /// Retain the destination image and the requested scene dimensions.
        ReadCloudComposite(osg::Image* image, int width, int height)
            : mImage(image)
            , mWidth(width)
            , mHeight(height)
        {
        }

        /// Capture normalized scene pixels after all dependent targets have completed.
        void operator()(osg::RenderInfo&) const override
        {
            mImage->readPixels(0, 0, mWidth, mHeight, GL_RGBA, GL_FLOAT);
        }

    private:
        osg::ref_ptr<osg::Image> mImage;
        int mWidth;
        int mHeight;
    };

    /// Exercise the production scheduling and target with analytic HDR transport and a changing reflected view.
    CloudBufferEvidence renderCloudBuffer(const CloudBufferScene& sceneSettings)
    {
        const int width = sceneSettings.mWidth;
        const int height = sceneSettings.mHeight;
        CloudBufferEvidence evidence;
        osg::ref_ptr<osg::GraphicsContext> context = SceneUtil::createOffscreenContext({ width, height, 0 });
        if (!context)
            throw std::runtime_error("Reflected cloud tests require an offscreen context");
        context->getState()->setUseModelViewAndProjectionUniforms(true);
        osgViewer::Viewer viewer;
        viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
        osg::Camera* camera = viewer.getCamera();
        camera->setGraphicsContext(context);
        camera->setViewport(0, 0, width, height);
        camera->setName(
            std::string(sceneSettings.mReflection ? SceneUtil::reflectionCameraName : SceneUtil::sceneCameraName));
        camera->setCullMask(MWRender::Mask_Sky);
        camera->setProjectionMatrix(osg::Matrix::scale(1, 1, SceneUtil::AutoDepth::isReversed() ? -1 : 1));
        SceneUtil::setCameraClearDepth(camera);
        camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
        camera->setFinalDrawCallback(new ReadCloudComposite(evidence.image, width, height));
        osg::ref_ptr<osg::Group> root = sceneSettings.mCameraRelative
            ? static_cast<osg::Group*>(new MWRender::CameraRelativeTransform)
            : new osg::Group;
        osg::ref_ptr<osg::Group> scene = new osg::Group;
        scene->addChild(root);
        if (sceneSettings.mForeground)
            root->addCullCallback(new MWRender::ReflectionSkyCullCallback);
        osg::ref_ptr<osg::Geode> atmosphere = new osg::Geode;
        osg::ref_ptr<osg::Geometry> quad
            = osg::createTexturedQuadGeometry(osg::Vec3(-3, -3, 0), osg::Vec3(6, 0, 0), osg::Vec3(0, 6, 0));
        quad->setUseDisplayList(false);
        quad->setUseVertexBufferObjects(true);
        quad->setDrawCallback(new ObserveCloudDraw(evidence));
        atmosphere->addDrawable(quad);
        root->addChild(atmosphere);
        osg::ref_ptr<osg::Program> program = new osg::Program;
        program->addShader(new osg::Shader(osg::Shader::VERTEX, R"glsl(#version 120
varying vec2 fieldPosition;
void main() { fieldPosition = gl_Vertex.xy; gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; }
)glsl"));
        program->addShader(new osg::Shader(osg::Shader::FRAGMENT, R"glsl(#version 120
#pragma import_defines(OPENMWSE_REFLECTED_CLOUD_MODE)
#ifndef OPENMWSE_REFLECTED_CLOUD_MODE
#define OPENMWSE_REFLECTED_CLOUD_MODE 0
#endif
uniform sampler2D reflectedCloudTexture;
uniform vec2 screenRes;
uniform float episode;
uniform mat4 osg_ViewMatrixInverse;
varying vec2 fieldPosition;
void main()
{
    vec4 clouds = vec4(2.5 + fieldPosition.x * 0.125 + osg_ViewMatrixInverse[3].x * 0.25,
        0.125 + fieldPosition.y * 0.03125, episode, 0.25);
#if OPENMWSE_REFLECTED_CLOUD_MODE == 1
    gl_FragColor = clouds;
#else
#if OPENMWSE_REFLECTED_CLOUD_MODE == 2
    clouds = texture2D(reflectedCloudTexture, gl_FragCoord.xy / screenRes);
#endif
    gl_FragColor = vec4(clouds.rgb * 0.25 + vec3(0.125) * clouds.a, 1.0);
#endif
}
)glsl"));
        osg::StateSet* state = atmosphere->getOrCreateStateSet();
        state->setAttributeAndModes(program);
        state->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        osg::ref_ptr<osg::Uniform> episode = new osg::Uniform("episode", 0.f);
        state->addUniform(episode);
        camera->getOrCreateStateSet()->addUniform(new osg::Uniform("screenRes", osg::Vec2f(width, height)));
        osg::ref_ptr<MWRender::ReflectedClouds> reflectedClouds = new MWRender::ReflectedClouds(atmosphere);
        if (sceneSettings.mBuffered)
            root->addCullCallback(reflectedClouds);
        if (sceneSettings.mForeground)
        {
            const auto addForeground = [&](float left, float right, float depth, osg::Vec4 color, bool transparent) {
                osg::ref_ptr<osg::Geode> geode = new osg::Geode;
                osg::ref_ptr<osg::Geometry> geometry = osg::createTexturedQuadGeometry(
                    osg::Vec3(left, -2, depth), osg::Vec3(right - left, 0, 0), osg::Vec3(0, 4, 0));
                geometry->setUseDisplayList(false);
                geode->addDrawable(geometry);
                osg::ref_ptr<osg::Program> foregroundProgram = new osg::Program;
                foregroundProgram->addShader(new osg::Shader(osg::Shader::VERTEX,
                    "#version 120\nvoid main() { gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; }"));
                foregroundProgram->addShader(new osg::Shader(osg::Shader::FRAGMENT,
                    "#version 120\nuniform vec4 surfaceColor; void main() { gl_FragColor = surfaceColor; }"));
                osg::StateSet* surface = geode->getOrCreateStateSet();
                surface->setAttributeAndModes(foregroundProgram);
                surface->addUniform(new osg::Uniform("surfaceColor", color));
                surface->setAttributeAndModes(new SceneUtil::AutoDepth(osg::Depth::LEQUAL, 0, 1, !transparent));
                surface->setMode(GL_DEPTH_TEST, osg::StateAttribute::ON);
                if (transparent)
                    surface->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
                surface->setRenderBinDetails(
                    transparent ? MWRender::RenderBin_DepthSorted : MWRender::RenderBin_Default, "RenderBin");
                scene->addChild(geode);
            };
            addForeground(-2.f, .5f, -.4f, osg::Vec4(.8f, 0, 0, 1), false);
            addForeground(0.f, 1.f, -.6f, osg::Vec4(0, .8f, 0, .5f), true);
        }
        if (sceneSettings.mSkyMask)
        {
            osg::ref_ptr<osg::Geode> sibling = new osg::Geode;
            sibling->addDrawable(
                osg::createTexturedQuadGeometry(osg::Vec3(0, -3, 0), osg::Vec3(3, 0, 0), osg::Vec3(0, 6, 0)));
            osg::ref_ptr<osg::Image> mask = new osg::Image;
            mask->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            mask->setColor(osg::Vec4(.2f, .4f, .6f, 1), 0, 0);
            osg::ref_ptr<osg::Program> maskProgram = new osg::Program;
            maskProgram->addShader(program->getShader(0));
            maskProgram->addShader(new osg::Shader(osg::Shader::FRAGMENT,
                "#version 120\nuniform sampler2D maskTexture; "
                "void main() { gl_FragColor = texture2D(maskTexture, vec2(.5)); }"));
            osg::StateSet* maskState = sibling->getOrCreateStateSet();
            maskState->setAttributeAndModes(maskProgram);
            maskState->setTextureAttribute(1, new osg::Texture2D(mask));
            maskState->addUniform(new osg::Uniform("maskTexture", 1));
            maskState->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
            maskState->setRenderBinDetails(MWRender::RenderBin_Sky + 1, "RenderBin");
            root->addChild(sibling);
        }
        viewer.setSceneData(scene);
        viewer.realize();
        for (unsigned frame = 0; frame < 4; ++frame)
        {
            if (sceneSettings.mResize && frame == 3)
            {
                camera->setViewport(0, 0, width / 2, height / 2);
                camera->getOrCreateStateSet()->getUniform("screenRes")->set(osg::Vec2f(width / 2, height / 2));
            }
            episode->set(frame == 3 ? 0.75f : 0.25f);
            reflectedClouds->setEnabled(sceneSettings.mEnabled[frame]);
            reflectedClouds->updateCloudCover(sceneSettings.mMainCoverage[frame], sceneSettings.mLowCoverage[frame]);
            camera->setViewMatrix(osg::Matrix::translate(frame == 3 ? -0.5 : 0.5, 0.25, 0));
            viewer.frame();
            evidence.sceneFrames.push_back(viewer.getFrameStamp()->getFrameNumber());
        }
        viewer.setDone(true);
        return evidence;
    }
}

/// Preserve reflected view placement, HDR light, transmittance and immediate changes across the independent target.
TEST(ReflectedCloudsTest, PreservesCurrentViewTransportAtHalfResolution)
{
    for (const auto& size : { std::array{ 32, 16 }, std::array{ 33, 17 }, std::array{ 1, 1 } })
    {
        const auto control = renderCloudBuffer({ .mBuffered = false, .mWidth = size[0], .mHeight = size[1] });
        const auto candidate = renderCloudBuffer({ .mWidth = size[0], .mHeight = size[1] });
        ASSERT_FALSE(candidate.cloudViewports.empty());
        for (const auto& viewport : candidate.cloudViewports)
            EXPECT_EQ(viewport, osg::Vec2f((size[0] + 1) / 2, (size[1] + 1) / 2));
        for (int y = 0; y < size[1]; ++y)
            for (int x = 0; x < size[0]; ++x)
            {
                const osg::Vec4f expected = control.image->getColor(x, y);
                const osg::Vec4f actual = candidate.image->getColor(x, y);
                for (unsigned channel = 0; channel < 4; ++channel)
                    EXPECT_NEAR(actual[channel], expected[channel], 1.f / 255.f)
                        << size[0] << 'x' << size[1] << " pixel " << x << ',' << y << " channel " << channel;
                EXPECT_GT(actual.r(), .6f);
                EXPECT_GT(actual.b(), .2f);
            }
    }
}

/// Keep ordinary sky rendering independent of reflected texture state and avoid scheduling cloud targets for it.
TEST(ReflectedCloudsTest, PreservesMainViewAndAvoidsDependentRendering)
{
    const auto control = renderCloudBuffer({ .mBuffered = false, .mReflection = false });
    const auto candidate = renderCloudBuffer({ .mReflection = false });
    EXPECT_TRUE(candidate.cloudViewports.empty());
    ASSERT_EQ(control.image->getTotalSizeInBytes(), candidate.image->getTotalSizeInBytes());
    EXPECT_TRUE(std::equal(
        control.image->data(), control.image->data() + control.image->getTotalSizeInBytes(), candidate.image->data()));
}

/// Retain the real observer's world-space origin after the native sky transform removes camera translation.
TEST(ReflectedCloudsTest, PreservesObserverBehindCameraRelativeSkyTransform)
{
    const auto control = renderCloudBuffer({ .mBuffered = false, .mCameraRelative = true });
    const auto candidate = renderCloudBuffer({ .mCameraRelative = true });
    const float expectedCenterRed = (2.5f + .03125f * .125f + .5f * .25f) * .25f + .125f * .25f;
    EXPECT_NEAR(control.image->getColor(16, 8).r(), expectedCenterRed, 1.f / 255.f);
    ASSERT_FALSE(candidate.cloudViewports.empty());
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 32; ++x)
            EXPECT_NEAR(candidate.image->getColor(x, y).r(), control.image->getColor(x, y).r(), 1.f / 255.f)
                << "pixel " << x << ',' << y;
}

/// Keep cloud transport behind opaque surfaces and compose transparency after the reflected background.
TEST(ReflectedCloudsTest, PreservesForegroundAndTransparency)
{
    const auto control = renderCloudBuffer({ .mBuffered = false, .mCameraRelative = true, .mForeground = true });
    const auto candidate = renderCloudBuffer({ .mCameraRelative = true, .mForeground = true });
    ASSERT_FALSE(candidate.cloudViewports.empty());
    unsigned opaquePixels = 0;
    unsigned transparentPixels = 0;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 32; ++x)
        {
            const osg::Vec4f actual = candidate.image->getColor(x, y);
            const osg::Vec4f expected = control.image->getColor(x, y);
            for (unsigned channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(actual[channel], expected[channel], 1.f / 255.f) << "pixel " << x << ',' << y;
            opaquePixels += actual.r() > .7f && actual.g() < .01f;
            transparentPixels += actual.g() > .3f;
        }
    EXPECT_GT(opaquePixels, 16u);
    EXPECT_GT(transparentPixels, 16u);
}

/// Rebuild cloud targets in the same frame as a reflection-size change without sampling a stale viewport.
TEST(ReflectedCloudsTest, TracksReflectionViewportChanges)
{
    const auto control = renderCloudBuffer({ .mBuffered = false, .mCameraRelative = true, .mResize = true });
    const auto candidate = renderCloudBuffer({ .mCameraRelative = true, .mResize = true });
    ASSERT_GE(candidate.cloudViewports.size(), 2u);
    EXPECT_EQ(candidate.cloudViewports.front(), osg::Vec2f(16, 8));
    EXPECT_EQ(candidate.cloudViewports.back(), osg::Vec2f(8, 4));
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 32; ++x)
            for (unsigned channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(
                    candidate.image->getColor(x, y)[channel], control.image->getColor(x, y)[channel], 1.f / 255.f)
                    << "pixel " << x << ',' << y;
}

/// Preserve a sibling sky pass's local mask texture when cloud transport uses the same texture unit.
TEST(ReflectedCloudsTest, PreservesSiblingSkyMask)
{
    const auto control = renderCloudBuffer({ .mBuffered = false, .mCameraRelative = true, .mSkyMask = true });
    const auto candidate = renderCloudBuffer({ .mCameraRelative = true, .mSkyMask = true });
    ASSERT_FALSE(candidate.cloudViewports.empty());
    unsigned maskPixels = 0;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 32; ++x)
        {
            const osg::Vec4f actual = candidate.image->getColor(x, y);
            const osg::Vec4f expected = control.image->getColor(x, y);
            for (unsigned channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(actual[channel], expected[channel], 1.f / 255.f) << "pixel " << x << ',' << y;
            maskPixels += std::abs(actual.r() - .2f) < 1.f / 255.f && std::abs(actual.g() - .4f) < 1.f / 255.f
                && std::abs(actual.b() - .6f) < 1.f / 255.f;
        }
    EXPECT_GT(maskPixels, 128u);
}

/// Preserve native sparse-sky pixels while avoiding the unoccluded cloud target entirely.
TEST(ReflectedCloudsTest, KeepsSparseCloudsInTheDepthTestedSkyPass)
{
    const auto control = renderCloudBuffer({ .mBuffered = false });
    const auto candidate = renderCloudBuffer(
        { .mMainCoverage = { .18f, .18f, .18f, .18f }, .mLowCoverage = { .03f, .03f, .03f, .03f } });
    EXPECT_TRUE(candidate.cloudViewports.empty());
    ASSERT_EQ(control.image->getTotalSizeInBytes(), candidate.image->getTotalSizeInBytes());
    EXPECT_TRUE(std::equal(
        control.image->data(), control.image->data() + control.image->getTotalSizeInBytes(), candidate.image->data()));
}

/// Recognize a broad combined deck without requiring either weather-controlled layer to cover half the sky alone.
TEST(ReflectedCloudsTest, BuffersCombinedCloudCover)
{
    const auto candidate = renderCloudBuffer(
        { .mMainCoverage = { .32f, .32f, .32f, .32f }, .mLowCoverage = { .32f, .32f, .32f, .32f } });
    EXPECT_EQ(candidate.cloudDrawFrames, candidate.sceneFrames);
}

/// Avoid repeatedly switching transport resolution near the coverage boundary and remove texture state when it clears.
TEST(ReflectedCloudsTest, RetainsModeThroughSmallCoverageChanges)
{
    const auto control = renderCloudBuffer({ .mBuffered = false });
    const auto candidate = renderCloudBuffer({ .mMainCoverage = { .6f, .45f, .46f, .39f } });
    ASSERT_EQ(candidate.sceneFrames.size(), 4u);
    EXPECT_EQ(candidate.cloudDrawFrames,
        (std::vector<unsigned>{ candidate.sceneFrames[0], candidate.sceneFrames[1], candidate.sceneFrames[2] }));
    ASSERT_EQ(control.image->getTotalSizeInBytes(), candidate.image->getTotalSizeInBytes());
    EXPECT_TRUE(std::equal(
        control.image->data(), control.image->data() + control.image->getTotalSizeInBytes(), candidate.image->data()));
}

/// Render the current observer and episode immediately when broad cloud cover returns after an inactive interval.
TEST(ReflectedCloudsTest, RefreshesTransportWhenBroadCoverReturns)
{
    const auto control = renderCloudBuffer({ .mBuffered = false });
    const auto candidate = renderCloudBuffer({ .mMainCoverage = { .2f, .6f, .3f, .7f } });
    ASSERT_EQ(candidate.sceneFrames.size(), 4u);
    EXPECT_EQ(candidate.cloudDrawFrames, (std::vector<unsigned>{ candidate.sceneFrames[1], candidate.sceneFrames[3] }));
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 32; ++x)
        {
            const osg::Vec4f actual = candidate.image->getColor(x, y);
            const osg::Vec4f expected = control.image->getColor(x, y);
            for (unsigned channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(actual[channel], expected[channel], 1.f / 255.f) << "pixel " << x << ',' << y;
            EXPECT_GT(actual.b(), .2f);
        }
}

/// Exercise the immutable reversed-depth convention with the same cloud, foreground and transparency composition.
TEST(ReflectedCloudsDeathTest, PreservesReversedDepthComposition)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            SceneUtil::AutoDepth::setReversed(true);
            const auto control
                = renderCloudBuffer({ .mBuffered = false, .mCameraRelative = true, .mForeground = true });
            const auto candidate = renderCloudBuffer({ .mCameraRelative = true, .mForeground = true });
            bool matches = !candidate.cloudViewports.empty();
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 32; ++x)
                    for (unsigned channel = 0; channel < 4; ++channel)
                        matches = matches
                            && std::abs(
                                   candidate.image->getColor(x, y)[channel] - control.image->getColor(x, y)[channel])
                                <= 1.f / 255.f;
            std::exit(matches ? EXIT_SUCCESS : EXIT_FAILURE);
        },
        testing::ExitedWithCode(EXIT_SUCCESS), "");
}

/// Stop offscreen cloud work when disabled and resume it from the current view and weather field.
TEST(ReflectedCloudsTest, ReleasesAndRestartsTransportAfterLiveToggle)
{
    const auto evidence = renderCloudBuffer({ .mEnabled = { true, false, false, true } });
    ASSERT_EQ(evidence.cloudDrawFrames.size(), 2u);
    EXPECT_EQ(evidence.cloudDrawFrames.front(), evidence.sceneFrames.front());
    EXPECT_EQ(evidence.cloudDrawFrames.back(), evidence.sceneFrames.back());
}
