#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_OPENMWSESHADOWS_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_OPENMWSESHADOWS_H

#include <components/settings/sanitizerimpl.hpp>
#include <components/settings/settingvalue.hpp>

#include <string>

namespace Settings
{
    /// Own OpenMWSE-specific shadow presentation without changing upstream shadow-setting semantics.
    struct OpenMWSEShadowsCategory : WithIndex
    {
        using WithIndex::WithIndex;

        /// Select the complete legacy presentation or an increasingly broad optimized percentage-closer filter.
        SettingValue<std::string> mFilteringQuality{ mIndex, "OpenMWSE Shadows", "quality",
            makeEnumSanitizerString({ "legacy", "low", "medium", "high", "ultra" }) };

        /// Extend filtered outdoor shadows beyond the detailed range to the visible horizon.
        SettingValue<bool> mDistantShadows{ mIndex, "OpenMWSE Shadows", "distant shadows" };

        /// Select how quickly horizon cascades reduce resolution and discard insignificant casters.
        SettingValue<std::string> mDistantShadowQuality{ mIndex, "OpenMWSE Shadows", "distant shadow quality",
            makeEnumSanitizerString({ "low", "medium", "high", "ultra" }) };

        /// Scale cloud optical depth while preserving the active cloud representation and weather state.
        SettingValue<float> mCloudShadowIntensity{ mIndex, "OpenMWSE Shadows", "cloud shadow intensity",
            makeClampSanitizerFloat(0.f, 1.f) };
    };
}

#endif
