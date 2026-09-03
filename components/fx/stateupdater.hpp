#ifndef OPENMW_COMPONENTS_FX_STATEUPDATER_H
#define OPENMW_COMPONENTS_FX_STATEUPDATER_H

#include <osg/BufferTemplate>

#include <optional>

#include <components/sceneutil/cloudadvection.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/statesetupdater.hpp>
#include <components/std140/ubo.hpp>

namespace Fx
{
    class StateUpdater : public SceneUtil::StateSetUpdater
    {
    public:
        StateUpdater(bool useUBO);

        void setProjectionMatrix(const osg::Matrixf& matrix)
        {
            mData.get<ProjectionMatrix>() = matrix;
            mData.get<InvProjectionMatrix>() = osg::Matrixf::inverse(matrix);
        }

        /// Return the current projection matrix used by native post-process passes.
        osg::Matrixf getProjectionMatrix() const { return mData.get<ProjectionMatrix>(); }

        /// Set the previous-frame projection matrix used for temporal reprojection.
        void setPrevProjectionMatrix(const osg::Matrixf& matrix) { mData.get<PrevProjectionMatrix>() = matrix; }

        void setViewMatrix(const osg::Matrixf& matrix)
        {
            mData.get<ViewMatrix>() = matrix;
            mData.get<InvViewMatrix>() = osg::Matrixf::inverse(matrix);
        }

        void setPrevViewMatrix(const osg::Matrixf& matrix) { mData.get<PrevViewMatrix>() = matrix; }

        void setEyePos(const osg::Vec3f& pos) { mData.get<EyePos>() = osg::Vec4f(pos, 0.f); }

        void setEyeVec(const osg::Vec3f& vec) { mData.get<EyeVec>() = osg::Vec4f(vec, 0.f); }

        void setFogColor(const osg::Vec4f& color) { mData.get<FogColor>() = color; }

        void setAmbientColor(const osg::Vec4f& color) { mData.get<AmbientColor>() = color; }

        void setSkyColor(const osg::Vec4f& color) { mData.get<SkyColor>() = color; }

        void setSunColor(const osg::Vec4f& color) { mData.get<SunColor>() = color; }

        void setSunPos(const osg::Vec4f& pos, bool night)
        {
            mData.get<SunPos>() = pos;
            mData.get<SunPos>().normalize();

            if (night)
                mData.get<SunPos>().z() *= -1.f;
        }

        void setSunVec(const osg::Vec4f& vec)
        {
            mData.get<SunVec>() = vec;
            mData.get<SunVec>().normalize();
        }

        void setResolution(const osg::Vec2f& size)
        {
            mData.get<Resolution>() = size;
            mData.get<RcpResolution>() = { 1.f / size.x(), 1.f / size.y() };
        }

        void setSunVis(float vis) { mData.get<SunVis>() = vis; }

        void setFogRange(float near, float far)
        {
            mData.get<FogNear>() = near;
            mData.get<FogFar>() = far;
        }

        void setNearFar(float near, float far)
        {
            mData.get<Near>() = near;
            mData.get<Far>() = far;
        }

        void setIsUnderwater(bool underwater) { mData.get<IsUnderwater>() = underwater; }

        void setIsInterior(bool interior) { mData.get<IsInterior>() = interior; }

        void setFov(float fov) { mData.get<Fov>() = fov; }

        void setGameHour(float hour) { mData.get<GameHour>() = hour; }

        void setWeatherId(int id) { mData.get<WeatherID>() = id; }

        void setNextWeatherId(int id) { mData.get<NextWeatherID>() = id; }

        void setWaterHeight(float height) { mData.get<WaterHeight>() = height; }

        void setIsWaterEnabled(bool enabled) { mData.get<IsWaterEnabled>() = enabled; }

        void setSimulationTime(float time) { mData.get<SimulationTime>() = time; }

        void setDeltaSimulationTime(float time) { mData.get<DeltaSimulationTime>() = time; }

        void setFrameNumber(int frame) { mData.get<FrameNumber>() = frame; }

        /// Read the frame identity published to a render state set, independent of subsequent updater changes.
        static std::optional<int> getPublishedFrameNumber(const osg::StateSet& stateset);

        void setWindSpeed(float speed) { mData.get<WindSpeed>() = speed; }

        void setWeatherTransition(float transition)
        {
            mData.get<WeatherTransition>() = transition > 0 ? 1 - transition : 0;
        }

