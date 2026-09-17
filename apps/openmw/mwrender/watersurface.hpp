#ifndef OPENMW_MWRENDER_WATERSURFACE_H
#define OPENMW_MWRENDER_WATERSURFACE_H

#include <components/settings/categories/openmwsewater.hpp>

namespace MWRender
{
    /// Describe optical behavior shared by a water surface and nearby material receivers.
    struct WaterOpticsParameters
    {
        Settings::WaterAppearance mAppearance = Settings::WaterAppearance::OpenMW;
        Settings::EnhancedWaterColor mEnhancedColor = Settings::EnhancedWaterColor::GreenBlue;
        bool mEnhancedFoam = true;
        bool mEnhancedDistantWaves = true;
        bool mEnhancedSewerRipples = false;
        bool mCaustics = false;
        float mCausticsStrength = 1.f;
    };

    /// Describe independently configurable presentation and displacement policy for one class of water surfaces.
    struct WaterSurfaceParameters
    {
        WaterOpticsParameters mOptics;
        float mGeometricWaveHeight = 0.f;

        /// Return whether this surface class needs displacement geometry and obstruction resources.
        bool useGeometricWaves() const { return mGeometricWaveHeight > 0.f; }

        /// Return whether this surface needs cached near-surface coverage for waves or transmitted caustic paths.
        bool useSurfaceCoverage() const { return useGeometricWaves() || mOptics.mCaustics; }
    };

    /// Snapshot the user settings that govern the default world-water surface.
    WaterSurfaceParameters defaultWaterSurfaceParameters();
}

#endif
