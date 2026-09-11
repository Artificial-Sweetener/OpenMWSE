#ifndef OPENMW_COMPONENTS_SETTINGS_ATMOSPHEREMIGRATION_H
#define OPENMW_COMPONENTS_SETTINGS_ATMOSPHEREMIGRATION_H

#include "settings.hpp"

namespace Settings
{
    /// Expand the former bundled atmosphere preference within one configuration layer before merging it.
    inline void migrateAtmosphereSettings(CategorySettingValueMap& settings)
    {
        const auto legacy = settings.find({ "Shaders", "atmospheric effects" });
        if (legacy == settings.end())
            return;
        settings.try_emplace({ "Shaders", "atmospheric scattering" }, legacy->second);
        settings.try_emplace({ "Shaders", "volumetric clouds" }, legacy->second);
        settings.erase(legacy);
    }
}

#endif
