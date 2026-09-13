#include <components/diagnostics/optimizationsettings.hpp>
#include <components/diagnostics/protocol.hpp>

#include <gtest/gtest.h>

namespace
{
    TEST(DiagnosticOptimizationSettings, RoundTripsPrerequisiteAndIndependentSelections)
    {
        Diagnostics::OptimizationSettings settings;
        settings.mGpuDrivenPaging = true;
        settings.mGpuSkinning = true;
        settings.mGpuActors = true;
        settings.mGpuTextureArrays = true;
        EXPECT_EQ(Diagnostics::deserializeOptimizationSettings(Diagnostics::serializeOptimizationSettings(settings)),
            settings);

        settings.mEnabled = true;
        settings.mGpuOcclusionCulling = true;
        settings.mIndexedLightLists = true;
        settings.mGpuSceneLighting = true;
        settings.mGpuStorageBudget = true;
        settings.mLowEndIndirectBatching = true;
        settings.mGpuLightOcclusion = true;
        settings.mGpuDistantPaging = true;
        settings.mSkipDormantRipples = true;
        EXPECT_EQ(Diagnostics::deserializeOptimizationSettings(Diagnostics::serializeOptimizationSettings(settings)),
            settings);
        EXPECT_TRUE(Diagnostics::isSupportedCommand(
            static_cast<std::uint16_t>(Diagnostics::CommandType::SetOptimizationSettings)));
    }

    TEST(DiagnosticOptimizationSettings, RejectsIncompleteUnknownAndOversizedUpdates)
    {
        const std::string valid = Diagnostics::serializeOptimizationSettings({});
        EXPECT_ANY_THROW(Diagnostics::deserializeOptimizationSettings("settings-version: 1"));
        EXPECT_ANY_THROW(Diagnostics::deserializeOptimizationSettings(valid + "\nunknown: true"));
        EXPECT_ANY_THROW(Diagnostics::deserializeOptimizationSettings(std::string(4097, 'x')));
    }
}
