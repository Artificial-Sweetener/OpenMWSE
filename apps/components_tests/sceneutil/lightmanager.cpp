#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/lightutil.hpp>

#include <components/esm3/refnum.hpp>

#include <gtest/gtest.h>

#include <osg/Camera>
#include <osg/Matrixf>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderStage>

#include <algorithm>
#include <set>
#include <vector>

namespace
{
    using SceneUtil::LightManager;
    using SceneUtil::LightSettings;
    using SceneUtil::LightSource;

    osg::ref_ptr<LightSource> makeLight(float radius, bool distant = false, std::uint64_t pagedId = 0)
    {
        osg::ref_ptr<LightSource> source = new LightSource;
        source->setLight(new SceneUtil::Light);
        source->setRadius(radius);
        source->setDistant(distant);
        source->setPagedId(pagedId);
        return source;
    }

    struct GridFixture
    {
        osg::ref_ptr<LightManager> mManager;
        osg::ref_ptr<osg::Camera> mCamera = new osg::Camera;
        osg::ref_ptr<osgUtil::RenderStage> mStage = new osgUtil::RenderStage;
        osg::ref_ptr<osgUtil::CullVisitor> mCull = new osgUtil::CullVisitor;
        osg::ref_ptr<osg::RefMatrix> mView = new osg::RefMatrix;
        osg::ref_ptr<osg::FrameStamp> mStamp = new osg::FrameStamp;
        std::vector<osg::ref_ptr<LightSource>> mSources;
        std::vector<osg::Vec3f> mPositions;

        explicit GridFixture(unsigned int frame, bool distant = false, bool indexed = true)
            : mManager(new LightManager(LightSettings{ .mIndexedLightLists = indexed }, nullptr))
        {
            mManager->setDistantLightsEnabled(distant);
            mStamp->setFrameNumber(frame);
            mStage->setCamera(mCamera);
            mCull->setFrameStamp(mStamp);
            mCull->setRenderStage(mStage);
            mManager->update(frame);
        }

        void addLight(const osg::Vec3f& position, float radius, unsigned int frame)
        {
            osg::ref_ptr<LightSource> source = makeLight(radius);
            mManager->addLight(source, osg::Matrixf::translate(position), frame);
            mSources.push_back(source);
            mPositions.push_back(position);
        }

        std::set<int> query(const osg::BoundingSphere& bound, unsigned int frame)
        {
            LightManager::LightList out;
            mManager->getLightsIntersecting(mCull, mView, frame, bound, out);
            std::set<int> ids;
            for (const auto* light : out)
                ids.insert(light->mLightSource->getId());
            EXPECT_EQ(out.size(), ids.size());
            return ids;
        }

        std::set<int> bruteForce(const osg::BoundingSphere& bound) const
        {
            std::set<int> ids;
            for (std::size_t i = 0; i < mSources.size(); ++i)
            {
                const float reach = bound.radius() + mSources[i]->getRadius();
                if ((mPositions[i] - bound.center()).length() < reach)
                    ids.insert(mSources[i]->getId());
            }
            return ids;
        }

        void enableClustered(float distance = 0.f, float fadeStart = 0.5f, float pixels = 4.f)
        {
            mManager->enableClustered(true);
            mManager->processChangedSettings(1.f, distance, fadeStart, pixels);
            mCull->pushViewport(new osg::Viewport(0, 0, 200, 200));
            mCull->pushProjectionMatrix(new osg::RefMatrix(osg::Matrix::perspective(90, 1, 1, 10000)));
            mCull->pushModelViewMatrix(mView, osg::Transform::ABSOLUTE_RF);
        }

        const std::vector<LightManager::LightSourceViewBound>& viewLights(unsigned int frame = 1)
        {
            return mManager->getLightsInViewSpace(mCull, mView, frame);
        }
    };

