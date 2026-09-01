#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "apps/openmw/mwworld/atmosphereenvironment.hpp"

namespace MWWorld
{
    namespace
    {
        TEST(MWWorldAtmosphereEnvironmentTest, accumulatesContinuousTraitsFromWeightedTerrainEvidence)
        {
            std::istringstream rules(R"({
                "version": 1,
                "textureRules": [
                    { "contains": ["mud", "moss"], "traits": { "wetland": 1.0 } },
                    { "contains": ["snow"], "traits": { "cold": 1.0 } },
                    { "contains": ["moss"], "traits": { "coastal": 0.5 } }
                ]
            })");
            AtmosphereEnvironmentClassifier classifier(rules);
            const std::array samples{
                AtmosphereEnvironmentSample{ "textures/tx_bc_mud.dds", 2.f },
                AtmosphereEnvironmentSample{ "textures/tx_moss.dds", 1.f },
                AtmosphereEnvironmentSample{ "textures/tx_snow.dds", 1.f },
            };

            const AtmosphereEnvironmentState state = classifier.classify(samples);
            EXPECT_FLOAT_EQ(state.get("wetland"), 0.75f);
            EXPECT_FLOAT_EQ(state.get("cold"), 0.25f);
            EXPECT_FLOAT_EQ(state.get("coastal"), 0.125f);
            EXPECT_FLOAT_EQ(state.get("unknown"), 0.f);
        }

