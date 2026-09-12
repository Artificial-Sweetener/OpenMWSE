#include <components/sceneutil/stateupdater.hpp>
#include <components/sceneutil/texturedclouds.hpp>

#include <osg/Geometry>
#include <osg/MatrixTransform>

#include <gtest/gtest.h>

namespace
{
    /// Recover UVs and vertex fade from the actual mesh, including its local transform and non-default authored
    /// mapping.
    TEST(TexturedClouds, DerivesDirectionalMappingFromAuthoredDome)
    {
        osg::ref_ptr<osg::Geometry> mesh = osg::createTexturedQuadGeometry(
            osg::Vec3(-100, -100, 0), osg::Vec3(200, 0, 0), osg::Vec3(0, 200, 0), 0.2f, 0.3f, 0.6f, 0.9f);
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        const auto* vertices = static_cast<const osg::Vec3Array*>(mesh->getVertexArray());
        for (const auto& vertex : *vertices)
            colors->push_back(osg::Vec4f(1, 1, 1, 0.5f + vertex.x() * 0.0025f));
        mesh->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
        osg::ref_ptr<osg::MatrixTransform> dome = new osg::MatrixTransform(osg::Matrix::translate(0, 0, 20));
        dome->addChild(mesh);
        SceneUtil::TexturedClouds clouds;
        clouds.setDome(*dome);
        const auto* image = clouds.getDomeMapping()->getImage();
        ASSERT_NE(image, nullptr);
        for (int x : { 110, 128, 151 })
            for (int y : { 105, 129, 160 })
            {
                const double px = 2.0 * (x + 0.5) / image->s() - 1.0;
                const double py = 2.0 * (y + 0.5) / image->t() - 1.0;
                const double hitX = 40.0 * px / (1.0 - px * px - py * py);
                const double hitY = 40.0 * py / (1.0 - px * px - py * py);
                const auto* pixel = reinterpret_cast<const float*>(image->data(x, y));
                ASSERT_GT(pixel[2], 0.f);
                EXPECT_NEAR(pixel[0] / pixel[2], 0.4 + hitX * 0.002, 0.00001);
                EXPECT_NEAR(pixel[1] / pixel[2], 0.6 + hitY * 0.003, 0.00001);
                EXPECT_NEAR(pixel[2], 0.5 + hitX * 0.0025, 0.00001);
            }
        EXPECT_EQ(reinterpret_cast<const float*>(image->data(0, 0))[2], 0.f);
        auto previous = osg::ref_ptr<osg::Texture2D>(clouds.getDomeMapping());
        osg::ref_ptr<osg::Group> empty = new osg::Group;
        clouds.setDome(*empty);
        EXPECT_EQ(reinterpret_cast<const float*>(clouds.getDomeMapping()->getImage()->data(128, 128))[2], 0.f);
        EXPECT_GT(reinterpret_cast<const float*>(previous->getImage()->data(128, 128))[2], 0.f);
    }

