#ifndef OPENMW_COMPONENTS_DIAGNOSTICS_ATMOSPHERESETTINGS_HPP
#define OPENMW_COMPONENTS_DIAGNOSTICS_ATMOSPHERESETTINGS_HPP

#include <string>
#include <string_view>

namespace Diagnostics
{
    /// Describe a complete, bounded graphics preference update for an isolated engine session.
    struct AtmosphereSettings
    {
        bool mScattering = true;
        bool mClouds = true;
        int mQuality = 2;
        bool mShadows = true;
        bool mReflections = true;
        int mAppearance = 0;
        bool mGodrays = true;
        bool mSunGlare = false;
        bool mCloudSunOcclusion = true;

        /// Compare every user-selected graphics preference.
        bool operator==(const AtmosphereSettings&) const = default;
    };

    /// Parse versioned preferences and reject unknown fields or unsupported fidelity levels before mutation.
    AtmosphereSettings deserializeAtmosphereSettings(std::string_view text);

    /// Serialize the complete graphics selection for replay and requested-versus-observed verification.
    std::string serializeAtmosphereSettings(const AtmosphereSettings& settings);
}

#endif
