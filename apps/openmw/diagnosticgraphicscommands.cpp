#include "engine.hpp"

#include "diagnosticserialization.hpp"

#include <components/diagnostics/atmospherescenario.hpp>
#include <components/diagnostics/atmospheresettings.hpp>
#include <components/diagnostics/distancesettings.hpp>
#include <components/diagnostics/session.hpp>
#include <components/settings/values.hpp>

#include <osgViewer/ViewerEventHandlers>
#include <yaml-cpp/yaml.h>

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
                mWindowManager->showVideoSettings(diagnosticText(command.mPayload));
                mWindowManager->setHudVisibility(false);
                const auto prefix = (mDiagnosticArtifactRoot / "video-settings").string();
                mScreenCaptureHandler->setCaptureOperation(new osgViewer::ScreenCaptureHandler::WriteToFile(
                    prefix, "png", osgViewer::ScreenCaptureHandler::WriteToFile::OVERWRITE));
                // Retain the last framebuffer after MyGUI and asynchronous pixel readback have advanced.
                mScreenCaptureHandler->setFramesToCapture(3);
                mScreenCaptureHandler->startCapture();
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
            response.mPayload = diagnosticBytes(Diagnostics::serializeDistanceSettings(
                { Settings::camera().mViewingDistance, Settings::groundcover().mRenderingDistance,
                    Settings::shadows().mMaximumShadowMapDistance, settings.mUseCellEditor,
                    settings.mShadowResolution ? std::optional<int>(Settings::shadows().mShadowMapResolution)
                                               : std::nullopt }));
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
