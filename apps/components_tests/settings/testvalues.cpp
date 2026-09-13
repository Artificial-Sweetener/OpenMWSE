#include "components/files/configurationmanager.hpp"
#include "components/misc/strings/conversion.hpp"
#include "components/settings/atmospheremigration.hpp"
#include "components/settings/parser.hpp"
#include "components/settings/values.hpp"
#include "components/testing/util.hpp"

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include <array>
#include <fstream>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#ifndef OPENMW_PROJECT_SOURCE_DIR
#define OPENMW_PROJECT_SOURCE_DIR "."
#endif

namespace Settings
{
    namespace
    {
        using namespace testing;

        /// Exercise startup and persistence against the installed defaults with isolated user configuration layers.
        struct SettingsStartupTest : Test
        {
            std::filesystem::path mDirectory;
            CategorySettingValueMap mSavedDefaults;
            CategorySettingValueMap mSavedUserSettings;
            CategorySettingVector mSavedChanges;

            /// Keep every writable configuration path inside the test directory.
            void SetUp() override
            {
                mSavedDefaults = Manager::mDefaultSettings;
                mSavedUserSettings = Manager::mUserSettings;
                mSavedChanges = Manager::mChangedSettings;
                Manager::clear();
                mDirectory = TestingOpenMW::currentTestDirPath();
                std::ofstream(mDirectory / "openmw.cfg");
            }

            /// Restore the process-wide settings used by subsequent rendering tests, including their static index.
            void TearDown() override
            {
                Manager::clear();
                Manager::mDefaultSettings = std::move(mSavedDefaults);
                StaticValues::initDefaults();
                Manager::mUserSettings = std::move(mSavedUserSettings);
                StaticValues::init();
                Manager::mChangedSettings = std::move(mSavedChanges);
            }

            /// Load the same defaults and settings path used by the launcher without reading the personal profile.
            void load()
            {
                Manager::clear();
                Files::ConfigurationManager config(true);
                boost::program_options::options_description description;
                Files::ConfigurationManager::addCommonOptions(description);
                boost::program_options::variables_map variables;
                const std::string path = mDirectory.string();
                const char* args[] = { "settings-startup-test", "--replace", "config", "--config", path.c_str(),
                    "--user-data", path.c_str() };
                Files::parseArgs(static_cast<int>(std::size(args)), args, variables, description);
                config.readConfiguration(variables, description, true);
                ASSERT_EQ(config.getUserConfigPath(), mDirectory);
                ASSERT_EQ(Manager::load(config), mDirectory / "settings.cfg");
            }
        };

        TEST_F(SettingsStartupTest, loadsExistingConfigurationWithoutAtmospherePreferences)
        {
            std::ofstream(mDirectory / "settings.cfg") << "[Camera]\nfield of view = 77\n";
            ASSERT_NO_FATAL_FAILURE(load());
            EXPECT_EQ(camera().mFieldOfView.get(), 77);
            EXPECT_TRUE(shaders().mAtmosphericScattering);
            EXPECT_TRUE(shaders().mVolumetricClouds);
            EXPECT_EQ(shaders().mAtmosphericQuality.get(), 2);
            EXPECT_EQ(shaders().mAtmosphereAppearance.get(), 0);
            EXPECT_TRUE(shaders().mGodrays);
            EXPECT_FALSE(shaders().mSunGlare);
            EXPECT_TRUE(shaders().mCloudSunOcclusion);
        }

        TEST_F(SettingsStartupTest, distanceValuesPreserveCanonicalSettingsAcrossSaveAndReload)
        {
            std::ofstream(mDirectory / "settings.cfg")
                << "[Camera]\nviewing distance = 123456.5\n[Groundcover]\nrendering distance = 28672\n"
                   "[Shadows]\nmaximum shadow map distance = -1\nshadow map resolution = 1537\n";
            ASSERT_NO_FATAL_FAILURE(load());
            Manager::saveUser(mDirectory / "settings.cfg");
            ASSERT_NO_FATAL_FAILURE(load());
            EXPECT_EQ(camera().mViewingDistance.get(), 123456.5f);
            EXPECT_EQ(groundcover().mRenderingDistance.get(), 28672.f);
            EXPECT_EQ(shadows().mMaximumShadowMapDistance.get(), -1.f);
            EXPECT_EQ(shadows().mShadowMapResolution.get(), 1537);
        }

