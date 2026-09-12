#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_SHADERS_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_SHADERS_H

#include <components/settings/sanitizerimpl.hpp>
#include <components/settings/settingvalue.hpp>

#include <osg/Math>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <cstdint>
#include <string>
#include <string_view>

namespace Settings
{
    struct ShadersCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<bool> mForcePerPixelLighting{ mIndex, "Shaders", "force per pixel lighting" };
        SettingValue<bool> mClampLighting{ mIndex, "Shaders", "clamp lighting" };
        SettingValue<bool> mParticlePointLighting{ mIndex, "Shaders", "particle point lighting" };
        SettingValue<bool> mAutoUseObjectNormalMaps{ mIndex, "Shaders", "auto use object normal maps" };
        SettingValue<bool> mAutoUseObjectSpecularMaps{ mIndex, "Shaders", "auto use object specular maps" };
        SettingValue<bool> mAutoUseTerrainNormalMaps{ mIndex, "Shaders", "auto use terrain normal maps" };
        SettingValue<bool> mAutoUseTerrainSpecularMaps{ mIndex, "Shaders", "auto use terrain specular maps" };
        SettingValue<std::string> mNormalMapPattern{ mIndex, "Shaders", "normal map pattern" };
        SettingValue<std::string> mNormalHeightMapPattern{ mIndex, "Shaders", "normal height map pattern" };
        SettingValue<std::string> mSpecularMapPattern{ mIndex, "Shaders", "specular map pattern" };
        SettingValue<std::string> mTerrainSpecularMapPattern{ mIndex, "Shaders", "terrain specular map pattern" };
        SettingValue<bool> mApplyLightingToEnvironmentMaps{ mIndex, "Shaders", "apply lighting to environment maps" };
        SettingValue<bool> mClusteredLighting{ mIndex, "Shaders", "clustered lighting" };
        SettingValue<bool> mClassicFalloff{ mIndex, "Shaders", "classic falloff" };
        SettingValue<bool> mMatchSunlightToSun{ mIndex, "Shaders", "match sunlight to sun" };
        SettingValue<float> mLightRadiusMultiplier{ mIndex, "Shaders", "light radius multiplier",
            makeClampSanitizerFloat(1, 100) };
        SettingValue<float> mMaximumLightDistance{ mIndex, "Shaders", "maximum light distance",
            makeMaxSanitizerFloat(0) };
        SettingValue<float> mLightFadeStart{ mIndex, "Shaders", "light fade start", makeClampSanitizerFloat(0, 1) };
        SettingValue<float> mMinimumLightRadius{ mIndex, "Shaders", "minimum light radius", makeMaxSanitizerFloat(0) };
        SettingValue<int> mMaxLights{ mIndex, "Shaders", "max lights", makeClampSanitizerInt(2, 64) };
        SettingValue<float> mMinimumInteriorBrightness{ mIndex, "Shaders", "minimum interior brightness",
            makeClampSanitizerFloat(0, 1) };
        SettingValue<bool> mAntialiasAlphaTest{ mIndex, "Shaders", "antialias alpha test" };
        SettingValue<bool> mAdjustCoverageForAlphaTest{ mIndex, "Shaders", "adjust coverage for alpha test" };
        SettingValue<bool> mSoftParticles{ mIndex, "Shaders", "soft particles" };
        SettingValue<bool> mWeatherParticleOcclusion{ mIndex, "Shaders", "weather particle occlusion" };
        /// Select enhanced aerial perspective independently of the cloud renderer.
        SettingValue<bool> mAtmosphericScattering{ mIndex, "Shaders", "atmospheric scattering" };
        /// Select current color response (0) or the original MGE XE-equivalent response (1).
        SettingValue<int> mAtmosphereAppearance{ mIndex, "Shaders", "atmosphere appearance",
            makeClampSanitizerInt(0, 1) };
        /// Enable depth-occluded sunlight shafts independently of atmospheric color response.
        SettingValue<bool> mGodrays{ mIndex, "Shaders", "godrays" };
        /// Display the authored solar glare texture without a fullscreen color overlay.
        SettingValue<bool> mSunGlare{ mIndex, "Shaders", "sun glare" };
        /// Attenuate the solar disc through cloud optical depth and illuminate textured cloud edges.
        SettingValue<bool> mCloudSunOcclusion{ mIndex, "Shaders", "cloud sun occlusion" };
        /// Select volumetric sky and low clouds while retaining the current weather episode.
        SettingValue<bool> mVolumetricClouds{ mIndex, "Shaders", "volumetric clouds" };
        SettingValue<int> mAtmosphericQuality{ mIndex, "Shaders", "atmospheric quality", makeClampSanitizerInt(0, 3) };
        SettingValue<float> mWeatherParticleOcclusionSmallFeatureCullingPixelSize{ mIndex, "Shaders",
            "weather particle occlusion small feature culling pixel size" };
        /// Require native composition for any independently selected atmosphere or solar effect.
        bool useNativeAtmosphere() const
        {
            return mAtmosphericScattering || mVolumetricClouds || mGodrays || mSunGlare || mCloudSunOcclusion;
        }
    };
}

#endif
