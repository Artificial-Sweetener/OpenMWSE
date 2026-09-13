#ifndef OPENMW_COMPONENTS_DIAGNOSTICS_OPTIMIZATIONSETTINGS_HPP
#define OPENMW_COMPONENTS_DIAGNOSTICS_OPTIMIZATIONSETTINGS_HPP

#include <string>
#include <string_view>

namespace Diagnostics
{
    /// Describe the prerequisite and every independently persisted optional optimization.
    struct OptimizationSettings
    {
        bool mEnabled = false;
        bool mGpuDrivenPaging = false;
        bool mGpuSkinning = false;
        bool mGpuOcclusionCulling = false;
        bool mGpuActors = false;
        bool mGpuTextureArrays = false;
        bool mIndexedLightLists = false;
        bool mGpuSceneLighting = false;
        bool mGpuStorageBudget = false;
        bool mLowEndIndirectBatching = false;
        bool mGpuLightOcclusion = false;
        bool mGpuDistantPaging = false;
        bool mSkipDormantRipples = false;

        /// Compare the prerequisite and every subordinate selection.
        bool operator==(const OptimizationSettings&) const = default;
    };

    /// Parse a complete versioned optimization selection and reject unknown or missing fields.
    OptimizationSettings deserializeOptimizationSettings(std::string_view text);

    /// Serialize every optimization selection for live replay and observed-state verification.
    std::string serializeOptimizationSettings(const OptimizationSettings& settings);
}

#endif