        TEST_F(SettingsStartupTest, migratesLegacyDisabledAtmosphereThroughSaveAndReload)
        {
            std::ofstream(mDirectory / "settings.cfg")
                << "[Shaders]\natmospheric effects = false\natmospheric quality = 1\n";
            ASSERT_NO_FATAL_FAILURE(load());
            EXPECT_FALSE(shaders().mAtmosphericScattering);
            EXPECT_FALSE(shaders().mVolumetricClouds);
            EXPECT_EQ(shaders().mAtmosphericQuality.get(), 1);
            Manager::saveUser(mDirectory / "settings.cfg");
            ASSERT_NO_FATAL_FAILURE(load());
            EXPECT_FALSE(shaders().mAtmosphericScattering);
            EXPECT_FALSE(shaders().mVolumetricClouds);
            EXPECT_EQ(shaders().mAtmosphericQuality.get(), 1);
        }

        TEST_F(SettingsStartupTest, explicitCloudChoiceOverridesLegacyBundledPreference)
        {
            std::ofstream(mDirectory / "settings.cfg")
                << "[Shaders]\natmospheric effects = false\nvolumetric clouds = true\n";
            ASSERT_NO_FATAL_FAILURE(load());
            EXPECT_FALSE(shaders().mAtmosphericScattering);
            EXPECT_TRUE(shaders().mVolumetricClouds);
        }

        TEST_F(SettingsStartupTest, preservesUnrecognizedSettingsWhenSavingSharedPreferences)
        {
            std::ofstream(mDirectory / "settings.cfg")
                << "[Camera]\nfield of view = 77\n[Shaders]\natmospheric scattering = false\n"
                   "volumetric clouds = true\n[Another OpenMW Version]\nretained preference = value\n";
            ASSERT_NO_FATAL_FAILURE(load());
            camera().mFieldOfView.set(81);
            Manager::saveUser(mDirectory / "settings.cfg");
            ASSERT_NO_FATAL_FAILURE(load());
            EXPECT_EQ(camera().mFieldOfView.get(), 81);
            EXPECT_FALSE(shaders().mAtmosphericScattering);
            EXPECT_TRUE(shaders().mVolumetricClouds);
            EXPECT_EQ(Manager::getString("retained preference", "Another OpenMW Version"), "value");
        }

        /// Preserve fork-owned shadow preferences while editing shared upstream shadow keys.
        TEST_F(SettingsStartupTest, preservesNamespacedShadowPreferencesAcrossSharedSettingsSave)
        {
            std::ofstream(mDirectory / "settings.cfg") << "[OpenMWSE Shadows]\nquality = legacy\ndistant shadows "
                                                          "= true\ndistant shadow quality = high\n"
                                                          "cloud shadow intensity = 0.35\n"
                                                          "[Shadows]\nshadow map resolution = 2048\n";
            ASSERT_NO_FATAL_FAILURE(load());
            shadows().mShadowMapResolution.set(1024);
            Manager::saveUser(mDirectory / "settings.cfg");
            ASSERT_NO_FATAL_FAILURE(load());
            EXPECT_EQ(openmwseShadows().mFilteringQuality.get(), "legacy");
            EXPECT_TRUE(openmwseShadows().mDistantShadows.get());
            EXPECT_EQ(openmwseShadows().mDistantShadowQuality.get(), "high");
            EXPECT_FLOAT_EQ(openmwseShadows().mCloudShadowIntensity.get(), 0.35f);
            EXPECT_EQ(shadows().mShadowMapResolution.get(), 1024);
        }

        struct SettingsValuesTest : Test
        {
            CategorySettingValueMap mSavedDefaults = Manager::mDefaultSettings;
            CategorySettingValueMap mSavedUserSettings = Manager::mUserSettings;
            CategorySettingVector mSavedChanges = Manager::mChangedSettings;
            const std::filesystem::path mSettingsDefaultPath = std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR }
                / "files" / Misc::StringUtils::stringToU8String("settings-default.cfg");

            SettingsValuesTest()
            {
                Manager::mDefaultSettings.clear();
                Manager::mUserSettings.clear();
                Manager::mChangedSettings.clear();
                SettingsFileParser parser;
                parser.loadSettingsFile(mSettingsDefaultPath, Manager::mDefaultSettings);
            }

            /// Keep local value-construction tests from changing later tests' process-wide settings maps.
            ~SettingsValuesTest() override
            {
                Manager::mDefaultSettings = std::move(mSavedDefaults);
                Manager::mUserSettings = std::move(mSavedUserSettings);
                Manager::mChangedSettings = std::move(mSavedChanges);
            }
        };

