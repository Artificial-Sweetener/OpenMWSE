#include <components/debug/debugging.hpp>
#include <components/diagnostics/atmospherescenario.hpp>
#include <components/diagnostics/capsule.hpp>
#include <components/diagnostics/gameplayroute.hpp>
#include <components/diagnostics/localtransport.hpp>
#include <components/diagnostics/session.hpp>
#include <components/files/hash.hpp>
#include <components/misc/strings/algorithm.hpp>

#include "gameplayrouteexecution.hpp"
#include "isolatedsettings.hpp"

#include <yaml-cpp/yaml.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <components/misc/windows.hpp>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace
{
    /// Own one native child process without routing arguments through a shell.
    class ChildProcess
    {
    public:
        /// Launch the executable with explicit arguments and no inherited interactive window.
        ChildProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments)
        {
#ifdef _WIN32
            std::wstring commandLine = quoteWindowsArgument(executable.wstring());
            for (const std::string& argument : arguments)
            {
                commandLine.push_back(L' ');
                commandLine += quoteWindowsArgument(std::filesystem::path(argument).wstring());
            }
            std::vector<wchar_t> writable(commandLine.begin(), commandLine.end());
            writable.push_back(L'\0');
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            if (!CreateProcessW(executable.c_str(), writable.data(), nullptr, nullptr, FALSE,
                    CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, executable.parent_path().c_str(), &startup,
                    &mProcess))
                throw std::runtime_error("failed launching diagnostic child: " + std::to_string(GetLastError()));
#else
            std::vector<std::string> ownedArguments{ executable.string() };
            ownedArguments.insert(ownedArguments.end(), arguments.begin(), arguments.end());
            std::vector<char*> rawArguments;
            for (std::string& argument : ownedArguments)
                rawArguments.push_back(argument.data());
            rawArguments.push_back(nullptr);
            if (posix_spawn(&mProcess, executable.c_str(), nullptr, nullptr, rawArguments.data(), environ) != 0)
                throw std::runtime_error("failed launching diagnostic child");
#endif
        }

        /// Close native process handles after the child has been reaped.
        ~ChildProcess()
        {
#ifdef _WIN32
            if (!mReaped && mProcess.hProcess != nullptr)
            {
                TerminateProcess(mProcess.hProcess, 1);
                WaitForSingleObject(mProcess.hProcess, 5000);
            }
            if (mProcess.hThread != nullptr)
                CloseHandle(mProcess.hThread);
            if (mProcess.hProcess != nullptr)
                CloseHandle(mProcess.hProcess);
#else
            if (!mReaped && mProcess > 0)
            {
                ::kill(mProcess, SIGTERM);
                waitpid(mProcess, nullptr, 0);
            }
#endif
        }

        ChildProcess(const ChildProcess&) = delete;
        ChildProcess& operator=(const ChildProcess&) = delete;

        /// Return a completed child exit code without blocking, or no value while it remains active.
        std::optional<int> tryExitCode()
        {
            if (mExitCode)
                return mExitCode;
#ifdef _WIN32
            const DWORD waitResult = WaitForSingleObject(mProcess.hProcess, 0);
            if (waitResult == WAIT_TIMEOUT)
                return std::nullopt;
            if (waitResult != WAIT_OBJECT_0)
                throw std::runtime_error("failed polling diagnostic child process");
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(mProcess.hProcess, &exitCode))
                throw std::runtime_error("failed reading diagnostic child exit status");
            mExitCode = static_cast<int>(exitCode);
#else
            int status = 0;
            const pid_t result = waitpid(mProcess, &status, WNOHANG);
            if (result == 0)
                return std::nullopt;
            if (result != mProcess)
                throw std::runtime_error("failed polling diagnostic child process");
            mExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
            mReaped = true;
            return mExitCode;
        }

        /// Fail if the child owns a visible desktop window or the foreground window.
        void requireNonDisruptiveWindowState() const
        {
#ifdef _WIN32
            struct WindowState
            {
                DWORD mProcessId = 0;
                bool mVisible = false;
            } state{ mProcess.dwProcessId, false };
            EnumWindows(
                [](HWND window, LPARAM parameter) -> BOOL {
                    auto& state = *reinterpret_cast<WindowState*>(parameter);
                    DWORD owner = 0;
                    GetWindowThreadProcessId(window, &owner);
                    if (owner == state.mProcessId && IsWindowVisible(window))
                    {
                        state.mVisible = true;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&state));
            if (state.mVisible)
                throw std::runtime_error("diagnostic child created a visible desktop window");

            if (const HWND foreground = GetForegroundWindow())
            {
                DWORD owner = 0;
                GetWindowThreadProcessId(foreground, &owner);
                if (owner == mProcess.dwProcessId)
                    throw std::runtime_error("diagnostic child acquired desktop focus");
            }
#endif
        }

        /// Wait for a graceful child exit within the supplied timeout.
        int wait(std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            do
            {
                if (const std::optional<int> exitCode = tryExitCode())
                    return *exitCode;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (std::chrono::steady_clock::now() < deadline);
            throw std::runtime_error("diagnostic child did not exit within its timeout");
        }

    private:
#ifdef _WIN32
        /// Apply the Windows command-line quoting algorithm to one argument.
        static std::wstring quoteWindowsArgument(std::wstring_view argument)
        {
            if (argument.find_first_of(L" \t\"") == std::wstring_view::npos)
                return std::wstring(argument);
            std::wstring result(1, L'"');
            std::size_t backslashes = 0;
            for (const wchar_t character : argument)
            {
                if (character == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (character == L'"')
                    result.append(backslashes * 2 + 1, L'\\');
                else
                    result.append(backslashes, L'\\');
                backslashes = 0;
                result.push_back(character);
            }
            result.append(backslashes * 2, L'\\');
            result.push_back(L'"');
            return result;
        }

        PROCESS_INFORMATION mProcess{};
#else
        pid_t mProcess = -1;
#endif
        bool mReaped = false;
        std::optional<int> mExitCode;
    };

    /// Preserve engine logs and remove an isolated controller workspace when its run completes.
    class TemporaryDirectory
    {
    public:
        /// Create a unique controller-owned directory and remember where run evidence belongs.
        explicit TemporaryDirectory(std::filesystem::path artifacts)
            : mArtifacts(std::move(artifacts))
        {
            std::random_device source;
            mPath = std::filesystem::temp_directory_path()
                / ("openmw-diagnostic-" + std::to_string(source()) + "-"
                    + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(mPath);
        }

        /// Preserve diagnostic engine logs before removing the exact controller-owned directory.
        ~TemporaryDirectory()
        {
            if (!mRemoved)
            {
                preserveEngineLogs();
                std::error_code error;
                std::filesystem::remove_all(mPath, error);
            }
        }

        /// Return the absolute controller-owned workspace path.
        const std::filesystem::path& path() const noexcept { return mPath; }

        /// Preserve logs, remove the controller-owned workspace, and prove cleanup completed.
        void cleanup()
        {
            preserveEngineLogs();
            std::error_code error;
            std::filesystem::remove_all(mPath, error);
            if (error || std::filesystem::exists(mPath))
                throw std::runtime_error("failed removing isolated diagnostic workspace");
            mRemoved = true;
        }

    private:
        /// Copy known engine logs from temporary writable roots without allowing cleanup to throw.
        void preserveEngineLogs() const noexcept
        {
            struct LogLocation
            {
                std::filesystem::path mSource;
                std::filesystem::path mDestination;
            };

            const std::filesystem::path logDirectory = mArtifacts / "logs";
            const std::array locations{
                LogLocation{ mPath / "config" / "openmw.log", logDirectory / "openmw.log" },
                LogLocation{ mPath / "config" / "openmw-crash.log", logDirectory / "openmw-crash.log" },
                LogLocation{ mPath / "userdata" / "openmw.log", logDirectory / "userdata-openmw.log" },
                LogLocation{ mPath / "userdata" / "openmw-crash.log", logDirectory / "userdata-openmw-crash.log" },
                LogLocation{ mPath / "openmw.log", logDirectory / "workspace-openmw.log" },
                LogLocation{ mPath / "openmw-crash.log", logDirectory / "workspace-openmw-crash.log" },
            };
            std::error_code error;
            for (const LogLocation& location : locations)
            {
                if (!std::filesystem::is_regular_file(location.mSource, error))
                {
                    error.clear();
                    continue;
                }
                std::filesystem::create_directories(logDirectory, error);
                if (error)
                    return;
                std::filesystem::copy_file(
                    location.mSource, location.mDestination, std::filesystem::copy_options::overwrite_existing, error);
                error.clear();
            }
        }

        std::filesystem::path mPath;
        std::filesystem::path mArtifacts;
        bool mRemoved = false;
    };

    /// Record content and metadata needed to prove a source profile remained unchanged.
    struct FileFingerprint
    {
        std::uintmax_t mSize = 0;
        std::filesystem::file_time_type mModified;
        std::array<std::uint64_t, 2> mHash{};

        /// Compare every stable source-file identity field.
        bool operator==(const FileFingerprint&) const = default;
    };

    using ProfileFingerprint = std::map<std::filesystem::path, FileFingerprint>;

    /// Fingerprint writable profile state while treating large content trees as read-only roots.
    ProfileFingerprint fingerprintProfile(const std::filesystem::path& root)
    {
        ProfileFingerprint result;
        if (!std::filesystem::exists(root))
            return result;

        const auto recordFile = [&](const std::filesystem::directory_entry& entry) {
            std::ifstream stream(entry.path(), std::ios::binary);
            result.emplace(std::filesystem::relative(entry.path(), root),
                FileFingerprint{
                    entry.file_size(), entry.last_write_time(), Files::getHash(entry.path().string(), stream) });
        };
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(root))
        {
            if (entry.is_regular_file())
            {
                recordFile(entry);
                continue;
            }
            if (!entry.is_directory())
                continue;

            const std::string name = entry.path().filename().string();
            if (name != "userdata")
            {
                result.emplace(std::filesystem::relative(entry.path(), root), FileFingerprint{});
                continue;
            }

            for (const std::filesystem::directory_entry& userEntry : std::filesystem::recursive_directory_iterator(
                     entry.path(), std::filesystem::directory_options::skip_permission_denied))
            {
                const bool insideScreenshots
                    = std::filesystem::relative(userEntry.path(), entry.path()).begin()->string() == "screenshots";
                if (userEntry.is_directory() && insideScreenshots)
                {
                    if (userEntry.path().filename() == "screenshots")
                        result.emplace(std::filesystem::relative(userEntry.path(), root),
                            FileFingerprint{ 0, userEntry.last_write_time(), {} });
                    continue;
                }
                if (userEntry.is_regular_file() && !insideScreenshots)
                    recordFile(userEntry);
            }
        }
        return result;
    }

    /// Describe bounded, path-level evidence when source-profile integrity verification fails.
    std::string describeProfileDifference(const ProfileFingerprint& before, const ProfileFingerprint& after)
    {
        constexpr std::size_t maximumReportedPaths = 8;
        std::vector<std::string> differences;
        auto beforeIt = before.begin();
        auto afterIt = after.begin();
        while ((beforeIt != before.end() || afterIt != after.end()) && differences.size() < maximumReportedPaths)
        {
            if (afterIt == after.end() || (beforeIt != before.end() && beforeIt->first < afterIt->first))
            {
                differences.push_back("removed " + beforeIt->first.string());
                ++beforeIt;
            }
            else if (beforeIt == before.end() || afterIt->first < beforeIt->first)
            {
                differences.push_back("added " + afterIt->first.string());
                ++afterIt;
            }
            else
            {
                if (!(beforeIt->second == afterIt->second))
                    differences.push_back("modified " + beforeIt->first.string());
                ++beforeIt;
                ++afterIt;
            }
        }

        std::string result;
        for (const std::string& difference : differences)
        {
            if (!result.empty())
                result += ", ";
            result += difference;
        }
        return result.empty() ? "unidentified fingerprint difference" : result;
    }

    /// Copy the selected profile's authored configuration into a writable isolated workspace.
    void copyProfileConfiguration(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        const std::filesystem::path sourceDirectory
            = std::filesystem::is_directory(source) ? source : source.parent_path();
        const std::filesystem::path configFile = std::filesystem::is_directory(source) ? source / "openmw.cfg" : source;
        if (!std::filesystem::is_regular_file(configFile))
            throw std::runtime_error("diagnostic source profile does not contain openmw.cfg");

        std::filesystem::create_directories(destination);
        std::filesystem::copy_file(configFile, destination / "openmw.cfg");
        for (const char* optional :
            { "settings.cfg", "shaders.yaml", "input_v3.xml", "global_storage.bin", "player_storage.bin" })
        {
            const std::filesystem::path candidate = sourceDirectory / optional;
            if (std::filesystem::is_regular_file(candidate))
                std::filesystem::copy_file(candidate, destination / optional);
        }
    }

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
        std::chrono::seconds readinessTimeout, ChildProcess* child = nullptr, bool allowVisibleChild = false)
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
            && provider != "gameplay@8")
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
    }

    /// Execute a single command against an already enabled live session.
    int runAttachCommand(std::string_view operation, std::span<char*> arguments)
    {
        const std::string endpoint = findOption(arguments, "--endpoint").value_or(Diagnostics::defaultLocalEndpoint());
        Diagnostics::Command command;
        command.mRequestId = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        if (operation == "status")
            command.mType = Diagnostics::CommandType::InspectSession;
        else if (operation == "graphics")
            command.mType = Diagnostics::CommandType::CaptureGraphics;
        else if (operation == "gameplay")
            command.mType = Diagnostics::CommandType::TraceGameplay;
        else if (operation == "performance")
        {
            command.mType = Diagnostics::CommandType::TracePerformance;
            const std::uint32_t sampleCount
                = static_cast<std::uint32_t>(std::stoul(findOption(arguments, "--performance-samples").value_or("1")));
            if (sampleCount == 0 || sampleCount > 600)
                throw std::invalid_argument("performance samples must be within [1, 600]");
            for (std::size_t byte = 0; byte < sizeof(sampleCount); ++byte)
                command.mPayload.push_back(static_cast<std::byte>((sampleCount >> (byte * 8)) & 0xffu));
        }
        else if (operation == "reload-shaders")
            command.mType = Diagnostics::CommandType::ReloadShaders;
        else if (operation == "shutdown")
            command.mType = Diagnostics::CommandType::ShutdownChild;
        else if (operation == "capsule")
        {
            command.mType = Diagnostics::CommandType::CaptureCapsule;
            command.mPayload = textPayload(findOption(arguments, "--output").value_or("capture.omwdiag"));
        }
        else if (operation == "step")
        {
            command.mType = Diagnostics::CommandType::StepFrames;
            const std::uint32_t count
                = static_cast<std::uint32_t>(std::stoul(findOption(arguments, "--frames").value_or("1")));
            for (std::size_t byte = 0; byte < sizeof(count); ++byte)
                command.mPayload.push_back(static_cast<std::byte>((count >> (byte * 8)) & 0xffu));
        }
        else
            throw std::invalid_argument("unknown diagnostic operation");
        const std::chrono::milliseconds timeout
            = operation == "performance" ? std::chrono::seconds(60) : std::chrono::seconds(10);
        return printResponse(Diagnostics::transact(endpoint, command, timeout));
    }

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
            std::size_t completedFrames = 0;
            unsigned segmentIndex = 0;
            while (completedFrames < routePlan.mFrameCount)
            {
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
        const Diagnostics::Response finalGameplay
            = Diagnostics::transact(endpoint, { requestId++, Diagnostics::CommandType::TraceGameplay, {} });
        requireSuccessfulResponse(finalGameplay);
        std::ofstream gameplayFile(artifacts / "gameplay.yaml", std::ios::binary | std::ios::trunc);
        gameplayFile.write(reinterpret_cast<const char*>(finalGameplay.mPayload.data()),
            static_cast<std::streamsize>(finalGameplay.mPayload.size()));
        if (!gameplayFile)
            throw std::runtime_error("failed writing final gameplay evidence");
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

    /// Inspect a capsule without extracting or trusting its attachment names as paths.
    int inspectCapsule(std::span<char*> arguments)
    {
        const std::filesystem::path path = findOption(arguments, "--capsule").value_or(std::string());
        if (path.empty())
            throw std::invalid_argument("inspect-capsule requires --capsule");
        const Diagnostics::Capsule capsule = Diagnostics::readCapsule(path);
        if (const std::optional<std::string> requestedEntry = findOption(arguments, "--entry"))
        {
            const Diagnostics::CapsuleEntry* entry = capsule.find(*requestedEntry);
            if (entry == nullptr)
                throw std::invalid_argument("requested capsule entry does not exist");
            if (entry->mType == Diagnostics::CapsuleEntryType::WorldSnapshot)
                throw std::invalid_argument("binary world snapshots cannot be printed");
            std::cout.write(
                reinterpret_cast<const char*>(entry->mData.data()), static_cast<std::streamsize>(entry->mData.size()));
            if (!std::cout)
                throw std::runtime_error("failed writing capsule entry");
            return 0;
        }
        std::cout << "capsule-version: " << Diagnostics::capsuleVersion << "\nentries:\n";
        for (const Diagnostics::CapsuleEntry& entry : capsule.entries())
            std::cout << "  - " << entry.mName << " (" << entry.mData.size() << " bytes)\n";
        return 0;
    }

    /// Attach a bounded, validated route to a new capsule without launching an engine or changing the source.
    int addRoute(std::span<char*> arguments)
    {
        const std::filesystem::path source = findOption(arguments, "--capsule").value_or(std::string());
        const std::filesystem::path route = findOption(arguments, "--route").value_or(std::string());
        const std::filesystem::path output = findOption(arguments, "--output").value_or(std::string());
        if (source.empty() || route.empty() || output.empty())
            throw std::invalid_argument("add-route requires --capsule, --route, and --output");
        std::ifstream input(route, std::ios::binary | std::ios::ate);
        if (!input)
            throw std::runtime_error("failed opening gameplay route");
        const std::streamoff size = input.tellg();
        if (size <= 0 || static_cast<std::uint64_t>(size) > Diagnostics::maximumCapsuleEntrySize)
            throw std::length_error("gameplay route file size is outside capsule entry bounds");
        std::vector<std::byte> payload(static_cast<std::size_t>(size));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!input)
            throw std::runtime_error("failed reading gameplay route");
        Diagnostics::writeCapsuleAtomically(
            output, Diagnostics::withGameplayRoute(Diagnostics::readCapsule(source), payload));
        std::cout << "route-capsule: " << output << '\n';
        return 0;
    }

    /// Parse and execute the bounded diagnostic controller command surface.
    int runController(int argc, char** argv)
    {
        if (argc < 2)
        {
            printUsage(std::cerr);
            return 2;
        }
        const std::string_view operation = argv[1];
        if (operation == "help" || operation == "--help" || operation == "-h")
        {
            printUsage(std::cout);
            return 0;
        }
        const std::span arguments(argv + 2, static_cast<std::size_t>(argc - 2));
        if (operation == "run" || operation == "replay")
            return runIsolated(arguments);
        if (operation == "inspect-capsule")
            return inspectCapsule(arguments);
        if (operation == "add-route")
            return addRoute(arguments);
        return runAttachCommand(operation, arguments);
    }
}

/// Run the diagnostic controller with standard OpenMW exception reporting.
int main(int argc, char** argv)
{
    try
    {
        return runController(argc, argv);
    }
    catch (const std::exception& error)
    {
        std::cerr << "openmw-diagnosticctl: " << error.what() << '\n';
        return 1;
    }
}
