#include "engine.hpp"

#include "diagnosticperformance.hpp"
#include "diagnosticrenderconfiguration.hpp"
#include "diagnosticserialization.hpp"

#include <components/diagnostics/atmospherescenario.hpp>
#include <components/diagnostics/capsule.hpp>
#include <components/files/hash.hpp>
#include <components/version/version.hpp>
#include <components/vfs/manager.hpp>

#include <iomanip>
#include <limits>
#include <sstream>

#include "mwrender/camera.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwstate/statemanagerimp.hpp"
#include "mwworld/datetimemanager.hpp"
#include "mwworld/timestamp.hpp"
#include "mwworld/weather.hpp"
#include "mwworld/worldimp.hpp"

namespace
{
    /// Select the newest frame whose asynchronous render measurements have reached the viewer statistics store.
    unsigned diagnosticStatisticsFrame(unsigned currentFrame) noexcept
    {
        constexpr unsigned renderStatisticsLatency = 3;
        return currentFrame > renderStatisticsLatency ? currentFrame - renderStatisticsLatency : 0;
    }
}

Diagnostics::Capsule OMW::Engine::createDiagnosticCapsule(unsigned frameNumber)
{
    MWWorld::DateTimeManager& time = *mWorld->getTimeManager();
    const MWRender::Camera* camera = mWorld->getCamera();
    const osg::Vec3d cameraPosition = camera->getPosition();
    const bool cameraUnderwater = mWorld->isUnderwater(
        mWorld->getPlayerPtr().getCell(), osg::Vec3f(cameraPosition.x(), cameraPosition.y(), cameraPosition.z()));
    const MWWorld::Weather& weather = mWorld->getCurrentWeather();

    Diagnostics::Capsule capsule;
    std::ostringstream manifest;
    manifest << "capsule-version: " << Diagnostics::capsuleVersion << '\n'
             << "build: " << quoteDiagnosticYaml(Version::getOpenmwVersionDescription()) << '\n'
             << "frame: " << frameNumber << '\n'
             << "providers: [core@2, graphics@3, gameplay@9, " << OMW::diagnosticPerformanceProvider << "]\n"
             << "content:\n";
    for (const std::string& content : mContentFiles)
    {
        manifest << "  - name: " << quoteDiagnosticYaml(content) << '\n';
        if (const Files::IStreamPtr stream = mVFS->find(VFS::Path::Normalized(content)))
        {
            const auto hash = Files::getHash(content, *stream);
            manifest << "    hash: " << formatDiagnosticHash(hash) << '\n';
        }
    }
    capsule.add({ "manifest.yaml", Diagnostics::CapsuleEntryType::Manifest, diagnosticBytes(manifest.str()) });

    const std::string configuration = serializeDiagnosticRenderConfiguration(captureDiagnosticRenderConfiguration());
    capsule.add({ "configuration/effective.yaml", Diagnostics::CapsuleEntryType::Configuration,
        diagnosticBytes(configuration) });
    if (mDiagnosticAtmosphereScenario)
    {
        capsule.add({ "configuration/atmosphere-scenario.yaml", Diagnostics::CapsuleEntryType::Configuration,
            diagnosticBytes(Diagnostics::serializeAtmosphereScenario(*mDiagnosticAtmosphereScenario)) });
    }

    std::ostringstream world;
    const auto& cloudTravel = mWorld->getRenderingManager()->getCloudAdvectionOffsetWorldUnits();
    const auto atmosphereRuntime = indentDiagnosticYaml(mWorld->captureAtmosphereRuntimeState());
    world << "cell: " << quoteDiagnosticYaml(mWorld->getCellName()) << '\n'
          << "frame: " << frameNumber << '\n'
          << "camera: [" << cameraPosition.x() << ", " << cameraPosition.y() << ", " << cameraPosition.z() << "]\n"
          << "camera-angles: [" << camera->getPitch() << ", " << camera->getYaw() << ", " << camera->getRoll() << "]\n"
          << "first-person: " << (mWorld->isFirstPerson() ? "true" : "false") << '\n'
          << "camera-underwater: " << (cameraUnderwater ? "true" : "false") << '\n'
          << "weather: " << quoteDiagnosticYaml(weather.mId.serializeText()) << '\n'
          << "next-weather: "
          << quoteDiagnosticYaml(
                 mWorld->getNextWeather() ? mWorld->getNextWeather()->mId.serializeText() : std::string_view())
          << '\n'
          << "weather-transition: " << (mWorld->getNextWeather() ? 1.f - mWorld->getWeatherTransition() : 0.f) << '\n'
          << "game-hour: " << time.getTimeStamp().getHour() << '\n'
          << "game-time-seconds: " << time.getGameTime() << '\n'
          << "simulation-time: " << time.getSimulationTime() << '\n'
          << "rendering-time: " << time.getRenderingSimulationTime() << '\n'
          << std::setprecision(std::numeric_limits<double>::max_digits10) << "cloud-advection-world-units: ["
          << cloudTravel.x() << ", " << cloudTravel.y() << "]\n"
          << "atmosphere-runtime:\n"
          << atmosphereRuntime;
    capsule.add({ "state/world.yaml", Diagnostics::CapsuleEntryType::GameplayTrace, diagnosticBytes(world.str()) });

    if (mStateManager->getState() == MWBase::StateManager::State_Running)
    {
        capsule.add({ "state/world.omwsave", Diagnostics::CapsuleEntryType::WorldSnapshot,
            mStateManager->captureDiagnosticSnapshot("OpenMWSE diagnostic capsule") });
    }

    std::ostringstream performance;
    const unsigned sampledFrame = diagnosticStatisticsFrame(frameNumber);
    performance << "request-frame: " << frameNumber << '\n'
                << "fixed-step: " << (mDiagnosticChild ? "true" : "false") << '\n'
                << "fixed-step-duration: " << mDiagnosticStepScheduler.frameDuration() << '\n';
    reportDiagnosticPerformance(sampledFrame, sampledFrame, *mViewer, performance);
    capsule.add({ "performance/frame.txt", Diagnostics::CapsuleEntryType::PerformanceTrace,
        diagnosticBytes(performance.str()) });
    return capsule;
}
