#ifndef OPENMW_APPS_OPENMW_DIAGNOSTICCTL_SETTINGSSEQUENCES_HPP
#define OPENMW_APPS_OPENMW_DIAGNOSTICCTL_SETTINGSSEQUENCES_HPP

#include <filesystem>
#include <span>
#include <vector>

#include <components/diagnostics/atmospheresettings.hpp>
#include <components/diagnostics/distancesettings.hpp>
#include <components/diagnostics/optimizationsettings.hpp>
#include <components/diagnostics/watersettings.hpp>

namespace DiagnosticCtl
{
    /// Describe the route segmentation and artifact destination shared by live settings sequences.
    struct SettingsSequenceContext
    {
        std::size_t mFrameCount = 0;
        std::size_t mSegmentFrameCount = 0;
        std::filesystem::path mArtifacts;
    };

    /// Load and preserve the optional atmosphere settings sequence for one route.
    std::vector<Diagnostics::AtmosphereSettings> loadAtmosphereSettingsSequence(
        std::span<char*> arguments, const SettingsSequenceContext& context);

    /// Load and preserve the optional distance settings sequence for one route.
    std::vector<Diagnostics::DistanceSettings> loadDistanceSettingsSequence(
        std::span<char*> arguments, const SettingsSequenceContext& context);

    /// Load and preserve the optional optimization settings sequence for one route.
    std::vector<Diagnostics::OptimizationSettings> loadOptimizationSettingsSequence(
        std::span<char*> arguments, const SettingsSequenceContext& context);

    /// Load and preserve the optional displacement-and-caustics settings sequence for one route.
    std::vector<Diagnostics::WaterSettings> loadWaterSettingsSequence(
        std::span<char*> arguments, const SettingsSequenceContext& context);
}

#endif
