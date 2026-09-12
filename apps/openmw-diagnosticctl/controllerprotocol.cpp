#include "controllerprotocol.hpp"

#include "isolatedworkspace.hpp"
#include "replaystate.hpp"

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <thread>

namespace DiagnosticCtl
{
    /// Convert a text argument into a bounded protocol payload.
    std::vector<std::byte> textPayload(std::string_view text)
    {
        if (text.size() > Diagnostics::maximumPayloadSize)
            throw std::length_error("diagnostic text argument exceeds the protocol limit");
        const auto* begin = reinterpret_cast<const std::byte*>(text.data());
        return { begin, begin + text.size() };
    }

    /// Convert a protocol payload into printable text without assuming a terminator.
    std::string payloadText(std::span<const std::byte> payload)
    {
        return { reinterpret_cast<const char*>(payload.data()), payload.size() };
    }

    /// Parse one named command-line option without accepting ambiguous duplicates.
    std::optional<std::string> findOption(std::span<char*> arguments, std::string_view name)
    {
        std::optional<std::string> result;
        for (std::size_t index = 0; index < arguments.size(); ++index)
        {
            if (arguments[index] != name)
                continue;
            if (result || index + 1 >= arguments.size())
                throw std::invalid_argument("missing or duplicated option " + std::string(name));
            result = arguments[++index];
        }
        return result;
    }

    /// Parse every occurrence of a repeatable named command-line option.
    std::vector<std::string> findOptions(std::span<char*> arguments, std::string_view name)
    {
        std::vector<std::string> result;
        for (std::size_t index = 0; index < arguments.size(); ++index)
        {
            if (arguments[index] != name)
                continue;
            if (index + 1 >= arguments.size())
                throw std::invalid_argument("missing option value " + std::string(name));
            result.emplace_back(arguments[++index]);
        }
        return result;
    }

    /// Return whether one non-valued command-line flag appears exactly once.
    bool hasFlag(std::span<char*> arguments, std::string_view name)
    {
        std::size_t count = 0;
        for (const char* argument : arguments)
        {
            if (argument == name)
                ++count;
        }
        if (count > 1)
            throw std::invalid_argument("duplicated flag " + std::string(name));
        return count == 1;
    }

    /// Parse one finite floating-point option and reject trailing non-numeric text.
    double parseFiniteOption(std::string_view name, const std::string& text)
    {
        std::size_t consumed = 0;
        const double value = std::stod(text, &consumed);
        if (consumed != text.size() || !std::isfinite(value))
            throw std::invalid_argument(std::string(name) + " requires a finite number");
        return value;
    }

