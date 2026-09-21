#ifndef OPENMW_COMPONENTS_SETTINGS_GRAPHICSSETTINGSVISIBILITY_H
#define OPENMW_COMPONENTS_SETTINGS_GRAPHICSSETTINGSVISIBILITY_H

#include <array>

#include "windowmode.hpp"

namespace Settings
{
    /// Identifies why the clustered-lighting preference is read-only.
    enum class ClusteredLightingConstraint
    {
        None,
        Unsupported,
        ForcedByGpuSceneLighting,
    };

    /// Describes the contextual groups shown for the selected window mode.
    struct DisplayControlVisibility
    {
        bool mWindowBorder;
        bool mResolution;
        bool mBorderlessHint;
    };

    /// Resolve display dependencies without changing any saved display preference.
    constexpr DisplayControlVisibility getDisplayControlVisibility(WindowMode mode)
    {
        return { mode == WindowMode::Windowed, true, mode == WindowMode::WindowedFullscreen };
    }

    /// Supplies the atmosphere parent selections used by the graphics menu.
    struct AtmosphereControlInputs
    {
        bool mScattering;
        bool mVolumetricClouds;
        bool mGodrays;
    };

    /// Describes the contextual atmosphere groups shown for the selected effects.
    struct AtmosphereControlVisibility
    {
        bool mAppearance;
        bool mQuality;
    };

    /// Resolve atmosphere dependencies without changing subordinate preferences.
    constexpr AtmosphereControlVisibility getAtmosphereControlVisibility(AtmosphereControlInputs inputs)
    {
        return { inputs.mScattering, inputs.mScattering || inputs.mVolumetricClouds || inputs.mGodrays };
    }

    /// Supplies the water parent selections used by the graphics menu.
    struct WaterControlInputs
    {
        bool mShader;
        bool mEnhancedAppearance;
        bool mCaustics;
    };

    /// Describes the contextual water groups shown for the selected renderer.
    struct WaterControlVisibility
    {
        bool mShaderControls;
        bool mEnhancedControls;
        bool mCausticsStrength;
    };

    /// Resolve water dependencies without changing subordinate preferences.
    constexpr WaterControlVisibility getWaterControlVisibility(WaterControlInputs inputs)
    {
        return { inputs.mShader, inputs.mShader && inputs.mEnhancedAppearance, inputs.mShader && inputs.mCaustics };
    }

    /// Supplies the shadow parent selections used by the graphics menu.
    struct ShadowControlInputs
    {
        bool mGeometryShadows;
        bool mCloudShadows;
        bool mFilteredShadows;
        bool mDistantShadows;
    };

    /// Describes the contextual shadow groups shown for the selected casters and filter.
    struct ShadowControlVisibility
    {
        bool mFiltering;
        bool mDistantShadows;
        bool mDistantQuality;
        bool mCloudIntensity;
        bool mGeometryControls;
    };

    /// Resolve shadow dependencies without changing subordinate preferences.
    constexpr ShadowControlVisibility getShadowControlVisibility(ShadowControlInputs inputs)
    {
        return { inputs.mGeometryShadows || inputs.mCloudShadows, inputs.mGeometryShadows && inputs.mFilteredShadows,
            inputs.mGeometryShadows && inputs.mFilteredShadows && inputs.mDistantShadows,
            inputs.mCloudShadows && inputs.mFilteredShadows, inputs.mGeometryShadows };
    }

    /// Supplies the optional-optimization prerequisite selections used by the graphics menu.
    struct OptimizationControlInputs
    {
        bool mEnabled;
        bool mGpuDrivenPaging;
        bool mGpuActors;
        bool mGpuOcclusionCulling;
        bool mGpuSceneLighting;
    };

    /// Resolve subordinate optimization rows in their stable binding order.
    constexpr std::array<bool, 12> getOptimizationControlVisibility(OptimizationControlInputs inputs)
    {
        const bool paging = inputs.mEnabled && inputs.mGpuDrivenPaging;
        return { inputs.mEnabled, paging && inputs.mGpuActors, paging, paging, paging, inputs.mEnabled, paging, paging,
            paging, paging && inputs.mGpuOcclusionCulling && inputs.mGpuSceneLighting, paging, inputs.mEnabled };
    }

    /// Supplies renderer capabilities and lighting selections used by the graphics menu.
    struct LightingControlInputs
    {
        bool mSupportsClustered;
        bool mGpuSceneLighting;
        bool mClusteredPreference;
        bool mClassicFalloff;
    };

    /// Describes effective lighting state, constraints, and contextual tuning groups.
    struct LightingControlVisibility
    {
        bool mClusteredEffective;
        ClusteredLightingConstraint mClusteredConstraint;
        bool mClassicFalloff;
        bool mMaximumLights;
        bool mMinimumLightRadius;
        bool mMinimumBrightness;
    };

    /// Resolve lighting dependencies while retaining the user's dormant preferences.
    constexpr LightingControlVisibility getLightingControlVisibility(LightingControlInputs inputs)
    {
        const bool clustered = inputs.mSupportsClustered && (inputs.mGpuSceneLighting || inputs.mClusteredPreference);
        const bool classic = inputs.mClassicFalloff && (!clustered || inputs.mGpuSceneLighting);
        const ClusteredLightingConstraint constraint = !inputs.mSupportsClustered
            ? ClusteredLightingConstraint::Unsupported
            : inputs.mGpuSceneLighting ? ClusteredLightingConstraint::ForcedByGpuSceneLighting
                                       : ClusteredLightingConstraint::None;
        return { clustered, constraint, inputs.mGpuSceneLighting || !clustered, !clustered || classic,
            inputs.mGpuSceneLighting && !classic, !classic };
    }
}

#endif
