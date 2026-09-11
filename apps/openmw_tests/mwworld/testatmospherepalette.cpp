#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
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

        /// Load canonical volume authoring data for public parser and episode tests.
        YAML::Node authoredVolumeProfiles()
        {
            return YAML::LoadFile(
                (std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data-mw/openmwse/atmosphere-volumes.json")
                    .string());
        }

        /// Construct the production palette owner with a modified volume document.
        std::unique_ptr<AtmospherePalette> paletteWithVolumes(const YAML::Node& volumes)
        {
            std::ifstream colors(
                std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data-mw/openmwse/atmosphere.json");
            std::istringstream source(YAML::Dump(volumes));
            return std::make_unique<AtmospherePalette>(colors, source);
        }

        /// Match the regional assignments and every weather color cycle from parity revision 15ee5e5d84.
        TEST(MWWorldAtmospherePaletteTest, originalRegionalColorsMatchParityAssignments)
        {
            const YAML::Node data = YAML::LoadFile(
                (std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data-mw/openmwse/atmosphere.json").string());
            const std::array<std::pair<std::string_view, std::string_view>, 18> assignments{
                { { "Thirsk Region", "Solstheim-Necro" }, { "Red Mountain Region", "BC-Necro" },
                    { "Brodir Grove Region", "Solstheim-Necro" }, { "Mournhold Region", "BC-Necro" },
                    { "West Gash Region", "WG-Necro" }, { "BITTER COAST REGION", "BC-Necro" },
                    { "Felsaad Coast Region", "Solstheim-Necro" }, { "Azura's Coast Region", "AC-Necro" },
                    { "Moesring Mountains Region", "Solstheim-Necro" }, { "Sheogorad", "SG-Necro" },
                    { "Grazelands Region", "BC-Necro" }, { "Firemoth Region", "BC-Necro" },
                    { "Hirstaang Forest Region", "Solstheim-Necro" }, { "Ashlands Region", "AL-Necro" },
                    { "Molag Mar Region", "BC-Necro" }, { "Ascadian Isles Region", "AI-Necro" },
                    { "Isinfier Plains Region", "Solstheim-Necro" }, { "mod-added region", "default" } }
            };
            auto palette = paletteWithVolumes(authoredVolumeProfiles());
            auto weather = makeWeatherDefinitions();
            palette->setColorSource(AtmosphereColorSource::Region);
            for (const auto& [region, name] : assignments)
            {
                SCOPED_TRACE(region);
                palette->setRegion(region);
                palette->update(20.f, false, weather);
                const YAML::Node expected = data["presets"][std::string(name)];
                for (const auto& definition : weather)
                {
                    const auto colors = expected[definition.mName];
                    const auto checkCycle = [&](const auto& actual, const std::string& prefix) {
                        const std::array values{ actual.getSunriseValue(), actual.getDayValue(),
                            actual.getSunsetValue(), actual.getNightValue() };
                        const std::array names{ "SunriseColor", "DayColor", "SunsetColor", "NightColor" };
                        for (std::size_t phase = 0; phase < values.size(); ++phase)
                            for (unsigned channel = 0; channel < 3; ++channel)
                                EXPECT_FLOAT_EQ(
                                    values[phase][channel], colors[prefix + names[phase]][channel].as<float>());
                    };
                    checkCycle(definition.mSkyColor, "sky");
                    checkCycle(definition.mFogColor, "fog");
                    checkCycle(definition.mAmbientColor, "ambient");
                    checkCycle(definition.mSunColor, "sun");
                }
                const auto state = palette->getState("Clear", {}, 0.f);
                for (unsigned channel = 0; channel < 3; ++channel)
                {
                    EXPECT_FLOAT_EQ(state.mInScattering[channel], expected["inscatter"][channel].as<float>());
                    EXPECT_FLOAT_EQ(state.mOutScattering[channel], expected["outscatter"][channel].as<float>());
                }
            }
        }

        /// Preserve regional transition timing, pause behavior, and cloud state across live color changes and replay.
        TEST(MWWorldAtmospherePaletteTest, regionalColorsSwitchLiveWithoutChangingCloudFormation)
        {
            auto palette = paletteWithVolumes(authoredVolumeProfiles());
            auto weather = makeWeatherDefinitions();
            AtmosphereEnvironmentState environment;
            environment.mTraits = { { "coastal", .6f }, { "wetland", .4f } };
            palette->setDiagnosticEnvironment(environment);
            palette->update(0.f, false, weather);
            const auto enhanced = palette->getState("Cloudy", {}, 0.f, 3u);
            const auto enhancedSky = weather.front().mSkyColor.getDayValue();
            palette->setColorSource(AtmosphereColorSource::Region);
            palette->update(0.f, false, weather);
            const auto from = weather.front().mSkyColor.getDayValue();
            palette->setRegion("Ascadian Isles Region");
            palette->update(10.f, false, weather);
            const auto midpoint = weather.front().mSkyColor.getDayValue();
            palette->update(5.f, true, weather);
            EXPECT_EQ(midpoint, weather.front().mSkyColor.getDayValue());
            auto replay = paletteWithVolumes(authoredVolumeProfiles());
            replay->restoreRuntimeState(palette->captureRuntimeState());
            replay->setColorSource(AtmosphereColorSource::Region);
            palette->update(10.f, false, weather);
            const auto endpoint = weather.front().mSkyColor.getDayValue();
            for (unsigned channel = 0; channel < 4; ++channel)
                EXPECT_NEAR(midpoint[channel], (from[channel] + endpoint[channel]) * .5f, 1e-6f);
            replay->update(10.f, false, weather);
            EXPECT_EQ(endpoint, weather.front().mSkyColor.getDayValue());
            EXPECT_EQ(palette->captureRuntimeState(), replay->captureRuntimeState());
            const auto regional = palette->getState("Cloudy", {}, 0.f, 3u);
            EXPECT_EQ(SceneUtil::packCloudLayer(enhanced.mVolumes.mCloudLayer),
                SceneUtil::packCloudLayer(regional.mVolumes.mCloudLayer));
            EXPECT_EQ(enhanced.mVolumes.mCloudBaseAltitudeWorldUnits, regional.mVolumes.mCloudBaseAltitudeWorldUnits);
            palette->setColorSource(AtmosphereColorSource::Environment);
            palette->update(0.f, true, weather);
            EXPECT_EQ(enhancedSky, weather.front().mSkyColor.getDayValue());
        }

        /// Accept pre-regional captures and reject malformed newer trajectories without partial mutation.
        TEST(MWWorldAtmospherePaletteTest, regionalRuntimeMigratesOldCapturesAtomically)
        {
            auto palette = paletteWithVolumes(authoredVolumeProfiles());
            auto weather = makeWeatherDefinitions();
            palette->setRegion("West Gash Region");
            palette->update(7.f, false, weather);
            const std::string original = palette->captureRuntimeState();
            auto invalid = YAML::Load(original);
            invalid["regional-colors"]["transition"] = 2.f;
            EXPECT_THROW(palette->restoreRuntimeState(YAML::Dump(invalid)), std::invalid_argument);
            EXPECT_EQ(original, palette->captureRuntimeState());
            invalid = YAML::Load(original);
            invalid["regional-colors"]["target"] = "missing";
            EXPECT_THROW(palette->restoreRuntimeState(YAML::Dump(invalid)), std::invalid_argument);
            EXPECT_EQ(original, palette->captureRuntimeState());
            auto old = YAML::Load(original);
            std::vector<std::string> regionalFields;
            for (const auto& entry : old)
                if (entry.first.as<std::string>().starts_with("regional-"))
                    regionalFields.push_back(entry.first.as<std::string>());
            for (const auto& name : regionalFields)
                old.remove(name);
            old["runtime-version"] = 1;
            auto restored = paletteWithVolumes(authoredVolumeProfiles());
            EXPECT_NO_THROW(restored->restoreRuntimeState(YAML::Dump(old)));
            const auto upgraded = YAML::Load(restored->captureRuntimeState());
            EXPECT_EQ(upgraded["runtime-version"].as<int>(), 2);
            for (const auto& entry : old)
                if (entry.first.as<std::string>() != "runtime-version")
                    EXPECT_EQ(YAML::Dump(entry.second), YAML::Dump(upgraded[entry.first.as<std::string>()]));
        }

        /// Publish custom secondary-layer controls through the normal palette boundary.
        TEST(MWWorldAtmospherePaletteTest, loadsCompleteAuthoredSourceLayer)
        {
            YAML::Node volumes = authoredVolumeProfiles();
            volumes["Clear"]["cloudLayer"] = YAML::Load(R"({profile: secondary, physicalScale: 0.5,
                coverageAmount: 1.3, typeMinimum: 0.2, gradientMedium: [0.4, 0.5, 0.6, 0.8],
                albedo: [0.2, 0.4, 0.6], extinctionCoefficient: [0.01, 0.02, 0.03],
                anvilDeformationMedium: [7, 0.2, 9, 0.1], windAngle: 0.3, detailNoiseModifier: 0.8})");
            const auto palette = paletteWithVolumes(volumes);
            SceneUtil::CloudLayer expected = SceneUtil::secondaryCloudLayer();
            expected.mPhysicalScale = .5f;
            expected.mShapeSkewMeters *= .5f;
            expected.mNoiseScalePerMeter /= .5f;
            expected.mCurlDisplacementMeters *= .5f;
            expected.mCoverageSkewMeters *= .5f;
            expected.mWeatherScalePerMeter /= .5f;
            expected.mWindSpeedMetersPerSecond *= .5f;
            expected.mCoverageWindSpeedMetersPerSecond *= .5f;
            expected.mCoverageAmount = 1.3f;
            expected.mTypeMinimum = .2f;
            expected.mGradientMedium = { .4f, .5f, .6f, .8f };
            expected.mAlbedo = { .2f, .4f, .6f };
            expected.mExtinctionPerMeter = { .01f, .02f, .03f };
            expected.mAnvilMedium = { 7.f, .2f, 9.f, .1f };
            expected.mWindAngleRadians = .3f;
            expected.mDetailErosion = .8f;
            EXPECT_EQ(SceneUtil::packCloudLayer(palette->getState("Clear", {}, 0.f).mVolumes.mCloudLayer),
                SceneUtil::packCloudLayer(expected));
        }

        /// Preserve source optical depth and formation shape when compacting the terrain preset.
        TEST(MWWorldAtmospherePaletteTest, loadsTerrainCloudPreset)
        {
            YAML::Node volumes = authoredVolumeProfiles();
            volumes["Clear"]["cloudLayer"] = YAML::Load("{profile: fair-weather, physicalScale: 0.5}");
            const auto palette = paletteWithVolumes(volumes);
            const auto layer = palette->getState("Clear", {}, 0.f).mVolumes.mCloudLayer;
            EXPECT_FLOAT_EQ(layer.mPhysicalScale, .5f);
            EXPECT_FLOAT_EQ(layer.mNoiseScalePerMeter, .0018f);
            EXPECT_FLOAT_EQ(layer.mWeatherScalePerMeter, .00007f);
            EXPECT_FLOAT_EQ(layer.mCurlScale, .15f);
            EXPECT_FLOAT_EQ(layer.mDetailErosion, .5f);
            EXPECT_FLOAT_EQ(layer.mCoverageMinimum, .25f);
            EXPECT_FLOAT_EQ(layer.mTypeAmount, 0.f);
            EXPECT_FLOAT_EQ(layer.mRainAmount, .9f);
            EXPECT_EQ(layer.mGradientSmall, osg::Vec4f(.01f, .07f, .08f, .14f));
            EXPECT_EQ(layer.mExtinctionPerMeter * .5f, SceneUtil::fairWeatherCloudLayer().mExtinctionPerMeter);
            EXPECT_FLOAT_EQ(layer.mWindSpeedMetersPerSecond, 10.f);
            EXPECT_FLOAT_EQ(layer.mCoverageWindSpeedMetersPerSecond, 17.5f);
        }

        /// Keep authored episode optics and gradients intact while realizing a stable weather identity.
        TEST(MWWorldAtmospherePaletteTest, episodeRecipesRemainAuthoritativeDuringTransitions)
        {
            YAML::Node volumes = authoredVolumeProfiles();
            auto family = volumes["cloudScenarios"]["FairCumulus"]["cloudLayer"];
            family["gradientLarge"] = YAML::Load("[0.02, 0.2, 0.6, 0.9]");
            family["extinctionCoefficient"] = YAML::Load("[0.02, 0.04, 0.06]");
            const auto palette = paletteWithVolumes(volumes);
            const auto endpoint = palette->getState("Clear", {}, 0.f, 1u).mOutgoingCloudVolumes;
            EXPECT_EQ(endpoint.mCloudLayer.mGradientLarge, osg::Vec4f(.02f, .2f, .6f, .9f));
            const float authoredScale = family["physicalScale"].as<float>();
            const osg::Vec3f authoredOptics(.02f, .04f, .06f);
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(endpoint.mCloudLayer.mExtinctionPerMeter[channel] * endpoint.mCloudLayer.mPhysicalScale,
                    authoredOptics[channel] * authoredScale, 1e-7f);
            for (float progress : { 0.f, .2f, .5f, .8f, 1.f })
            {
                const auto transition = palette->getState("Clear", "Rain", progress, 1u, 9u);
                EXPECT_EQ(SceneUtil::packCloudLayer(transition.mOutgoingCloudVolumes.mCloudLayer),
                    SceneUtil::packCloudLayer(endpoint.mCloudLayer));
            }
        }

        /// Grow and erode the same environmental field throughout every authored weather transition.
        TEST(MWWorldAtmospherePaletteTest, environmentalCloudCoverageFormsAndDispersesContinuously)
        {
            const auto palette = paletteWithVolumes(authoredVolumeProfiles());
            const auto weather = makeWeatherDefinitions();
            for (const Weather& from : weather)
                for (const Weather& to : weather)
                {
                    SCOPED_TRACE(from.mName + " -> " + to.mName);
                    const auto first = palette->getState(from.mName, {}, 0.f).mVolumes.mEnvironmentalCloudLayer;
                    const auto last = palette->getState(to.mName, {}, 0.f).mVolumes.mEnvironmentalCloudLayer;
                    for (const float progress : { 0.f, .1f, .4999f, .5f, .5001f, .9f, 1.f })
                    {
                        SCOPED_TRACE(progress);
                        const auto current
                            = palette->getState(from.mName, to.mName, progress).mVolumes.mEnvironmentalCloudLayer;
                        const float weight = progress * progress * (3.f - 2.f * progress);
                        EXPECT_NEAR(current.mCoverageAmount,
                            std::lerp(first.mCoverageAmount, last.mCoverageAmount, weight), 1e-6f);
                        EXPECT_FLOAT_EQ(current.mNoiseScalePerMeter, first.mNoiseScalePerMeter);
                        EXPECT_FLOAT_EQ(current.mWeatherScalePerMeter, first.mWeatherScalePerMeter);
                        EXPECT_FLOAT_EQ(current.mWindSpeedMetersPerSecond, first.mWindSpeedMetersPerSecond);
                    }
                }
        }

        /// Preserve continuous publication for custom environmental models as well as bundled weather profiles.
        TEST(MWWorldAtmospherePaletteTest, authoredEnvironmentalCloudControlsRemainContinuousAtWeatherMidpoint)
        {
            YAML::Node volumes = authoredVolumeProfiles();
            volumes["Clear"]["environmentalCloudLayer"] = YAML::Load("{profile: primary, physicalScale: 0.18}");
            volumes["Rain"]["environmentalCloudLayer"] = YAML::Load("{profile: secondary, physicalScale: 0.3}");
            const auto palette = paletteWithVolumes(volumes);
            const auto first
                = SceneUtil::packCloudLayer(palette->getState("Clear", {}, 0.f).mVolumes.mEnvironmentalCloudLayer);
            const auto last
                = SceneUtil::packCloudLayer(palette->getState("Rain", {}, 0.f).mVolumes.mEnvironmentalCloudLayer);
            const auto middle
                = SceneUtil::packCloudLayer(palette->getState("Clear", "Rain", .5f).mVolumes.mEnvironmentalCloudLayer);
            const auto before = SceneUtil::packCloudLayer(
                palette->getState("Clear", "Rain", .49999f).mVolumes.mEnvironmentalCloudLayer);
            const auto after = SceneUtil::packCloudLayer(
                palette->getState("Clear", "Rain", .50001f).mVolumes.mEnvironmentalCloudLayer);
            for (std::size_t field = 0; field < middle.size(); ++field)
                for (int channel = 0; channel < 4; ++channel)
                {
                    SCOPED_TRACE(field);
                    SCOPED_TRACE(channel);
                    const float scale = std::max(1.f, std::abs(last[field][channel] - first[field][channel]));
                    EXPECT_NEAR(
                        middle[field][channel], (first[field][channel] + last[field][channel]) * .5f, scale * 1e-6f);
                    EXPECT_NEAR(before[field][channel], after[field][channel], scale * 1e-4f);
                }
        }

        /// Reject malformed source gradients, optics, and episode references at the authoring boundary.
        TEST(MWWorldAtmospherePaletteTest, rejectsInvalidSourceModelsAndEpisodeChoices)
        {
            for (const std::string_view layer : { "{gradientSmall: [0.2, 0.2, 0.3, 0.4]}",
                     "{gradientLarge: [0, 0.1, 0.8, 1.1]}", "{gradientMedium: [0, 0.1, 0.9]}", "{totalNoiseScale: 0}",
                     "{coverageMinimum: 1.1}", "{extinctionCoefficient: [0.1, -0.1, 0.1]}", "{albedo: [.nan, 1, 1]}",
                     "{profile: unknown}", "{coverageAmount: 10.1}", "{typeAmount: 10.1}" })
            {
                SCOPED_TRACE(layer);
                YAML::Node volumes = authoredVolumeProfiles();
                volumes["Clear"]["cloudLayer"] = YAML::Load(std::string(layer));
                EXPECT_THROW(paletteWithVolumes(volumes), std::runtime_error);
            }
            YAML::Node volumes = authoredVolumeProfiles();
            volumes["Clear"]["cloudScenarios"][0] = "MissingFamily";
            EXPECT_THROW(paletteWithVolumes(volumes), std::runtime_error);
            volumes["Clear"]["cloudScenarios"] = YAML::Load("[FairCumulus]");
            EXPECT_THROW(paletteWithVolumes(volumes), std::runtime_error);
        }

        /// Preserve legacy volume placement and source-model customizations in files without episode tables.
        TEST(MWWorldAtmospherePaletteTest, volumeFilesWithoutEpisodesUseTheirAuthoredLayer)
        {
            YAML::Node volumes = authoredVolumeProfiles();
            volumes.remove("cloudScenarios");
            for (auto entry : volumes)
                entry.second.remove("cloudScenarios");
            volumes["Clear"]["cloudLayer"]["gradientLarge"] = YAML::Load("[0.1, 0.2, 0.3, 0.4]");
            auto palette = paletteWithVolumes(volumes);
            EXPECT_EQ(palette->getState("Clear", {}, 0.f, 1u).mVolumes.mCloudLayer.mGradientLarge,
                osg::Vec4f(.1f, .2f, .3f, .4f));
            volumes["Clear"].remove("cloudLayer");
            volumes["Clear"]["cloudDeckType"] = .3f;
            volumes["Clear"]["cloudFeatureScale"] = .5f;
            palette = paletteWithVolumes(volumes);
            const auto legacy = palette->getState("Clear", {}, 0.f).mVolumes;
            EXPECT_FLOAT_EQ(legacy.mCloudLayer.mTypeMinimum, .3f);
            EXPECT_FLOAT_EQ(legacy.mCloudLayer.mCoverageAmount, legacy.mCloudDeckCoverage * 2.f);
            EXPECT_FLOAT_EQ(legacy.mCloudLayer.mNoiseScalePerMeter, .0012f);
            EXPECT_FLOAT_EQ(legacy.mCloudBaseAltitudeWorldUnits, volumes["Clear"]["cloudAltitude"].as<float>());
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
            EXPECT_FLOAT_EQ(clear.mVolumes.mCloudDeckCoverage, 0.18f);

            EXPECT_FLOAT_EQ(clear.mVolumes.mTerrainReferenceHeightWorldUnits, 12000.f);
            EXPECT_FLOAT_EQ(clear.mVolumes.mMistDensity, 0.0432f);
            EXPECT_FLOAT_EQ(clear.mVolumes.mShaftIntensity, 1.f);

            EXPECT_FLOAT_EQ(palette.getState("Foggy", {}, 0.f).mWeatherWeight, 0.f);
            EXPECT_FLOAT_EQ(palette.getState("Clear", "Foggy", 0.5f).mWeatherWeight, 0.25f);
            EXPECT_FLOAT_EQ(palette.getState("Rain", "Cloudy", 0.5f).mWeatherWeight, 0.25f);

            const MWRender::AtmosphereState rainToCloudy = palette.getState("Rain", "Cloudy", 0.5f);
            EXPECT_NEAR(rainToCloudy.mVolumes.mCloudDeckCoverage, 0.72f, 1e-6f);
            EXPECT_NEAR(rainToCloudy.mVolumes.mMistDensity, 0.0702f, 1e-6f);

            const MWRender::AtmosphereState cloudy = palette.getState("Cloudy", {}, 0.f);
            const MWRender::AtmosphereState thunderstorm = palette.getState("Thunderstorm", {}, 0.f);
            EXPECT_LT(clear.mVolumes.mCloudThicknessWorldUnits, cloudy.mVolumes.mCloudThicknessWorldUnits);
            EXPECT_LT(cloudy.mVolumes.mCloudThicknessWorldUnits, thunderstorm.mVolumes.mCloudThicknessWorldUnits);

            EXPECT_GT(thunderstorm.mVolumes.mCloudDeckCoverage, 0.9f);

            const MWRender::AtmosphereState foggy = palette.getState("Foggy", {}, 0.f);
            EXPECT_GT(foggy.mVolumes.mFogDensity, foggy.mVolumes.mMistDensity);
            EXPECT_GT(foggy.mVolumes.mFogThicknessWorldUnits, foggy.mVolumes.mMistThicknessWorldUnits);

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
            const std::string valid = R"("cloudDeckCoverage": 0.18)";
            const std::size_t position = contents.find(valid);
            ASSERT_NE(position, std::string::npos);
            contents.replace(position, valid.size(), R"("cloudDeckCoverage": 1.8)");

            std::istringstream invalid(contents);
            try
            {
                AtmospherePalette palette(paletteStream, invalid);
                FAIL() << "Expected the invalid cloud coverage to be rejected";
            }
            catch (const std::runtime_error& error)
            {
                EXPECT_NE(
                    std::string(error.what()).find("atmosphere-volumes.Clear.cloudDeckCoverage"), std::string::npos);
            }
        }

        /// Accept a five-kilometre nominal convective shell while retaining a finite, field-identified loading bound.
        TEST(MWWorldAtmospherePaletteTest, boundsDeepConvectiveShellThickness)
        {
            const std::filesystem::path dataDirectory
                = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data-mw/openmwse";
            std::ifstream volumeStream(dataDirectory / "atmosphere-volumes.json");
            ASSERT_TRUE(volumeStream.is_open());
            const std::string original(
                (std::istreambuf_iterator<char>(volumeStream)), std::istreambuf_iterator<char>());
            const std::size_t thicknessStart = original.find("\"cloudThickness\":");
            ASSERT_NE(thicknessStart, std::string::npos);
            const std::string originalThickness
                = original.substr(thicknessStart, original.find(',', thicknessStart) - thicknessStart);
            const std::size_t position = original.find(originalThickness);
            ASSERT_NE(position, std::string::npos);

            for (const float thickness : { 1.f, 350000.f, 0.f, 350001.f })
            {
                std::string contents = original;
                contents.replace(
                    position, originalThickness.size(), "\"cloudThickness\": " + std::to_string(thickness));
                std::istringstream volumes(contents);
                std::ifstream paletteStream(dataDirectory / "atmosphere.json");
                ASSERT_TRUE(paletteStream.is_open());
                if (thickness >= 1.f && thickness <= 350000.f)
                {
                    AtmospherePalette palette(paletteStream, volumes);
                    EXPECT_FLOAT_EQ(palette.getState("Clear", {}, 0.f).mVolumes.mCloudThicknessWorldUnits, thickness);
                }
                else
                {
                    try
                    {
                        AtmospherePalette palette(paletteStream, volumes);
                        FAIL() << "Expected invalid shell thickness to be rejected: " << thickness;
                    }
                    catch (const std::runtime_error& error)
                    {
                        EXPECT_NE(std::string(error.what()).find("atmosphere-volumes.Clear.cloudThickness"),
                            std::string::npos);
                    }
                }
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
            const float cloudBase = palette.getState("Clear", {}, 0.f).mVolumes.mCloudBaseAltitudeWorldUnits;
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
            EXPECT_FLOAT_EQ(clear.mVolumes.mCloudDeckCoverage, 0.18f);
            EXPECT_FLOAT_EQ(clear.mVolumes.mCloudBaseAltitudeWorldUnits, cloudBase);
            EXPECT_NEAR(clear.mVolumes.mMistDensity, 0.0424f, 1e-6f);
        }

        /// Preserve the same world-space terrain plane when movement recenters its sampling origin.
        TEST(MWWorldAtmospherePaletteTest, recenteringTerrainSamplesPreservesWorldSpaceHeights)
        {
            for (const float elapsedSeconds : { 0.f, 1.f / 60.f, 0.5f })
            {
                const auto palette = paletteWithVolumes(authoredVolumeProfiles());
                std::vector<Weather> weather = makeWeatherDefinitions();
                AtmosphereEnvironmentState environment;
                environment.mTerrainHeightWorldUnits = 500.f;
                environment.mTerrainSampleCenterWorldUnits = { 10000.f, 20000.f };
                environment.mTerrainGradient = { 0.0330388f, 0.110573f };
                palette->setEnvironment(environment);
                palette->update(20.f, false, weather);
                const osg::Vec2f fixedWorldPosition{ 15000.f, 22000.f };
                const float expectedHeight = environment.mTerrainHeightWorldUnits
                    + (fixedWorldPosition - environment.mTerrainSampleCenterWorldUnits) * environment.mTerrainGradient;
                for (const osg::Vec2f displacement : { osg::Vec2f(2048.f, 445.f), osg::Vec2f(2048.f, 445.f) })
                {
                    environment.mTerrainSampleCenterWorldUnits += displacement;
                    environment.mTerrainHeightWorldUnits += displacement * environment.mTerrainGradient;
                    palette->setEnvironment(environment);
                    palette->update(elapsedSeconds, false, weather);
                    const auto volumes = palette->getState("Thunderstorm", {}, 0.f).mVolumes;
                    const float publishedHeight = volumes.mTerrainReferenceHeightWorldUnits
                        + (fixedWorldPosition - volumes.mTerrainSampleCenterWorldUnits) * volumes.mTerrainGradient;
                    EXPECT_NEAR(publishedHeight, expectedHeight, 0.001f) << "elapsed=" << elapsedSeconds;
                }
            }
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

        TEST(MWWorldAtmospherePaletteTest, usesTheCompleteVanillaTransitionForResolvedScenarios)
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

            const MWRender::AtmosphereState clear = palette.getState("Clear", {}, 0.f);
            const MWRender::AtmosphereState rain = palette.getState("Rain", {}, 0.f);
            const MWRender::AtmosphereState building = palette.getState("Clear", "Rain", 0.5f);
            EXPECT_FLOAT_EQ(building.mVolumes.mCloudDeckCoverage,
                std::lerp(clear.mVolumes.mCloudDeckCoverage, rain.mVolumes.mCloudDeckCoverage, 0.5f));

            const MWRender::AtmosphereState clearing = palette.getState("Rain", "Clear", 0.5f);
            EXPECT_FLOAT_EQ(clearing.mVolumes.mCloudDeckCoverage,
                std::lerp(rain.mVolumes.mCloudDeckCoverage, clear.mVolumes.mCloudDeckCoverage, 0.5f));

            EXPECT_FLOAT_EQ(clearing.mVolumes.mMistDensity,
                std::lerp(rain.mVolumes.mMistDensity, clear.mVolumes.mMistDensity, 0.5f));

            const MWRender::AtmosphereState early = palette.getState("Clear", "Rain", 0.25f);
            constexpr float earlyHermite = 0.15625f;
            EXPECT_NEAR(early.mVolumes.mCloudDeckCoverage,
                std::lerp(clear.mVolumes.mCloudDeckCoverage, rain.mVolumes.mCloudDeckCoverage, earlyHermite), 1e-6f);

            EXPECT_FLOAT_EQ(early.mCloudScenarioTransition, earlyHermite);

            EXPECT_FLOAT_EQ(palette.getState("Cloudy", "Thunderstorm", 0.f).mVolumes.mCloudDeckCoverage,
                palette.getState("Cloudy", {}, 0.f).mVolumes.mCloudDeckCoverage);
        }

        /// Resolve several credible episodes inside the same coarse weather without per-frame rerolls.
        /// Preserve full source models within episodes and use the secondary density profile for high clouds.
        TEST(MWWorldAtmospherePaletteTest, coarseWeatherIdsResolveStableDiverseCloudScenarios)
        {
            const std::filesystem::path dataDirectory
                = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data-mw/openmwse";
            std::ifstream paletteStream(dataDirectory / "atmosphere.json");
            std::ifstream volumeStream(dataDirectory / "atmosphere-volumes.json");
            ASSERT_TRUE(paletteStream.is_open());
            ASSERT_TRUE(volumeStream.is_open());
            AtmospherePalette palette(paletteStream, volumeStream);
            const auto cloudless = palette.getState("Clear", {}, 0.f, 8u).mVolumes;
            const auto fair = palette.getState("Clear", {}, 0.f, 1u).mVolumes;
            const auto anotherFair = palette.getState("Clear", {}, 0.f, 5u).mVolumes;
            const auto high = palette.getState("Clear", {}, 0.f, 3u).mVolumes;
            EXPECT_FLOAT_EQ(cloudless.mCloudLayer.mCoverageAmount, 0.f);
            EXPECT_FLOAT_EQ(cloudless.mCloudLayer.mCoverageMinimum, 0.f);
            EXPECT_EQ(SceneUtil::packCloudLayer(fair.mCloudLayer),
                SceneUtil::packCloudLayer(palette.getState("Clear", {}, 0.f, 1u).mVolumes.mCloudLayer));
            EXPECT_EQ(fair.mCloudScenarioIdentity, 1u);
            EXPECT_NE(SceneUtil::packCloudLayer(fair.mCloudLayer), SceneUtil::packCloudLayer(anotherFair.mCloudLayer));
            EXPECT_NE(fair.mCloudBaseAltitudeWorldUnits, anotherFair.mCloudBaseAltitudeWorldUnits);
            EXPECT_EQ(high.mCloudLayer.mGradientSmall, SceneUtil::secondaryCloudLayer().mGradientSmall);
            EXPECT_FLOAT_EQ(high.mCloudLayer.mDetailHeightFraction, 0.f);
            EXPECT_FLOAT_EQ(high.mCloudLayer.mDetailErosion, 1.f);
            EXPECT_LT(high.mCloudLayer.mExtinctionPerMeter.y(), fair.mCloudLayer.mExtinctionPerMeter.y());
            const float highCloudBottom = high.mCloudBaseAltitudeWorldUnits
                + high.mCloudThicknessWorldUnits * high.mCloudLayer.mGradientSmall.x();
            const float fairCloudBottom = fair.mCloudBaseAltitudeWorldUnits
                + fair.mCloudThicknessWorldUnits * fair.mCloudLayer.mGradientSmall.x();
            EXPECT_GT(highCloudBottom, fairCloudBottom);
            for (float transition : { 0.f, .1f, .5f, .9f, 1.f })
            {
                const auto state = palette.getState("Clear", "Cloudy", transition, 1u, 3u);
                EXPECT_EQ(SceneUtil::packCloudLayer(state.mOutgoingCloudVolumes.mCloudLayer),
                    SceneUtil::packCloudLayer(fair.mCloudLayer));
                EXPECT_EQ(SceneUtil::packCloudLayer(state.mIncomingCloudVolumes.mCloudLayer),
                    SceneUtil::packCloudLayer(palette.getState("Cloudy", {}, 0.f, 3u).mVolumes.mCloudLayer));
                EXPECT_EQ(state.mOutgoingCloudVolumes.mCloudScenarioIdentity, 1u);
                EXPECT_EQ(state.mIncomingCloudVolumes.mCloudScenarioIdentity, 3u);
            }
        }

        /// Choose a credible scenario once from trajectory, moisture, terrain climate, and episode sequence.
        TEST(MWWorldAtmospherePaletteTest, cloudScenarioIdentityEncodesMeteorologicalEpisodeContext)
        {
            const std::filesystem::path dataDirectory
                = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR) / "files/data-mw/openmwse";
            std::ifstream paletteStream(dataDirectory / "atmosphere.json");
            std::ifstream volumeStream(dataDirectory / "atmosphere-volumes.json");
            ASSERT_TRUE(paletteStream.is_open());
            ASSERT_TRUE(volumeStream.is_open());
            AtmospherePalette palette(paletteStream, volumeStream);

            AtmosphereEnvironmentState wetland;
            wetland.mTraits.emplace("wetland", 1.f);
            wetland.mTraits.emplace("coastal", 0.6f);
            const std::uint32_t clearing
                = palette.createCloudScenarioIdentity("Clear", "Thunderstorm", 17u, 0.8f, 15.f, 6.f, wetland);
            EXPECT_EQ(
                clearing, palette.createCloudScenarioIdentity("Clear", "Thunderstorm", 17u, 0.8f, 15.f, 6.f, wetland));
            EXPECT_NE(clearing & 7u, 0u);
            EXPECT_NE(clearing & 7u, 4u) << "A wet post-storm sky must not jump straight to cloudless";

            const std::uint32_t dawnFog
                = palette.createCloudScenarioIdentity("Foggy", "Clear", 18u, 0.2f, 6.f, 6.f, wetland);
            constexpr std::array<std::uint32_t, 4> moistFogSlots{ 0u, 4u, 5u, 6u };
            EXPECT_NE(std::ranges::find(moistFogSlots, dawnFog & 7u), moistFogSlots.end());
            EXPECT_NE(dawnFog, palette.createCloudScenarioIdentity("Foggy", "Clear", 19u, 0.2f, 6.f, 6.f, wetland));
        }

        /// Make empty Clear skies rare while preserving sparse clouds and the full range of Cloudy episodes.
        TEST(MWWorldAtmospherePaletteTest, clearEpisodesFavorCloudsAndCloudyEpisodesRemainDiverse)
        {
            const auto palette = paletteWithVolumes(authoredVolumeProfiles());
            std::array<unsigned, 8> clearCounts{};
            std::array<unsigned, 8> cloudyCounts{};
            constexpr unsigned episodeCount = 4096;
            unsigned emptyClearCount = 0;
            for (unsigned sequence = 1; sequence <= episodeCount; ++sequence)
            {
                const auto clearIdentity
                    = palette->createCloudScenarioIdentity("Clear", "Cloudy", sequence, 0.f, 12.f, 6.f, {});
                const auto cloudyIdentity
                    = palette->createCloudScenarioIdentity("Cloudy", "Clear", sequence, 0.f, 12.f, 6.f, {});
                ++clearCounts[clearIdentity & 7u];
                ++cloudyCounts[cloudyIdentity & 7u];
                const auto clear = palette->getState("Clear", {}, 0.f, clearIdentity).mVolumes;
                if (clear.mCloudLayer.mCoverageAmount == 0.f && clear.mCloudLayer.mCoverageMinimum == 0.f)
                    ++emptyClearCount;
            }
            EXPECT_GT(emptyClearCount, 0u);
            EXPECT_LT(emptyClearCount, episodeCount / 50u);
            EXPECT_GT(clearCounts[2] + clearCounts[4], episodeCount / 3u);
            EXPECT_LT(clearCounts[2] + clearCounts[4], episodeCount / 2u);
            for (const auto count : cloudyCounts)
            {
                EXPECT_GT(count, episodeCount / 12u);
                EXPECT_LT(count, episodeCount / 5u);
            }
            const auto almostClear = palette->getState("Clear", {}, 0.f, 4u).mVolumes;
            const auto fair = palette->getState("Clear", {}, 0.f, 1u).mVolumes;
            EXPECT_GT(almostClear.mCloudLayer.mCoverageAmount, 0.f);
            EXPECT_LT(almostClear.mCloudLayer.mCoverageAmount, fair.mCloudLayer.mCoverageAmount);
            EXPECT_FLOAT_EQ(almostClear.mCloudLayer.mCoverageMinimum, 0.f);
        }

        /// Exercise recurring Cloudy periods through the production transition owner and climate-conditioned selector.
        TEST(MWWorldAtmospherePaletteTest, repeatedCloudyWeatherRenewsAcrossAllFamiliesInHumidTerrain)
        {
            const auto palette = paletteWithVolumes(authoredVolumeProfiles());
            AtmosphereEnvironmentState environment;
            environment.mTraits.emplace("wetland", 1.f);
            environment.mTraits.emplace("highland", .8f);
            WeatherTransition transition;
            std::uint32_t sequence = 0;
            const auto select = [&](int weather, int origin) {
                EXPECT_EQ(weather, 1);
                EXPECT_EQ(origin, 1);
                return palette->createCloudScenarioIdentity(
                    "Cloudy", "Cloudy", ++sequence, .7f, 12.f, 6.f, environment);
            };
            transition.force(1, select(1, 1));
            std::array<unsigned, 8> familyCounts{};
            for (unsigned episode = 0; episode < 512; ++episode)
            {
                const auto outgoing = transition.mCurrentCloudScenario;
                transition.request(1, true, select);
                ASSERT_TRUE(transition.active());
                EXPECT_FALSE(transition.weatherActive());
                transition.advance(5.f, false, [](int) { return .1f; });
                const auto sky = palette->getState(
                    "Cloudy", "Cloudy", 1.f - transition.mRemaining, outgoing, transition.mNextCloudScenario);
                EXPECT_EQ(sky.mOutgoingCloudVolumes.mCloudScenarioIdentity, outgoing);
                EXPECT_EQ(sky.mIncomingCloudVolumes.mCloudScenarioIdentity, transition.mNextCloudScenario);
                EXPECT_FLOAT_EQ(sky.mCloudScenarioTransition, .5f);
                transition.advance(5.f, false, [](int) { return .1f; });
                ASSERT_FALSE(transition.active());
                EXPECT_EQ(transition.mCurrentWeather, 1);
                ++familyCounts[transition.mCurrentCloudScenario & 7u];
            }
            EXPECT_EQ(sequence, 513u);
            for (const auto count : familyCounts)
                EXPECT_GT(count, 0u) << "Terrain may favor a cloud structure but must not remove sky variety";
        }

        /// Continue a partially blended regional sky instead of restarting its low clouds from the default palette.
        TEST(MWWorldAtmospherePaletteTest, runtimeRestoreContinuesRegionalCloudCoverageAndLighting)
        {
            const auto live = paletteWithVolumes(authoredVolumeProfiles());
            const auto replay = paletteWithVolumes(authoredVolumeProfiles());
            auto liveWeather = makeWeatherDefinitions();
            auto replayWeather = makeWeatherDefinitions();
            AtmosphereEnvironmentState wetland;
            wetland.mTraits.emplace("wetland", 1.f);
            wetland.mTerrainHeightWorldUnits = 600.f;
            wetland.mTerrainSampleCenterWorldUnits.set(12000.f, -3000.f);
            wetland.mTerrainGradient.set(.07f, -.12f);
            wetland.mValleyDepthWorldUnits = 90.f;
            live->setEnvironment(wetland);
            live->update(7.f, false, liveWeather);
            const auto before = live->getState("Cloudy", "Clear", .35f, 5, 2).mVolumes;
            const auto neutral = replay->getState("Cloudy", "Clear", .35f, 5, 2).mVolumes;
            EXPECT_NE(before.mLowCloudCoverage, neutral.mLowCloudCoverage);
            replay->restoreRuntimeState(live->captureRuntimeState());
            for (float elapsed : { 0.f, .016f, .2f, 2.5f })
            {
                live->update(elapsed, false, liveWeather);
                replay->update(elapsed, false, replayWeather);
                const auto a = live->getState("Cloudy", "Clear", .35f, 5, 2);
                const auto b = replay->getState("Cloudy", "Clear", .35f, 5, 2);
                EXPECT_EQ(a.mInScattering, b.mInScattering);
                EXPECT_EQ(a.mOutScattering, b.mOutScattering);
                EXPECT_FLOAT_EQ(a.mVolumes.mLowCloudCoverage, b.mVolumes.mLowCloudCoverage);
                EXPECT_FLOAT_EQ(a.mVolumes.mLowCloudDensity, b.mVolumes.mLowCloudDensity);
                EXPECT_FLOAT_EQ(a.mVolumes.mLowCloudBaseAltitudeWorldUnits, b.mVolumes.mLowCloudBaseAltitudeWorldUnits);
                EXPECT_EQ(SceneUtil::packCloudLayer(a.mVolumes.mEnvironmentalCloudLayer),
                    SceneUtil::packCloudLayer(b.mVolumes.mEnvironmentalCloudLayer));
                EXPECT_EQ(live->captureRuntimeState(), replay->captureRuntimeState());
            }
            const auto original = replay->captureRuntimeState();
            auto invalid = YAML::Load(original);
            invalid["target-volumes-low-density"] = -.1f;
            EXPECT_THROW(replay->restoreRuntimeState(YAML::Dump(invalid)), std::invalid_argument);
            EXPECT_EQ(replay->captureRuntimeState(), original);
        }

        /// Reject invalid authored episode weights instead of silently changing selection probabilities.
        TEST(MWWorldAtmospherePaletteTest, rejectsInvalidCloudEpisodeWeights)
        {
            for (const auto weight : { "0", "-1", "1.5", "1001", ".nan" })
            {
                SCOPED_TRACE(weight);
                auto volumes = authoredVolumeProfiles();
                volumes["Clear"]["cloudScenarios"][0]
                    = YAML::Load(std::string("{family: Cloudless, weight: ") + weight + "}");
                EXPECT_THROW(paletteWithVolumes(volumes), std::runtime_error);
            }
        }
    }
}
