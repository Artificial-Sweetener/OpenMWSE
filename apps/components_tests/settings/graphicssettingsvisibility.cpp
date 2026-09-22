#include <gtest/gtest.h>

#include <components/settings/graphicssettingsvisibility.hpp>

namespace
{
    TEST(GraphicsSettingsVisibility, ResolvesEveryWindowMode)
    {
        const auto fullscreen = Settings::getDisplayControlVisibility(Settings::WindowMode::Fullscreen);
        EXPECT_FALSE(fullscreen.mWindowBorder);
        EXPECT_TRUE(fullscreen.mResolution);
        EXPECT_FALSE(fullscreen.mBorderlessHint);

        const auto borderless = Settings::getDisplayControlVisibility(Settings::WindowMode::WindowedFullscreen);
        EXPECT_FALSE(borderless.mWindowBorder);
        EXPECT_TRUE(borderless.mResolution);
        EXPECT_TRUE(borderless.mBorderlessHint);

        const auto windowed = Settings::getDisplayControlVisibility(Settings::WindowMode::Windowed);
        EXPECT_TRUE(windowed.mWindowBorder);
        EXPECT_TRUE(windowed.mResolution);
        EXPECT_FALSE(windowed.mBorderlessHint);
    }

    TEST(GraphicsSettingsVisibility, BorderlessWindowSizeDoesNotExceedTheDisplay)
    {
        EXPECT_EQ(Settings::clampWindowSize(Settings::WindowMode::WindowedFullscreen, 2560, 1440, 3840, 2160),
            (std::pair{ 2560, 1440 }));
        EXPECT_EQ(Settings::clampWindowSize(Settings::WindowMode::WindowedFullscreen, 5120, 2880, 3840, 2160),
            (std::pair{ 3840, 2160 }));
        EXPECT_EQ(Settings::clampWindowSize(Settings::WindowMode::Windowed, 5120, 2880, 3840, 2160),
            (std::pair{ 5120, 2880 }));
    }

    TEST(GraphicsSettingsVisibility, AtmosphereQualityFollowsAnyQualityConsumer)
    {
        for (int mask = 0; mask < 8; ++mask)
        {
            const bool scattering = (mask & 1) != 0;
            const bool clouds = (mask & 2) != 0;
            const bool godrays = (mask & 4) != 0;
            const auto visibility = Settings::getAtmosphereControlVisibility({ scattering, clouds, godrays });
            EXPECT_EQ(visibility.mAppearance, scattering);
            EXPECT_EQ(visibility.mQuality, scattering || clouds || godrays);
        }
    }

    TEST(GraphicsSettingsVisibility, WaterDependenciesCollapseFromTheirImmediateParent)
    {
        const auto disabled = Settings::getWaterControlVisibility({ false, true, true, true });
        EXPECT_FALSE(disabled.mShaderControls);
        EXPECT_FALSE(disabled.mMeshControls);
        EXPECT_FALSE(disabled.mEnhancedControls);
        EXPECT_FALSE(disabled.mCausticsStrength);

        const auto basic = Settings::getWaterControlVisibility({ true, false, false, false });
        EXPECT_TRUE(basic.mShaderControls);
        EXPECT_FALSE(basic.mMeshControls);
        EXPECT_FALSE(basic.mEnhancedControls);
        EXPECT_FALSE(basic.mCausticsStrength);

        const auto meshDisabled = Settings::getWaterControlVisibility({ true, false, true, true });
        EXPECT_FALSE(meshDisabled.mMeshControls);
        EXPECT_TRUE(meshDisabled.mEnhancedControls);
        EXPECT_TRUE(meshDisabled.mCausticsStrength);

        const auto enhanced = Settings::getWaterControlVisibility({ true, true, true, true });
        EXPECT_TRUE(enhanced.mMeshControls);
        EXPECT_TRUE(enhanced.mEnhancedControls);
        EXPECT_TRUE(enhanced.mCausticsStrength);
    }

    TEST(GraphicsSettingsVisibility, ShadowDependenciesCoverGeometryCloudAndFilteringCombinations)
    {
        for (int mask = 0; mask < 16; ++mask)
        {
            const bool geometry = (mask & 1) != 0;
            const bool clouds = (mask & 2) != 0;
            const bool filtered = (mask & 4) != 0;
            const bool distant = (mask & 8) != 0;
            const auto visibility = Settings::getShadowControlVisibility({ geometry, clouds, filtered, distant });
            EXPECT_EQ(visibility.mFiltering, geometry || clouds);
            EXPECT_EQ(visibility.mDistantShadows, geometry && filtered);
            EXPECT_EQ(visibility.mDistantQuality, geometry && filtered && distant);
            EXPECT_EQ(visibility.mCloudIntensity, clouds && filtered);
            EXPECT_EQ(visibility.mGeometryControls, geometry);
        }
    }

    TEST(GraphicsSettingsVisibility, OptimizationRowsFollowTheFullPrerequisiteChain)
    {
        const auto disabled = Settings::getOptimizationControlVisibility({ false, true, true, true, true });
        EXPECT_EQ(disabled, (std::array<bool, 12>{}));

        const auto rootOnly = Settings::getOptimizationControlVisibility({ true, false, true, true, true });
        EXPECT_EQ(rootOnly,
            (std::array<bool, 12>{ true, false, false, false, false, true, false, false, false, false, false, true }));

        const auto paging = Settings::getOptimizationControlVisibility({ true, true, false, false, false });
        EXPECT_EQ(
            paging, (std::array<bool, 12>{ true, false, true, true, true, true, true, true, true, false, true, true }));

        const auto complete = Settings::getOptimizationControlVisibility({ true, true, true, true, true });
        EXPECT_EQ(
            complete, (std::array<bool, 12>{ true, true, true, true, true, true, true, true, true, true, true, true }));
    }

    TEST(GraphicsSettingsVisibility, LightingDistinguishesUnavailableForcedAndUserSelectedStates)
    {
        const auto unsupported = Settings::getLightingControlVisibility({ false, false, true, false });
        EXPECT_FALSE(unsupported.mClusteredEffective);
        EXPECT_EQ(unsupported.mClusteredConstraint, Settings::ClusteredLightingConstraint::Unsupported);
        EXPECT_TRUE(unsupported.mClassicFalloff);
        EXPECT_TRUE(unsupported.mMaximumLights);

        const auto selected = Settings::getLightingControlVisibility({ true, false, true, false });
        EXPECT_TRUE(selected.mClusteredEffective);
        EXPECT_EQ(selected.mClusteredConstraint, Settings::ClusteredLightingConstraint::None);
        EXPECT_FALSE(selected.mClassicFalloff);
        EXPECT_FALSE(selected.mMaximumLights);
        EXPECT_TRUE(selected.mMinimumBrightness);

        const auto forced = Settings::getLightingControlVisibility({ true, true, false, false });
        EXPECT_TRUE(forced.mClusteredEffective);
        EXPECT_EQ(forced.mClusteredConstraint, Settings::ClusteredLightingConstraint::ForcedByGpuSceneLighting);
        EXPECT_TRUE(forced.mClassicFalloff);
        EXPECT_FALSE(forced.mMaximumLights);
        EXPECT_TRUE(forced.mMinimumLightRadius);
    }
}
