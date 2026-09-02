#include "engine.hpp"

#include "diagnosticcollection.hpp"
#include "diagnosticgameplayroute.hpp"
#include "diagnosticperformance.hpp"
#include "diagnosticrenderconfiguration.hpp"

#include <components/diagnostics/atmospherescenario.hpp>
#include <components/diagnostics/capsule.hpp>
#include <components/diagnostics/session.hpp>
#include <components/files/hash.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/version/version.hpp>
#include <components/vfs/manager.hpp>

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <iomanip>

#include "mwrender/camera.hpp"
#include "mwrender/postprocessor.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwrender/vismask.hpp"

#include "mwstate/statemanagerimp.hpp"

#include "mwworld/atmosphereenvironment.hpp"
#include "mwworld/datetimemanager.hpp"
#include "mwworld/timestamp.hpp"
#include "mwworld/weather.hpp"
#include "mwworld/worldimp.hpp"

namespace
{
    /// Convert UTF-8 text into the protocol's byte payload without an implicit terminator.
    std::vector<std::byte> diagnosticBytes(std::string_view text)
    {
        const auto* begin = reinterpret_cast<const std::byte*>(text.data());
        return { begin, begin + text.size() };
    }

    /// Convert a protocol payload to text after rejecting embedded null bytes.
    std::string diagnosticText(std::span<const std::byte> payload)
    {
        std::string result(reinterpret_cast<const char*>(payload.data()), payload.size());
        if (result.find('\0') != std::string::npos)
            throw std::invalid_argument("diagnostic text payload contains an embedded null byte");
        return result;
    }

    /// Quote a scalar for the small YAML manifests emitted by core diagnostics.
    std::string quoteDiagnosticYaml(std::string_view value)
    {
        std::string result = "\"";
        for (const char character : value)
        {
            if (character == '\\' || character == '"')
                result.push_back('\\');
            if (character == '\n')
                result += "\\n";
            else if (character != '\r')
                result.push_back(character);
        }
        result.push_back('"');
        return result;
    }

