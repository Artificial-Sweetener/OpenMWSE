#ifndef OPENMW_COMPONENTS_DIAGNOSTICS_DISTANCESETTINGS_H
#define OPENMW_COMPONENTS_DIAGNOSTICS_DISTANCESETTINGS_H

#include <optional>
#include <string>
#include <string_view>

namespace Diagnostics
{
    /// Describe the custom exterior fog range and the scaling policy for each endpoint.
    struct FogDistanceSettings
    {
        bool mEnabled = true;
        float mStart = 24576.f;
        float mEnd = 221184.f;
        bool mStartLocked = false;
        bool mEndLocked = false;

        /// Compare the complete fog-range preference snapshot.
        bool operator==(const FogDistanceSettings&) const = default;
    };

    /// Describe canonical world-unit distances and the editor used for isolated menu verification.
    struct DistanceSettings
    {
        float mView = 7168;
        float mGrass = 6144;
        float mShadow = 8192;
        bool mUseCellEditor = true;
        std::optional<int> mShadowResolution;
        std::optional<FogDistanceSettings> mFog;

        /// Compare stored distances and the input editor.
        bool operator==(const DistanceSettings&) const = default;
    };

    /// Reject malformed, unknown, non-finite, or out-of-domain values before touching live controls.
    DistanceSettings deserializeDistanceSettings(std::string_view text);
    /// Serialize a complete preference snapshot with explicit world-unit field names.
    std::string serializeDistanceSettings(const DistanceSettings& settings);
}

#endif
