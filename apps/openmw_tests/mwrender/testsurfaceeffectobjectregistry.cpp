#include <gtest/gtest.h>

#include <osg/Group>

#include <apps/openmw/mwrender/surfaceeffectobjectregistry.hpp>

namespace MWRender
{
    TEST(SurfaceEffectObjectRegistryTest, PrunesExpiredRootsBeforeLiveRebuild)
    {
        SurfaceEffectObjectRegistry registry;
        osg::ref_ptr<osg::Group> live = new osg::Group;
        osg::ref_ptr<osg::Group> expired = new osg::Group;
        const osg::Node* expiredAddress = expired.get();
        registry.remember(*live);
        registry.remember(*expired);

        expired = nullptr;
        const SurfaceEffectObjectRegistry::Snapshot snapshot = registry.lockLive();

        ASSERT_EQ(snapshot.mLive.size(), 1);
        EXPECT_EQ(snapshot.mLive.front(), live);
        ASSERT_EQ(snapshot.mExpired.size(), 1);
        EXPECT_EQ(snapshot.mExpired.front(), expiredAddress);
        EXPECT_TRUE(registry.contains(live.get()));
        EXPECT_FALSE(registry.contains(expiredAddress));
    }
}
