#ifndef OPENMW_COMPONENTS_SCENEUTIL_STATEUPDATER_H
#define OPENMW_COMPONENTS_SCENEUTIL_STATEUPDATER_H

#include <osg/Matrixf>
#include <osg/Texture2D>
#include <osg/Texture3D>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/sceneutil/statesetupdater.hpp>

namespace osg
{
    class StateSet;
}

namespace Resource
{
    class SceneManager;
}

namespace SceneUtil
{
    class RTTNode;

    class PerViewUniformStateUpdater final : public StateSetUpdater
    {
    public:
        PerViewUniformStateUpdater(
            Resource::SceneManager* sceneManager, int opaqueTextureUnit = -1, int opaqueColorTextureUnit = -1);

        void setDefaults(osg::StateSet* stateset) override;

        void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override;

        void applyLeft(osg::StateSet* stateset, osgUtil::CullVisitor* nv) override;

        void applyRight(osg::StateSet* stateset, osgUtil::CullVisitor* nv) override;

        void setProjectionMatrix(const osg::Matrixf& projectionMatrix);

        const osg::Matrixf& getProjectionMatrix() const;

        void enableSkyRTT(int skyTextureUnit, RTTNode* skyRTT);

    private:
        osg::Matrixf getEyeProjectionMatrix(int view);

        osg::Matrixf mProjectionMatrix;
        int mSkyTextureUnit = -1;
        RTTNode* mSkyRTT = nullptr;

        Resource::SceneManager* mSceneManager;
        int mOpaqueTextureUnit = -1;
        int mOpaqueColorTextureUnit = -1;
    };

    class SharedUniformStateUpdater : public StateSetUpdater
    {
    public:
        SharedUniformStateUpdater(float skyBlendingStartCoef);

        void setDefaults(osg::StateSet* stateset) override;
        void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override;
        void setNear(float near);

        void setFar(float far);

        void setScreenRes(float width, float height);

        void setWindSpeed(float windSpeed);

        void setPlayerPos(osg::Vec3f playerPos);

    private:
        float mNear = 0.f;
        float mFar = 0.f;
        float mWindSpeed = 0.f;
        float mSkyBlendingStartCoef = 0.f;
        osg::Vec3f mPlayerPos;
        osg::Vec2f mScreenRes;
    };

    class StateUpdater : public StateSetUpdater
    {
    public:
        /// Bind the canonical atmosphere weather, base, and detail fields to scene shaders using reserved units.
        StateUpdater(osg::Texture3D* atmosphereNoiseTexture = nullptr, int atmosphereNoiseTextureUnit = -1,
            osg::Texture3D* atmosphereDetailNoiseTexture = nullptr, int atmosphereDetailNoiseTextureUnit = -1,
            osg::Texture2D* atmosphereWeatherTexture = nullptr, int atmosphereWeatherTextureUnit = -1);

        void setDefaults(osg::StateSet* stateset) override;

        void apply(osg::StateSet* stateset, osg::NodeVisitor*) override;

        void setAmbientColor(const osg::Vec4f& col);

        void setFogColor(const osg::Vec4f& col);

        void setFogStart(float start);

        void setFogEnd(float end);

        /// Publish canonical scene-linear atmosphere, volume, and lighting state to compatibility shaders.
        void setAtmosphere(bool enabled, const osg::Vec4f& skyColor, const osg::Vec3f& inScattering,
            const osg::Vec3f& outScattering, float weatherWeight, const osg::Vec3f& sunDirection,
            const osg::Vec4f& cloudParameters, const osg::Vec4f& cloudShapeParameters,
            const osg::Vec4f& cloudOrganizationParameters, const osg::Vec4f& cloudStructureParameters,
            const osg::Vec4f& lowCloudParameters, const osg::Vec4f& lowCloudShapeParameters,
            const osg::Vec4f& mistParameters, const osg::Vec4f& fogVolumeParameters,
            const osg::Vec4f& mediumReferenceParameters, const osg::Vec4f& orographicParameters,
            const osg::Vec4f& sunDiscColor, const osg::Vec3f& solarRadiance, const osg::Vec3f& skyIrradiance,
            const osg::Vec3f& groundIrradiance, const osg::Vec2f& cloudAdvectionOffsetWorldUnits, int quality);

        void setUnderwaterFogColor(const osg::Vec4f& col);

        void setUnderwaterFogStart(float start);

        void setUnderwaterFogEnd(float end);

        void setWireframe(bool wireframe);
        bool getWireframe() const;

        void setWaterHeight(float waterHeight);

        void setWaterEnabled(bool enabled);

        /// Select the inert or capture-only compatibility-shader diagnostic output.
        void setAtmosphereDiagnosticMode(int mode);

    private:
        osg::ref_ptr<osg::Texture3D> mAtmosphereNoiseTexture;
        int mAtmosphereNoiseTextureUnit = -1;
        osg::ref_ptr<osg::Texture3D> mAtmosphereDetailNoiseTexture;
        int mAtmosphereDetailNoiseTextureUnit = -1;
        osg::ref_ptr<osg::Texture2D> mAtmosphereWeatherTexture;
        int mAtmosphereWeatherTextureUnit = -1;
        osg::Vec4f mAmbientColor;
        osg::Vec4f mFogColor;
        float mFogStart = 0.f;
        float mFogEnd = 0.f;
        bool mAtmosphereEnabled = false;
        osg::Vec4f mAtmosphereSkyColor;
        osg::Vec3f mAtmosphereInScattering;
        osg::Vec3f mAtmosphereOutScattering;
        float mAtmosphereWeatherWeight = 0.f;
        osg::Vec3f mAtmosphereSunDirection{ 0.f, 0.f, 1.f };
        osg::Vec4f mAtmosphereCloudParameters;
        osg::Vec4f mAtmosphereCloudShapeParameters;
        osg::Vec4f mAtmosphereCloudOrganizationParameters;
        osg::Vec4f mAtmosphereCloudStructureParameters;
        osg::Vec4f mAtmosphereLowCloudParameters;
        osg::Vec4f mAtmosphereLowCloudShapeParameters;
        osg::Vec4f mAtmosphereMistParameters;
        osg::Vec4f mAtmosphereFogVolumeParameters;
        osg::Vec4f mAtmosphereMediumReferenceParameters;
        osg::Vec4f mAtmosphereOrographicParameters;
        osg::Vec4f mAtmosphereSunDiscColor;
        osg::Vec3f mAtmosphereSolarRadiance;
        osg::Vec3f mAtmosphereSkyIrradiance;
        osg::Vec3f mAtmosphereGroundIrradiance;
        osg::Vec2f mAtmosphereCloudAdvectionOffsetWorldUnits;
        int mAtmosphereQuality = 2;
        osg::Vec4f mUnderwaterFogColor;
        float mUnderwaterFogStart = 0.f;
        float mUnderwaterFogEnd = 0.f;
        bool mWireframe = false;
        float mWaterHeight = 0.f;
        bool mWaterEnabled = false;
        int mAtmosphereDiagnosticMode = 0;
    };
}

#endif
