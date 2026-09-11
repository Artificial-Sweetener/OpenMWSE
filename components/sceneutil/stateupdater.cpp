#include "stateupdater.hpp"

#include <osg/Fog>
#include <osg/PolygonMode>

#include "cloudadvection.hpp"
#include "cloudshaderstate.hpp"
#include "depth.hpp"
#include "rtt.hpp"
#include "statesetupdater.hpp"

#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/fog.hpp>
#include <components/stereo/multiview.hpp>
#include <components/stereo/stereomanager.hpp>

namespace SceneUtil
{
    StateUpdater::StateUpdater(osg::Texture3D* atmosphereNoiseTexture, int atmosphereNoiseTextureUnit,
        osg::Texture3D* atmosphereDetailNoiseTexture, int atmosphereDetailNoiseTextureUnit,
        osg::Texture2D* atmosphereCurlNoiseTexture, int atmosphereCurlNoiseTextureUnit,
        osg::Texture2D* atmosphereWeatherTexture, int atmosphereWeatherTextureUnit, int texturedCloudTextureUnit)
        : mAtmosphereNoiseTexture(atmosphereNoiseTexture)
        , mAtmosphereNoiseTextureUnit(atmosphereNoiseTextureUnit)
        , mAtmosphereDetailNoiseTexture(atmosphereDetailNoiseTexture)
        , mAtmosphereDetailNoiseTextureUnit(atmosphereDetailNoiseTextureUnit)
        , mAtmosphereCurlNoiseTexture(atmosphereCurlNoiseTexture)
        , mAtmosphereCurlNoiseTextureUnit(atmosphereCurlNoiseTextureUnit)
        , mAtmosphereWeatherTexture(atmosphereWeatherTexture)
        , mAtmosphereWeatherTextureUnit(atmosphereWeatherTextureUnit)
        , mTexturedCloudTextureUnit(texturedCloudTextureUnit)
    {
    }

    PerViewUniformStateUpdater::PerViewUniformStateUpdater(
        Resource::SceneManager* sceneManager, int opaqueTextureUnit, int opaqueColorTextureUnit)
        : mSceneManager(sceneManager)
        , mOpaqueTextureUnit(opaqueTextureUnit)
        , mOpaqueColorTextureUnit(opaqueColorTextureUnit)
    {
    }

