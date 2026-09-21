#include "engine.hpp"

#include "diagnosticserialization.hpp"

#include <components/diagnostics/atmospherescenario.hpp>
#include <components/diagnostics/atmospheresettings.hpp>
#include <components/diagnostics/distancesettings.hpp>
#include <components/diagnostics/optimizationsettings.hpp>
#include <components/diagnostics/session.hpp>
#include <components/diagnostics/watersettings.hpp>
#include <components/settings/values.hpp>

#include <osg/Camera>
#include <osg/Image>
#include <osg/RenderInfo>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <optional>
#include <sstream>
#include <stdexcept>

#include "mwgui/windowmanagerimp.hpp"
#include "mwrender/atmospherediagnosticcontroller.hpp"
#include "mwrender/postprocessor.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwworld/atmosphereenvironment.hpp"
#include "mwworld/datetimemanager.hpp"
#include "mwworld/timestamp.hpp"
#include "mwworld/weather.hpp"
#include "mwworld/worldimp.hpp"

namespace
{
    /// Captures completed UI frames from either a windowed back buffer or a single-buffered diagnostic pbuffer.
    class DiagnosticScreenCapture final : public osg::Camera::DrawCallback
    {
    public:
        /// Create a capture that overwrites the same per-context image for the requested number of frames.
        DiagnosticScreenCapture(std::string prefix, int frames)
            : mOperation(new osgViewer::ScreenCaptureHandler::WriteToFile(
                std::move(prefix), "png", osgViewer::ScreenCaptureHandler::WriteToFile::OVERWRITE))
            , mFrames(frames)
        {
        }

        /// Read the framebuffer appropriate to the current context and remove the callback after the final frame.
        void operator()(osg::RenderInfo& renderInfo) const override
        {
            osg::State* state = renderInfo.getState();
            osg::GraphicsContext* context = state != nullptr ? state->getGraphicsContext() : nullptr;
            const osg::GraphicsContext::Traits* traits = context != nullptr ? context->getTraits() : nullptr;
            if (state == nullptr || traits == nullptr)
                throw std::runtime_error("diagnostic screenshot requires a valid graphics context");

            glReadBuffer(traits->doubleBuffer ? GL_BACK : GL_FRONT);
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->readPixels(
                0, 0, traits->width, traits->height, traits->alpha > 0 ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE);
            (*mOperation)(*image, state->getContextID());

            if (--mFrames == 0)
                renderInfo.getCurrentCamera()->setFinalDrawCallback(nullptr);
        }

    private:
        osg::ref_ptr<osgViewer::ScreenCaptureHandler::CaptureOperation> mOperation;
        mutable int mFrames;
    };

    /// Attach a capture callback to the last active camera so the resulting image includes post-render UI.
    void captureVideoSettings(osgViewer::ViewerBase& viewer, std::string prefix)
    {
        osgViewer::ViewerBase::Cameras cameras;
        viewer.getCameras(cameras, true);
        const auto camera = std::max_element(cameras.begin(), cameras.end(), osg::CameraRenderOrderSortOp{});
        if (camera == cameras.end())
            throw std::runtime_error("diagnostic screenshot requires an active camera");
        (*camera)->setFinalDrawCallback(new DiagnosticScreenCapture(std::move(prefix), 3));
    }
}