        TEST_F(SettingsValuesTest, atmosphereChoicesAreIndependentAndDefaultToCurrentQuality)
        {
            Index index;
            Values values(index);
            EXPECT_TRUE(values.mShaders.mAtmosphericScattering);
            EXPECT_TRUE(values.mShaders.mVolumetricClouds);
            EXPECT_EQ(values.mShaders.mAtmosphericQuality.get(), 2);
            values.mShaders.mAtmosphericScattering.set(false);
            EXPECT_TRUE(values.mShaders.mVolumetricClouds);
            values.mShaders.mVolumetricClouds.set(false);
            values.mShaders.mAtmosphericScattering.set(true);
            EXPECT_FALSE(values.mShaders.mVolumetricClouds);
            values.mShaders.mAtmosphericQuality.set(9);
            EXPECT_EQ(values.mShaders.mAtmosphericQuality.get(), 3);
            values.mShaders.mAtmosphericQuality.set(-1);
            EXPECT_EQ(values.mShaders.mAtmosphericQuality.get(), 0);
        }

        /// Keep fork shadow controls bounded and represented by stable human-readable values.
        TEST_F(SettingsValuesTest, openmwseShadowControlsUseStableDefaultsAndSanitizers)
        {
            Index index;
            Values values(index);
            EXPECT_EQ(values.mOpenMWSEShadows.mFilteringQuality.get(), "medium");
            EXPECT_FALSE(values.mOpenMWSEShadows.mDistantShadows.get());
            EXPECT_EQ(values.mOpenMWSEShadows.mDistantShadowQuality.get(), "medium");
            EXPECT_FLOAT_EQ(values.mOpenMWSEShadows.mCloudShadowIntensity.get(), 1.f);
            EXPECT_THROW(values.mOpenMWSEShadows.mFilteringQuality.set("invalid"), std::runtime_error);
            EXPECT_THROW(values.mOpenMWSEShadows.mDistantShadowQuality.set("invalid"), std::runtime_error);
            values.mOpenMWSEShadows.mCloudShadowIntensity.set(2.f);
            EXPECT_EQ(values.mOpenMWSEShadows.mFilteringQuality.get(), "medium");
            EXPECT_FLOAT_EQ(values.mOpenMWSEShadows.mCloudShadowIntensity.get(), 1.f);
        }

        /// Keep subordinate optimization selections dormant until the shared prerequisite is enabled.
        TEST_F(SettingsValuesTest, optionalOptimizationsRequireThePrerequisite)
        {
            Index index;
            Values values(index);
            const std::array options{ &values.mOptimizations.mGpuDrivenPaging, &values.mOptimizations.mGpuSkinning,
                &values.mOptimizations.mGpuOcclusionCulling, &values.mOptimizations.mGpuActors,
                &values.mOptimizations.mGpuTextureArrays, &values.mOptimizations.mIndexedLightLists,
                &values.mOptimizations.mGpuSceneLighting, &values.mOptimizations.mGpuStorageBudget,
                &values.mOptimizations.mLowEndIndirectBatching, &values.mOptimizations.mGpuLightOcclusion,
                &values.mOptimizations.mGpuDistantPaging, &values.mOptimizations.mSkipDormantRipples };

            EXPECT_FALSE(values.mOptimizations.mEnabled);
            for (SettingValue<bool>* option : options)
            {
                EXPECT_FALSE(*option);
                option->set(true);
                EXPECT_TRUE(*option);
                EXPECT_FALSE(values.mOptimizations.isActive(*option));
            }

            values.mOptimizations.mEnabled.set(true);
            for (const SettingValue<bool>* option : options)
                EXPECT_TRUE(values.mOptimizations.isActive(*option));

            values.mOptimizations.mEnabled.set(false);
            for (const SettingValue<bool>* option : options)
            {
                EXPECT_TRUE(*option);
                EXPECT_FALSE(values.mOptimizations.isActive(*option));
            }

            values.mOptimizations.mEnabled.set(true);
            EXPECT_TRUE(values.mOptimizations.useGpuDrivenPaging());
            EXPECT_TRUE(values.mOptimizations.useGpuActors());
            EXPECT_TRUE(values.mOptimizations.useGpuSkinning());
            EXPECT_TRUE(values.mOptimizations.useGpuTextureArrays());
            EXPECT_TRUE(values.mOptimizations.useGpuOcclusionCulling());
            EXPECT_TRUE(values.mOptimizations.useIndexedLightLists());
            EXPECT_TRUE(values.mOptimizations.useGpuSceneLighting());
            EXPECT_TRUE(values.mOptimizations.useGpuStorageBudget());
            EXPECT_TRUE(values.mOptimizations.useLowEndIndirectBatching());
            EXPECT_TRUE(values.mOptimizations.useGpuLightOcclusion());
            EXPECT_TRUE(values.mOptimizations.useGpuDistantPaging());
            EXPECT_TRUE(values.mOptimizations.useSkipDormantRipples());

            values.mOptimizations.mGpuActors.set(false);
            EXPECT_FALSE(values.mOptimizations.useGpuActors());
            EXPECT_FALSE(values.mOptimizations.useGpuSkinning());
            values.mOptimizations.mGpuActors.set(true);
            values.mOptimizations.mGpuOcclusionCulling.set(false);
            EXPECT_FALSE(values.mOptimizations.useGpuOcclusionCulling());
            EXPECT_FALSE(values.mOptimizations.useGpuLightOcclusion());
            values.mOptimizations.mGpuOcclusionCulling.set(true);
            values.mOptimizations.mGpuDrivenPaging.set(false);
            EXPECT_FALSE(values.mOptimizations.useGpuDrivenPaging());
            EXPECT_FALSE(values.mOptimizations.useGpuActors());
            EXPECT_FALSE(values.mOptimizations.useGpuSceneLighting());
            EXPECT_TRUE(values.mOptimizations.useIndexedLightLists());
            EXPECT_TRUE(values.mOptimizations.useSkipDormantRipples());

            values.mOptimizations.mEnabled.set(false);
            for (const SettingValue<bool>* option : options)
                EXPECT_FALSE(values.mOptimizations.isActive(*option));
        }

