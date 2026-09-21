#include <apps/openmw/mwrender/waterawaretransparentbin.hpp>

#include <gtest/gtest.h>

namespace MWRender
{
    TEST(WaterAwareTransparentBin, ResolvesSceneOnceBeforeAllWaterSurfaces)
    {
        const WaterDrawPlan plan = makeWaterDrawPlan(true, true);

        ASSERT_EQ(plan.mSize, 3);
        EXPECT_EQ(plan.mStages[0], WaterDrawStage::ResolveScene);
        EXPECT_EQ(plan.mStages[1], WaterDrawStage::MeshWater);
        EXPECT_EQ(plan.mStages[2], WaterDrawStage::WorldWater);
    }

    TEST(WaterAwareTransparentBin, KeepsSingleSurfacePlansFreeOfFeedback)
    {
        const WaterDrawPlan meshPlan = makeWaterDrawPlan(true, false);
        ASSERT_EQ(meshPlan.mSize, 2);
        EXPECT_EQ(meshPlan.mStages[0], WaterDrawStage::ResolveScene);
        EXPECT_EQ(meshPlan.mStages[1], WaterDrawStage::MeshWater);

        const WaterDrawPlan worldPlan = makeWaterDrawPlan(false, true);
        ASSERT_EQ(worldPlan.mSize, 2);
        EXPECT_EQ(worldPlan.mStages[0], WaterDrawStage::ResolveScene);
        EXPECT_EQ(worldPlan.mStages[1], WaterDrawStage::WorldWater);
    }

    TEST(WaterAwareTransparentBin, KeepsSlightlySubmergedSurfaceCoveringsOnCameraSide)
    {
        constexpr double clipHeight = -4.0;

        EXPECT_EQ(classifyWaterDrawSide(-1.0, -1.0, clipHeight, false), WaterDrawSide::Camera);
        EXPECT_EQ(classifyWaterDrawSide(-5.0, -5.0, clipHeight, false), WaterDrawSide::Opposite);
        EXPECT_EQ(classifyWaterDrawSide(-5.0, -3.0, clipHeight, false), WaterDrawSide::Straddling);
    }

    TEST(WaterAwareTransparentBin, UsesMirroredOrderingForUnderwaterCamera)
    {
        constexpr double clipHeight = 4.0;

        EXPECT_EQ(classifyWaterDrawSide(1.0, 1.0, clipHeight, true), WaterDrawSide::Camera);
        EXPECT_EQ(classifyWaterDrawSide(5.0, 5.0, clipHeight, true), WaterDrawSide::Opposite);
        EXPECT_EQ(classifyWaterDrawSide(3.0, 5.0, clipHeight, true), WaterDrawSide::Straddling);
    }
}