    /// Create small weather assets with independently specified coverage and irrelevant color channels.
    osg::ref_ptr<osg::Image> makeWeatherImage(int width, int height, unsigned char alpha)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                auto* pixel = image->data(x, y);
                pixel[0] = 255;
                pixel[1] = 63;
                pixel[2] = 127;
                pixel[3] = alpha;
            }
        return image;
    }

    /// Each channel must preserve the actual asset alpha, including differently sized and absent layers.
    TEST(TexturedClouds, PacksAuthoredWeatherOpacityIndependently)
    {
        SceneUtil::TexturedClouds clouds;
        auto outgoing = makeWeatherImage(4, 2, 64);
        auto incoming = makeWeatherImage(2, 1, 192);
        clouds.setImages(outgoing, incoming);
        const auto* opacity = clouds.getOpacity()->getImage();
        ASSERT_EQ(opacity->s(), 4);
        ASSERT_EQ(opacity->t(), 2);
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 4; ++x)
            {
                EXPECT_EQ(opacity->data(x, y)[0], 64);
                EXPECT_EQ(opacity->data(x, y)[1], 192);
                EXPECT_NEAR(opacity->data(x, y)[2], 64, 1);
                EXPECT_NEAR(opacity->data(x, y)[3], 192, 1);
            }
        clouds.setImages(outgoing, nullptr);
        EXPECT_EQ(clouds.getOpacity()->getImage()->data()[0], 64);
        EXPECT_EQ(clouds.getOpacity()->getImage()->data()[1], 0);
    }

    /// Weather changes replace the atlas while already-published frames retain their source opacity.
    TEST(TexturedClouds, ReusesUnchangedAssetsAndPreservesPublishedFrames)
    {
        SceneUtil::TexturedClouds clouds;
        auto outgoing = makeWeatherImage(4, 4, 64);
        auto incoming = makeWeatherImage(4, 4, 192);
        clouds.setImages(outgoing, incoming);
        SceneUtil::TexturedCloudState previous;
        previous.mOpacity = clouds.getOpacity();
        previous.mEnabled = true;
        previous.mWeights = { 0.25f, 0.75f };
        osg::ref_ptr<osg::StateSet> frame = new osg::StateSet;
        SceneUtil::applyTexturedCloudState(*frame, previous, 7, 8);
        clouds.setImages(outgoing, incoming);
        EXPECT_EQ(clouds.getOpacity(), previous.mOpacity);
        clouds.setImages(incoming, outgoing);
        EXPECT_NE(clouds.getOpacity(), previous.mOpacity);
        EXPECT_EQ(previous.mOpacity->getImage()->data()[0], 64);
        EXPECT_EQ(clouds.getOpacity()->getImage()->data()[0], 192);
        EXPECT_EQ(frame->getTextureAttribute(7, osg::StateAttribute::TEXTURE), previous.mOpacity);
        osg::Vec2f weights;
        ASSERT_TRUE(frame->getUniform("openmwseTexturedCloudWeights")->get(weights));
        EXPECT_EQ(weights, previous.mWeights);
    }

    /// Empty weather remains clear and large mod textures cannot cause unbounded derived allocations.
    TEST(TexturedClouds, HandlesEmptyAndBoundsLargeWeatherAssets)
    {
        SceneUtil::TexturedClouds clouds;
        clouds.setImages(nullptr, nullptr);
        EXPECT_EQ(clouds.getOpacity()->getImage()->data()[0], 0);
        EXPECT_EQ(clouds.getOpacity()->getImage()->data()[1], 0);
        auto wide = makeWeatherImage(2048, 1, 255);
        clouds.setImages(wide, nullptr);
        EXPECT_EQ(clouds.getOpacity()->getImage()->s(), 1024);
        EXPECT_EQ(clouds.getOpacity()->getImage()->t(), 1);
        EXPECT_EQ(clouds.getOpacity()->getImage()->data()[0], 255);
    }

    /// Blur projected opacity across the periodic tile seam without altering authored source channels.
    TEST(TexturedClouds, PrefiltersCloudShadowsAcrossPeriodicWeatherSeams)
    {
        auto weather = makeWeatherImage(64, 1, 0);
        weather->data(0, 0)[3] = 255;
        SceneUtil::TexturedClouds clouds;
        clouds.setImages(weather, nullptr);
        const osg::Image* opacity = clouds.getOpacity()->getImage();
        ASSERT_NE(opacity, nullptr);
        EXPECT_EQ(opacity->data(0, 0)[0], 255);
        EXPECT_EQ(opacity->data(63, 0)[0], 0);
        EXPECT_GT(opacity->data(63, 0)[2], 0);
        EXPECT_EQ(opacity->data(63, 0)[2], opacity->data(1, 0)[2]);
        EXPECT_GT(opacity->data(0, 0)[2], opacity->data(1, 0)[2]);
        EXPECT_EQ(opacity->data(0, 0)[1], 0);
        EXPECT_EQ(opacity->data(0, 0)[3], 0);
    }

    /// Publish a bounded cloud-shadow strength through the scene uniform contract.
    TEST(TexturedClouds, PublishesBoundedCloudShadowIntensity)
    {
        SceneUtil::StateUpdater updater;
        osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
        updater.setAtmosphereFeatures(true, true, true, true, 2.f, true);
        updater.setDefaults(state);
        updater.apply(state, nullptr);
        float intensity = 0.f;
        ASSERT_TRUE(state->getUniform("atmosphere.cloudShadowIntensity")->get(intensity));
        EXPECT_FLOAT_EQ(intensity, 1.f);
        bool legacy = false;
        ASSERT_TRUE(state->getUniform("atmosphere.legacyCloudShadows")->get(legacy));
        EXPECT_TRUE(legacy);

        updater.setAtmosphereFeatures(true, true, true, false, -1.f, true);
        updater.apply(state, nullptr);
        ASSERT_TRUE(state->getUniform("atmosphere.cloudShadowIntensity")->get(intensity));
        EXPECT_FLOAT_EQ(intensity, 0.f);
        ASSERT_TRUE(state->getUniform("atmosphere.legacyCloudShadows")->get(legacy));
        EXPECT_FALSE(legacy);
    }
}