        /// Publish canonical scene-linear atmosphere, volume, and lighting state to native passes.
        void setAtmosphere(const osg::Vec3f& inScattering, const osg::Vec3f& outScattering,
            const osg::Vec4f& cloudParameters, const osg::Vec4f& cloudShapeParameters,
            const osg::Vec4f& cloudOrganizationParameters, const osg::Vec4f& cloudStructureParameters,
            const osg::Vec4f& lowCloudParameters, const osg::Vec4f& lowCloudShapeParameters,
            const osg::Vec4f& mistParameters, const osg::Vec4f& fogVolumeParameters,
            const osg::Vec4f& mediumReferenceParameters, const osg::Vec4f& orographicParameters,
            const osg::Vec4f& sunDiscColor, const osg::Vec3f& solarRadiance, const osg::Vec3f& skyIrradiance,
            const osg::Vec3f& groundIrradiance, const osg::Vec2f& cloudAdvectionOffsetWorldUnits,
            const osg::Vec2f& cloudAdvectionDeltaWorldUnits, float historyLightingWeight, float weatherWeight,
            int quality, float volumeResolutionScale, bool enabled)
        {
            mData.get<AtmosphereInScattering>() = inScattering;
            mData.get<AtmosphereOutScattering>() = outScattering;
            mData.get<AtmosphereCloudParameters>() = cloudParameters;
            mData.get<AtmosphereCloudShapeParameters>() = cloudShapeParameters;
            mData.get<AtmosphereCloudOrganizationParameters>() = cloudOrganizationParameters;
            mData.get<AtmosphereCloudStructureParameters>() = cloudStructureParameters;
            mData.get<AtmosphereLowCloudParameters>() = lowCloudParameters;
            mData.get<AtmosphereLowCloudShapeParameters>() = lowCloudShapeParameters;
            mData.get<AtmosphereMistParameters>() = mistParameters;
            mData.get<AtmosphereFogVolumeParameters>() = fogVolumeParameters;
            mData.get<AtmosphereMediumReferenceParameters>() = mediumReferenceParameters;
            mData.get<AtmosphereOrographicParameters>() = orographicParameters;
            mData.get<AtmosphereSunDiscColor>() = sunDiscColor;
            mData.get<AtmosphereSolarRadiance>() = solarRadiance;
            mData.get<AtmosphereSkyIrradiance>() = skyIrradiance;
            mData.get<AtmosphereGroundIrradiance>() = groundIrradiance;
            mData.get<AtmosphereCloudAdvectionOffset>() = cloudAdvectionOffsetWorldUnits;
            mData.get<AtmosphereCloudShapeAdvectionOffset>()
                = SceneUtil::cloudShapeAdvectionOffset(cloudAdvectionOffsetWorldUnits);
            mData.get<AtmosphereCloudAdvectionDelta>() = cloudAdvectionDeltaWorldUnits;
            mData.get<AtmosphereHistoryLightingWeight>() = historyLightingWeight;
            mData.get<AtmosphereWeatherWeight>() = weatherWeight;
            mData.get<AtmosphereQuality>() = quality;
            mData.get<AtmosphereVolumeResolutionScale>() = volumeResolutionScale;
            mData.get<AtmosphereEnabled>() = enabled;
        }

        void bindPointLights(std::shared_ptr<SceneUtil::PPLightBuffer> buffer)
        {
            mPointLightBuffer = std::move(buffer);
        }

        static const std::string& getStructDefinition() { return sDefinition; }

        void setDefaults(osg::StateSet* stateset) override;

        void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override;

    private:
        struct ProjectionMatrix : Std140::Mat4
        {
            static constexpr std::string_view sName = "projectionMatrix";
        };

        struct InvProjectionMatrix : Std140::Mat4
        {
            static constexpr std::string_view sName = "invProjectionMatrix";
        };

        struct ViewMatrix : Std140::Mat4
        {
            static constexpr std::string_view sName = "viewMatrix";
        };

        struct PrevViewMatrix : Std140::Mat4
        {
            static constexpr std::string_view sName = "prevViewMatrix";
        };

        struct InvViewMatrix : Std140::Mat4
        {
            static constexpr std::string_view sName = "invViewMatrix";
        };

        struct EyePos : Std140::Vec4
        {
            static constexpr std::string_view sName = "eyePos";
        };

        struct EyeVec : Std140::Vec4
        {
            static constexpr std::string_view sName = "eyeVec";
        };

        struct AmbientColor : Std140::Vec4
        {
            static constexpr std::string_view sName = "ambientColor";
        };

        struct SkyColor : Std140::Vec4
        {
            static constexpr std::string_view sName = "skyColor";
        };

        struct FogColor : Std140::Vec4
        {
            static constexpr std::string_view sName = "fogColor";
        };

        struct SunColor : Std140::Vec4
        {
            static constexpr std::string_view sName = "sunColor";
        };

        struct SunPos : Std140::Vec4
        {
            static constexpr std::string_view sName = "sunPos";
        };

        struct SunVec : Std140::Vec4
        {
            static constexpr std::string_view sName = "sunVec";
        };

        struct Resolution : Std140::Vec2
        {
            static constexpr std::string_view sName = "resolution";
        };