        TEST(MWWorldAtmosphereEnvironmentTest, rejectsOutOfRangeTraitEvidence)
        {
            std::istringstream rules(R"({
                "version": 1,
                "textureRules": [
                    { "contains": ["mud"], "traits": { "wetland": 1.5 } }
                ]
            })");
            EXPECT_THROW(AtmosphereEnvironmentClassifier classifier(rules), std::runtime_error);
        }

        TEST(MWWorldAtmosphereEnvironmentTest, resolvesEveryEvidenceTierWithoutRegionNameOwnership)
        {
            std::istringstream rules(R"({
                "version": 1,
                "textureRules": [
                    { "contains": ["mud"], "traits": { "wetland": 0.8 } }
                ],
                "terrainEvidence": {
                    "waterProximity": { "edge": 1000, "full": 0, "traits": { "coastal": 0.5 } },
                    "elevation": { "edge": 1000, "full": 2000, "traits": { "highland": 0.4 } }
                },
                "diagnosticAssetRules": [
                    { "contains": ["pond_scum"], "traits": { "wetland": 0.6 } }
                ],
                "floraRules": [
                    { "contains": ["reed"], "traits": { "cultivated": 0.4 } }
                ],
                "weatherProbabilityRules": [
                    { "contains": ["snow"], "traits": { "cold": 0.5 } }
                ],
                "regionFallbackRules": [
                    { "contains": ["fallback highland"], "traits": { "highland": 0.2 } }
                ]
            })");
            AtmosphereEnvironmentClassifier classifier(rules);
            AtmosphereEnvironmentEvidence evidence;
            evidence.mTerrainHeightWorldUnits = 1500.f;
            evidence.mWaterHeightWorldUnits = 1000.f;
            evidence.mTerrainMaterials.push_back({ "textures/mud.dds", 1.f });
            evidence.mDiagnosticAssets.push_back({ "meshes/pond_scum.nif", 1.f });
            evidence.mFlora.push_back({ "meshes/reed.nif", 1.f });
            evidence.mWeatherProbabilities.push_back({ "Snow", 1.f });
            evidence.mRegionName = "Fallback Highland";
            AtmosphereEnvironmentState prior;
            prior.mTraits.emplace("dry", 0.3f);
            evidence.mMacroClimatePrior = prior;

            const AtmosphereEnvironmentState state = classifier.classify(evidence);
            EXPECT_FLOAT_EQ(state.mTerrainHeightWorldUnits, 1500.f);
            EXPECT_GT(state.get("wetland"), 0.9f);
            EXPECT_GT(state.get("coastal"), 0.f);
            EXPECT_FLOAT_EQ(state.get("cultivated"), 0.4f);
            EXPECT_FLOAT_EQ(state.get("cold"), 0.5f);
            EXPECT_FLOAT_EQ(state.get("dry"), 0.3f);
            EXPECT_FLOAT_EQ(state.get("highland"), 0.36f);
        }

        TEST(MWWorldAtmosphereEnvironmentTest, exactOverrideSuppressesAllAutomaticAndPriorEvidence)
        {
            std::istringstream rules(R"({
                "version": 1,
                "textureRules": [
                    { "contains": ["mud"], "traits": { "wetland": 1.0 } }
                ]
            })");
            AtmosphereEnvironmentClassifier classifier(rules);
            AtmosphereEnvironmentEvidence evidence;
            evidence.mTerrainMaterials.push_back({ "textures/mud.dds", 1.f });
            AtmosphereEnvironmentState macroClimatePrior;
            macroClimatePrior.mTraits.emplace("cold", 1.f);
            evidence.mMacroClimatePrior = macroClimatePrior;
            AtmosphereEnvironmentState override;
            override.mTraits.emplace("cultivated", 0.35f);
            override.mTerrainHeightWorldUnits = 42.f;
            evidence.mOverride = override;

            const AtmosphereEnvironmentState state = classifier.classify(evidence);
            EXPECT_FLOAT_EQ(state.get("cultivated"), 0.35f);
            EXPECT_FLOAT_EQ(state.get("wetland"), 0.f);
            EXPECT_FLOAT_EQ(state.get("cold"), 0.f);
            EXPECT_FLOAT_EQ(state.mTerrainHeightWorldUnits, 42.f);
        }

        TEST(MWWorldAtmosphereEnvironmentTest, canonicalRulesRecognizeMistifyCueWithoutFreezingSheogorad)
        {
            const std::filesystem::path rulesPath = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR)
                / "files/data-mw/openmwse/atmosphere-environments.json";
            std::ifstream rules(rulesPath);
            ASSERT_TRUE(rules.is_open());
            AtmosphereEnvironmentClassifier classifier(rules);

            AtmosphereEnvironmentEvidence wetland;
            wetland.mDiagnosticAssets.push_back({ "meshes/f/terrain_bc_scum_03.nif", 1.f });
            EXPECT_FLOAT_EQ(classifier.classify(wetland).get("wetland"), 0.72f);

            AtmosphereEnvironmentEvidence sheogorad;
            sheogorad.mRegionName = "Sheogorad";
            const AtmosphereEnvironmentState sheogoradState = classifier.classify(sheogorad);
            EXPECT_FLOAT_EQ(sheogoradState.get("coastal"), 0.2f);
            EXPECT_FLOAT_EQ(sheogoradState.get("cold"), 0.f);

            AtmosphereEnvironmentEvidence solstheim;
            solstheim.mRegionName = "Solstheim";
            EXPECT_FLOAT_EQ(classifier.classify(solstheim).get("cold"), 0.24f);
        }

        TEST(MWWorldAtmosphereEnvironmentTest, derivesSlopeAndValleyDepthFromSparseTerrainSamples)
        {
            const AtmosphereEnvironmentState state
                = makeAtmosphereOrographicState({ 12000.f, -4000.f }, 1000.f, 1800.f, 200.f, 1400.f, 1000.f, 4000.f);

            EXPECT_EQ(state.mTerrainSampleCenterWorldUnits, osg::Vec2f(12000.f, -4000.f));
            EXPECT_FLOAT_EQ(state.mTerrainHeightWorldUnits, 1000.f);
            EXPECT_FLOAT_EQ(state.mTerrainGradient.x(), 0.2f);
            EXPECT_FLOAT_EQ(state.mTerrainGradient.y(), 0.05f);
            EXPECT_FLOAT_EQ(state.mValleyDepthWorldUnits, 100.f);
        }

        TEST(MWWorldAtmosphereEnvironmentTest, recentRainAndDawnStrengthenMistWithoutChangingFogPolicy)
        {
            AtmosphereEnvironmentState wetland;
            wetland.mTraits.emplace("wetland", 1.f);
            AtmosphereCondensateFormationController controller;

            const float rainyDawn = controller.update(1.f, false, 1.f, 6.f, 6.f, wetland);
            EXPECT_GT(controller.getRecentPrecipitation(), 0.9f);
            EXPECT_GT(rainyDawn, 1.9f);

            const float postRainDawn = controller.update(1.f, false, 0.f, 6.5f, 6.f, wetland);
            EXPECT_GT(controller.getRecentPrecipitation(), 0.7f);
            EXPECT_GT(postRainDawn, 1.6f);

            const float dryAfternoon = controller.update(16.f, false, 0.f, 14.f, 6.f, wetland);
            EXPECT_LT(controller.getRecentPrecipitation(), 0.02f);
            EXPECT_NEAR(dryAfternoon, 1.f, 0.02f);
        }

        TEST(MWWorldAtmosphereEnvironmentTest, diagnosticRecentRainRejectsInvalidWeights)
        {
            AtmosphereCondensateFormationController controller;

            controller.setRecentPrecipitation(0.75f);
            EXPECT_FLOAT_EQ(controller.getRecentPrecipitation(), 0.75f);
            EXPECT_GT(controller.update(0.f, false, 0.f, 6.f, 6.f, {}), 1.3f);
            EXPECT_FLOAT_EQ(controller.getRecentPrecipitation(), 0.75f);
            EXPECT_THROW(controller.setRecentPrecipitation(-0.01f), std::invalid_argument);
            EXPECT_THROW(controller.setRecentPrecipitation(1.01f), std::invalid_argument);
            EXPECT_THROW(
                controller.setRecentPrecipitation(std::numeric_limits<float>::quiet_NaN()), std::invalid_argument);
        }
    }
}
