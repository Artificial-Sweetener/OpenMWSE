#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

#include "apps/openmw/mwworld/atmosphereenvironment.hpp"
#include "apps/openmw/mwworld/atmospherepalette.hpp"
#include "apps/openmw/mwworld/weather.hpp"

namespace MWWorld
{
    namespace
    {
        /// Build canonical weather definitions for regional palette transition tests.
        std::vector<Weather> makeWeatherDefinitions()
        {
            constexpr std::array<std::string_view, 10> weatherNames{ "Clear", "Cloudy", "Foggy", "Overcast", "Rain",
                "Thunderstorm", "Ashstorm", "Blight", "Snow", "Blizzard" };
            std::vector<Weather> result;
            result.reserve(weatherNames.size());
            for (std::size_t index = 0; index < weatherNames.size(); ++index)
                result.emplace_back(ESM::RefId{}, static_cast<int>(index), std::string(weatherNames[index]), 0.f, 0.f,
                    0.f, 0.f, std::string{});
            return result;
        }

        TEST(MWWorldAtmospherePaletteTest, loadsEnvironmentPaletteAndSeparateVolumeProfiles)
        {
            const std::filesystem::path palettePath
                = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data-mw/openmwse/atmosphere.json";
            const std::filesystem::path volumePath
                = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data-mw/openmwse/atmosphere-volumes.json";
            std::ifstream paletteStream(palettePath);
            std::ifstream volumeStream(volumePath);
            ASSERT_TRUE(paletteStream.is_open());
            ASSERT_TRUE(volumeStream.is_open());

            AtmospherePalette palette(paletteStream, volumeStream);
            AtmosphereEnvironmentState environment;
            environment.mTraits.emplace("cultivated", 1.f);
            environment.mTerrainHeightWorldUnits = 12000.f;
            palette.setEnvironment(environment);
            std::vector<Weather> weather = makeWeatherDefinitions();
            palette.update(20.f, false, weather);

            const MWRender::AtmosphereState clear = palette.getState("Clear", {}, 0.f);
            EXPECT_TRUE(clear.mEnabled);
            EXPECT_FLOAT_EQ(clear.mWeatherWeight, 1.f);
            EXPECT_EQ(clear.mInScattering, osg::Vec3f(0.25f, 0.38f, 0.48f));
            EXPECT_EQ(clear.mOutScattering, osg::Vec3f(0.07f, 0.36f, 0.76f));
            EXPECT_FLOAT_EQ(clear.mVolumes.mCloudCoverage, 0.22f);
            EXPECT_FLOAT_EQ(clear.mVolumes.mTerrainReferenceHeightWorldUnits, 12000.f);
            EXPECT_FLOAT_EQ(clear.mVolumes.mMistDensity, 0.0432f);
            EXPECT_FLOAT_EQ(clear.mVolumes.mShaftIntensity, 0.75f);

            EXPECT_FLOAT_EQ(palette.getState("Foggy", {}, 0.f).mWeatherWeight, 0.f);
            EXPECT_FLOAT_EQ(palette.getState("Clear", "Foggy", 0.5f).mWeatherWeight, 0.25f);
            EXPECT_FLOAT_EQ(palette.getState("Rain", "Cloudy", 0.5f).mWeatherWeight, 0.25f);

            const MWRender::AtmosphereState rainToCloudy = palette.getState("Rain", "Cloudy", 0.5f);
            EXPECT_FLOAT_EQ(rainToCloudy.mVolumes.mCloudCoverage, 0.77f);
            EXPECT_FLOAT_EQ(rainToCloudy.mVolumes.mMistDensity, 0.065f * 1.08f);
            EXPECT_FLOAT_EQ(rainToCloudy.mVolumes.mShaftIntensity, 0.575f);

            const MWRender::AtmosphereState cloudy = palette.getState("Cloudy", {}, 0.f);
            const MWRender::AtmosphereState thunderstorm = palette.getState("Thunderstorm", {}, 0.f);
            EXPECT_GT(thunderstorm.mVolumes.mCloudType, cloudy.mVolumes.mCloudType);
            EXPECT_GT(thunderstorm.mVolumes.mCloudVerticalDevelopment, cloudy.mVolumes.mCloudVerticalDevelopment);
            EXPECT_GT(thunderstorm.mVolumes.mCloudThicknessWorldUnits, cloudy.mVolumes.mCloudThicknessWorldUnits);
            EXPECT_GT(thunderstorm.mVolumes.mCloudAnvilStrength, cloudy.mVolumes.mCloudAnvilStrength);

            const MWRender::AtmosphereState foggy = palette.getState("Foggy", {}, 0.f);
            EXPECT_GT(foggy.mVolumes.mFogDensity, foggy.mVolumes.mMistDensity);
            EXPECT_GT(foggy.mVolumes.mFogThicknessWorldUnits, foggy.mVolumes.mMistThicknessWorldUnits);
            EXPECT_LT(foggy.mVolumes.mCloudType, cloudy.mVolumes.mCloudType);
            EXPECT_LT(foggy.mVolumes.mCloudVerticalDevelopment, cloudy.mVolumes.mCloudVerticalDevelopment);
            EXPECT_FLOAT_EQ(foggy.mVolumes.mCloudTerrainFollowing, 0.75f);
            EXPECT_FLOAT_EQ(foggy.mVolumes.mCloudTerrainClearanceWorldUnits, 1500.f);
        }