        struct RcpResolution : Std140::Vec2
        {
            static constexpr std::string_view sName = "rcpResolution";
        };

        struct FogNear : Std140::Float
        {
            static constexpr std::string_view sName = "fogNear";
        };

        struct FogFar : Std140::Float
        {
            static constexpr std::string_view sName = "fogFar";
        };

        struct Near : Std140::Float
        {
            static constexpr std::string_view sName = "near";
        };

        struct Far : Std140::Float
        {
            static constexpr std::string_view sName = "far";
        };

        struct Fov : Std140::Float
        {
            static constexpr std::string_view sName = "fov";
        };

        struct GameHour : Std140::Float
        {
            static constexpr std::string_view sName = "gameHour";
        };

        struct SunVis : Std140::Float
        {
            static constexpr std::string_view sName = "sunVis";
        };

        struct WaterHeight : Std140::Float
        {
            static constexpr std::string_view sName = "waterHeight";
        };

        struct IsWaterEnabled : Std140::Bool
        {
            static constexpr std::string_view sName = "isWaterEnabled";
        };

        struct SimulationTime : Std140::Float
        {
            static constexpr std::string_view sName = "simulationTime";
        };

        struct DeltaSimulationTime : Std140::Float
        {
            static constexpr std::string_view sName = "deltaSimulationTime";
        };

        struct FrameNumber : Std140::Int
        {
            static constexpr std::string_view sName = "frameNumber";
        };

        struct WindSpeed : Std140::Float
        {
            static constexpr std::string_view sName = "windSpeed";
        };

        struct WeatherTransition : Std140::Float
        {
            static constexpr std::string_view sName = "weatherTransition";
        };

        struct WeatherID : Std140::Int
        {
            static constexpr std::string_view sName = "weatherID";
        };

        struct NextWeatherID : Std140::Int
        {
            static constexpr std::string_view sName = "nextWeatherID";
        };

        struct IsUnderwater : Std140::Bool
        {
            static constexpr std::string_view sName = "isUnderwater";
        };

        struct IsInterior : Std140::Bool
        {
            static constexpr std::string_view sName = "isInterior";
        };

        /// Bind the previous-frame projection used by temporal reprojection.
        struct PrevProjectionMatrix : Std140::Mat4
        {
            static constexpr std::string_view sName = "prevProjectionMatrix";
        };

        /// Bind the atmosphere's forward spectral scattering coefficient.
        struct AtmosphereInScattering : Std140::Vec3
        {
            static constexpr std::string_view sName = "atmosphereInScattering";
        };

        /// Bind the atmosphere's backward spectral scattering coefficient.
        struct AtmosphereOutScattering : Std140::Vec3
        {
            static constexpr std::string_view sName = "atmosphereOutScattering";
        };

        /// Bind weather-controlled cloud coverage, density, base height, and thickness.
        struct AtmosphereCloudParameters : Std140::Vec4
        {
            static constexpr std::string_view sName = "atmosphereCloudParameters";
        };

        /// Bind weather-controlled cloud morphology, scale, vertical development, and anvil strength.
        struct AtmosphereCloudShapeParameters : Std140::Vec4
        {
            static constexpr std::string_view sName = "atmosphereCloudShapeParameters";
        };

        /// Bind independent deck continuity, convective frequency, maturity, and storm organization.
        struct AtmosphereCloudOrganizationParameters : Std140::Vec4
        {
            static constexpr std::string_view sName = "atmosphereCloudOrganizationParameters";
        };

        /// Bind tower taper, cloud-base variation, and the stratiform deck family.
        struct AtmosphereCloudStructureParameters : Std140::Vec4
        {
            static constexpr std::string_view sName = "atmosphereCloudStructureParameters";
        };

        /// Bind low-cloud coverage, density, base height, and thickness.
        struct AtmosphereLowCloudParameters : Std140::Vec4
        {
            static constexpr std::string_view sName = "atmosphereLowCloudParameters";
        };

        /// Bind low-cloud morphology, scale, vertical development, and environment influence.
        struct AtmosphereLowCloudShapeParameters : Std140::Vec4
        {
            static constexpr std::string_view sName = "atmosphereLowCloudShapeParameters";
        };

        /// Bind weather-controlled mist density, base height, thickness, and shaft intensity.
        struct AtmosphereMistParameters : Std140::Vec4
        {
            static constexpr std::string_view sName = "atmosphereMistParameters";
        };

        /// Bind weather-controlled volumetric fog density, base height, thickness, and anisotropy.
        struct AtmosphereFogVolumeParameters : Std140::Vec4
        {
            static constexpr std::string_view sName = "atmosphereFogVolumeParameters";
        };

        /// Bind local terrain height and terrain-following cloud-layer controls.
        struct AtmosphereMediumReferenceParameters : Std140::Vec4
        {
            static constexpr std::string_view sName = "atmosphereMediumReferenceParameters";
        };