    void PerViewUniformStateUpdater::setDefaults(osg::StateSet* stateset)
    {
        stateset->addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf{}));
        if (mSkyRTT)
            stateset->addUniform(new osg::Uniform("sky", mSkyTextureUnit));
    }

    void PerViewUniformStateUpdater::apply(osg::StateSet* stateset, osg::NodeVisitor* nv)
    {
        stateset->getUniform("projectionMatrix")->set(mProjectionMatrix);
        if (mSkyRTT && nv->getVisitorType() == osg::NodeVisitor::CULL_VISITOR)
        {
            osg::Texture* skyTexture = mSkyRTT->getColorTexture(static_cast<osgUtil::CullVisitor*>(nv));
            stateset->setTextureAttribute(
                mSkyTextureUnit, skyTexture, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        }

        if (mOpaqueTextureUnit > 0)
            stateset->setTextureAttribute(mOpaqueTextureUnit,
                mSceneManager->getOpaqueDepthTex(nv->getTraversalNumber()), osg::StateAttribute::ON);
        if (mOpaqueColorTextureUnit > 0)
            stateset->setTextureAttribute(mOpaqueColorTextureUnit,
                mSceneManager->getOpaqueColorTex(nv->getTraversalNumber()), osg::StateAttribute::ON);
    }

    void PerViewUniformStateUpdater::applyLeft(osg::StateSet* stateset, osgUtil::CullVisitor* nv)
    {
        stateset->getUniform("projectionMatrix")->set(getEyeProjectionMatrix(0));
    }

    void PerViewUniformStateUpdater::applyRight(osg::StateSet* stateset, osgUtil::CullVisitor* nv)
    {
        stateset->getUniform("projectionMatrix")->set(getEyeProjectionMatrix(1));
    }

    void PerViewUniformStateUpdater::setProjectionMatrix(const osg::Matrixf& projectionMatrix)
    {
        mProjectionMatrix = projectionMatrix;
    }

    const osg::Matrixf& PerViewUniformStateUpdater::getProjectionMatrix() const
    {
        return mProjectionMatrix;
    }

    void PerViewUniformStateUpdater::enableSkyRTT(int skyTextureUnit, RTTNode* skyRTT)
    {
        mSkyTextureUnit = skyTextureUnit;
        mSkyRTT = skyRTT;
    }

    osg::Matrixf PerViewUniformStateUpdater::getEyeProjectionMatrix(int view)
    {
        return Stereo::Manager::instance().computeEyeProjection(view, AutoDepth::isReversed());
    }

    SharedUniformStateUpdater::SharedUniformStateUpdater(float skyBlendingStartCoef)
        : mSkyBlendingStartCoef(skyBlendingStartCoef)
    {
    }

    void SharedUniformStateUpdater::setDefaults(osg::StateSet* stateset)
    {
        stateset->addUniform(new osg::Uniform("near", 0.f));
        stateset->addUniform(new osg::Uniform("far", 0.f));
        stateset->addUniform(new osg::Uniform("skyBlendingStart", 0.f));
        stateset->addUniform(new osg::Uniform("screenRes", osg::Vec2f{}));
        stateset->addUniform(new osg::Uniform("isReflection", false));
        stateset->addUniform(new osg::Uniform("windSpeed", 0.0f));
        stateset->addUniform(new osg::Uniform("playerPos", osg::Vec3f(0.f, 0.f, 0.f)));
        stateset->addUniform(new osg::Uniform("useTreeAnim", false));
    }

    void SharedUniformStateUpdater::apply(osg::StateSet* stateset, osg::NodeVisitor* nv)
    {
        stateset->getUniform("near")->set(mNear);
        stateset->getUniform("far")->set(mFar);
        stateset->getUniform("skyBlendingStart")->set(mFar * mSkyBlendingStartCoef);
        stateset->getUniform("screenRes")->set(mScreenRes);
        stateset->getUniform("windSpeed")->set(mWindSpeed);
        stateset->getUniform("playerPos")->set(mPlayerPos);
    }

    void SharedUniformStateUpdater::setNear(float near)
    {
        mNear = near;
    }

    void SharedUniformStateUpdater::setFar(float far)
    {
        mFar = far;
    }

    void SharedUniformStateUpdater::setScreenRes(float width, float height)
    {
        mScreenRes = osg::Vec2f(width, height);
    }

    void SharedUniformStateUpdater::setWindSpeed(float windSpeed)
    {
        mWindSpeed = windSpeed;
    }

    void SharedUniformStateUpdater::setPlayerPos(osg::Vec3f playerPos)
    {
        mPlayerPos = playerPos;
    }

    void StateUpdater::setDefaults(osg::StateSet* stateset)
    {
        if (mWireframe)
        {
            osg::PolygonMode* polygonmode = new osg::PolygonMode;
            polygonmode->setMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::LINE);
            stateset->setAttributeAndModes(polygonmode, osg::StateAttribute::ON);
        }
        else
            stateset->removeAttribute(osg::StateAttribute::POLYGONMODE);

        SceneUtil::setFogColor(*stateset, mFogColor);
        SceneUtil::setFogStart(*stateset, mFogStart);
        SceneUtil::setFogEnd(*stateset, mFogEnd);
        SceneUtil::setUnderwaterFogColor(*stateset, mUnderwaterFogColor);
        SceneUtil::setUnderwaterFogStart(*stateset, mUnderwaterFogStart);
        SceneUtil::setUnderwaterFogEnd(*stateset, mUnderwaterFogEnd);
        SceneUtil::setFogDepth(*stateset, -1.f);

        if (mAtmosphereNoiseTexture && mAtmosphereNoiseTextureUnit >= 0)
        {
            stateset->setTextureAttribute(
                mAtmosphereNoiseTextureUnit, mAtmosphereNoiseTexture, osg::StateAttribute::ON);
            stateset->addUniform(new osg::Uniform("openmwseCloudNoiseTexture", mAtmosphereNoiseTextureUnit));
        }

        if (mAtmosphereDetailNoiseTexture && mAtmosphereDetailNoiseTextureUnit >= 0)
        {
            stateset->setTextureAttribute(
                mAtmosphereDetailNoiseTextureUnit, mAtmosphereDetailNoiseTexture, osg::StateAttribute::ON);
            stateset->addUniform(
                new osg::Uniform("openmwseCloudDetailNoiseTexture", mAtmosphereDetailNoiseTextureUnit));
        }

        if (mAtmosphereCurlNoiseTexture && mAtmosphereCurlNoiseTextureUnit >= 0)
        {
            stateset->setTextureAttribute(
                mAtmosphereCurlNoiseTextureUnit, mAtmosphereCurlNoiseTexture, osg::StateAttribute::ON);
            stateset->addUniform(new osg::Uniform("openmwseCloudCurlNoiseTexture", mAtmosphereCurlNoiseTextureUnit));
        }

        if (mAtmosphereWeatherTexture && mAtmosphereWeatherTextureUnit >= 0)
        {
            stateset->setTextureAttribute(
                mAtmosphereWeatherTextureUnit, mAtmosphereWeatherTexture, osg::StateAttribute::ON);
            stateset->addUniform(new osg::Uniform("openmwseCloudWeatherTexture", mAtmosphereWeatherTextureUnit));
        }

        osg::ref_ptr<osg::Uniform> terrainHeights = new osg::Uniform(osg::Uniform::FLOAT_VEC4,
            "atmosphere.terrainHeights", static_cast<unsigned int>(AtmosphereTerrainGrid::sPackedSize));
        for (unsigned int index = 0; index < AtmosphereTerrainGrid::sPackedSize; ++index)
            terrainHeights->setElement(index, mAtmosphereTerrain.mHeights[index]);
        stateset->addUniform(terrainHeights);
        stateset->addUniform(new osg::Uniform("atmosphere.terrainOriginSpacing", mAtmosphereTerrain.mOriginSpacing));
        stateset->addUniform(new osg::Uniform("atmosphere.terrainHeightBounds", mAtmosphereTerrain.mHeightBounds));
        stateset->addUniform(new osg::Uniform("atmosphere.scatteringEnabled", mAtmosphereScatteringEnabled));
        stateset->addUniform(new osg::Uniform("atmosphere.appearance", mAtmosphereAppearance));
        stateset->addUniform(new osg::Uniform("atmosphere.sunlightEnabled", mSunlightEnabled));
        stateset->addUniform(new osg::Uniform("atmosphere.cloudSunOcclusion", mCloudSunOcclusion));
        stateset->addUniform(new osg::Uniform("atmosphere.cloudsEnabled", mAtmosphereCloudsEnabled));
        stateset->addUniform(new osg::Uniform("atmosphere.cloudShadowsEnabled", mAtmosphereCloudShadowsEnabled));
        stateset->addUniform(
            new osg::Uniform("atmosphere.cloudReflectionsEnabled", mAtmosphereCloudReflectionsEnabled));
        stateset->addUniform(new osg::Uniform("atmosphere.enabled", mAtmosphereEnabled));
        stateset->addUniform(new osg::Uniform("atmosphere.skyColor", mAtmosphereSkyColor));
        stateset->addUniform(new osg::Uniform("atmosphere.inScattering", mAtmosphereInScattering));
        stateset->addUniform(new osg::Uniform("atmosphere.outScattering", mAtmosphereOutScattering));
        stateset->addUniform(new osg::Uniform("atmosphere.weatherWeight", mAtmosphereWeatherWeight));
        stateset->addUniform(new osg::Uniform("atmosphere.sunDirection", mAtmosphereSunDirection));
        osg::ref_ptr<osg::Uniform> cloudLayers = new osg::Uniform(osg::Uniform::FLOAT_VEC4, "atmosphere.cloudLayers",
            static_cast<unsigned int>(mAtmosphereCloudLayers.size()));
        for (unsigned int index = 0; index < mAtmosphereCloudLayers.size(); ++index)
            cloudLayers->setElement(index, mAtmosphereCloudLayers[index]);
        stateset->addUniform(cloudLayers);
        stateset->addUniform(new osg::Uniform("atmosphere.cloudParameters", mAtmosphereCloudParameters));

        stateset->addUniform(
            new osg::Uniform("atmosphere.incomingCloudParameters", mAtmosphereIncomingCloudParameters));

        stateset->addUniform(
            new osg::Uniform("atmosphere.cloudScenarioParameters", mAtmosphereCloudScenarioParameters));
        stateset->addUniform(new osg::Uniform("atmosphere.lowCloudParameters", mAtmosphereLowCloudParameters));
        stateset->addUniform(
            new osg::Uniform("atmosphere.lowCloudEnvironmentInfluence", mAtmosphereLowCloudEnvironmentInfluence));
        stateset->addUniform(new osg::Uniform("atmosphere.mistParameters", mAtmosphereMistParameters));
        stateset->addUniform(new osg::Uniform("atmosphere.fogVolumeParameters", mAtmosphereFogVolumeParameters));
        stateset->addUniform(
            new osg::Uniform("atmosphere.mediumReferenceParameters", mAtmosphereMediumReferenceParameters));
        stateset->addUniform(new osg::Uniform("atmosphere.orographicParameters", mAtmosphereOrographicParameters));
        stateset->addUniform(new osg::Uniform("atmosphere.sunDiscColor", mAtmosphereSunDiscColor));
        stateset->addUniform(new osg::Uniform("atmosphere.solarRadiance", mAtmosphereSolarRadiance));
        stateset->addUniform(new osg::Uniform("atmosphere.skyIrradiance", mAtmosphereSkyIrradiance));
        stateset->addUniform(new osg::Uniform("atmosphere.groundIrradiance", mAtmosphereGroundIrradiance));
        stateset->addUniform(
            new osg::Uniform("atmosphere.cloudAdvectionOffsetWorldUnits", mAtmosphereCloudAdvectionOffsetWorldUnits));
        stateset->addUniform(new osg::Uniform("atmosphere.quality", mAtmosphereQuality));

        stateset->addUniform(new osg::Uniform("waterHeight", mWaterHeight));
        stateset->addUniform(new osg::Uniform("waterEnabled", mWaterEnabled));
        stateset->addUniform(new osg::Uniform("waterSurface", false));
        stateset->addUniform(new osg::Uniform("openmwseAtmosphereDiagnosticMode", mAtmosphereDiagnosticMode));
    }

    /// Publish atmosphere uniforms and cloud specialization together for compatibility rendering.
    void StateUpdater::apply(osg::StateSet* stateset, osg::NodeVisitor*)
    {
        updateCloudShaderState(*stateset, mAtmosphereCloudScenarioParameters);
        configureSunAmbientOverride(mAmbientColor, stateset);

        SceneUtil::updateFogColor(*stateset, mFogColor);
        SceneUtil::updateFogStart(*stateset, mFogStart);
        SceneUtil::updateFogEnd(*stateset, mFogEnd);
        SceneUtil::updateUnderwaterFogColor(*stateset, mUnderwaterFogColor);
        SceneUtil::updateUnderwaterFogStart(*stateset, mUnderwaterFogStart);
        SceneUtil::updateUnderwaterFogEnd(*stateset, mUnderwaterFogEnd);

        stateset->getUniform("atmosphere.scatteringEnabled")->set(mAtmosphereScatteringEnabled);
        stateset->getUniform("atmosphere.appearance")->set(mAtmosphereAppearance);
        stateset->getUniform("atmosphere.sunlightEnabled")->set(mSunlightEnabled);
        stateset->getUniform("atmosphere.cloudSunOcclusion")->set(mCloudSunOcclusion);
        if (mTexturedCloudTextureUnit >= 0)
            applyTexturedCloudState(
                *stateset, mTexturedClouds, mTexturedCloudTextureUnit, mTexturedCloudTextureUnit + 1);
        stateset->getUniform("atmosphere.cloudsEnabled")->set(mAtmosphereCloudsEnabled);
        stateset->getUniform("atmosphere.cloudShadowsEnabled")->set(mAtmosphereCloudShadowsEnabled);
        stateset->getUniform("atmosphere.cloudReflectionsEnabled")->set(mAtmosphereCloudReflectionsEnabled);
        stateset->getUniform("atmosphere.enabled")->set(mAtmosphereEnabled);
        stateset->getUniform("atmosphere.skyColor")->set(mAtmosphereSkyColor);
        stateset->getUniform("atmosphere.inScattering")->set(mAtmosphereInScattering);
        stateset->getUniform("atmosphere.outScattering")->set(mAtmosphereOutScattering);
        stateset->getUniform("atmosphere.weatherWeight")->set(mAtmosphereWeatherWeight);
        stateset->getUniform("atmosphere.sunDirection")->set(mAtmosphereSunDirection);
        for (unsigned int index = 0; index < mAtmosphereCloudLayers.size(); ++index)
            stateset->getUniform("atmosphere.cloudLayers")->setElement(index, mAtmosphereCloudLayers[index]);
        stateset->getUniform("atmosphere.cloudParameters")->set(mAtmosphereCloudParameters);

        stateset->getUniform("atmosphere.incomingCloudParameters")->set(mAtmosphereIncomingCloudParameters);

        stateset->getUniform("atmosphere.cloudScenarioParameters")->set(mAtmosphereCloudScenarioParameters);
        stateset->getUniform("atmosphere.lowCloudParameters")->set(mAtmosphereLowCloudParameters);
        stateset->getUniform("atmosphere.lowCloudEnvironmentInfluence")->set(mAtmosphereLowCloudEnvironmentInfluence);
        stateset->getUniform("atmosphere.mistParameters")->set(mAtmosphereMistParameters);
        stateset->getUniform("atmosphere.fogVolumeParameters")->set(mAtmosphereFogVolumeParameters);
        stateset->getUniform("atmosphere.mediumReferenceParameters")->set(mAtmosphereMediumReferenceParameters);
        stateset->getUniform("atmosphere.orographicParameters")->set(mAtmosphereOrographicParameters);
        for (unsigned int index = 0; index < AtmosphereTerrainGrid::sPackedSize; ++index)
            stateset->getUniform("atmosphere.terrainHeights")->setElement(index, mAtmosphereTerrain.mHeights[index]);
        stateset->getUniform("atmosphere.terrainOriginSpacing")->set(mAtmosphereTerrain.mOriginSpacing);
        stateset->getUniform("atmosphere.terrainHeightBounds")->set(mAtmosphereTerrain.mHeightBounds);
        stateset->getUniform("atmosphere.sunDiscColor")->set(mAtmosphereSunDiscColor);
        stateset->getUniform("atmosphere.solarRadiance")->set(mAtmosphereSolarRadiance);
        stateset->getUniform("atmosphere.skyIrradiance")->set(mAtmosphereSkyIrradiance);
        stateset->getUniform("atmosphere.groundIrradiance")->set(mAtmosphereGroundIrradiance);
        stateset->getUniform("atmosphere.cloudAdvectionOffsetWorldUnits")
            ->set(mAtmosphereCloudAdvectionOffsetWorldUnits);
        stateset->getUniform("atmosphere.quality")->set(mAtmosphereQuality);

        stateset->getUniform("waterHeight")->set(mWaterHeight);
        stateset->getUniform("waterEnabled")->set(mWaterEnabled);
        stateset->getUniform("openmwseAtmosphereDiagnosticMode")->set(mAtmosphereDiagnosticMode);
    }

    void StateUpdater::setAmbientColor(const osg::Vec4f& col)
    {
        mAmbientColor = col;
    }

    void StateUpdater::setFogColor(const osg::Vec4f& col)
    {
        mFogColor = col;
    }

    void StateUpdater::setFogStart(float start)
    {
        mFogStart = start;
    }

    void StateUpdater::setFogEnd(float end)
    {
        mFogEnd = end;
    }

    void StateUpdater::setAtmosphereFeatures(bool scattering, bool clouds, bool shadows, bool reflections)
    {
        mAtmosphereScatteringEnabled = scattering;
        mAtmosphereCloudsEnabled = clouds;
        mAtmosphereCloudShadowsEnabled = shadows;
        mAtmosphereCloudReflectionsEnabled = reflections;
    }

    void StateUpdater::setAtmosphere(bool enabled, const osg::Vec4f& skyColor, const osg::Vec3f& inScattering,
        const osg::Vec3f& outScattering, float weatherWeight, const osg::Vec3f& sunDirection,
        const osg::Vec4f& cloudParameters, const osg::Vec4f& incomingCloudParameters,
        const osg::Vec4f& cloudScenarioParameters, const osg::Vec4f& lowCloudParameters,
        float lowCloudEnvironmentInfluence, const osg::Vec4f& mistParameters, const osg::Vec4f& fogVolumeParameters,
        const osg::Vec4f& mediumReferenceParameters, const osg::Vec4f& orographicParameters,
        const osg::Vec4f& sunDiscColor, const osg::Vec3f& solarRadiance, const osg::Vec3f& skyIrradiance,
        const osg::Vec3f& groundIrradiance, const osg::Vec2f& cloudAdvectionOffsetWorldUnits, int quality)
    {
        mAtmosphereEnabled = enabled;
        mAtmosphereSkyColor = skyColor;
        mAtmosphereInScattering = inScattering;
        mAtmosphereOutScattering = outScattering;
        mAtmosphereWeatherWeight = weatherWeight;
        mAtmosphereSunDirection = sunDirection;
        mAtmosphereCloudParameters = cloudParameters;
        mAtmosphereIncomingCloudParameters = incomingCloudParameters;
        mAtmosphereCloudScenarioParameters = cloudScenarioParameters;
        mAtmosphereLowCloudParameters = lowCloudParameters;
        mAtmosphereLowCloudEnvironmentInfluence = lowCloudEnvironmentInfluence;
        mAtmosphereMistParameters = mistParameters;
        mAtmosphereFogVolumeParameters = fogVolumeParameters;
        mAtmosphereMediumReferenceParameters = mediumReferenceParameters;
        mAtmosphereOrographicParameters = orographicParameters;
        mAtmosphereSunDiscColor = sunDiscColor;
        mAtmosphereSolarRadiance = solarRadiance;
        mAtmosphereSkyIrradiance = skyIrradiance;
        mAtmosphereGroundIrradiance = groundIrradiance;
        mAtmosphereCloudAdvectionOffsetWorldUnits = cloudAdvectionOffsetWorldUnits;
        mAtmosphereQuality = quality;
    }

    void StateUpdater::setUnderwaterFogColor(const osg::Vec4f& col)
    {
        mUnderwaterFogColor = col;
    }

    void StateUpdater::setUnderwaterFogStart(float start)
    {
        mUnderwaterFogStart = start;
    }

    void StateUpdater::setUnderwaterFogEnd(float end)
    {
        mUnderwaterFogEnd = end;
    }

    void StateUpdater::setWaterHeight(float waterHeight)
    {
        mWaterHeight = waterHeight;
    }

    void StateUpdater::setWaterEnabled(bool enabled)
    {
        mWaterEnabled = enabled;
    }

    void StateUpdater::setAtmosphereDiagnosticMode(int mode)
    {
        mAtmosphereDiagnosticMode = mode;
    }

    void StateUpdater::setWireframe(bool wireframe)
    {
        if (mWireframe != wireframe)
        {
            mWireframe = wireframe;
            reset();
        }
    }

    bool StateUpdater::getWireframe() const
    {
        return mWireframe;
    }

}