    TEST(SceneUtilLightGrid, matchesBruteForce)
    {
        constexpr unsigned int frame = 1;
        GridFixture fixture(frame);
        for (int x = 0; x < 3; ++x)
            for (int y = 0; y < 3; ++y)
                for (int z = 0; z < 3; ++z)
                    fixture.addLight(osg::Vec3f(x * 512.f, y * 512.f, z * 512.f), 96.f, frame);
        for (const osg::BoundingSphere& bound :
            { osg::BoundingSphere({ 512, 512, 512 }, 64), osg::BoundingSphere({ 512, 512, 512 }, 700),
                osg::BoundingSphere({ 512, 512, 512 }, 4096), osg::BoundingSphere({ 4096, 4096, 4096 }, 32) })
        {
            SCOPED_TRACE(bound.radius());
            const auto expected = fixture.bruteForce(bound);
            EXPECT_EQ(fixture.query(bound, frame), expected);
            EXPECT_EQ(fixture.query(bound, frame), expected);
        }
        EXPECT_EQ(fixture.query(osg::BoundingSphere({ 512, 512, 512 }, 4096), frame).size(), 27u);
        EXPECT_TRUE(fixture.query(osg::BoundingSphere({ 4096, 4096, 4096 }, 32), frame).empty());
    }

    TEST(SceneUtilLightGrid, baselineAndIndexedQueriesMatch)
    {
        constexpr unsigned int frame = 1;
        GridFixture baseline(frame, false, false);
        GridFixture indexed(frame, false, true);
        for (const osg::Vec3f position : { osg::Vec3f(-256.f, 0.f, 0.f), osg::Vec3f(), osg::Vec3f(256.f, 0.f, 0.f) })
        {
            baseline.addLight(position, 96.f, frame);
            indexed.addLight(position, 96.f, frame);
        }
        const osg::BoundingSphere query({ 0.f, 0.f, 0.f }, 180.f);
        EXPECT_EQ(baseline.query(query, frame).size(), indexed.query(query, frame).size());
        EXPECT_EQ(baseline.query(query, frame), baseline.bruteForce(query));
        EXPECT_EQ(indexed.query(query, frame), indexed.bruteForce(query));
    }

    TEST(SceneUtilLightGrid, hidesDistantLightsWhileDisabled)
    {
        constexpr unsigned int frame = 1;
        GridFixture fixture(frame);
        const osg::BoundingSphere bound(osg::Vec3f(), 64.f);
        const osg::ref_ptr<LightSource> source = makeLight(64.f);
        source->setDistant(true);

        fixture.mManager->setDistantLightsEnabled(true);
        fixture.mManager->addDistantBound(source, bound, frame);
        fixture.mManager->update(frame + 1);
        EXPECT_EQ(fixture.query(bound, frame + 1), std::set{ source->getId() });

        fixture.mManager->setDistantLightsEnabled(false);
        fixture.mManager->update(frame + 2);
        EXPECT_TRUE(fixture.query(bound, frame + 2).empty());

        fixture.mManager->setDistantLightsEnabled(true);
        fixture.mManager->update(frame + 3);
        EXPECT_EQ(fixture.query(bound, frame + 3), std::set{ source->getId() });
    }

    TEST(SceneUtilLightGrid, tracksClusterRangePerBuffer)
    {
        SceneUtil::LightManagerCullCallback::ViewData view;
        const osg::Matrixd projection;

        EXPECT_TRUE(view.updateCluster(0, 8192.f, projection));
        EXPECT_TRUE(view.updateCluster(1, 8192.f, projection));
        EXPECT_FALSE(view.updateCluster(0, 8192.f, projection));
        EXPECT_FALSE(view.updateCluster(1, 8192.f, projection));
        EXPECT_TRUE(view.updateCluster(0, 16384.f, projection));
        EXPECT_TRUE(view.updateCluster(1, 16384.f, projection));
    }