        /// Save and reload the prerequisite independently from retained subordinate selections.
        TEST_F(SettingsStartupTest, preservesDormantOptimizationSelectionsAcrossSaveAndReload)
        {
            std::ofstream(mDirectory / "settings.cfg")
                << "[Optimizations]\nenabled = false\ngpu driven paging = true\ngpu skinning = true\n"
                   "gpu occlusion culling = true\ngpu actors = true\ngpu texture arrays = true\n"
                   "indexed light lists = true\ngpu scene lighting = true\ngpu storage budget = true\n"
                   "low end indirect batching = true\ngpu light occlusion = true\ngpu distant paging = true\n"
                   "skip dormant ripples = true\n";
            const auto selections = [] {
                auto& settings = optimizations();
                return std::array{ &settings.mGpuDrivenPaging, &settings.mGpuSkinning, &settings.mGpuOcclusionCulling,
                    &settings.mGpuActors, &settings.mGpuTextureArrays, &settings.mIndexedLightLists,
                    &settings.mGpuSceneLighting, &settings.mGpuStorageBudget, &settings.mLowEndIndirectBatching,
                    &settings.mGpuLightOcclusion, &settings.mGpuDistantPaging, &settings.mSkipDormantRipples };
            };
            ASSERT_NO_FATAL_FAILURE(load());
            EXPECT_FALSE(optimizations().mEnabled);
            for (const SettingValue<bool>* selection : selections())
            {
                EXPECT_TRUE(*selection);
                EXPECT_FALSE(optimizations().isActive(*selection));
            }

            Manager::saveUser(mDirectory / "settings.cfg");
            ASSERT_NO_FATAL_FAILURE(load());
            EXPECT_FALSE(optimizations().mEnabled);
            for (const SettingValue<bool>* selection : selections())
            {
                EXPECT_TRUE(*selection);
                EXPECT_FALSE(optimizations().isActive(*selection));
            }
        }

        TEST_F(SettingsValuesTest, migrateBundledAtmosphereWithoutOverwritingExplicitChoices)
        {
            Manager::mUserSettings
                = { { { "Shaders", "atmospheric effects" }, "false" }, { { "Shaders", "volumetric clouds" }, "true" } };
            migrateAtmosphereSettings(Manager::mUserSettings);
            Index index;
            Values values(index);
            EXPECT_FALSE(values.mShaders.mAtmosphericScattering);
            EXPECT_TRUE(values.mShaders.mVolumetricClouds);
            EXPECT_EQ(Manager::mUserSettings.count({ "Shaders", "atmospheric effects" }), 0u);
        }

        TEST_F(SettingsValuesTest, migrateDisabledAtmosphereKeepsBothFeaturesDisabled)
        {
            Manager::mUserSettings = { { { "Shaders", "atmospheric effects" }, "false" } };
            migrateAtmosphereSettings(Manager::mUserSettings);
            Index index;
            Values values(index);
            EXPECT_FALSE(values.mShaders.mAtmosphericScattering);
            EXPECT_FALSE(values.mShaders.mVolumetricClouds);
        }