void OMW::Engine::processDiagnosticGraphicsCommand(
    const Diagnostics::Command& command, Diagnostics::Response& response, std::ostringstream& output)
{
    MWWorld::DateTimeManager& time = *mWorld->getTimeManager();
    switch (command.mType)
    {
        case Diagnostics::CommandType::ShowVideoSettings:
            if (!mDiagnosticChild)
                response.mError = Diagnostics::ErrorCode::PermissionDenied;
            else
            {
                const std::string page = diagnosticText(command.mPayload);
                mWindowManager->showVideoSettings(page);
                mWindowManager->setHudVisibility(false);
                const auto prefix = (mDiagnosticArtifactRoot / ("video-settings-" + page)).string();
                captureVideoSettings(*mViewer, prefix);
            }
            break;

        case Diagnostics::CommandType::SetDistanceSettings:
        {
            if (!mDiagnosticChild)
            {
                response.mError = Diagnostics::ErrorCode::PermissionDenied;
                break;
            }
            const auto settings = Diagnostics::deserializeDistanceSettings(diagnosticText(command.mPayload));
            mWindowManager->setVideoDistanceValues(
                settings.mUseCellEditor, settings.mView, settings.mGrass, settings.mShadow, settings.mShadowResolution);
            if (settings.mFog)
                mWindowManager->setVideoFogValues(settings.mUseCellEditor, settings.mFog->mEnabled,
                    settings.mFog->mStart, settings.mFog->mEnd, settings.mFog->mStartLocked, settings.mFog->mEndLocked);
            Diagnostics::DistanceSettings observed{ Settings::camera().mViewingDistance,
                Settings::groundcover().mRenderingDistance, Settings::shadows().mMaximumShadowMapDistance,
                settings.mUseCellEditor,
                settings.mShadowResolution ? std::optional<int>(Settings::shadows().mShadowMapResolution)
                                           : std::nullopt,
                std::nullopt };
            if (settings.mFog)
                observed.mFog = Diagnostics::FogDistanceSettings{ Settings::fog().mUseDistantFog,
                    Settings::fog().mDistantLandFogStart, Settings::fog().mDistantLandFogEnd,
                    Settings::fog().mDistantLandFogStartLocked, Settings::fog().mDistantLandFogEndLocked };
            response.mPayload = diagnosticBytes(Diagnostics::serializeDistanceSettings(observed));
            break;
        }

        case Diagnostics::CommandType::SetWaterSettings:
        {
            if (!mDiagnosticChild)
            {
                response.mError = Diagnostics::ErrorCode::PermissionDenied;
                break;
            }
            const auto settings = Diagnostics::deserializeWaterSettings(diagnosticText(command.mPayload));
            mWindowManager->showVideoSettings("water");
            mWindowManager->setVideoWaterValues(settings.mAppearance, settings.mGeometricWaveHeight, settings.mCaustics,
                settings.mCausticsStrength, settings.mInteriorLocalLights, settings.mExteriorLocalLights);
            response.mPayload = diagnosticBytes(Diagnostics::serializeWaterSettings(
                { Settings::openmwseWater().mGeometricWaveHeight, Settings::openmwseWater().mCaustics,
                    Settings::openmwseWater().mCausticsStrength, Settings::openmwseWater().mInteriorLocalLights,
                    Settings::openmwseWater().mExteriorLocalLights, Settings::openmwseWater().mAppearance }));
            break;
        }

        case Diagnostics::CommandType::SetOptimizationSettings:
        {
            if (!mDiagnosticChild)
            {
                response.mError = Diagnostics::ErrorCode::PermissionDenied;
                break;
            }
            const auto requested = Diagnostics::deserializeOptimizationSettings(diagnosticText(command.mPayload));
            auto& settings = Settings::optimizations();
            settings.mEnabled.set(requested.mEnabled);
            settings.mGpuDrivenPaging.set(requested.mGpuDrivenPaging);
            settings.mGpuSkinning.set(requested.mGpuSkinning);
            settings.mGpuOcclusionCulling.set(requested.mGpuOcclusionCulling);
            settings.mGpuActors.set(requested.mGpuActors);
            settings.mGpuTextureArrays.set(requested.mGpuTextureArrays);
            settings.mIndexedLightLists.set(requested.mIndexedLightLists);
            settings.mGpuSceneLighting.set(requested.mGpuSceneLighting);
            settings.mGpuStorageBudget.set(requested.mGpuStorageBudget);
            settings.mLowEndIndirectBatching.set(requested.mLowEndIndirectBatching);
            settings.mGpuLightOcclusion.set(requested.mGpuLightOcclusion);
            settings.mGpuDistantPaging.set(requested.mGpuDistantPaging);
            settings.mSkipDormantRipples.set(requested.mSkipDormantRipples);
            const auto changed = Settings::Manager::getPendingChanges();
            mWorld->processChangedSettings(changed);
            Settings::Manager::resetPendingChanges();
            response.mPayload = diagnosticBytes(Diagnostics::serializeOptimizationSettings(
                { settings.mEnabled, settings.mGpuDrivenPaging, settings.mGpuSkinning, settings.mGpuOcclusionCulling,
                    settings.mGpuActors, settings.mGpuTextureArrays, settings.mIndexedLightLists,
                    settings.mGpuSceneLighting, settings.mGpuStorageBudget, settings.mLowEndIndirectBatching,
                    settings.mGpuLightOcclusion, settings.mGpuDistantPaging, settings.mSkipDormantRipples }));
            break;
        }

        case Diagnostics::CommandType::SetAtmosphereSettings:
        {
            if (!mDiagnosticChild)
            {
                response.mError = Diagnostics::ErrorCode::PermissionDenied;
                break;
            }
            const auto settings = Diagnostics::deserializeAtmosphereSettings(diagnosticText(command.mPayload));
            Settings::shaders().mAtmosphericScattering.set(settings.mScattering);
            Settings::shaders().mVolumetricClouds.set(settings.mClouds);
            Settings::shaders().mAtmosphericQuality.set(settings.mQuality);
            Settings::shaders().mAtmosphereAppearance.set(settings.mAppearance);
            Settings::shaders().mGodrays.set(settings.mGodrays);
            Settings::shaders().mSunGlare.set(settings.mSunGlare);
            Settings::shaders().mCloudSunOcclusion.set(settings.mCloudSunOcclusion);
            Settings::shadows().mCloudShadows.set(settings.mShadows);
            Settings::water().mReflectClouds.set(settings.mReflections);
            const auto changed = Settings::Manager::getPendingChanges();
            mWorld->processChangedSettings(changed);
            Settings::Manager::resetPendingChanges();
            response.mPayload
                = diagnosticBytes(Diagnostics::serializeAtmosphereSettings({ Settings::shaders().mAtmosphericScattering,
                    Settings::shaders().mVolumetricClouds, Settings::shaders().mAtmosphericQuality,
                    Settings::shadows().mCloudShadows, Settings::water().mReflectClouds,
                    Settings::shaders().mAtmosphereAppearance, Settings::shaders().mGodrays,
                    Settings::shaders().mSunGlare, Settings::shaders().mCloudSunOcclusion }));
            break;
        }

        case Diagnostics::CommandType::SetAtmosphereScenario:
        {
            if (!mDiagnosticChild)
            {
                response.mError = Diagnostics::ErrorCode::PermissionDenied;
                break;
            }
            Diagnostics::AtmosphereScenario scenario;
            try
            {
                scenario = Diagnostics::deserializeAtmosphereScenario(diagnosticText(command.mPayload));
                if (scenario.mGameHour)
                    mWorld->setDiagnosticGameHour(*scenario.mGameHour);
                if (scenario.mStartGameHour)
                    mWorld->setDiagnosticGameHour(*scenario.mStartGameHour);
                if (scenario.mWeather)
                {
                    const std::optional<ESM::RefId> next = scenario.mWeather->mNext
                        ? std::optional<ESM::RefId>(ESM::RefId::deserializeText(*scenario.mWeather->mNext))
                        : std::nullopt;
                    mWorld->setDiagnosticWeatherState(ESM::RefId::deserializeText(scenario.mWeather->mCurrent), next,
                        scenario.mWeather->mTransition, scenario.mCloudSeed.value_or(0));
                }
                if (scenario.mEnvironment)
                {
                    MWWorld::AtmosphereEnvironmentState environment = mWorld->getAtmosphereEnvironmentState();
                    environment.mTraits = scenario.mEnvironment->mTraits;
                    if (scenario.mEnvironment->mTerrainHeightWorldUnits)
                        environment.mTerrainHeightWorldUnits = *scenario.mEnvironment->mTerrainHeightWorldUnits;
                    mWorld->setDiagnosticEnvironmentState(std::move(environment));
                }
                if (scenario.mRecentPrecipitation)
                    mWorld->setDiagnosticRecentPrecipitation(*scenario.mRecentPrecipitation);
                const Diagnostics::DiagnosticWeatherEffects weatherEffects
                    = scenario.mWeatherEffects.value_or(Diagnostics::DiagnosticWeatherEffects{});
                mWorld->setDiagnosticWeatherEffects(weatherEffects.mPrecipitation, weatherEffects.mLightning);
                const Diagnostics::DiagnosticAtmosphereMedia media
                    = scenario.mMedia.value_or(Diagnostics::DiagnosticAtmosphereMedia{});
                mWorld->getRenderingManager()->atmosphereDiagnostics().setMedia(
                    { media.mMainClouds, media.mLowClouds, media.mMist, media.mFog });
                mWorld->getRenderingManager()->atmosphereDiagnostics().setDistanceFogEnabled(
                    scenario.mDistanceFog.value_or(true));
            }
            catch (const std::exception& error)
            {
                response.mError = Diagnostics::ErrorCode::InvalidFrame;
                response.mPayload = diagnosticBytes(error.what());
                break;
            }
            mWorld->getRenderingManager()->invalidateAtmosphereHistory();
            mDiagnosticAtmosphereScenario = scenario;
            output << "scenario-version: " << Diagnostics::atmosphereScenarioVersion << '\n'
                   << "weather: " << quoteDiagnosticYaml(mWorld->getCurrentWeather().mId.serializeText()) << '\n'
                   << "next-weather: "
                   << quoteDiagnosticYaml(
                          mWorld->getNextWeather() ? mWorld->getNextWeather()->mId.serializeText() : std::string_view())
                   << '\n'
                   << "weather-transition: " << (mWorld->getNextWeather() ? 1.f - mWorld->getWeatherTransition() : 0.f)
                   << '\n'
                   << "game-hour: " << time.getTimeStamp().getHour() << '\n'
                   << "main-clouds: "
                   << (mDiagnosticAtmosphereScenario->mMedia ? mDiagnosticAtmosphereScenario->mMedia->mMainClouds
                                                             : true)
                   << '\n'
                   << "low-clouds: "
                   << (mDiagnosticAtmosphereScenario->mMedia ? mDiagnosticAtmosphereScenario->mMedia->mLowClouds : true)
                   << '\n'
                   << "mist: "
                   << (mDiagnosticAtmosphereScenario->mMedia ? mDiagnosticAtmosphereScenario->mMedia->mMist : true)
                   << '\n'
                   << "fog: "
                   << (mDiagnosticAtmosphereScenario->mMedia ? mDiagnosticAtmosphereScenario->mMedia->mFog : true)
                   << '\n'
                   << "distance-fog: " << mDiagnosticAtmosphereScenario->mDistanceFog.value_or(true) << '\n'
                   << "cloud-seed: " << mDiagnosticAtmosphereScenario->mCloudSeed.value_or(0) << '\n'
                   << "recent-precipitation: " << mDiagnosticAtmosphereScenario->mRecentPrecipitation.value_or(0.f)
                   << '\n'
                   << "precipitation: "
                   << (mDiagnosticAtmosphereScenario->mWeatherEffects
                                  && mDiagnosticAtmosphereScenario->mWeatherEffects->mPrecipitation
                              ? "true"
                              : "false")
                   << '\n'
                   << "lightning: "
                   << (mDiagnosticAtmosphereScenario->mWeatherEffects
                                  && mDiagnosticAtmosphereScenario->mWeatherEffects->mLightning
                              ? "true"
                              : "false")
                   << '\n'
                   << "environment-traits:\n";
            for (const auto& [trait, weight] : mWorld->getAtmosphereEnvironmentState().mTraits)
                output << "  " << quoteDiagnosticYaml(trait) << ": " << weight << '\n';
            output << "environment-terrain-height: " << mWorld->getAtmosphereEnvironmentState().mTerrainHeightWorldUnits
                   << '\n'
                   << "environment-terrain-sample-center: ["
                   << mWorld->getAtmosphereEnvironmentState().mTerrainSampleCenterWorldUnits.x() << ", "
                   << mWorld->getAtmosphereEnvironmentState().mTerrainSampleCenterWorldUnits.y() << "]\n"
                   << "environment-terrain-gradient: [" << mWorld->getAtmosphereEnvironmentState().mTerrainGradient.x()
                   << ", " << mWorld->getAtmosphereEnvironmentState().mTerrainGradient.y() << "]\n"
                   << "environment-valley-depth-world-units: "
                   << mWorld->getAtmosphereEnvironmentState().mValleyDepthWorldUnits << '\n'
                   << "atmosphere-history: reset\n";
            response.mPayload = diagnosticBytes(output.str());
            break;
        }

        default:
            throw std::logic_error("non-graphics command reached the graphics diagnostic owner");
    }
}