    /// Format the repository's 128-bit content identity without ambiguous variable-width fields.
    std::string formatDiagnosticHash(const std::array<std::uint64_t, 2>& hash)
    {
        std::ostringstream output;
        output << std::hex << std::setfill('0') << std::setw(16) << hash[0] << std::setw(16) << hash[1];
        return output.str();
    }

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
             << "providers: [core@2, graphics@1, gameplay@8, performance@13]\n"
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
          << "rendering-time: " << time.getRenderingSimulationTime() << '\n';
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

void OMW::Engine::processDiagnosticCommands(unsigned frameNumber)
{
    if (!mDiagnosticSession)
        return;

    if (const std::optional<CompletedDiagnosticCollection> completed = mDiagnosticCollection->update(frameNumber))
    {
        if (mDiagnosticShadowOverrideRequest == completed->mRequestId)
        {
            SceneUtil::ShadowManager::instance().setDiagnosticShadowRenderingEnabled(true);
            SceneUtil::ShadowManager::instance().restoreDiagnosticOutdoorShadowCastingMask();
            mDiagnosticShadowOverrideRequest.reset();
        }
        std::ostringstream output;
        output << "request-frame: " << completed->mFirstFrame << '\n'
               << "fixed-step: " << (mDiagnosticChild ? "true" : "false") << '\n'
               << "fixed-step-duration: " << mDiagnosticStepScheduler.frameDuration() << '\n';
        reportDiagnosticPerformance(completed->mFirstFrame, completed->mLastFrame, *mViewer, output);
        mDiagnosticSession->respond(
            { completed->mRequestId, Diagnostics::ErrorCode::None, diagnosticBytes(output.str()) });
    }

    for (const Diagnostics::Command& command : mDiagnosticSession->drain())
    {
        Diagnostics::Response response{ command.mRequestId, Diagnostics::ErrorCode::None, {} };
        bool deferredResponse = false;
        try
        {
            std::ostringstream output;
            MWWorld::DateTimeManager& time = *mWorld->getTimeManager();
            const MWRender::Camera* camera = mWorld->getCamera();
            const osg::Vec3d cameraPosition = camera->getPosition();
            const bool cameraUnderwater = mWorld->isUnderwater(mWorld->getPlayerPtr().getCell(),
                osg::Vec3f(cameraPosition.x(), cameraPosition.y(), cameraPosition.z()));
            const MWWorld::Weather& weather = mWorld->getCurrentWeather();

            switch (command.mType)
            {
                case Diagnostics::CommandType::Hello:
                case Diagnostics::CommandType::InspectSession:
                    if (!command.mPayload.empty())
                    {
                        const YAML::Node expected = YAML::Load(diagnosticText(command.mPayload));
                        const YAML::Node expectedContent = expected["content"];
                        if (!expectedContent.IsSequence() || expectedContent.size() != mContentFiles.size())
                        {
                            response.mError = Diagnostics::ErrorCode::ContentMismatch;
                            response.mPayload = diagnosticBytes("content list length does not match the capsule\n");
                            break;
                        }
                        bool mismatch = false;
                        for (std::size_t index = 0; index < mContentFiles.size(); ++index)
                        {
                            const std::string expectedName = expectedContent[index]["name"].as<std::string>();
                            const std::string expectedHash = expectedContent[index]["hash"].as<std::string>("");
                            if (expectedName != mContentFiles[index])
                            {
                                mismatch = true;
                                output << "content order mismatch at " << index << ": expected " << expectedName
                                       << ", loaded " << mContentFiles[index] << '\n';
                                break;
                            }
                            const Files::IStreamPtr stream = mVFS->find(VFS::Path::Normalized(mContentFiles[index]));
                            if (!stream
                                || formatDiagnosticHash(Files::getHash(mContentFiles[index], *stream)) != expectedHash)
                            {
                                mismatch = true;
                                output << "content hash mismatch for " << mContentFiles[index] << '\n';
                                break;
                            }
                        }
                        if (mismatch)
                        {
                            response.mError = Diagnostics::ErrorCode::ContentMismatch;
                            response.mPayload = diagnosticBytes(output.str());
                            break;
                        }
                    }
                    output << "protocol: " << Diagnostics::protocolVersion << '\n'
                           << "build: " << quoteDiagnosticYaml(Version::getOpenmwVersionDescription()) << '\n'
                           << "mode: " << (mDiagnosticChild ? "isolated-child" : "live") << '\n'
                           << "graphics-context: "
                           << (mDiagnosticChild && !mDiagnosticHeaded ? "offscreen-pbuffer" : "interactive-window")
                           << '\n'
                           << "native-window: " << (mDiagnosticChild && !mDiagnosticHeaded ? "absent" : "present")
                           << '\n'
                           << "input-source: " << (mDiagnosticChild ? "device-free" : "host-devices") << '\n'
                           << "scheduled-frames-remaining: " << mDiagnosticStepScheduler.remainingFrames() << '\n'
                           << "completed-fixed-frames: " << mDiagnosticStepScheduler.completedFrames() << '\n'
                           << "route-frames-remaining: "
                           << (mDiagnosticGameplayRoute ? mDiagnosticGameplayRoute->remainingReplayFrames() : 0) << '\n'
                           << "frame: " << frameNumber << '\n'
                           << "graphics-capture: "
                           << (mWorld->getPostProcessor()->atmosphereDiagnosticCaptureInProgress() ? "in-progress"
                                                                                                   : "idle")
                           << '\n'
                           << "render-frame-capture: "
                           << (mWorld->getPostProcessor()->renderFrameDiagnosticCaptureInProgress() ? "in-progress"
                                                                                                    : "idle")
                           << '\n'
                           << "performance-capture: " << (mDiagnosticCollection->active() ? "collecting" : "idle")
                           << '\n'
                           << "providers: [core@2, graphics@1, gameplay@8, performance@13]\n";
                    response.mPayload = diagnosticBytes(output.str());
                    break;

                case Diagnostics::CommandType::StepFrames:
                {
                    if (!mDiagnosticChild)
                    {
                        response.mError = Diagnostics::ErrorCode::PermissionDenied;
                        break;
                    }
                    if (command.mPayload.size() != sizeof(std::uint32_t))
                    {
                        response.mError = Diagnostics::ErrorCode::InvalidFrame;
                        break;
                    }
                    std::uint32_t count = 0;
                    for (std::size_t byte = 0; byte < sizeof(count); ++byte)
                        count |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(command.mPayload[byte]))
                            << (byte * 8);
                    try
                    {
                        mDiagnosticStepScheduler.schedule(count);
                    }
                    catch (const std::invalid_argument&)
                    {
                        response.mError = Diagnostics::ErrorCode::InvalidFrame;
                        break;
                    }
                    output << "scheduled-frames: " << count << '\n';
                    response.mPayload = diagnosticBytes(output.str());
                    break;
                }

                case Diagnostics::CommandType::LoadGameplayRoute:
                {
                    if (!mDiagnosticChild || !mDiagnosticGameplayRoute)
                    {
                        response.mError = Diagnostics::ErrorCode::PermissionDenied;
                        break;
                    }
                    mDiagnosticGameplayRoute->loadReplay(command.mPayload);
                    output << "route-frames: " << mDiagnosticGameplayRoute->remainingReplayFrames() << '\n';
                    response.mPayload = diagnosticBytes(output.str());
                    break;
                }

                case Diagnostics::CommandType::RunGameplayRouteSegment:
                {
                    const unsigned routeOptions = command.mPayload.size() == sizeof(std::uint32_t) + 1
                        ? std::to_integer<unsigned char>(command.mPayload.back())
                        : 0;
                    const bool captureRenderedFrames = (routeOptions & 0x1u) != 0;
                    const bool bypassShadows = (routeOptions & 0x2u) != 0;
                    const bool terrainCastersOnly = (routeOptions & 0x4u) != 0;
                    const bool objectCastersOnly = (routeOptions & 0x8u) != 0;
                    if (!mDiagnosticChild || !mDiagnosticGameplayRoute
                        || (command.mPayload.size() != sizeof(std::uint32_t)
                            && command.mPayload.size() != sizeof(std::uint32_t) + 1)
                        || (bypassShadows && (terrainCastersOnly || objectCastersOnly))
                        || (terrainCastersOnly && objectCastersOnly))
                    {
                        response.mError = Diagnostics::ErrorCode::InvalidFrame;
                        break;
                    }
                    std::uint32_t count = 0;
                    for (std::size_t byte = 0; byte < sizeof(count); ++byte)
                        count |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(command.mPayload[byte]))
                            << (byte * 8);
                    if (count == 0 || count > 600 || count > mDiagnosticGameplayRoute->remainingReplayFrames()
                        || !mDiagnosticCollection->start(command.mRequestId, count, frameNumber + 1))
                    {
                        response.mError = Diagnostics::ErrorCode::InvalidState;
                        break;
                    }
                    if (captureRenderedFrames)
                    {
                        const std::filesystem::path outputPath = mDiagnosticArtifactRoot
                            / ("route-frames-" + std::to_string(command.mRequestId) + ".omwdiag");
                        mWorld->getPostProcessor()->requestRenderFrameDiagnosticCapture(outputPath, count);
                    }
                    if (bypassShadows)
                    {
                        SceneUtil::ShadowManager::instance().setDiagnosticShadowRenderingEnabled(false);
                        mDiagnosticShadowOverrideRequest = command.mRequestId;
                    }
                    else if (terrainCastersOnly || objectCastersOnly)
                    {
                        const unsigned int mask = MWRender::Mask_Scene
                            | (terrainCastersOnly ? MWRender::Mask_Terrain
                                                  : MWRender::Mask_Object | MWRender::Mask_Static);
                        SceneUtil::ShadowManager::instance().setDiagnosticOutdoorShadowCastingMask(mask);
                        mDiagnosticShadowOverrideRequest = command.mRequestId;
                    }
                    mDiagnosticStepScheduler.schedule(count);
                    deferredResponse = true;
                    break;
                }

                case Diagnostics::CommandType::CaptureGraphics:
                {
                    const std::string captureMode = diagnosticText(command.mPayload);
                    if (!captureMode.empty() && captureMode != "normal-only")
                        throw std::invalid_argument("unsupported graphics capture mode");
                    const std::filesystem::path outputDirectory
                        = mDiagnosticArtifactRoot / ("graphics-" + std::to_string(frameNumber));
                    mWorld->getPostProcessor()->requestAtmosphereDiagnosticCapture(
                        outputDirectory, captureMode != "normal-only");
                    output << "capture-root: " << quoteDiagnosticYaml(outputDirectory.filename().string()) << '\n'
                           << "capture-mode: " << (captureMode.empty() ? "comparison" : captureMode) << '\n'
                           << "frame: " << frameNumber << '\n';
                    response.mPayload = diagnosticBytes(output.str());
                    break;
                }

                case Diagnostics::CommandType::TraceGameplay:
                {
                    const MWRender::AtmosphereDiagnosticState& atmosphere
                        = mWorld->getRenderingManager()->getAtmosphereDiagnosticState();
                    const MWRender::AtmosphereVolumeState& volumes = atmosphere.mVolumes;
                    output << "provider: gameplay@8\nframe: " << frameNumber << '\n'
                           << "state: " << static_cast<int>(mStateManager->getState()) << '\n'
                           << "cell: " << quoteDiagnosticYaml(mWorld->getCellName()) << '\n'
                           << "camera: [" << cameraPosition.x() << ", " << cameraPosition.y() << ", "
                           << cameraPosition.z() << "]\n"
                           << "camera-angles: [" << camera->getPitch() << ", " << camera->getYaw() << ", "
                           << camera->getRoll() << "]\n"
                           << "first-person: " << (mWorld->isFirstPerson() ? "true" : "false") << '\n'
                           << "camera-underwater: " << (cameraUnderwater ? "true" : "false") << '\n'
                           << "weather: " << quoteDiagnosticYaml(weather.mId.serializeText()) << '\n'
                           << "next-weather: "
                           << quoteDiagnosticYaml(mWorld->getNextWeather()
                                      ? mWorld->getNextWeather()->mId.serializeText()
                                      : std::string_view())
                           << '\n'
                           << "weather-transition: "
                           << (mWorld->getNextWeather() ? 1.f - mWorld->getWeatherTransition() : 0.f) << '\n'
                           << "game-hour: " << time.getTimeStamp().getHour() << '\n'
                           << "game-time-seconds: " << time.getGameTime() << '\n'
                           << "simulation-time: " << time.getSimulationTime() << '\n'
                           << "atmosphere:\n"
                           << "  enabled: " << (atmosphere.mEnabled ? "true" : "false") << '\n'
                           << "  fog-range-world-units: [" << atmosphere.mFogStartWorldUnits << ", "
                           << atmosphere.mFogEndWorldUnits << "]\n"
                           << "  camera-range-world-units: [" << atmosphere.mNearClipWorldUnits << ", "
                           << atmosphere.mViewDistanceWorldUnits << "]\n"
                           << "  sun-direction: [" << atmosphere.mSunDirection.x() << ", "
                           << atmosphere.mSunDirection.y() << ", " << atmosphere.mSunDirection.z() << "]\n"
                           << "  solar-radiance: [" << atmosphere.mLighting.mSolarRadiance.x() << ", "
                           << atmosphere.mLighting.mSolarRadiance.y() << ", " << atmosphere.mLighting.mSolarRadiance.z()
                           << "]\n"
                           << "  sky-irradiance: [" << atmosphere.mLighting.mSkyIrradiance.x() << ", "
                           << atmosphere.mLighting.mSkyIrradiance.y() << ", " << atmosphere.mLighting.mSkyIrradiance.z()
                           << "]\n"
                           << "  ground-irradiance: [" << atmosphere.mLighting.mGroundIrradiance.x() << ", "
                           << atmosphere.mLighting.mGroundIrradiance.y() << ", "
                           << atmosphere.mLighting.mGroundIrradiance.z() << "]\n"
                           << "  sun-disc-color: [" << atmosphere.mSunDiscColor.x() << ", "
                           << atmosphere.mSunDiscColor.y() << ", " << atmosphere.mSunDiscColor.z() << ", "
                           << atmosphere.mSunDiscColor.w() << "]\n"
                           << "  cloud-field-offset-world-units: [" << atmosphere.mCloudFieldOffsetWorldUnits.x()
                           << ", " << atmosphere.mCloudFieldOffsetWorldUnits.y() << "]\n"
                           << "  main-cloud: [" << volumes.mCloudDeckCoverage << ", " << volumes.mCloudDensity << ", "
                           << volumes.mCloudBaseAltitudeWorldUnits << ", " << volumes.mCloudThicknessWorldUnits << ", "
                           << volumes.mCloudType << ", " << volumes.mCloudFeatureScale << ", "
                           << volumes.mCloudVerticalDevelopment << ", " << volumes.mCloudAnvilStrength << "]\n"
                           << "  cloud-deck-type: " << volumes.mCloudDeckType << "\n"
                           << "  cloud-organization: [" << volumes.mCloudDeckContinuity << ", "
                           << volumes.mCloudConvectiveFrequency << ", " << volumes.mCloudConvectiveMaturity << ", "
                           << volumes.mCloudStormOrganization << "]\n"
                           << "  cloud-structure: [" << volumes.mCloudTowerTaper << ", " << volumes.mCloudBaseVariation
                           << "]\n"
                           << "  low-cloud: [" << volumes.mLowCloudCoverage << ", " << volumes.mLowCloudDensity << ", "
                           << volumes.mLowCloudBaseAltitudeWorldUnits << ", " << volumes.mLowCloudThicknessWorldUnits
                           << ", " << volumes.mLowCloudType << ", " << volumes.mLowCloudFeatureScale << ", "
                           << volumes.mLowCloudVerticalDevelopment << ", " << volumes.mLowCloudEnvironmentInfluence
                           << "]\n"
                           << "  mist: [" << volumes.mMistDensity << ", " << volumes.mMistBaseAltitudeWorldUnits << ", "
                           << volumes.mMistThicknessWorldUnits << "]\n"
                           << "  fog: [" << volumes.mFogDensity << ", " << volumes.mFogBaseAltitudeWorldUnits << ", "
                           << volumes.mFogThicknessWorldUnits << ", " << volumes.mFogAnisotropy << "]\n"
                           << "  terrain: [" << volumes.mTerrainReferenceHeightWorldUnits << ", "
                           << volumes.mCloudTerrainFollowing << ", " << volumes.mCloudTerrainClearanceWorldUnits
                           << "]\n"
                           << "  terrain-sample-center: [" << volumes.mTerrainSampleCenterWorldUnits.x() << ", "
                           << volumes.mTerrainSampleCenterWorldUnits.y() << "]\n"
                           << "  terrain-gradient: [" << volumes.mTerrainGradient.x() << ", "
                           << volumes.mTerrainGradient.y() << "]\n"
                           << "  valley-depth-world-units: " << volumes.mValleyDepthWorldUnits << '\n'
                           << "environment-traits:\n";
                    for (const auto& [trait, contribution] : mWorld->getAtmosphereEnvironmentState().mTraits)
                        output << "  " << quoteDiagnosticYaml(trait) << ": " << contribution << '\n';
                    response.mPayload = diagnosticBytes(output.str());
                    break;
                }

                case Diagnostics::CommandType::TracePerformance:
                {
                    std::uint32_t sampleCount = 1;
                    if (!command.mPayload.empty())
                    {
                        if (command.mPayload.size() != sizeof(sampleCount))
                        {
                            response.mError = Diagnostics::ErrorCode::InvalidFrame;
                            break;
                        }
                        sampleCount = 0;
                        for (std::size_t byte = 0; byte < sizeof(sampleCount); ++byte)
                            sampleCount
                                |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(command.mPayload[byte]))
                                << (byte * 8);
                    }
                    constexpr std::uint32_t maximumPerformanceSamples = 600;
                    if (sampleCount == 0 || sampleCount > maximumPerformanceSamples)
                    {
                        response.mError = Diagnostics::ErrorCode::InvalidFrame;
                        break;
                    }
                    if (!mDiagnosticCollection->start(command.mRequestId, sampleCount, frameNumber))
                    {
                        response.mError = Diagnostics::ErrorCode::InvalidState;
                        break;
                    }
                    deferredResponse = true;
                    break;
                }

                case Diagnostics::CommandType::CaptureCapsule:
                {
                    const std::string requestedName = diagnosticText(command.mPayload);
                    if (requestedName.empty()
                        || requestedName != std::filesystem::path(requestedName).filename().string())
                    {
                        response.mError = Diagnostics::ErrorCode::PermissionDenied;
                        break;
                    }

                    const std::filesystem::path destination = mDiagnosticArtifactRoot / requestedName;
                    Diagnostics::writeCapsuleAtomically(destination, createDiagnosticCapsule(frameNumber));
                    output << "capsule: " << quoteDiagnosticYaml(destination.filename().string()) << '\n';
                    response.mPayload = diagnosticBytes(output.str());
                    break;
                }

                case Diagnostics::CommandType::ReloadShaders:
                    mWorld->getPostProcessor()->triggerShaderReload();
                    break;

                case Diagnostics::CommandType::ShutdownChild:
                    if (!mDiagnosticChild)
                        response.mError = Diagnostics::ErrorCode::PermissionDenied;
                    else
                        mStateManager->requestQuit();
                    break;

                case Diagnostics::CommandType::SetWorldState:
                {
                    if (!mDiagnosticChild)
                    {
                        response.mError = Diagnostics::ErrorCode::PermissionDenied;
                        break;
                    }
                    const YAML::Node state = YAML::Load(diagnosticText(command.mPayload));
                    mViewer->getFrameStamp()->setFrameNumber(state["frame"].as<unsigned>());
                    time.setSimulationTime(state["simulation-time"].as<double>());
                    time.setRenderingSimulationTime(state["rendering-time"].as<double>());
                    const YAML::Node angles = state["camera-angles"];
                    if (!angles.IsSequence() || angles.size() != 3)
                        throw std::invalid_argument("diagnostic world state has invalid camera angles");
                    MWRender::Camera* mutableCamera = mWorld->getCamera();
                    mutableCamera->setPitch(angles[0].as<float>(), true);
                    mutableCamera->setYaw(angles[1].as<float>(), true);
                    mutableCamera->setRoll(angles[2].as<float>());
                    mWorld->getRenderingManager()->invalidateAtmosphereHistory();
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
                        if (scenario.mWeather)
                        {
                            const std::optional<ESM::RefId> next = scenario.mWeather->mNext
                                ? std::optional<ESM::RefId>(ESM::RefId::deserializeText(*scenario.mWeather->mNext))
                                : std::nullopt;
                            mWorld->setDiagnosticWeatherState(ESM::RefId::deserializeText(scenario.mWeather->mCurrent),
                                next, scenario.mWeather->mTransition);
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
                        mWorld->getRenderingManager()->setAtmosphereDiagnosticMedia(
                            { media.mMainClouds, media.mLowClouds, media.mMist, media.mFog });
                        mWorld->getRenderingManager()->setAtmosphereDiagnosticDistanceFogEnabled(
                            scenario.mDistanceFog.value_or(true));
                        mWorld->getRenderingManager()->setAtmosphereDiagnosticCloudSeed(
                            scenario.mCloudSeed.value_or(0));
                    }
                    catch (const std::exception& error)
                    {
                        response.mError = Diagnostics::ErrorCode::InvalidFrame;
                        response.mPayload = diagnosticBytes(error.what());
                        break;
                    }
                    mWorld->getRenderingManager()->invalidateAtmosphereHistory();
                    mDiagnosticAtmosphereScenario = scenario;
                    output
                        << "scenario-version: " << Diagnostics::atmosphereScenarioVersion << '\n'
                        << "weather: " << quoteDiagnosticYaml(mWorld->getCurrentWeather().mId.serializeText()) << '\n'
                        << "next-weather: "
                        << quoteDiagnosticYaml(mWorld->getNextWeather() ? mWorld->getNextWeather()->mId.serializeText()
                                                                        : std::string_view())
                        << '\n'
                        << "weather-transition: "
                        << (mWorld->getNextWeather() ? 1.f - mWorld->getWeatherTransition() : 0.f) << '\n'
                        << "game-hour: " << time.getTimeStamp().getHour() << '\n'
                        << "main-clouds: "
                        << (mDiagnosticAtmosphereScenario->mMedia ? mDiagnosticAtmosphereScenario->mMedia->mMainClouds
                                                                  : true)
                        << '\n'
                        << "low-clouds: "
                        << (mDiagnosticAtmosphereScenario->mMedia ? mDiagnosticAtmosphereScenario->mMedia->mLowClouds
                                                                  : true)
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
                    output << "environment-terrain-height: "
                           << mWorld->getAtmosphereEnvironmentState().mTerrainHeightWorldUnits << '\n'
                           << "environment-terrain-sample-center: ["
                           << mWorld->getAtmosphereEnvironmentState().mTerrainSampleCenterWorldUnits.x() << ", "
                           << mWorld->getAtmosphereEnvironmentState().mTerrainSampleCenterWorldUnits.y() << "]\n"
                           << "environment-terrain-gradient: ["
                           << mWorld->getAtmosphereEnvironmentState().mTerrainGradient.x() << ", "
                           << mWorld->getAtmosphereEnvironmentState().mTerrainGradient.y() << "]\n"
                           << "environment-valley-depth-world-units: "
                           << mWorld->getAtmosphereEnvironmentState().mValleyDepthWorldUnits << '\n'
                           << "atmosphere-history: reset\n";
                    response.mPayload = diagnosticBytes(output.str());
                    break;
                }

                case Diagnostics::CommandType::SetCamera:
                {
                    if (!mDiagnosticChild)
                    {
                        response.mError = Diagnostics::ErrorCode::PermissionDenied;
                        break;
                    }
                    const YAML::Node state = YAML::Load(diagnosticText(command.mPayload));
                    if (!state.IsMap() || !state["camera-angles"] || state.size() > 2
                        || (state.size() == 2 && !state["camera-position"]))
                        throw std::invalid_argument(
                            "diagnostic camera state requires camera-angles and optional camera-position");
                    MWRender::Camera* camera = mWorld->getCamera();
                    if (const YAML::Node position = state["camera-position"])
                    {
                        if (!position.IsSequence() || position.size() != 3)
                            throw std::invalid_argument("diagnostic camera state has invalid camera coordinates");
                        const osg::Vec3d requestedPosition(
                            position[0].as<double>(), position[1].as<double>(), position[2].as<double>());
                        if (!std::isfinite(requestedPosition.x()) || !std::isfinite(requestedPosition.y())
                            || !std::isfinite(requestedPosition.z()))
                            throw std::invalid_argument("diagnostic camera coordinates are outside their finite range");
                        camera->setMode(MWRender::Camera::Mode::Static);
                        camera->setStaticPosition(requestedPosition);
                    }
                    const YAML::Node angles = state["camera-angles"];
                    if (!angles.IsSequence() || angles.size() != 3)
                        throw std::invalid_argument("diagnostic camera state has invalid camera angles");
                    const float pitch = angles[0].as<float>();
                    const float yaw = angles[1].as<float>();
                    const float roll = angles[2].as<float>();
                    if (!std::isfinite(pitch) || !std::isfinite(yaw) || !std::isfinite(roll) || pitch < -1.5707964f
                        || pitch > 1.5707964f)
                        throw std::invalid_argument("diagnostic camera angles are outside their finite range");
                    camera->setPitch(pitch, true);
                    camera->setYaw(yaw, true);
                    camera->setRoll(roll);
                    mWorld->getRenderingManager()->invalidateAtmosphereHistory();
                    output << "camera-position: [" << camera->getPosition().x() << ", " << camera->getPosition().y()
                           << ", " << camera->getPosition().z() << "]\n"
                           << "camera-angles: [" << camera->getPitch() << ", " << camera->getYaw() << ", "
                           << camera->getRoll() << "]\n"
                           << "atmosphere-history: reset\n";
                    response.mPayload = diagnosticBytes(output.str());
                    break;
                }

                case Diagnostics::CommandType::Cancel:
                    response.mError = Diagnostics::ErrorCode::UnsupportedCommand;
                    break;
            }
        }
        catch (const std::exception& error)
        {
            response.mError = Diagnostics::ErrorCode::InternalFailure;
            response.mPayload = diagnosticBytes(error.what());
        }
        if (!deferredResponse)
            mDiagnosticSession->respond(std::move(response));
    }
}