        TEST_F(SettingsValuesTest, shouldLoadFromSettingsManager)
        {
            Index index;
            Values values(index);
            EXPECT_EQ(values.mCamera.mFieldOfView.get(), 60);
        }

        TEST_F(SettingsValuesTest, shouldFillIndexOnLoad)
        {
            Index index;
            Values values(index);
            EXPECT_EQ(index.get<float>("Camera", "field of view").get(), 60);
        }

        TEST_F(SettingsValuesTest, constructorShouldThrowExceptionOnMissingSetting)
        {
            Manager::mDefaultSettings.erase({ "Camera", "field of view" });
            Index index;
            EXPECT_THROW([&] { Values values(index); }(), std::runtime_error);
        }

        TEST_F(SettingsValuesTest, constructorShouldSanitize)
        {
            Manager::mUserSettings[std::make_pair("Camera", "field of view")] = "-1";
            Index index;
            Values values(index);
            EXPECT_EQ(values.mCamera.mFieldOfView.get(), 1);
        }

        TEST_F(SettingsValuesTest, constructorWithDefaultShouldDoLookup)
        {
            Manager::mUserSettings[std::make_pair("category", "value")] = "13";
            Index index;
            SettingValue<int> value{ index, "category", "value", 42 };
            EXPECT_EQ(value.get(), 13);
            value.reset();
            EXPECT_EQ(value.get(), 42);
        }

        TEST_F(SettingsValuesTest, constructorWithDefaultShouldSanitize)
        {
            Manager::mUserSettings[std::make_pair("category", "value")] = "2";
            Index index;
            SettingValue<int> value{ index, "category", "value", -1, Settings::makeClampSanitizerInt(0, 1) };
            EXPECT_EQ(value.get(), 1);
            value.reset();
            EXPECT_EQ(value.get(), 0);
        }

        TEST_F(SettingsValuesTest, constructorWithDefaultShouldFallbackToDefault)
        {
            Index index;
            const SettingValue<int> value{ index, "category", "value", 42 };
            EXPECT_EQ(value.get(), 42);
        }

        TEST_F(SettingsValuesTest, moveConstructorShouldSetDefaults)
        {
            Index index;
            Values defaultValues(index);
            Manager::mUserSettings.emplace(std::make_pair("Camera", "field of view"), "61");
            Values values(std::move(defaultValues));
            EXPECT_EQ(values.mCamera.mFieldOfView.get(), 61);
            values.mCamera.mFieldOfView.reset();
            EXPECT_EQ(values.mCamera.mFieldOfView.get(), 60);
        }

        TEST_F(SettingsValuesTest, moveConstructorShouldSanitize)
        {
            Index index;
            Values defaultValues(index);
            Manager::mUserSettings[std::make_pair("Camera", "field of view")] = "-1";
            Values values(std::move(defaultValues));
            EXPECT_EQ(values.mCamera.mFieldOfView.get(), 1);
        }

        TEST_F(SettingsValuesTest, moveConstructorShouldThrowOnMissingSetting)
        {
            Index index;
            SettingValue<int> defaultValue{ index, "category", "value", 42 };
            EXPECT_THROW([&] { SettingValue<int> value(std::move(defaultValue)); }(), std::runtime_error);
        }

        TEST_F(SettingsValuesTest, findShouldThrowExceptionOnTypeMismatch)
        {
            Index index;
            Values values(index);
            EXPECT_THROW(index.find<int>("Camera", "field of view"), std::invalid_argument);
        }

        TEST_F(SettingsValuesTest, findShouldReturnNullptrForAbsentSetting)
        {
            Index index;
            Values values(index);
            EXPECT_EQ(index.find<int>("foo", "bar"), nullptr);
        }

        TEST_F(SettingsValuesTest, getShouldThrowExceptionForAbsentSetting)
        {
            Index index;
            Values values(index);
            EXPECT_THROW(index.get<int>("foo", "bar").get(), std::invalid_argument);
        }

        TEST_F(SettingsValuesTest, setShouldChangeManagerUserSettings)
        {
            Index index;
            Values values(index);
            values.mCamera.mFieldOfView.set(42);
            EXPECT_EQ(Manager::mUserSettings.at({ "Camera", "field of view" }), "42");
            EXPECT_THAT(Manager::mChangedSettings, ElementsAre(std::make_pair("Camera", "field of view")));
        }

        TEST_F(SettingsValuesTest, setShouldNotChangeManagerChangedSettingsForNoChange)
        {
            Index index;
            Values values(index);
            values.mCamera.mFieldOfView.set(values.mCamera.mFieldOfView.get());
            EXPECT_THAT(Manager::mChangedSettings, ElementsAre());
        }
    }
}
