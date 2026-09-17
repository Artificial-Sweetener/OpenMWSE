#include <apps/openmw/mwrender/watersurface.hpp>

#include <gtest/gtest.h>

namespace MWRender
{
    /// Keep the cached surface field available for geometric waves and transmitted caustics.
    TEST(WaterSurfaceParameters, EnablesCoverageForSupportedWaterEffects)
    {
        WaterSurfaceParameters parameters;
        EXPECT_FALSE(parameters.useSurfaceCoverage());

        parameters.mOptics.mCaustics = true;
        EXPECT_TRUE(parameters.useSurfaceCoverage());
    }
}
