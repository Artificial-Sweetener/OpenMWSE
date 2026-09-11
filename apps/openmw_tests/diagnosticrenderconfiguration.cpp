#include <gtest/gtest.h>

#include <apps/openmw/diagnosticrenderconfiguration.hpp>

namespace OMW
{
    TEST(DiagnosticRenderConfigurationTest, SerializesReplayCriticalShadowSettings)
    {
        DiagnosticRenderConfiguration configuration;
        configuration.mResolutionWidth = 3440;
        configuration.mResolutionHeight = 1440;
        configuration.mAntialiasing = 8;
        configuration.mNearClip = 1.f;
        configuration.mViewingDistance = 229376.f;
        configuration.mFieldOfView = 75.f;
        configuration.mReverseZ = true;
        configuration.mAtmosphericEffects = true;
        configuration.mAtmosphericQuality = 2;
        configuration.mAtmosphericScattering = false;
        configuration.mVolumetricClouds = true;
        configuration.mCloudShadows = true;
        configuration.mReflectClouds = false;
        configuration.mAtmosphereAppearance = 1;
        configuration.mGodrays = true;
        configuration.mSunGlare = true;
        configuration.mCloudSunOcclusion = false;
        configuration.mShadowsEnabled = true;
        configuration.mShadowMapCount = 3;
        configuration.mMaximumShadowMapDistance = 16384.f;
        configuration.mShadowFadeStart = 0.9f;
        configuration.mShadowSplitRatio = 0.5f;
        configuration.mShadowSceneBounds = "bounds";
        configuration.mShadowMapResolution = 16384;
        configuration.mMinimumShadowNearFarRatio = 0.25f;
        configuration.mShadowPolygonOffsetFactor = 1.1f;
        configuration.mShadowPolygonOffsetUnits = 4.f;
        configuration.mShadowNormalOffsetDistance = 1.f;
        configuration.mActorShadows = true;
        configuration.mPlayerShadows = true;
        configuration.mTerrainShadows = true;
        configuration.mObjectShadows = true;
        configuration.mIndoorShadows = true;

        const std::string serialized = serializeDiagnosticRenderConfiguration(configuration);

        EXPECT_NE(serialized.find("resolution: [3440, 1440]"), std::string::npos);
        EXPECT_NE(serialized.find("atmospheric-quality: 2"), std::string::npos);
        EXPECT_NE(serialized.find("atmospheric-scattering: false"), std::string::npos);
        EXPECT_NE(serialized.find("volumetric-clouds: true"), std::string::npos);
        EXPECT_NE(serialized.find("cloud-shadows: true"), std::string::npos);
        EXPECT_NE(serialized.find("reflect-clouds: false"), std::string::npos);
        EXPECT_NE(serialized.find("atmosphere-appearance: 1"), std::string::npos);
        EXPECT_NE(serialized.find("godrays: true"), std::string::npos);
        EXPECT_NE(serialized.find("sun-glare: true"), std::string::npos);
        EXPECT_NE(serialized.find("cloud-sun-occlusion: false"), std::string::npos);
        EXPECT_NE(serialized.find("map-count: 3"), std::string::npos);
        EXPECT_NE(serialized.find("maximum-distance: 16384"), std::string::npos);
        EXPECT_NE(serialized.find("scene-bounds: \"bounds\""), std::string::npos);
        EXPECT_NE(serialized.find("map-resolution: 16384"), std::string::npos);
        EXPECT_NE(serialized.find("object-casters: true"), std::string::npos);
    }
}
