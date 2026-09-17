#include "watersurface.hpp"

#include <components/settings/values.hpp>

namespace MWRender
{
    WaterSurfaceParameters defaultWaterSurfaceParameters()
    {
        const Settings::OpenMWSEWaterCategory& settings = Settings::openmwseWater();
        return {
            .mOptics = {
                .mAppearance = settings.appearance(),
                .mEnhancedColor = settings.enhancedColor(),
                .mEnhancedFoam = settings.mEnhancedFoam,
                .mEnhancedDistantWaves = settings.mEnhancedDistantWaves,
                .mEnhancedSewerRipples = settings.mEnhancedSewerRipples,
                .mCaustics = settings.mCaustics,
                .mCausticsStrength = settings.mCausticsStrength,
            },
            .mGeometricWaveHeight = settings.mGeometricWaveHeight,
        };
    }
}
