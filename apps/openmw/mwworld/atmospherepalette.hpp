#ifndef OPENMW_MWWORLD_ATMOSPHEREPALETTE_H
#define OPENMW_MWWORLD_ATMOSPHEREPALETTE_H

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "../mwrender/atmosphere.hpp"

namespace VFS
{
    class Manager;
}

namespace MWWorld
{
    struct AtmosphereEnvironmentState;
    class Weather;

    class AtmospherePalette
    {
    public:
        /// Load canonical atmosphere palettes and weather volumes from the runtime VFS.
        explicit AtmospherePalette(const VFS::Manager& vfs);
        /// Load regional scattering colors and independently owned weather volume profiles.
        AtmospherePalette(std::istream& paletteStream, std::istream& volumeStream);
        ~AtmospherePalette();

        AtmospherePalette(const AtmospherePalette&) = delete;
        AtmospherePalette& operator=(const AtmospherePalette&) = delete;

        /// Select a continuous environment-derived palette and volume target.
        void setEnvironment(const AtmosphereEnvironmentState& environment);
        /// Select and immediately apply exact environment evidence for an isolated diagnostic.
        void setDiagnosticEnvironment(const AtmosphereEnvironmentState& environment);
        /// Smooth environment changes and publish the resulting weather color cycles.
        void update(float elapsedSeconds, bool paused, std::span<Weather> weather);

        /// Capture the applied regional blend and its target so replay can continue the same atmosphere trajectory.
        std::string captureRuntimeState() const;
        /// Restore a validated regional blend without forcing it immediately to the current region's target.
        void restoreRuntimeState(std::string_view state);

        /// Select a stable episode using authored weights, weather trajectory, and meteorological context.
        std::uint32_t createCloudScenarioIdentity(std::string_view weather, std::string_view originWeather,
            std::uint32_t sequence, float recentPrecipitation, float gameHour, float sunriseHour,
            const AtmosphereEnvironmentState& environment) const;

        /// Resolve stable episode scenarios and blend them across the authoritative weather transition.
        MWRender::AtmosphereState getState(std::string_view currentWeather, std::string_view nextWeather,
            float weatherTransition, std::uint32_t currentScenarioIdentity = 0,
            std::uint32_t nextScenarioIdentity = 0) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