    TEST(SceneUtilLightGrid, activationKeepsOneLight)
    {
        GridFixture fixture(1, true);
        const osg::BoundingSphere bound(osg::Vec3f(), 64.f);
        const auto distant = makeLight(64.f, true, 42);
        const auto active = makeLight(64.f, false, 42);
        fixture.mManager->addDistantBound(distant, bound, 1);
        fixture.mManager->update(2);
        ASSERT_EQ(fixture.query(bound, 2), std::set{ distant->getId() });
        fixture.mManager->addDistantBound(distant, bound, 2);

        for (unsigned int frame = 3; frame <= 8; ++frame)
        {
            SCOPED_TRACE(frame);
            fixture.mManager->update(frame);
            fixture.mManager->addLight(active, osg::Matrixf(), frame);
            EXPECT_EQ(fixture.query(bound, frame), std::set{ active->getId() });
        }
    }

    TEST(SceneUtilLightGrid, deactivationKeepsOneLight)
    {
        GridFixture fixture(1, true);
        const osg::BoundingSphere bound(osg::Vec3f(), 64.f);
        const osg::ref_ptr<LightSource> active = makeLight(64.f, false, 42);
        osg::ref_ptr<LightSource> distant = makeLight(64.f, true, 42);
        fixture.mManager->addLight(active, osg::Matrixf(), 1);
        ASSERT_EQ(fixture.query(bound, 1), std::set{ active->getId() });

        fixture.mManager->update(2);
        fixture.mManager->addDistantBound(distant, bound, 2);
        EXPECT_EQ(fixture.query(bound, 2), std::set{ distant->getId() });
        fixture.mManager->update(3);
        EXPECT_EQ(fixture.query(bound, 3), std::set{ distant->getId() });
    }

    TEST(SceneUtilLightGrid, deduplicatesPagedCopies)
    {
        GridFixture fixture(1, true);
        const osg::BoundingSphere bound(osg::Vec3f(), 64.f);
        const auto first = makeLight(64.f, true, 42);
        const auto second = makeLight(64.f, true, 42);
        fixture.mManager->addDistantBound(first, bound, 1);
        fixture.mManager->addDistantBound(second, bound, 1);
        fixture.mManager->update(2);
        EXPECT_EQ(fixture.query(bound, 2), std::set{ second->getId() });
    }

    TEST(SceneUtilLightGrid, prefersActiveDuringCull)
    {
        GridFixture fixture(1, true);
        const osg::BoundingSphere bound(osg::Vec3f(), 64.f);
        std::set<int> expected;
        for (std::uint64_t id : { 3, 1, 2 })
        {
            const osg::ref_ptr<LightSource> active = makeLight(64.f, false, id);
            fixture.mManager->addLight(active, osg::Matrixf(), 1);
            expected.insert(active->getId());
            osg::ref_ptr<LightSource> distant = makeLight(64.f, true, id);
            fixture.mManager->addDistantBound(distant, bound, 1);
        }
        EXPECT_EQ(fixture.query(bound, 1), expected);
    }

    TEST(SceneUtilLightGrid, keepsCoincidentReferences)
    {
        GridFixture fixture(1, true);
        const osg::BoundingSphere bound(osg::Vec3f(), 64.f);
        const osg::ref_ptr<LightSource> active = makeLight(64.f, false, 1);
        fixture.mManager->addLight(active, osg::Matrixf(), 1);
        std::set<int> expected{ active->getId() };
        for (std::uint64_t id : { 0, 2 })
        {
            osg::ref_ptr<LightSource> distant = makeLight(64.f, true, id);
            fixture.mManager->addDistantBound(distant, bound, 1);
            expected.insert(distant->getId());
        }
        EXPECT_EQ(fixture.query(bound, 1), expected);
    }

    TEST(SceneUtilLightGrid, usesActiveTransform)
    {
        GridFixture fixture(1, true);
        const osg::BoundingSphere origin(osg::Vec3f(), 64.f);
        const osg::BoundingSphere moved(osg::Vec3f(512.f, 0.f, 0.f), 64.f);
        osg::ref_ptr<LightSource> distant = makeLight(64.f, true, 42);
        fixture.mManager->addDistantBound(distant, origin, 1);
        const osg::ref_ptr<LightSource> active = makeLight(64.f, false, 42);
        fixture.mManager->addLight(active, osg::Matrixf::translate(moved.center()), 1);
        EXPECT_TRUE(fixture.query(origin, 1).empty());
        EXPECT_EQ(fixture.query(moved, 1), std::set{ active->getId() });
    }

