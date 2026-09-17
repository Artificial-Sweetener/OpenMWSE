#include "engine.hpp"

#include "diagnosticcollection.hpp"
#include "diagnosticgameplayroute.hpp"
#include "diagnosticperformance.hpp"
#include "diagnosticrenderconfiguration.hpp"
#include "diagnosticserialization.hpp"

#include <components/diagnostics/atmospherescenario.hpp>
#include <components/diagnostics/atmospheresettings.hpp>
#include <components/diagnostics/capsule.hpp>
#include <components/diagnostics/distancesettings.hpp>
#include <components/diagnostics/session.hpp>
#include <components/files/hash.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/settings/values.hpp>
#include <components/version/version.hpp>
#include <components/vfs/manager.hpp>

#include <osgViewer/ViewerEventHandlers>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>

#include "mwgui/windowmanagerimp.hpp"
#include "mwphysics/physicssystem.hpp"
#include "mwrender/atmospherediagnosticcontroller.hpp"
#include "mwrender/camera.hpp"
#include "mwrender/postprocessor.hpp"
#include "mwrender/postprocessordiagnostics.hpp"
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
    /// Append one complete request-scoped Bullet route-clearance report as stable YAML.
    void reportRouteClearance(const std::vector<OMW::DiagnosticRouteClearanceFrame>& frames, std::ostringstream& output)
    {
        const std::size_t blockedFrames
            = std::count_if(frames.begin(), frames.end(), [](const auto& frame) { return !frame.mContacts.empty(); });
        const std::streamsize originalPrecision = output.precision();
        output << std::setprecision(std::numeric_limits<float>::max_digits10);
        output << "route-clearance:\n"
               << "  provider: bullet-standing-actor@1\n"
               << "  tested-frames: " << frames.size() << '\n'
               << "  blocked-frames: " << blockedFrames << '\n'
               << "  clear: " << (blockedFrames == 0 ? "true" : "false") << "\n"
               << "  frames:\n";
        for (const OMW::DiagnosticRouteClearanceFrame& frame : frames)
        {
            output << "    - route-frame: " << frame.mRouteFrame << '\n'
                   << "      cell: " << OMW::quoteDiagnosticYaml(frame.mCell) << '\n'
                   << "      position: [" << frame.mPosition.x() << ", " << frame.mPosition.y() << ", "
                   << frame.mPosition.z() << "]\n"
                   << "      actor-shape: " << frame.mShape << '\n'
                   << "      half-extents: [" << frame.mHalfExtents.x() << ", " << frame.mHalfExtents.y() << ", "
                   << frame.mHalfExtents.z() << "]\n"
                   << "      support-clearance: " << frame.mLift << '\n'
                   << "      blocked: " << (frame.mContacts.empty() ? "false" : "true") << '\n'
                   << "      contacts:\n";
            for (const OMW::DiagnosticRouteClearanceContact& contact : frame.mContacts)
            {
                output << "        - object-id: " << OMW::quoteDiagnosticYaml(contact.mObjectId) << '\n'
                       << "          reference: " << OMW::quoteDiagnosticYaml(contact.mReference) << '\n'
                       << "          point: [" << contact.mPoint.x() << ", " << contact.mPoint.y() << ", "
                       << contact.mPoint.z() << "]\n"
                       << "          normal: [" << contact.mNormal.x() << ", " << contact.mNormal.y() << ", "
                       << contact.mNormal.z() << "]\n"
                       << "          penetration-depth: " << contact.mPenetrationDepth << '\n';
            }
        }
        output.precision(originalPrecision);
    }

    /// Append one complete request-scoped authoritative player-movement report as stable YAML.
    void reportRouteTraversal(const std::vector<OMW::DiagnosticRouteTraversalFrame>& frames, std::ostringstream& output)
    {
        const std::size_t testedTransitions
            = std::count_if(frames.begin(), frames.end(), [](const auto& frame) { return !frame.mInitialized; });
        const std::size_t exactTargetTransitions = std::count_if(frames.begin(), frames.end(),
            [](const auto& frame) { return !frame.mInitialized && frame.mTargetReached; });
        const std::streamsize originalPrecision = output.precision();
        output << std::setprecision(std::numeric_limits<float>::max_digits10);
        output << "route-traversal:\n"
               << "  provider: bullet-player-movement@1\n"
               << "  target-frames: " << frames.size() << '\n'
               << "  tested-transitions: " << testedTransitions << '\n'
               << "  exact-target-transitions: " << exactTargetTransitions << '\n'
               << "  missed-target-transitions: " << testedTransitions - exactTargetTransitions << '\n'
               << "  horizontal-tolerance: " << OMW::diagnosticRouteHorizontalTolerance << '\n'
               << "  vertical-tolerance: " << OMW::diagnosticRouteVerticalTolerance << '\n'
               << "  frames:\n";
        for (const OMW::DiagnosticRouteTraversalFrame& frame : frames)
        {
            output << "    - route-frame: " << frame.mRouteFrame << '\n'
                   << "      target-cell: " << OMW::quoteDiagnosticYaml(frame.mTargetCell) << '\n'
                   << "      target-position: [" << frame.mTargetPosition.x() << ", " << frame.mTargetPosition.y()
                   << ", " << frame.mTargetPosition.z() << "]\n"
                   << "      actual-cell: " << OMW::quoteDiagnosticYaml(frame.mActualCell) << '\n'
                   << "      actual-position: [" << frame.mActualPosition.x() << ", " << frame.mActualPosition.y()
                   << ", " << frame.mActualPosition.z() << "]\n"
                   << "      horizontal-error: " << frame.mHorizontalError << '\n'
                   << "      vertical-error: " << frame.mVerticalError << '\n'
                   << "      initialized: " << (frame.mInitialized ? "true" : "false") << '\n'
                   << "      target-reached: " << (frame.mTargetReached ? "true" : "false") << '\n';
        }
        output.precision(originalPrecision);
    }
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
        if (completed->mHistoryExpired)
        {
            if (mDiagnosticGameplayRoute)
            {
                mDiagnosticGameplayRoute->discardClearanceCapture(completed->mRequestId);
                mDiagnosticGameplayRoute->discardTraversalCapture(completed->mRequestId);
            }
            mDiagnosticSession->respond({ completed->mRequestId, Diagnostics::ErrorCode::InvalidFrame,
                diagnosticBytes("route rendering exceeded retained statistics; replay with fewer performance samples "
                                "per segment") });
        }
        else
        {
            const auto clearance = mDiagnosticGameplayRoute
                ? mDiagnosticGameplayRoute->finishClearanceCapture(completed->mRequestId)
                : std::nullopt;
            const auto traversal = mDiagnosticGameplayRoute
                ? mDiagnosticGameplayRoute->finishTraversalCapture(completed->mRequestId)
                : std::nullopt;
            output << "request-frame: " << completed->mFirstFrame << '\n'
                   << "readback-rendered-frames: " << frameNumber - completed->mLastFrame << '\n'
                   << "fixed-step: " << (mDiagnosticChild ? "true" : "false") << '\n'
                   << "fixed-step-duration: " << mDiagnosticStepScheduler.frameDuration() << '\n';
            reportDiagnosticRouteFrames(completed->mRouteFrames, output);
            if (clearance)
                reportRouteClearance(*clearance, output);
            if (traversal)
                reportRouteTraversal(*traversal, output);
            reportDiagnosticPerformance(completed->mFirstFrame, completed->mLastFrame, *mViewer, output);
            mDiagnosticSession->respond(
                { completed->mRequestId, Diagnostics::ErrorCode::None, diagnosticBytes(output.str()) });
        }
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
                           << (mWorld->getPostProcessor()->diagnostics().atmosphereCaptureInProgress() ? "in-progress"
                                                                                                       : "idle")
                           << '\n'
                           << "render-frame-capture: "
                           << (mWorld->getPostProcessor()->diagnostics().renderFrameCaptureInProgress() ? "in-progress"
                                                                                                        : "idle")
                           << '\n'
                           << "performance-capture: " << (mDiagnosticCollection->active() ? "collecting" : "idle")
                           << '\n'
                           << "providers: [core@2, graphics@3, gameplay@9, " << OMW::diagnosticPerformanceProvider
                           << "]\n";
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
                    const bool nativeResolution = (routeOptions & 0x10u) != 0;
                    const bool bypassShadows = (routeOptions & 0x2u) != 0;
                    const bool terrainCastersOnly = (routeOptions & 0x4u) != 0;
                    const bool objectCastersOnly = (routeOptions & 0x8u) != 0;
                    const bool captureClearance = (routeOptions & 0x20u) != 0;
                    const bool captureTraversal = (routeOptions & 0x40u) != 0;
                    if ((routeOptions & ~0x7fu) != 0 || (nativeResolution && !captureRenderedFrames)
                        || !mDiagnosticChild || !mDiagnosticGameplayRoute
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
                        || !mDiagnosticCollection->start(
                            command.mRequestId, count, frameNumber + 1, DiagnosticSampleBasis::GameplayRouteFrames))
                    {
                        response.mError = Diagnostics::ErrorCode::InvalidState;
                        break;
                    }
                    if (captureClearance)
                        mDiagnosticGameplayRoute->startClearanceCapture(command.mRequestId, count);
                    if (captureTraversal)
                        mDiagnosticGameplayRoute->startTraversalCapture(command.mRequestId, count);
                    if (captureRenderedFrames)
                    {
                        const std::filesystem::path outputPath = mDiagnosticArtifactRoot
                            / ("route-frames-" + std::to_string(command.mRequestId) + ".omwdiag");
                        mWorld->getPostProcessor()->diagnostics().requestRenderFrames(
                            outputPath, count, nativeResolution, frameNumber + 1);
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
                    mWorld->getPostProcessor()->diagnostics().requestAtmosphereCapture(
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
                        = mWorld->getRenderingManager()->atmosphereDiagnostics().state();
                    const MWRender::AtmosphereVolumeState& volumes = atmosphere.mVolumes;
                    const auto& cloudTravel = mWorld->getRenderingManager()->getCloudAdvectionOffsetWorldUnits();
                    const auto atmosphereRuntime = indentDiagnosticYaml(mWorld->captureAtmosphereRuntimeState());
                    output << "provider: gameplay@9\nframe: " << frameNumber << '\n'
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
                           << std::setprecision(std::numeric_limits<double>::max_digits10)
                           << "cloud-advection-world-units: [" << cloudTravel.x() << ", " << cloudTravel.y() << "]\n"
                           << "atmosphere-runtime:\n"
                           << atmosphereRuntime << std::setprecision(6) << "atmosphere:\n"
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
                           << "  cloud-model-version: 2\n"
                           << "  main-cloud: [" << volumes.mCloudDeckCoverage << ", " << volumes.mCloudDensity << ", "
                           << volumes.mCloudBaseAltitudeWorldUnits << ", " << volumes.mCloudThicknessWorldUnits << "]\n"
                           << "  cloud-layer:\n"
                           << "    coverage: [" << volumes.mCloudLayer.mCoverageAmount << ", "
                           << volumes.mCloudLayer.mCoverageMinimum << "]\n"
                           << "    type: [" << volumes.mCloudLayer.mTypeAmount << ", "
                           << volumes.mCloudLayer.mTypeMinimum << "]\n"
                           << "    rain: [" << volumes.mCloudLayer.mRainAmount << ", "
                           << volumes.mCloudLayer.mRainMinimum << "]\n"
                           << "    noise-scale-per-meter: " << volumes.mCloudLayer.mNoiseScalePerMeter << "\n"
                           << "    weather-scale-per-meter: " << volumes.mCloudLayer.mWeatherScalePerMeter << "\n"
                           << "    detail: [" << volumes.mCloudLayer.mDetailScale << ", "
                           << volumes.mCloudLayer.mDetailHeightFraction << ", " << volumes.mCloudLayer.mDetailErosion
                           << "]\n"
                           << "  cloud-scenario-identity: " << volumes.mCloudScenarioIdentity << "\n"
                           << "  low-cloud: [" << volumes.mLowCloudCoverage << ", " << volumes.mLowCloudDensity << ", "
                           << volumes.mLowCloudBaseAltitudeWorldUnits << ", " << volumes.mLowCloudThicknessWorldUnits
                           << ", " << volumes.mLowCloudEnvironmentInfluence << "]\n"
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
                    if (!mDiagnosticCollection->start(
                            command.mRequestId, sampleCount, frameNumber, DiagnosticSampleBasis::RenderedFrames))
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
                    if (const auto cloudTravel = state["cloud-advection-world-units"])
                    {
                        if (!cloudTravel.IsSequence() || cloudTravel.size() != 2)
                            throw std::invalid_argument("diagnostic cloud wind displacement requires two coordinates");
                        mWorld->getRenderingManager()->restoreCloudAdvectionOffsetWorldUnits(
                            { cloudTravel[0].as<double>(), cloudTravel[1].as<double>() });
                    }
                    if (const auto atmosphereRuntime = state["atmosphere-runtime"])
                        mWorld->restoreAtmosphereRuntimeState(YAML::Dump(atmosphereRuntime));
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

                case Diagnostics::CommandType::ShowVideoSettings:
                case Diagnostics::CommandType::SetDistanceSettings:
                case Diagnostics::CommandType::SetOptimizationSettings:
                case Diagnostics::CommandType::SetWaterSettings:
                case Diagnostics::CommandType::SetAtmosphereSettings:
                case Diagnostics::CommandType::SetAtmosphereScenario:
                    processDiagnosticGraphicsCommand(command, response, output);
                    break;
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
