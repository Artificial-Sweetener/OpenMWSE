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
        explicit AtmospherePalette(const VFS::Manager& vfs);
        explicit AtmospherePalette(std::istream& stream);
        ~AtmospherePalette();

        AtmospherePalette(const AtmospherePalette&) = delete;
        AtmospherePalette& operator=(const AtmospherePalette&) = delete;

        void setRegion(std::string_view regionId);
        void update(float elapsedSeconds, bool paused, std::span<Weather> weather);

        MWRender::AtmosphereState getState(
            std::string_view currentWeather, std::string_view nextWeather, float weatherTransition) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