    TEST(SceneUtilLightGrid, expiresPagedLights)
    {
        GridFixture fixture(1, true);
        const osg::BoundingSphere bound(osg::Vec3f(), 64.f);
        osg::ref_ptr<LightSource> distant = makeLight(64.f, true, 42);
        fixture.mManager->addDistantBound(distant, bound, 1);
        ASSERT_EQ(fixture.query(bound, 1), std::set{ distant->getId() });
        for (unsigned int frame = 2; frame <= 5; ++frame)
        {
            fixture.mManager->update(frame);
            EXPECT_EQ(fixture.query(bound, frame), std::set{ distant->getId() });
        }
        fixture.mManager->update(6);
        EXPECT_TRUE(fixture.query(bound, 6).empty());
    }

    TEST(SceneUtilLightGrid, retainsEarlierViewSources)
    {
        GridFixture fixture(1, true);
        const osg::BoundingSphere bound(osg::Vec3f(), 64.f);
        osg::ref_ptr<LightSource> first = makeLight(64.f, true, 42);
        fixture.mManager->addDistantBound(first, bound, 1);
        ASSERT_EQ(fixture.query(bound, 1), std::set{ first->getId() });
        const osg::observer_ptr<LightSource> observer(first);
        first = nullptr;

        osg::ref_ptr<LightSource> second = makeLight(64.f, true, 42);
        fixture.mManager->addDistantBound(second, bound, 1);
        const osg::ref_ptr<osg::Camera> camera = new osg::Camera;
        fixture.mStage->setCamera(camera);
        EXPECT_EQ(fixture.query(bound, 1), std::set{ second->getId() });
        EXPECT_TRUE(observer.valid());
        fixture.mStage->setCamera(fixture.mCamera);
        ASSERT_TRUE(observer.valid());
        EXPECT_EQ(fixture.query(bound, 1), std::set{ observer->getId() });
        fixture.mManager->update(2);
        EXPECT_FALSE(observer.valid());
    }

    TEST(SceneUtilLightGrid, separatesContentFiles)
    {
        EXPECT_EQ(SceneUtil::pagedLightId(ESM::RefNum{ 42, -1 }), 0u);
        EXPECT_NE(SceneUtil::pagedLightId(ESM::RefNum{ 42, 0 }), 0u);
        EXPECT_NE(SceneUtil::pagedLightId(ESM::RefNum{ 42, 0 }), SceneUtil::pagedLightId(ESM::RefNum{ 42, 1 }));
    }

    TEST(SceneUtilLightGrid, cullsSmallLights)
    {
        GridFixture fixture(1);
        fixture.enableClustered();
        for (float radius : { 3.f, 4.f, 6.f, 8.f })
            fixture.addLight(osg::Vec3f(0, 0, -100), radius, 1);
        const auto& lights = fixture.viewLights();
        ASSERT_EQ(lights.size(), 2u);
        EXPECT_EQ((std::set{ lights[0].mLightSource->getId(), lights[1].mLightSource->getId() }),
            (std::set{ fixture.mSources[2]->getId(), fixture.mSources[3]->getId() }));
        for (const auto& light : lights)
        {
            EXPECT_FLOAT_EQ(light.mFade, light.mLightSource == fixture.mSources[2] ? 0.5f : 1.f);
            EXPECT_FALSE(light.mCulled);
        }
    }

