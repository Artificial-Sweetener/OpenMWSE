#ifndef OPENMW_MWWORLD_ATMOSPHEREPALETTE_H
#define OPENMW_MWWORLD_ATMOSPHEREPALETTE_H

#include <iosfwd>
#include <memory>
#include <span>
#include <string_view>

#include "../mwrender/atmosphere.hpp"

namespace VFS
{
    class Manager;
}

namespace MWWorld
{
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

        /// Select the regional palette that weather colors will transition toward.
        void setRegion(std::string_view regionId);
        /// Advance the regional transition and publish its weather color cycles.
        void update(float elapsedSeconds, bool paused, std::span<Weather> weather);

        /// Resolve blended scattering and weather-volume controls for the active weather transition.
        MWRender::AtmosphereState getState(
            std::string_view currentWeather, std::string_view nextWeather, float weatherTransition) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