    /// Parse one unsigned frame-count option without allowing narrowing or trailing text.
    std::uint32_t parseFrameCountOption(std::string_view name, const std::string& text, std::uint32_t maximum)
    {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed);
        if (consumed != text.size() || value > maximum)
            throw std::invalid_argument(std::string(name) + " must be within [0, " + std::to_string(maximum) + "]");
        return static_cast<std::uint32_t>(value);
    }

    /// Parse comma-separated semantic environment weights without assigning built-in biome identities.
    std::map<std::string, float, std::less<>> parseEnvironmentTraits(const std::string& text)
    {
        std::map<std::string, float, std::less<>> traits;
        std::size_t start = 0;
        while (start < text.size())
        {
            const std::size_t end = text.find(',', start);
            const std::string_view assignment(
                text.data() + start, (end == std::string::npos ? text.size() : end) - start);
            const std::size_t separator = assignment.find('=');
            if (separator == std::string_view::npos || separator == 0 || separator + 1 >= assignment.size()
                || assignment.find('=', separator + 1) != std::string_view::npos)
                throw std::invalid_argument("--environment-traits requires comma-separated name=weight pairs");
            const std::string trait(assignment.substr(0, separator));
            const float weight = static_cast<float>(
                parseFiniteOption("--environment-traits", std::string(assignment.substr(separator + 1))));
            if (weight < 0.f || weight > 1.f || !traits.emplace(trait, weight).second)
                throw std::invalid_argument("--environment-traits requires unique weights within [0, 1]");
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        if (traits.empty())
            throw std::invalid_argument("--environment-traits requires at least one name=weight pair");
        return traits;
    }

    /// Build one exact atmosphere scenario from mutually exclusive steady and transitioning weather options.
    std::optional<Diagnostics::AtmosphereScenario> parseAtmosphereScenarioOptions(std::span<char*> arguments)
    {
        const std::optional<std::string> weather = findOption(arguments, "--weather");
        const std::optional<std::string> weatherFrom = findOption(arguments, "--weather-from");
        const std::optional<std::string> weatherTo = findOption(arguments, "--weather-to");
        const std::optional<std::string> transition = findOption(arguments, "--weather-transition");
        const std::optional<std::string> gameHour = findOption(arguments, "--game-hour");
        const std::optional<std::string> startGameHour = findOption(arguments, "--start-game-hour");
        const std::optional<std::string> cloudSeed = findOption(arguments, "--cloud-seed");
        const std::optional<std::string> recentPrecipitation = findOption(arguments, "--recent-precipitation");
        const std::optional<std::string> environmentTraits = findOption(arguments, "--environment-traits");
        const std::optional<std::string> environmentTerrainHeight
            = findOption(arguments, "--environment-terrain-height");
        const bool disableMainClouds = hasFlag(arguments, "--disable-main-clouds");
        const bool disableLowClouds = hasFlag(arguments, "--disable-low-clouds");
        const bool disableMist = hasFlag(arguments, "--disable-mist");
        const bool disableFog = hasFlag(arguments, "--disable-fog");
        const bool disableDistanceFog = hasFlag(arguments, "--disable-distance-fog");
        const bool disablePrecipitation = hasFlag(arguments, "--disable-precipitation");
        const bool disableLightning = hasFlag(arguments, "--disable-lightning");
        const bool anyTransitionOption = weatherFrom || weatherTo || transition;
        if (weather && anyTransitionOption)
            throw std::invalid_argument("--weather conflicts with transitioning weather options");
        if (anyTransitionOption && (!weatherFrom || !weatherTo || !transition))
            throw std::invalid_argument(
                "transitioning weather requires --weather-from, --weather-to, and --weather-transition");

        Diagnostics::AtmosphereScenario scenario;
        if (weather)
            scenario.mWeather = Diagnostics::DiagnosticWeatherState{ *weather, std::nullopt, 0.f };
        else if (anyTransitionOption)
        {
            scenario.mWeather = Diagnostics::DiagnosticWeatherState{ *weatherFrom, *weatherTo,
                static_cast<float>(parseFiniteOption("--weather-transition", *transition)) };
        }
        if (gameHour)
            scenario.mGameHour = parseFiniteOption("--game-hour", *gameHour);
        if (startGameHour)
            scenario.mStartGameHour = parseFiniteOption("--start-game-hour", *startGameHour);
        if (disableMainClouds || disableLowClouds || disableMist || disableFog)
            scenario.mMedia = Diagnostics::DiagnosticAtmosphereMedia{ !disableMainClouds, !disableLowClouds,
                !disableMist, !disableFog };
        if (disableDistanceFog)
            scenario.mDistanceFog = false;
        if (cloudSeed)
            scenario.mCloudSeed
                = parseFrameCountOption("--cloud-seed", *cloudSeed, std::numeric_limits<std::uint32_t>::max());
        if (recentPrecipitation)
            scenario.mRecentPrecipitation
                = static_cast<float>(parseFiniteOption("--recent-precipitation", *recentPrecipitation));
        if (disablePrecipitation || disableLightning)
            scenario.mWeatherEffects
                = Diagnostics::DiagnosticWeatherEffects{ !disablePrecipitation, !disableLightning };
        if (environmentTraits || environmentTerrainHeight)
        {
            Diagnostics::DiagnosticEnvironmentState environment;
            if (environmentTraits)
                environment.mTraits = parseEnvironmentTraits(*environmentTraits);
            if (environmentTerrainHeight)
                environment.mTerrainHeightWorldUnits
                    = static_cast<float>(parseFiniteOption("--environment-terrain-height", *environmentTerrainHeight));
            scenario.mEnvironment = std::move(environment);
        }
        if (scenario.empty())
            return std::nullopt;

        // Canonical serialization performs the same strict range validation used by the child.
        static_cast<void>(Diagnostics::serializeAtmosphereScenario(scenario));
        return scenario;
    }

    /// Generate a unique protected endpoint within the transport's reserved namespace.
    std::string uniqueEndpoint()
    {
        std::random_device source;
        return Diagnostics::defaultLocalEndpoint() + "-" + std::to_string(source());
    }

    /// Send a command after the child or live session has reached its safe-point loop.
    Diagnostics::Response transactWhenReady(const std::string& endpoint, const Diagnostics::Command& command,
        std::chrono::seconds readinessTimeout, ChildProcess* child, bool allowVisibleChild)
    {
        const auto deadline = std::chrono::steady_clock::now() + readinessTimeout;
        std::exception_ptr lastFailure;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (child)
            {
                if (const std::optional<int> exitCode = child->tryExitCode())
                    throw std::runtime_error(
                        "diagnostic child exited before readiness with code " + std::to_string(*exitCode));
                if (!allowVisibleChild)
                    child->requireNonDisruptiveWindowState();
            }
            try
            {
                return Diagnostics::transact(endpoint, command, std::chrono::milliseconds(250));
            }
            catch (...)
            {
                lastFailure = std::current_exception();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        if (lastFailure)
            std::rethrow_exception(lastFailure);
        throw std::runtime_error("diagnostic session did not become ready");
    }

    /// Require the engine to attest to the isolated graphics and input owners used by the child.
    void validateIsolatedSession(const Diagnostics::Response& response, bool headed)
    {
        const YAML::Node status = YAML::Load(payloadText(response.mPayload));
        if (status["mode"].as<std::string>("") != "isolated-child"
            || status["graphics-context"].as<std::string>("") != (headed ? "interactive-window" : "offscreen-pbuffer")
            || status["native-window"].as<std::string>("") != (headed ? "present" : "absent")
            || status["input-source"].as<std::string>("") != "device-free")
            throw std::runtime_error("diagnostic child did not attest to the requested isolated runtime");
    }

    /// Print one structured response and convert protocol errors to a failing exit status.
    int printResponse(const Diagnostics::Response& response)
    {
        if (!response.mPayload.empty())
            std::cout << payloadText(response.mPayload);
        if (response.mError != Diagnostics::ErrorCode::None)
        {
            std::cerr << "diagnostic command failed: " << Diagnostics::getErrorName(response.mError) << '\n';
            return 2;
        }
        return 0;
    }

    /// Print one response and stop an automated workflow when the engine rejected the command.
    void requireSuccessfulResponse(const Diagnostics::Response& response)
    {
        if (printResponse(response) != 0)
            throw std::runtime_error("diagnostic workflow command failed");
    }

    /// Print the stable controller command surface and isolation workflow options.
    void printUsage(std::ostream& stream)
    {
        stream << "Usage:\n"
                  "  openmw-diagnosticctl <status|graphics|gameplay|reload-shaders> [--endpoint name]\n"
                  "  openmw-diagnosticctl performance [--endpoint name] [--performance-samples count]\n"
                  "  openmw-diagnosticctl capsule [--endpoint name] [--output capture.omwdiag]\n"
                  "  openmw-diagnosticctl step [--endpoint name] [--frames count]\n"
                  "  openmw-diagnosticctl shutdown [--endpoint name]\n"
                  "  openmw-diagnosticctl <run|replay> --openmw openmw.exe --config openmw.cfg\n"
                  "      --artifacts directory [--save save.omwsave | --capsule capture.omwdiag]\n"
                  "      [--frames count] [--performance-samples count] [--fixed-dt seconds]\n"
                  "      [--weather name | --weather-from name --weather-to name --weather-transition progress]\n"
                  "      [--game-hour hour | --start-game-hour hour] [--cloud-seed integer] [--recent-precipitation "
                  "weight]\n"
                  "      [--atmosphere-warmup-frames count]\n"
                  "      [--environment-traits name=weight,...] [--environment-terrain-height world-units]\n"
                  "      [--camera-x world-units] [--camera-y world-units] [--camera-z world-units]\n"
                  "      [--camera-pitch radians] [--camera-yaw radians] [--camera-roll radians]\n"
                  "      [--disable-main-clouds] [--disable-low-clouds] [--disable-mist] [--disable-fog]\n"
                  "      [--disable-distance-fog] [--disable-precipitation] [--disable-lightning]\n"
                  "      [--capture-route-frames | --capture-route-native-frames] [--skip-graphics-capture | "
                  "--normal-only-graphics-capture]\n"
                  "      [--bypass-route-shadows]\n"
                  "      [--route-shadow-casters terrain|objects]\n"
                  "      [--headed]\n"
                  "      [--atmosphere-settings-sequence sequence.yaml]\n"
                  "      [--video-settings-page atmosphere|water|shadows|detail]\n"
                  "      [--distance-settings-sequence <yaml>]\n"
                  "      [--setting \"Section:name=value\"]...\n"
                  "  openmw-diagnosticctl inspect-capsule --capsule capture.omwdiag [--entry name]\n"
                  "  openmw-diagnosticctl add-route --capsule capture.omwdiag --route route.yaml --output "
                  "motion.omwdiag\n";
    }

    /// Wait for every production render stage to restore normal state before continuing.
    void waitForGraphicsCapture(const std::string& endpoint, std::uint64_t& requestId)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const Diagnostics::Response status
                = Diagnostics::transact(endpoint, { requestId++, Diagnostics::CommandType::InspectSession, {} });
            if (status.mError != Diagnostics::ErrorCode::None)
                throw std::runtime_error("failed polling production graphics capture");
            if (payloadText(status.mPayload).find("graphics-capture: idle") != std::string::npos)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("production graphics capture did not complete within its timeout");
    }

    /// Wait until an exact final-composite sequence has finished serializing.
    void waitForRenderFrameCapture(const std::string& endpoint, std::uint64_t& requestId)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const Diagnostics::Response status
                = Diagnostics::transact(endpoint, { requestId++, Diagnostics::CommandType::InspectSession, {} });
            if (status.mError != Diagnostics::ErrorCode::None)
                throw std::runtime_error("failed polling exact render-frame capture");
            if (payloadText(status.mPayload).find("render-frame-capture: idle") != std::string::npos)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("exact render-frame capture did not complete within its timeout");
    }

    /// Wait for an exact cumulative fixed-frame count without advancing simulation after completion.
    void waitForScheduledFrames(
        const std::string& endpoint, std::uint64_t& requestId, std::uint64_t expectedCompletedFrames)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const Diagnostics::Response status
                = Diagnostics::transact(endpoint, { requestId++, Diagnostics::CommandType::InspectSession, {} });
            if (status.mError != Diagnostics::ErrorCode::None)
                throw std::runtime_error("failed polling deterministic diagnostic frames");
            const YAML::Node parsed = YAML::Load(payloadText(status.mPayload));
            if (parsed["scheduled-frames-remaining"].as<std::uint32_t>() == 0)
            {
                if (parsed["completed-fixed-frames"].as<std::uint64_t>() != expectedCompletedFrames)
                    throw std::runtime_error("diagnostic child completed an unexpected fixed-frame count");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error("deterministic diagnostic frames did not complete within their timeout");
    }

    /// Compare stable replay state while allowing an explicit camera command to restore transient camera placement.
    void validateReplayState(
        std::span<const std::byte> expectedBytes, std::span<const std::byte> actualBytes, bool validateCamera)
    {
        const YAML::Node expected = YAML::Load(payloadText(expectedBytes));
        const YAML::Node actual = YAML::Load(payloadText(actualBytes));
        const std::string provider = actual["provider"].as<std::string>("");
        if (provider != "gameplay@4" && provider != "gameplay@5" && provider != "gameplay@6" && provider != "gameplay@7"
            && provider != "gameplay@8" && provider != "gameplay@9")
            throw std::runtime_error("replay state was reported by an incompatible gameplay provider");
        for (const char* key : { "cell", "weather" })
        {
            if (expected[key].as<std::string>() != actual[key].as<std::string>())
                throw std::runtime_error(std::string("replay state mismatch for ") + key);
        }
        if (validateCamera)
        {
            const YAML::Node expectedCamera = expected["camera"];
            const YAML::Node actualCamera = actual["camera"];
            if (!expectedCamera.IsSequence() || !actualCamera.IsSequence() || expectedCamera.size() != 3
                || actualCamera.size() != 3)
                throw std::runtime_error("replay state contains invalid camera coordinates");
            constexpr double cameraPositionToleranceWorldUnits = 32.0;
            for (std::size_t index = 0; index < 3; ++index)
            {
                if (std::abs(expectedCamera[index].as<double>() - actualCamera[index].as<double>())
                    > cameraPositionToleranceWorldUnits)
                    throw std::runtime_error("replay camera position does not match its capsule at component "
                        + std::to_string(index) + ": expected " + expectedCamera[index].as<std::string>()
                        + ", observed " + actualCamera[index].as<std::string>());
            }
            const YAML::Node expectedAngles = expected["camera-angles"];
            const YAML::Node actualAngles = actual["camera-angles"];
            if (!expectedAngles.IsSequence() || !actualAngles.IsSequence() || expectedAngles.size() != 3
                || actualAngles.size() != 3)
                throw std::runtime_error("replay state contains invalid camera angles");
            for (std::size_t index = 0; index < 3; ++index)
            {
                if (std::abs(expectedAngles[index].as<double>() - actualAngles[index].as<double>()) > 0.0001)
                    throw std::runtime_error("replay camera angles do not match its capsule");
            }
        }
        for (const char* key : { "first-person", "camera-underwater" })
        {
            if (expected[key] && expected[key].as<bool>() != actual[key].as<bool>())
                throw std::runtime_error(std::string("replay state mismatch for ") + key);
        }
        if (std::abs(expected["game-time-seconds"].as<double>() - actual["game-time-seconds"].as<double>()) > 0.01
            || std::abs(expected["simulation-time"].as<double>() - actual["simulation-time"].as<double>()) > 0.01)
            throw std::runtime_error("replay clocks do not match their capsule");
        if (const auto cloudTravel = expected["cloud-advection-world-units"])
        {
            const auto restoredTravel = actual["cloud-advection-world-units"];
            if (!cloudTravel.IsSequence() || cloudTravel.size() != 2 || !restoredTravel.IsSequence()
                || restoredTravel.size() != 2)
                throw std::runtime_error("replay cloud wind displacement is missing or invalid");
            for (std::size_t axis = 0; axis < 2; ++axis)
            {
                if (cloudTravel[axis].as<double>() != restoredTravel[axis].as<double>())
                    throw std::runtime_error("replay cloud wind displacement does not match its capsule");
            }
            std::cout << "cloud-advection-parity: true\n";
        }
        else
            std::cout << "cloud-advection-parity: unavailable (capture predates wind-state recording)\n";
        if (const auto atmosphereRuntime = expected["atmosphere-runtime"])
        {
            DiagnosticCtl::validateAtmosphereRuntimeParity(
                YAML::Dump(atmosphereRuntime), YAML::Dump(actual["atmosphere-runtime"]));
            std::cout << "atmosphere-runtime-parity: true\n";
        }
        else
            std::cout << "atmosphere-runtime-parity: unavailable (capture predates regional history recording)\n";
    }

}