    TEST(SceneUtilLightGrid, appliesLightCutoffs)
    {
        struct Cutoff
        {
            float mDistance;
            float mFadeStart;
            float mPixels;
            float mPosition;
            float mRadius;
            float mExpected;
            bool mDistant;
        };
        for (const Cutoff& test :
            { Cutoff{ 100, 0.5f, 0, 50, 64, 1.f, true }, Cutoff{ 100, 0.5f, 0, 75, 64, 0.5f, true },
                Cutoff{ 100, 0.5f, 0, 100, 64, 0.f, true }, Cutoff{ 100, 0.5f, 0, 101, 64, 0.f, true },
                Cutoff{ 0, 1, 0, 20000, 0.01f, 1.f, false }, Cutoff{ 100, 1, 0, 99, 64, 1.f, false },
                Cutoff{ 100, 1, 0, 100, 64, 0.f, false }, Cutoff{ 200, 0.5f, 4, 150, 9, 0.25f, false } })
        {
            SCOPED_TRACE(test.mPosition);
            GridFixture fixture(1, test.mDistant);
            fixture.enableClustered(test.mDistance, test.mFadeStart, test.mPixels);
            const auto light = makeLight(test.mRadius, test.mDistant);
            const osg::Vec3f position(0, 0, -test.mPosition);
            if (test.mDistant)
                fixture.mManager->addDistantBound(light, osg::BoundingSphere(position, test.mRadius), 1);
            else
                fixture.mManager->addLight(light, osg::Matrixf::translate(position), 1);
            const auto& lights = fixture.viewLights();
            if (test.mExpected == 0.f)
                EXPECT_TRUE(lights.empty());
            else
            {
                ASSERT_EQ(lights.size(), 1u);
                EXPECT_EQ(lights[0].mLightSource, light);
                EXPECT_FLOAT_EQ(lights[0].mFade, test.mExpected);
            }
        }
    }

    TEST(SceneUtilLightGrid, keepsFadePerView)
    {
        GridFixture fixture(1);
        fixture.enableClustered(0, 0, 4);
        fixture.addLight(osg::Vec3f(0, 0, -100), 6, 1);
        const auto color = fixture.mSources[0]->getLight(1)->getDiffuse();
        const auto& first = fixture.viewLights();
        ASSERT_EQ(first.size(), 1u);
        EXPECT_FLOAT_EQ(first[0].mFade, 0.5f);

        const osg::ref_ptr<osg::Camera> camera = new osg::Camera;
        fixture.mStage->setCamera(camera);
        fixture.mCull->pushViewport(new osg::Viewport(0, 0, 400, 400));
        const auto& second = fixture.viewLights();
        ASSERT_EQ(second.size(), 1u);
        EXPECT_FLOAT_EQ(second[0].mFade, 1.f);
        EXPECT_FLOAT_EQ(first[0].mFade, 0.5f);
        EXPECT_EQ(fixture.mSources[0]->getLight(1)->getDiffuse(), color);
    }

    TEST(SceneUtilLightGrid, scalesProjectedBounds)
    {
        GridFixture fixture(1);
        fixture.enableClustered();
        const auto light = makeLight(3);
        fixture.mManager->addLight(light, osg::Matrixf::scale(2, 2, 2) * osg::Matrixf::translate(0, 0, -100), 1);
        ASSERT_EQ(fixture.viewLights().size(), 1u);
        EXPECT_FLOAT_EQ(fixture.viewLights()[0].mFade, 0.5f);
    }

    TEST(SceneUtilLightGrid, handlesOrthographicSize)
    {
        GridFixture fixture(1);
        fixture.enableClustered();
        fixture.mCull->pushProjectionMatrix(new osg::RefMatrix(osg::Matrix::ortho(-100, 100, -100, 100, 1, 10000)));
        fixture.addLight(osg::Vec3f(0, 0, -1000), 6, 1);
        ASSERT_EQ(fixture.viewLights().size(), 1u);
        EXPECT_FLOAT_EQ(fixture.viewLights()[0].mFade, 0.5f);
    }

    TEST(SceneUtilLightGrid, updatesLightCutoffs)
    {
        GridFixture fixture(1);
        fixture.enableClustered();
        fixture.addLight(osg::Vec3f(0, 0, -100), 3, 1);
        EXPECT_TRUE(fixture.viewLights().empty());
        fixture.mManager->processChangedSettings(1, 0, 0, 0);
        fixture.mManager->update(2);
        fixture.addLight(osg::Vec3f(0, 0, -100), 3, 2);
        ASSERT_EQ(fixture.viewLights(2).size(), 1u);
        EXPECT_FLOAT_EQ(fixture.viewLights(2)[0].mFade, 1.f);
    }
}
