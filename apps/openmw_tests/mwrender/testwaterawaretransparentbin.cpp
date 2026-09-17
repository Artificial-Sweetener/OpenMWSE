#include <apps/openmw/mwrender/waterawaretransparentbin.hpp>

#include <gtest/gtest.h>

namespace MWRender
{
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
