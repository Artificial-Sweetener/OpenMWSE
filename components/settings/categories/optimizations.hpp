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

        /// Render eligible paged world geometry through the GPU scene.
        SettingValue<bool> mGpuDrivenPaging{ mIndex, "Optimizations", "gpu driven paging" };

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

        /// Return whether the GPU scene prerequisite and paging selection are active.
        bool useGpuDrivenPaging() const { return isActive(mGpuDrivenPaging); }

        /// Return whether actor capture is active within the GPU scene.
        bool useGpuActors() const { return useGpuDrivenPaging() && mGpuActors; }

        /// Return whether GPU actor capture may replace CPU skinning.
        bool useGpuSkinning() const { return useGpuActors() && mGpuSkinning; }

        /// Return whether GPU-scene materials may use packed texture storage.
        bool useGpuTextureArrays() const { return useGpuDrivenPaging() && mGpuTextureArrays; }

        /// Return whether GPU-scene depth-pyramid rejection is active.
        bool useGpuOcclusionCulling() const { return useGpuDrivenPaging() && mGpuOcclusionCulling; }

        /// Return whether GPU-scene receivers use the optimized lighting path.
        bool useGpuSceneLighting() const { return useGpuDrivenPaging() && mGpuSceneLighting; }
    };
}

#endif
