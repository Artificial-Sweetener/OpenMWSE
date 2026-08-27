#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "apps/openmw/mwworld/atmospherepalette.hpp"

namespace MWWorld
{
    namespace
    {
        TEST(MWWorldAtmospherePaletteTest, loadsShippedRegionalPaletteAndCalculatesWeatherWeight)
        {
            const std::filesystem::path path
                = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data-mw/openmwse/atmosphere.json";
            std::ifstream stream(path);
            ASSERT_TRUE(stream.is_open());

            AtmospherePalette palette(stream);
            palette.setRegion("Ascadian Isles Region");

            const MWRender::AtmosphereState clear = palette.getState("Clear", {}, 0.f);
            EXPECT_TRUE(clear.mEnabled);
            EXPECT_FLOAT_EQ(clear.mWeatherWeight, 1.f);
            EXPECT_EQ(clear.mInScattering, osg::Vec3f(0.25f, 0.38f, 0.48f));
            EXPECT_EQ(clear.mOutScattering, osg::Vec3f(0.07f, 0.36f, 0.76f));

            EXPECT_FLOAT_EQ(palette.getState("Foggy", {}, 0.f).mWeatherWeight, 0.f);
            EXPECT_FLOAT_EQ(palette.getState("Clear", "Foggy", 0.5f).mWeatherWeight, 0.25f);
            EXPECT_FLOAT_EQ(palette.getState("Rain", "Cloudy", 0.5f).mWeatherWeight, 0.25f);
        }

    }
}
