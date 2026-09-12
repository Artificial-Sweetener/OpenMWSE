#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_OPTIMIZATIONS_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_OPTIMIZATIONS_H

#include <components/settings/settingvalue.hpp>

namespace Settings
{
    /// Own the prerequisite and independently selected optional optimization policies.
    struct OptimizationsCategory : WithIndex
    {
        using WithIndex::WithIndex;

        /// Permit individually selected optional optimizations to become effective.
        SettingValue<bool> mEnabled{ mIndex, "Optimizations", "enabled" };

        /// Skin eligible animated geometry through the GPU scene.
        SettingValue<bool> mGpuSkinning{ mIndex, "Optimizations", "gpu skinning" };

        /// Reject hidden GPU-scene instances through the scene depth pyramid.
        SettingValue<bool> mGpuOcclusionCulling{ mIndex, "Optimizations", "gpu occlusion culling" };

        /// Render and raycast eligible actors through the GPU scene.
        SettingValue<bool> mGpuActors{ mIndex, "Optimizations", "gpu actors" };

        /// Pack eligible GPU-scene textures into arrays or bindless material storage.
        SettingValue<bool> mGpuTextureArrays{ mIndex, "Optimizations", "gpu texture arrays" };

        /// Reuse indexed scene-light collections across compatible views.
        SettingValue<bool> mIndexedLightLists{ mIndex, "Optimizations", "indexed light lists" };

        /// Evaluate compatible scene lighting through the GPU-scene receiver path.
        SettingValue<bool> mGpuSceneLighting{ mIndex, "Optimizations", "gpu scene lighting" };

        /// Return whether the prerequisite and a subordinate option are both selected.
        bool isActive(const SettingValue<bool>& option) const { return mEnabled && option; }
    };
}

#endif
