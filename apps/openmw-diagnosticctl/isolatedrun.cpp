#include "isolatedrun.hpp"

#include "controllerprotocol.hpp"
#include "gameplayrouteexecution.hpp"
#include "isolatedsettings.hpp"
#include "isolatedworkspace.hpp"

#include <components/diagnostics/atmospheresettings.hpp>
#include <components/diagnostics/capsule.hpp>
#include <components/diagnostics/distancesettings.hpp>
#include <components/diagnostics/gameplayroute.hpp>
#include <components/misc/strings/algorithm.hpp>

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace DiagnosticCtl
{
    /// Run the real engine invisibly against copied state, collect evidence, and shut it down.
    int runIsolated(std::span<char*> arguments)
    {
        const std::filesystem::path executableOption = findOption(arguments, "--openmw").value_or("openmw.exe");
        const std::filesystem::path sourceConfigOption = findOption(arguments, "--config").value_or(std::string());
        const std::filesystem::path artifactsOption = findOption(arguments, "--artifacts").value_or(std::string());
        const bool headed = hasFlag(arguments, "--headed");
        const bool captureRouteNativeFrames = hasFlag(arguments, "--capture-route-native-frames");
        const bool captureRouteFrames = captureRouteNativeFrames || hasFlag(arguments, "--capture-route-frames");
        const bool captureGraphics = !hasFlag(arguments, "--skip-graphics-capture");
        const bool normalOnlyGraphicsCapture = hasFlag(arguments, "--normal-only-graphics-capture");
        if (!captureGraphics && normalOnlyGraphicsCapture)
            throw std::invalid_argument("--skip-graphics-capture conflicts with --normal-only-graphics-capture");
        const std::vector<std::byte> graphicsCapturePayload
            = normalOnlyGraphicsCapture ? textPayload("normal-only") : std::vector<std::byte>{};
        const bool bypassRouteShadows = hasFlag(arguments, "--bypass-route-shadows");
        const std::optional<std::string> cameraPitch = findOption(arguments, "--camera-pitch");
        const std::optional<std::string> cameraYaw = findOption(arguments, "--camera-yaw");
        const std::optional<std::string> cameraRoll = findOption(arguments, "--camera-roll");
        const std::optional<std::string> cameraX = findOption(arguments, "--camera-x");
        const std::optional<std::string> cameraY = findOption(arguments, "--camera-y");
        const std::optional<std::string> cameraZ = findOption(arguments, "--camera-z");
        const bool cameraPositionOverride = cameraX || cameraY || cameraZ;
        const bool cameraOverride = cameraPitch || cameraYaw || cameraRoll || cameraPositionOverride;
        std::optional<Diagnostics::AtmosphereScenario> atmosphereScenario = parseAtmosphereScenarioOptions(arguments);
        const std::uint32_t atmosphereWarmupFrames = parseFrameCountOption(
            "--atmosphere-warmup-frames", findOption(arguments, "--atmosphere-warmup-frames").value_or("0"), 600);
        const std::optional<std::string> routeShadowCasters = findOption(arguments, "--route-shadow-casters");
        if (routeShadowCasters && *routeShadowCasters != "terrain" && *routeShadowCasters != "objects")
            throw std::invalid_argument("--route-shadow-casters requires terrain or objects");
        if (bypassRouteShadows && routeShadowCasters)
            throw std::invalid_argument("--bypass-route-shadows conflicts with --route-shadow-casters");
        if (sourceConfigOption.empty() || artifactsOption.empty())
            throw std::invalid_argument("run requires --openmw, --config, and --artifacts");

        const std::filesystem::path executable = std::filesystem::absolute(executableOption).lexically_normal();
        const std::filesystem::path sourceConfig = std::filesystem::absolute(sourceConfigOption).lexically_normal();
        const std::filesystem::path artifacts = std::filesystem::absolute(artifactsOption).lexically_normal();
        const std::filesystem::path sourceDirectory
            = std::filesystem::is_directory(sourceConfig) ? sourceConfig : sourceConfig.parent_path();
        if (!std::filesystem::is_regular_file(executable))
            throw std::invalid_argument("run requires --openmw, --config, and --artifacts");
        std::filesystem::create_directories(artifacts);

        const ProfileFingerprint before = fingerprintProfile(sourceDirectory);
        TemporaryDirectory workspace(artifacts);
        const std::filesystem::path isolatedConfig = workspace.path() / "config";
        const std::filesystem::path isolatedData = workspace.path() / "userdata";
        copyProfileConfiguration(sourceConfig, isolatedConfig);
        for (const std::string& setting : findOptions(arguments, "--setting"))
            DiagnosticCtl::applyIsolatedSetting(isolatedConfig, DiagnosticCtl::parseIsolatedSetting(setting));
        std::filesystem::create_directories(isolatedData);

        const std::string endpoint = uniqueEndpoint();
        std::vector<std::string> childArguments{ "--config", isolatedConfig.string(), "--replace", "config",
            "--user-data", isolatedData.string(), "--diagnostic-session", endpoint, "--diagnostic-artifacts",
            artifacts.string(), "--diagnostic-child", "--diagnostic-fixed-dt",
            findOption(arguments, "--fixed-dt").value_or("0.0166666666666667"), "--no-sound", "--no-grab" };
        if (headed)
            childArguments.emplace_back("--diagnostic-headed");
        std::optional<std::filesystem::path> isolatedSave;
        std::vector<std::byte> replayManifest;
        std::vector<std::byte> expectedWorldState;
        std::vector<std::byte> gameplayRoute;
        if (const auto capsulePath = findOption(arguments, "--capsule"))
        {
            const Diagnostics::Capsule capsule = Diagnostics::readCapsule(*capsulePath);
            const Diagnostics::CapsuleEntry* manifest = capsule.find("manifest.yaml");
            const Diagnostics::CapsuleEntry* snapshot = capsule.find("state/world.omwsave");
            const Diagnostics::CapsuleEntry* worldState = capsule.find("state/world.yaml");
            const Diagnostics::CapsuleEntry* route = capsule.find("state/gameplay-route.yaml");
            const Diagnostics::CapsuleEntry* capturedAtmosphereScenario
                = capsule.find("configuration/atmosphere-scenario.yaml");
            if (manifest == nullptr || manifest->mType != Diagnostics::CapsuleEntryType::Manifest || snapshot == nullptr
                || snapshot->mType != Diagnostics::CapsuleEntryType::WorldSnapshot || worldState == nullptr)
                throw std::runtime_error("diagnostic capsule does not contain its replay state");
            replayManifest = manifest->mData;
            expectedWorldState = worldState->mData;
            if (route != nullptr)
            {
                if (route->mType != Diagnostics::CapsuleEntryType::GameplayTrace)
                    throw std::runtime_error("diagnostic capsule gameplay route has an invalid type");
                gameplayRoute = route->mData;
            }
            if (!atmosphereScenario && capturedAtmosphereScenario != nullptr)
            {
                if (capturedAtmosphereScenario->mType != Diagnostics::CapsuleEntryType::Configuration)
                    throw std::runtime_error("diagnostic capsule atmosphere scenario has an invalid type");
                atmosphereScenario
                    = Diagnostics::deserializeAtmosphereScenario(payloadText(capturedAtmosphereScenario->mData));
            }
            isolatedSave = workspace.path() / "replay.omwsave";
            std::ofstream stream(*isolatedSave, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(snapshot->mData.data()),
                static_cast<std::streamsize>(snapshot->mData.size()));
            if (!stream)
                throw std::runtime_error("failed materializing isolated capsule snapshot");
        }
        else if (const auto save = findOption(arguments, "--save"))
        {
            isolatedSave = workspace.path() / "replay.omwsave";
            std::filesystem::copy_file(*save, *isolatedSave);
        }
        if (isolatedSave)
            childArguments.insert(childArguments.end(), { "--load-savegame", isolatedSave->string(), "--skip-menu" });
        if (captureRouteNativeFrames && gameplayRoute.empty())
            throw std::runtime_error("native route capture requires a capsule containing a gameplay route");
        if (findOption(arguments, "--atmosphere-settings-sequence") && gameplayRoute.empty())
            throw std::invalid_argument("live atmosphere settings require a capsule containing a gameplay route");
        if (findOption(arguments, "--distance-settings-sequence") && gameplayRoute.empty())
            throw std::invalid_argument("live distance settings require a capsule containing a gameplay route");

        ChildProcess child(executable, childArguments);
        Diagnostics::Command hello{ 1, Diagnostics::CommandType::Hello, std::move(replayManifest) };
        const Diagnostics::Response ready = transactWhenReady(endpoint, hello, std::chrono::minutes(3), &child, headed);
        if (printResponse(ready) != 0)
            return 2;
        validateIsolatedSession(ready, headed);
        if (!headed)
            child.requireNonDisruptiveWindowState();
        std::cout << "desktop-window: " << (headed ? "present" : "absent") << '\n'
                  << "desktop-focus-acquired: " << (headed ? "allowed" : "false") << '\n';

        std::uint64_t requestId = 2;
        requireSuccessfulResponse(Diagnostics::transact(
            endpoint, { requestId++, Diagnostics::CommandType::CaptureCapsule, textPayload("capture.omwdiag") }));

        if (expectedWorldState.empty())
        {
            const Diagnostics::Capsule captured = Diagnostics::readCapsule(artifacts / "capture.omwdiag");
            const Diagnostics::CapsuleEntry* worldState = captured.find("state/world.yaml");
            if (worldState == nullptr)
                throw std::runtime_error("new diagnostic capsule omitted its world state");
            expectedWorldState = worldState->mData;
        }
        const Diagnostics::Response restored = Diagnostics::transact(
            endpoint, { requestId++, Diagnostics::CommandType::SetWorldState, expectedWorldState });
        requireSuccessfulResponse(restored);
        const Diagnostics::Response observed
            = Diagnostics::transact(endpoint, { requestId++, Diagnostics::CommandType::TraceGameplay, {} });
        if (observed.mError != Diagnostics::ErrorCode::None)
            return printResponse(observed);
        validateReplayState(expectedWorldState, observed.mPayload, !cameraOverride);
        std::cout << "initial-state-parity: true\n";

        if (cameraOverride)
        {
            const YAML::Node restoredState = YAML::Load(payloadText(expectedWorldState));
            const YAML::Node restoredAngles = restoredState["camera-angles"];
            const YAML::Node restoredPosition = restoredState["camera"];
            if (!restoredAngles.IsSequence() || restoredAngles.size() != 3)
                throw std::runtime_error("replay state contains invalid camera angles");
            if (!restoredPosition.IsSequence() || restoredPosition.size() != 3)
                throw std::runtime_error("replay state contains invalid camera coordinates");
            const double pitch
                = cameraPitch ? parseFiniteOption("--camera-pitch", *cameraPitch) : restoredAngles[0].as<double>();
            const double yaw
                = cameraYaw ? parseFiniteOption("--camera-yaw", *cameraYaw) : restoredAngles[1].as<double>();
            const double roll
                = cameraRoll ? parseFiniteOption("--camera-roll", *cameraRoll) : restoredAngles[2].as<double>();
            if (pitch < -1.5707963267948966 || pitch > 1.5707963267948966)
                throw std::invalid_argument("--camera-pitch must be within [-pi/2, pi/2]");

            YAML::Emitter cameraState;
            cameraState << YAML::BeginMap << YAML::Key << "camera-angles" << YAML::Value << YAML::Flow << YAML::BeginSeq
                        << pitch << yaw << roll << YAML::EndSeq;
            std::array<double, 3> requestedPosition{};
            if (cameraPositionOverride)
            {
                requestedPosition
                    = { cameraX ? parseFiniteOption("--camera-x", *cameraX) : restoredPosition[0].as<double>(),
                          cameraY ? parseFiniteOption("--camera-y", *cameraY) : restoredPosition[1].as<double>(),
                          cameraZ ? parseFiniteOption("--camera-z", *cameraZ) : restoredPosition[2].as<double>() };
                cameraState << YAML::Key << "camera-position" << YAML::Value << YAML::Flow << YAML::BeginSeq
                            << requestedPosition[0] << requestedPosition[1] << requestedPosition[2] << YAML::EndSeq;
            }
            cameraState << YAML::EndMap;
            if (!cameraState.good())
                throw std::runtime_error("failed serializing diagnostic camera state");
            const Diagnostics::Response applied = Diagnostics::transact(
                endpoint, { requestId++, Diagnostics::CommandType::SetCamera, textPayload(cameraState.c_str()) });
            requireSuccessfulResponse(applied);
            const YAML::Node actual = YAML::Load(payloadText(applied.mPayload));
            const YAML::Node actualAngles = actual["camera-angles"];
            if (!actualAngles.IsSequence() || actualAngles.size() != 3
                || std::abs(actualAngles[0].as<double>() - pitch) > 0.0001
                || std::abs(actualAngles[1].as<double>() - yaw) > 0.0001
                || std::abs(actualAngles[2].as<double>() - roll) > 0.0001)
                throw std::runtime_error("diagnostic child did not apply the requested camera angles");
            if (cameraPositionOverride)
            {
                const YAML::Node actualPosition = actual["camera-position"];
                if (!actualPosition.IsSequence() || actualPosition.size() != 3)
                    throw std::runtime_error("diagnostic child omitted the requested camera coordinates");
                for (std::size_t index = 0; index < requestedPosition.size(); ++index)
                {
                    if (std::abs(actualPosition[index].as<double>() - requestedPosition[index]) > 0.01)
                        throw std::runtime_error("diagnostic child did not apply the requested camera coordinates");
                }
            }
            std::cout << "camera-override-parity: true\n";
        }

        if (atmosphereScenario)
        {
            const std::string serialized = Diagnostics::serializeAtmosphereScenario(*atmosphereScenario);
            const Diagnostics::Response applied = Diagnostics::transact(
                endpoint, { requestId++, Diagnostics::CommandType::SetAtmosphereScenario, textPayload(serialized) });
            requireSuccessfulResponse(applied);
            const YAML::Node actual = YAML::Load(payloadText(applied.mPayload));
            if (atmosphereScenario->mWeather)
            {
                const Diagnostics::DiagnosticWeatherState& expected = *atmosphereScenario->mWeather;
                if (!Misc::StringUtils::ciEqual(actual["weather"].as<std::string>(), expected.mCurrent)
                    || !Misc::StringUtils::ciEqual(
                        actual["next-weather"].as<std::string>(), expected.mNext.value_or(std::string()))
                    || std::abs(actual["weather-transition"].as<float>() - expected.mTransition) > 0.0001f)
                    throw std::runtime_error("diagnostic child did not apply the requested weather state");
            }
            if (atmosphereScenario->mGameHour
                && std::abs(actual["game-hour"].as<double>() - *atmosphereScenario->mGameHour) > 0.0001)
                throw std::runtime_error("diagnostic child did not apply the requested game hour");
            if (atmosphereScenario->mStartGameHour
                && std::abs(actual["game-hour"].as<double>() - *atmosphereScenario->mStartGameHour) > 0.0001)
                throw std::runtime_error("diagnostic child did not apply the requested starting game hour");
            if (atmosphereScenario->mCloudSeed
                && actual["cloud-seed"].as<std::uint32_t>() != *atmosphereScenario->mCloudSeed)
                throw std::runtime_error("diagnostic child did not apply the requested cloud seed");
            if (atmosphereScenario->mRecentPrecipitation
                && std::abs(actual["recent-precipitation"].as<float>() - *atmosphereScenario->mRecentPrecipitation)
                    > 1e-6f)
                throw std::runtime_error("diagnostic child did not apply the requested recent precipitation");
            if (atmosphereScenario->mWeatherEffects
                && (actual["precipitation"].as<bool>() != atmosphereScenario->mWeatherEffects->mPrecipitation
                    || actual["lightning"].as<bool>() != atmosphereScenario->mWeatherEffects->mLightning))
                throw std::runtime_error("diagnostic child did not apply the requested weather effects");
            if (atmosphereScenario->mEnvironment)
            {
                const YAML::Node actualTraits = actual["environment-traits"];
                if (!actualTraits.IsMap() || actualTraits.size() != atmosphereScenario->mEnvironment->mTraits.size())
                    throw std::runtime_error("diagnostic child did not apply the requested environment traits");
                for (const auto& [trait, weight] : atmosphereScenario->mEnvironment->mTraits)
                {
                    if (!actualTraits[trait] || std::abs(actualTraits[trait].as<float>() - weight) > 0.0001f)
                        throw std::runtime_error("diagnostic child did not apply the requested environment traits");
                }
                if (atmosphereScenario->mEnvironment->mTerrainHeightWorldUnits
                    && std::abs(actual["environment-terrain-height"].as<float>()
                           - *atmosphereScenario->mEnvironment->mTerrainHeightWorldUnits)
                        > 0.0001f)
                    throw std::runtime_error("diagnostic child did not apply the requested environment terrain height");
            }
            std::cout << "atmosphere-scenario-parity: true\n";
        }
        if (atmosphereScenario || cameraOverride)
            requireSuccessfulResponse(Diagnostics::transact(
                endpoint, { requestId++, Diagnostics::CommandType::CaptureCapsule, textPayload("scenario.omwdiag") }));

        std::uint64_t completedFixedFrames = 0;
        if (atmosphereWarmupFrames > 0)
        {
            Diagnostics::Command warmup{ requestId++, Diagnostics::CommandType::StepFrames, {} };
            for (std::size_t byte = 0; byte < sizeof(atmosphereWarmupFrames); ++byte)
                warmup.mPayload.push_back(static_cast<std::byte>((atmosphereWarmupFrames >> (byte * 8)) & 0xffu));
            requireSuccessfulResponse(Diagnostics::transact(endpoint, warmup));
            completedFixedFrames += atmosphereWarmupFrames;
            waitForScheduledFrames(endpoint, requestId, completedFixedFrames);
            std::cout << "atmosphere-warmup-frames: " << atmosphereWarmupFrames << '\n';
        }

        if (!gameplayRoute.empty())
        {
            const std::vector<Diagnostics::GameplayRouteFrame> routeFrames
                = Diagnostics::decodeGameplayRoute(gameplayRoute);
            const DiagnosticCtl::GameplayRouteExecutionPlan routePlan = DiagnosticCtl::makeGameplayRouteExecutionPlan(
                routeFrames.size(), findOption(arguments, "--frames"), findOption(arguments, "--performance-samples"));
            requireSuccessfulResponse(Diagnostics::transact(
                endpoint, { requestId++, Diagnostics::CommandType::LoadGameplayRoute, gameplayRoute }));
            if (captureRouteNativeFrames)
            {
                const Diagnostics::Response capabilities
                    = Diagnostics::transact(endpoint, { requestId++, Diagnostics::CommandType::InspectSession, {} });
                requireSuccessfulResponse(capabilities);
                if (payloadText(capabilities.mPayload).find("graphics@3") == std::string::npos)
                    throw std::runtime_error("native route capture requires graphics@3");
            }
            std::vector<Diagnostics::AtmosphereSettings> settingsSequence;
            if (const auto sequencePath = findOption(arguments, "--atmosphere-settings-sequence"))
            {
                const YAML::Node sequence = YAML::LoadFile(*sequencePath);
                const std::size_t segmentCount
                    = (routePlan.mFrameCount + routePlan.mSegmentFrameCount - 1) / routePlan.mSegmentFrameCount;
                if (!sequence.IsSequence() || sequence.size() != segmentCount || sequence.size() > 64)
                    throw std::invalid_argument(
                        "atmosphere settings sequence requires one entry per route segment, at most 64");
                for (const auto& entry : sequence)
                    settingsSequence.push_back(Diagnostics::deserializeAtmosphereSettings(YAML::Dump(entry)));
                std::filesystem::copy_file(*sequencePath, artifacts / "atmosphere-settings-sequence.yaml");
            }
            std::size_t completedFrames = 0;
            unsigned segmentIndex = 0;
            std::vector<Diagnostics::DistanceSettings> distanceSequence;
            if (const auto sequencePath = findOption(arguments, "--distance-settings-sequence"))
            {
                const YAML::Node sequence = YAML::LoadFile(*sequencePath);
                const std::size_t segmentCount
                    = (routePlan.mFrameCount + routePlan.mSegmentFrameCount - 1) / routePlan.mSegmentFrameCount;
                if (!sequence.IsSequence() || sequence.size() != segmentCount || sequence.size() > 64)
                    throw std::invalid_argument(
                        "distance settings sequence must contain one entry per route segment, at most 64");
                for (const auto& entry : sequence)
                    distanceSequence.push_back(Diagnostics::deserializeDistanceSettings(YAML::Dump(entry)));
                std::filesystem::copy_file(*sequencePath, artifacts / "distance-settings-sequence.yaml");
            }
            while (completedFrames < routePlan.mFrameCount)
            {
                if (!distanceSequence.empty())
                {
                    const auto& requested = distanceSequence.at(segmentIndex);
                    const auto applied = Diagnostics::transact(endpoint,
                        { requestId++, Diagnostics::CommandType::SetDistanceSettings,
                            textPayload(Diagnostics::serializeDistanceSettings(requested)) });
                    requireSuccessfulResponse(applied);
                    const auto observed = Diagnostics::deserializeDistanceSettings(payloadText(applied.mPayload));
                    if (observed.mView != requested.mView || observed.mGrass != requested.mGrass
                        || observed.mUseCellEditor != requested.mUseCellEditor
                        || (requested.mShadow > 0 ? observed.mShadow != requested.mShadow : observed.mShadow > 0))
                        throw std::runtime_error("live distance controls did not match the requested preferences");
                    std::ofstream evidence(artifacts / ("distance-settings-" + std::to_string(segmentIndex) + ".yaml"));
                    evidence << Diagnostics::serializeDistanceSettings(observed);
                    if (!evidence)
                        throw std::runtime_error("failed writing live distance settings evidence");
                    std::cout << "live-distance-settings-parity: " << segmentIndex << " true\n";
                }
                if (!settingsSequence.empty())
                {
                    const auto& requested = settingsSequence.at(segmentIndex);
                    const auto applied = Diagnostics::transact(endpoint,
                        { requestId++, Diagnostics::CommandType::SetAtmosphereSettings,
                            textPayload(Diagnostics::serializeAtmosphereSettings(requested)) });
                    requireSuccessfulResponse(applied);
                    if (Diagnostics::deserializeAtmosphereSettings(payloadText(applied.mPayload)) != requested)
                        throw std::runtime_error("live atmosphere settings did not match the requested preferences");
                    std::cout << "live-atmosphere-settings-parity: " << segmentIndex << " true\n";
                }
                const std::uint32_t segmentFrames = static_cast<std::uint32_t>(
                    std::min<std::size_t>(routePlan.mSegmentFrameCount, routePlan.mFrameCount - completedFrames));
                Diagnostics::Command runSegment{ requestId++, Diagnostics::CommandType::RunGameplayRouteSegment, {} };
                const std::uint64_t segmentRequestId = runSegment.mRequestId;
                for (std::size_t byte = 0; byte < sizeof(segmentFrames); ++byte)
                    runSegment.mPayload.push_back(static_cast<std::byte>((segmentFrames >> (byte * 8)) & 0xffu));
                if (captureRouteFrames || bypassRouteShadows || routeShadowCasters)
                {
                    const unsigned routeOptions = (captureRouteFrames ? 0x1u : 0u) | (bypassRouteShadows ? 0x2u : 0u)
                        | (routeShadowCasters == "terrain" ? 0x4u : 0u) | (routeShadowCasters == "objects" ? 0x8u : 0u)
                        | (captureRouteNativeFrames ? 0x10u : 0u);
                    runSegment.mPayload.push_back(static_cast<std::byte>(routeOptions));
                }
                const Diagnostics::Response performance
                    = Diagnostics::transact(endpoint, runSegment, std::chrono::minutes(3));
                if (performance.mError != Diagnostics::ErrorCode::None)
                    return printResponse(performance);
                std::ostringstream performanceName;
                performanceName << "performance-segment-" << std::setw(3) << std::setfill('0') << segmentIndex
                                << ".txt";
                std::ofstream performanceFile(artifacts / performanceName.str(), std::ios::binary | std::ios::trunc);
                performanceFile.write(reinterpret_cast<const char*>(performance.mPayload.data()),
                    static_cast<std::streamsize>(performance.mPayload.size()));
                if (!performanceFile)
                    throw std::runtime_error("failed writing gameplay-route performance evidence");
                completedFixedFrames += segmentFrames;
                waitForScheduledFrames(endpoint, requestId, completedFixedFrames);
                const Diagnostics::Response routeStatus
                    = Diagnostics::transact(endpoint, { requestId++, Diagnostics::CommandType::InspectSession, {} });
                requireSuccessfulResponse(routeStatus);
                const YAML::Node status = YAML::Load(payloadText(routeStatus.mPayload));
                DiagnosticCtl::validateGameplayRouteProgress(routeFrames.size() - completedFrames, segmentFrames,
                    status["route-frames-remaining"].as<std::size_t>());
                completedFrames += segmentFrames;
                ++segmentIndex;
                if (captureRouteFrames)
                {
                    waitForRenderFrameCapture(endpoint, requestId);
                    const std::filesystem::path frameBundle
                        = artifacts / ("route-frames-" + std::to_string(segmentRequestId) + ".omwdiag");
                    if (!std::filesystem::is_regular_file(frameBundle))
                        throw std::runtime_error("exact render-frame capture did not produce its bundle");
                    std::cout << "route-frame-capture: " << frameBundle << '\n';
                }
                if (captureGraphics)
                {
                    requireSuccessfulResponse(Diagnostics::transact(
                        endpoint, { requestId++, Diagnostics::CommandType::CaptureGraphics, graphicsCapturePayload }));
                    waitForGraphicsCapture(endpoint, requestId);
                }
            }
            std::cout << "executed-route-frames: " << completedFrames << '\n';
        }
        else
        {
            const std::uint32_t frames
                = static_cast<std::uint32_t>(std::stoul(findOption(arguments, "--frames").value_or("16")));
            const std::uint32_t performanceSamples = static_cast<std::uint32_t>(std::stoul(
                findOption(arguments, "--performance-samples").value_or(std::to_string(std::min(frames, 16u)))));
            if (performanceSamples == 0 || performanceSamples > frames || performanceSamples > 600)
                throw std::invalid_argument("performance samples must be within [1, min(frames, 600)]");
            Diagnostics::Command step{ requestId++, Diagnostics::CommandType::StepFrames, {} };
            for (std::size_t byte = 0; byte < sizeof(frames); ++byte)
                step.mPayload.push_back(static_cast<std::byte>((frames >> (byte * 8)) & 0xffu));
            requireSuccessfulResponse(Diagnostics::transact(endpoint, step));
            completedFixedFrames += frames;
            waitForScheduledFrames(endpoint, requestId, completedFixedFrames);
            std::cout << "executed-fixed-frames: " << frames << '\n';

            Diagnostics::Command performanceCommand{ requestId++, Diagnostics::CommandType::TracePerformance, {} };
            for (std::size_t byte = 0; byte < sizeof(performanceSamples); ++byte)
                performanceCommand.mPayload.push_back(
                    static_cast<std::byte>((performanceSamples >> (byte * 8)) & 0xffu));
            const Diagnostics::Response performance
                = Diagnostics::transact(endpoint, performanceCommand, std::chrono::seconds(60));
            if (performance.mError != Diagnostics::ErrorCode::None)
                return printResponse(performance);
            std::ofstream performanceFile(artifacts / "performance.txt", std::ios::binary | std::ios::trunc);
            performanceFile.write(reinterpret_cast<const char*>(performance.mPayload.data()),
                static_cast<std::streamsize>(performance.mPayload.size()));
            if (!performanceFile)
                throw std::runtime_error("failed writing performance evidence");

            if (captureGraphics)
            {
                requireSuccessfulResponse(Diagnostics::transact(
                    endpoint, { requestId++, Diagnostics::CommandType::CaptureGraphics, graphicsCapturePayload }));
                waitForGraphicsCapture(endpoint, requestId);
            }
        }
        requireSuccessfulResponse(Diagnostics::transact(
            endpoint, { requestId++, Diagnostics::CommandType::CaptureCapsule, textPayload("completed.omwdiag") }));
        const Diagnostics::Response finalGameplay
            = Diagnostics::transact(endpoint, { requestId++, Diagnostics::CommandType::TraceGameplay, {} });
        requireSuccessfulResponse(finalGameplay);
        std::ofstream gameplayFile(artifacts / "gameplay.yaml", std::ios::binary | std::ios::trunc);
        gameplayFile.write(reinterpret_cast<const char*>(finalGameplay.mPayload.data()),
            static_cast<std::streamsize>(finalGameplay.mPayload.size()));
        if (!gameplayFile)
            throw std::runtime_error("failed writing final gameplay evidence");
        if (const auto page = findOption(arguments, "--video-settings-page"))
        {
            requireSuccessfulResponse(Diagnostics::transact(
                endpoint, { requestId++, Diagnostics::CommandType::ShowVideoSettings, textPayload(*page) }));
            Diagnostics::Command settle{ requestId++, Diagnostics::CommandType::StepFrames,
                { std::byte{ 8 }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 0 } } };
            requireSuccessfulResponse(Diagnostics::transact(endpoint, settle));
            waitForScheduledFrames(endpoint, requestId, completedFixedFrames + 8);
        }
        if (!headed)
            child.requireNonDisruptiveWindowState();
        requireSuccessfulResponse(
            Diagnostics::transact(endpoint, { requestId++, Diagnostics::CommandType::ShutdownChild, {} }));

        const int exitCode = child.wait(std::chrono::seconds(30));
        const ProfileFingerprint after = fingerprintProfile(sourceDirectory);
        if (after != before)
            throw std::runtime_error(
                "source profile changed during isolated diagnostic run: " + describeProfileDifference(before, after));
        if (exitCode != 0)
            throw std::runtime_error("diagnostic child exited with status " + std::to_string(exitCode));
        workspace.cleanup();
        std::cout << "child-reaped: true\n"
                     "workspace-cleaned: true\n"
                     "profile-preserved: true\n";
        return 0;
    }

}