        TEST(MWWorldAtmospherePaletteTest, rejectsOutOfRangeVolumeProfilesWithFieldContext)
        {
            const std::filesystem::path palettePath
                = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data-mw/openmwse/atmosphere.json";
            const std::filesystem::path volumePath
                = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data-mw/openmwse/atmosphere-volumes.json";
            std::ifstream paletteStream(palettePath);
            std::ifstream volumeStream(volumePath);
            ASSERT_TRUE(paletteStream.is_open());
            ASSERT_TRUE(volumeStream.is_open());
            std::string contents((std::istreambuf_iterator<char>(volumeStream)), std::istreambuf_iterator<char>());
            const std::string valid = R"("cloudCoverage": 0.22)";
            const std::size_t position = contents.find(valid);
            ASSERT_NE(position, std::string::npos);
            contents.replace(position, valid.size(), R"("cloudCoverage": 1.8)");

            std::istringstream invalid(contents);
            try
            {
                AtmospherePalette palette(paletteStream, invalid);
                FAIL() << "Expected the invalid cloud coverage to be rejected";
            }
            catch (const std::runtime_error& error)
            {
                EXPECT_NE(std::string(error.what()).find("atmosphere-volumes.Clear.cloudCoverage"), std::string::npos);
            }
        }

        TEST(MWWorldAtmospherePaletteTest, transitionsEnvironmentColorsAndVolumeModifiersContinuously)
        {
            const std::filesystem::path palettePath
                = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data-mw/openmwse/atmosphere.json";
            const std::filesystem::path volumePath
                = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data-mw/openmwse/atmosphere-volumes.json";
            std::ifstream paletteStream(palettePath);
            std::ifstream volumeStream(volumePath);
            ASSERT_TRUE(paletteStream.is_open());
            ASSERT_TRUE(volumeStream.is_open());

            AtmospherePalette palette(paletteStream, volumeStream);
            std::vector<Weather> weather = makeWeatherDefinitions();
            AtmosphereEnvironmentState environment;
            environment.mTraits.emplace("cultivated", 1.f);
            palette.setEnvironment(environment);

            palette.update(0.f, false, weather);
            EXPECT_NEAR(weather.front().mSkyColor.getDayValue().r(), 0.68990815f, 1e-6f);
            palette.update(10.f, false, weather);
            EXPECT_NEAR(weather.front().mSkyColor.getDayValue().r(), 0.70238364f, 1e-6f);
            palette.update(10.f, false, weather);
            EXPECT_NEAR(weather.front().mSkyColor.getDayValue().r(), 0.70862132f, 1e-6f);

            const MWRender::AtmosphereState clear = palette.getState("Clear", {}, 0.f);
            EXPECT_FLOAT_EQ(clear.mVolumes.mCloudCoverage, 0.22f);
            EXPECT_FLOAT_EQ(clear.mVolumes.mCloudBaseAltitudeWorldUnits, 52000.f);
            EXPECT_NEAR(clear.mVolumes.mMistDensity, 0.0424f, 1e-6f);
        }

        TEST(MWWorldAtmospherePaletteTest, appliesDiagnosticEnvironmentWithoutTransitionDelay)
        {
            const std::filesystem::path palettePath
                = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data-mw/openmwse/atmosphere.json";
            const std::filesystem::path volumePath
                = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data-mw/openmwse/atmosphere-volumes.json";
            std::ifstream paletteStream(palettePath);
            std::ifstream volumeStream(volumePath);
            ASSERT_TRUE(paletteStream.is_open());
            ASSERT_TRUE(volumeStream.is_open());

            AtmospherePalette palette(paletteStream, volumeStream);
            AtmosphereEnvironmentState environment;
            environment.mTraits.emplace("wetland", 1.f);
            environment.mTerrainHeightWorldUnits = 42.f;
            palette.setDiagnosticEnvironment(environment);

            const MWRender::AtmosphereState clear = palette.getState("Clear", {}, 0.f);
            EXPECT_FLOAT_EQ(clear.mVolumes.mTerrainReferenceHeightWorldUnits, 42.f);
            EXPECT_FLOAT_EQ(clear.mVolumes.mMistDensity, 0.04f * 1.65f);
            EXPECT_GT(clear.mVolumes.mLowCloudCoverage, 0.03f);
        }
    }
}
