#ifndef OPENMW_COMPONENTS_DIAGNOSTICS_WATERSETTINGS_H
#define OPENMW_COMPONENTS_DIAGNOSTICS_WATERSETTINGS_H

#include <string>
#include <string_view>

namespace Diagnostics
{
    /// Describe live displacement and optical controls exercised by isolated water verification.
    struct WaterSettings
    {
        float mGeometricWaveHeight = 0.f;
        bool mCaustics = false;
        float mCausticsStrength = 1.f;
        int mInteriorLocalLights = 2;
        int mExteriorLocalLights = 0;
        std::string mAppearance = "openmw";
        bool mMeshWater = true;
        float mMeshGeometricWaveHeight = 0.f;

        /// Compare the complete requested water-control state.
        bool operator==(const WaterSettings&) const = default;
    };

    /// Reject malformed, unknown, non-finite, or out-of-range water controls before touching the live menu.
    WaterSettings deserializeWaterSettings(std::string_view text);

    /// Serialize one complete live water-control snapshot.
    std::string serializeWaterSettings(const WaterSettings& settings);
}

#endif