        /// Bind terrain sample center and local gradient for sparse orographic formation.
        struct AtmosphereOrographicParameters : Std140::Vec4
        {
            static constexpr std::string_view sName = "atmosphereOrographicParameters";
        };

        /// Bind scene-linear analytic sun color and visibility.
        struct AtmosphereSunDiscColor : Std140::Vec4
        {
            static constexpr std::string_view sName = "atmosphereSunDiscColor";
        };

        /// Bind scene-linear direct solar radiance.
        struct AtmosphereSolarRadiance : Std140::Vec3
        {
            static constexpr std::string_view sName = "atmosphereSolarRadiance";
        };

        /// Bind scene-linear hemispherical sky irradiance.
        struct AtmosphereSkyIrradiance : Std140::Vec3
        {
            static constexpr std::string_view sName = "atmosphereSkyIrradiance";
        };

        /// Bind scene-linear lower-hemisphere ground irradiance.
        struct AtmosphereGroundIrradiance : Std140::Vec3
        {
            static constexpr std::string_view sName = "atmosphereGroundIrradiance";
        };

        /// Store integrated cloud displacement in world units for every procedural cloud consumer.
        struct AtmosphereCloudAdvectionOffset : Std140::Vec2
        {
            static constexpr std::string_view sName = "atmosphereCloudAdvectionOffsetWorldUnits";
        };

        /// Store independent three-dimensional shape motion without changing coverage or low-media advection.
        struct AtmosphereCloudShapeAdvectionOffset : Std140::Vec3
        {
            static constexpr std::string_view sName = "atmosphereCloudShapeAdvectionOffsetWorldUnits";
        };

        /// Store exact current-frame cloud displacement in world units for temporal reprojection.
        struct AtmosphereCloudAdvectionDelta : Std140::Vec2
        {
            static constexpr std::string_view sName = "atmosphereCloudAdvectionDeltaWorldUnits";
        };

        /// Bind confidence that previous-frame atmosphere radiance remains compatible with the current frame.
        struct AtmosphereHistoryLightingWeight : Std140::Float
        {
            static constexpr std::string_view sName = "atmosphereHistoryLightingWeight";
        };

        /// Bind the regional directional-scattering blend weight.
        struct AtmosphereWeatherWeight : Std140::Float
        {
            static constexpr std::string_view sName = "atmosphereWeatherWeight";
        };

        /// Bind whether the unified atmosphere system contributes to the frame.
        struct AtmosphereEnabled : Std140::Bool
        {
            static constexpr std::string_view sName = "atmosphereEnabled";
        };

        /// Bind the bounded atmosphere quality tier.
        struct AtmosphereQuality : Std140::Int
        {
            static constexpr std::string_view sName = "atmosphereQuality";
        };

        /// Bind the internal volume-buffer scale for reconstruction footprints.
        struct AtmosphereVolumeResolutionScale : Std140::Float
        {
            static constexpr std::string_view sName = "atmosphereVolumeResolutionScale";
        };

        using UniformData = Std140::UBO<ProjectionMatrix, InvProjectionMatrix, PrevProjectionMatrix, ViewMatrix,
            PrevViewMatrix, InvViewMatrix, EyePos, EyeVec, FogColor, AmbientColor, SkyColor, SunColor, SunPos, SunVec,
            Resolution, RcpResolution, FogNear, FogFar, Near, Far, Fov, GameHour, SunVis, WaterHeight, IsWaterEnabled,
            SimulationTime, DeltaSimulationTime, FrameNumber, WindSpeed, WeatherTransition, WeatherID, NextWeatherID,
            IsUnderwater, IsInterior, AtmosphereInScattering, AtmosphereOutScattering, AtmosphereCloudParameters,
            AtmosphereCloudShapeParameters, AtmosphereCloudOrganizationParameters, AtmosphereCloudStructureParameters,
            AtmosphereLowCloudParameters, AtmosphereLowCloudShapeParameters, AtmosphereMistParameters,
            AtmosphereFogVolumeParameters, AtmosphereMediumReferenceParameters, AtmosphereOrographicParameters,
            AtmosphereSunDiscColor, AtmosphereSolarRadiance, AtmosphereSkyIrradiance, AtmosphereGroundIrradiance,
            AtmosphereCloudAdvectionOffset, AtmosphereCloudShapeAdvectionOffset, AtmosphereCloudAdvectionDelta,
            AtmosphereHistoryLightingWeight, AtmosphereWeatherWeight, AtmosphereQuality,
            AtmosphereVolumeResolutionScale, AtmosphereEnabled>;

        UniformData mData;
        bool mUseUBO;

        static std::string sDefinition;

        std::shared_ptr<SceneUtil::PPLightBuffer> mPointLightBuffer;
    };
}

#endif
