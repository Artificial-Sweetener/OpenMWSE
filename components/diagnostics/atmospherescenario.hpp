#ifndef OPENMW_COMPONENTS_DIAGNOSTICS_ATMOSPHERESCENARIO_HPP
#define OPENMW_COMPONENTS_DIAGNOSTICS_ATMOSPHERESCENARIO_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace Diagnostics
{
    inline constexpr unsigned atmosphereScenarioVersion = 6;

    /// Defines one exact steady or transitioning weather state for an isolated diagnostic run.
    struct DiagnosticWeatherState
    {
        std::string mCurrent;
        std::optional<std::string> mNext;
        float mTransition = 0.f;

        /// Compare every weather identity and transition field.
        bool operator==(const DiagnosticWeatherState&) const = default;
    };

    /// Select the participating media retained for one isolated atmosphere diagnostic.
    struct DiagnosticAtmosphereMedia
    {
        bool mMainClouds = true;
        bool mLowClouds = true;
        bool mMist = true;
        bool mFog = true;

        /// Compare every medium-selection field.
        bool operator==(const DiagnosticAtmosphereMedia&) const = default;
    };

    /// Select weather-side visual effects retained for one isolated atmosphere diagnostic.
    struct DiagnosticWeatherEffects
    {
        bool mPrecipitation = true;
        bool mLightning = true;

        /// Compare every weather-effect selection field.
        bool operator==(const DiagnosticWeatherEffects&) const = default;
    };

    /// Defines exact semantic environment evidence for an isolated atmosphere diagnostic.
    struct DiagnosticEnvironmentState
    {
        std::map<std::string, float, std::less<>> mTraits;
        std::optional<float> mTerrainHeightWorldUnits;

        /// Compare every semantic trait and optional terrain datum.
        bool operator==(const DiagnosticEnvironmentState&) const = default;
    };

    /// Defines deterministic atmosphere inputs that may safely override a copied save.
    struct AtmosphereScenario
    {
        std::optional<DiagnosticWeatherState> mWeather;
        std::optional<double> mGameHour;
        std::optional<DiagnosticAtmosphereMedia> mMedia;
        std::optional<bool> mDistanceFog;
        std::optional<std::uint32_t> mCloudSeed;
        std::optional<DiagnosticWeatherEffects> mWeatherEffects;
        std::optional<DiagnosticEnvironmentState> mEnvironment;
        std::optional<float> mRecentPrecipitation;

        /// Return whether this scenario changes any save-derived atmosphere input.
        bool empty() const noexcept
        {
            return !mWeather && !mGameHour && !mMedia && !mDistanceFog && !mCloudSeed && !mWeatherEffects
                && !mEnvironment && !mRecentPrecipitation;
        }

        /// Compare every deterministic atmosphere input.
        bool operator==(const AtmosphereScenario&) const = default;
    };

    /// Serialize a validated atmosphere scenario to its canonical versioned YAML representation.
    std::string serializeAtmosphereScenario(const AtmosphereScenario& scenario);

    /// Parse and strictly validate a versioned atmosphere scenario from YAML text.
    AtmosphereScenario deserializeAtmosphereScenario(std::string_view text);
}

#endif
