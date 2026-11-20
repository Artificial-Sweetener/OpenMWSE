#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_OPENMWSEWATER_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_OPENMWSEWATER_H

#include <components/settings/sanitizerimpl.hpp>
#include <components/settings/settingvalue.hpp>

#include <string>

namespace Settings
{
    /// Identify the selected authored water-surface presentation independently from surface geometry.
    enum class WaterAppearance
    {
        OpenMW,
        MgeXe,
        Enhanced,
        Vanilla,
    };

    /// Identify the color treatment used by the enhanced water presentation.
    enum class EnhancedWaterColor
    {
        GreenBlue,
        Blue,
        Green,
    };

    /// Own OpenMWSE-specific water presentation and default-surface wave policy.
    struct OpenMWSEWaterCategory : WithIndex
    {
        using WithIndex::WithIndex;

        /// Select the authored appearance without changing the existing water-shader prerequisite.
        SettingValue<std::string> mAppearance{ mIndex, "OpenMWSE Water", "appearance",
            makeFallbackEnumSanitizerString({ "openmw", "mge-xe", "enhanced", "vanilla" }, "openmw") };

        /// Set the default water surface's wave height on MGE XE's world-unit scale.
        SettingValue<float> mGeometricWaveHeight{ mIndex, "OpenMWSE Water", "geometric wave height",
            makeClampSanitizerFloat(0.f, 250.f) };

        /// Replace classified authored water materials with renderer-owned mesh water.
        SettingValue<bool> mMeshWater{ mIndex, "OpenMWSE Water", "mesh water" };

        /// Reconstruct every receiver from one full-resolution source capture; disabled renders each plane directly.
        SettingValue<bool> mSharedPlanarReflections{ mIndex, "OpenMWSE Water", "shared planar reflections" };

        /// Set classified mesh-water wave height independently from the default world surface.
        SettingValue<float> mMeshGeometricWaveHeight{ mIndex, "OpenMWSE Water", "mesh geometric wave height",
            makeClampSanitizerFloat(0.f, 250.f) };

        /// Enable MGE XE-style animated light modulation below the active water surface.
        SettingValue<bool> mCaustics{ mIndex, "OpenMWSE Water", "caustics" };

        /// Scale the MGE XE caustic response relative to its default intensity of 50.
        SettingValue<float> mCausticsStrength{ mIndex, "OpenMWSE Water", "caustics strength",
            makeClampSanitizerFloat(0.f, 2.f) };

        /// Bound the local lights allowed to illuminate and focus through water in true interiors.
        SettingValue<int> mInteriorLocalLights{ mIndex, "OpenMWSE Water", "interior local lights",
            makeClampSanitizerInt(0, 4) };

        /// Bound the local lights allowed to illuminate and focus through water in exterior spaces.
        SettingValue<int> mExteriorLocalLights{ mIndex, "OpenMWSE Water", "exterior local lights",
            makeClampSanitizerInt(0, 4) };

        /// Select the paired surface and underwater color treatment for enhanced water.
        SettingValue<std::string> mEnhancedColor{ mIndex, "OpenMWSE Water", "enhanced color",
            makeFallbackEnumSanitizerString({ "green-blue", "blue", "green" }, "green-blue") };

        /// Add shoreline and wave-crest foam to enhanced water.
        SettingValue<bool> mEnhancedFoam{ mIndex, "OpenMWSE Water", "enhanced foam" };

        /// Preserve enhanced-water normal detail at long viewing distances.
        SettingValue<bool> mEnhancedDistantWaves{ mIndex, "OpenMWSE Water", "enhanced distant waves" };

        /// Feed bounded Vivec and Molag Mar sewer disturbances into the shared ripple field.
        SettingValue<bool> mEnhancedSewerRipples{ mIndex, "OpenMWSE Water", "enhanced sewer ripples" };

        /// Return the validated authored appearance.
        WaterAppearance appearance() const;

        /// Return the validated enhanced-water color treatment.
        EnhancedWaterColor enhancedColor() const;

        /// Return whether the default surface requires geometric displacement resources.
        bool useGeometricWaves() const { return mGeometricWaveHeight.get() > 0.f; }
    };
}

#endif
