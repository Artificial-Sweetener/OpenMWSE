#include <components/debug/debugging.hpp>
#include <components/diagnostics/capsule.hpp>
#include <components/diagnostics/localtransport.hpp>
#include <components/diagnostics/session.hpp>
#include <components/files/hash.hpp>

#include "isolatedsettings.hpp"

#include <yaml-cpp/yaml.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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
#ifdef _WIN32
            const auto count
                = static_cast<DWORD>(std::min<std::int64_t>(timeout.count(), std::numeric_limits<DWORD>::max()));
            if (WaitForSingleObject(mProcess.hProcess, count) != WAIT_OBJECT_0)
                throw std::runtime_error("diagnostic child did not exit within its timeout");
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(mProcess.hProcess, &exitCode))
                throw std::runtime_error("failed reading diagnostic child exit status");
            mReaped = true;
            return static_cast<int>(exitCode);
#else
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            int status = 0;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (waitpid(mProcess, &status, WNOHANG) == mProcess)
                {
                    mReaped = true;
                    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            throw std::runtime_error("diagnostic child did not exit within its timeout");
#endif
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
                result.emplace(
                    std::filesystem::relative(entry.path(), root), FileFingerprint{ 0, entry.last_write_time(), {} });
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

    /// Generate a unique protected endpoint within the transport's reserved namespace.
    std::string uniqueEndpoint()
    {
        std::random_device source;
        return Diagnostics::defaultLocalEndpoint() + "-" + std::to_string(source());
    }

    /// Send a command after the child or live session has reached its safe-point loop.
    Diagnostics::Response transactWhenReady(const std::string& endpoint, const Diagnostics::Command& command,
        std::chrono::seconds readinessTimeout, const ChildProcess* child = nullptr)
    {
        const auto deadline = std::chrono::steady_clock::now() + readinessTimeout;
        std::exception_ptr lastFailure;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (child)
                child->requireNonDisruptiveWindowState();
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
    void validateIsolatedSession(const Diagnostics::Response& response)
    {
        const YAML::Node status = YAML::Load(payloadText(response.mPayload));
        if (status["mode"].as<std::string>("") != "isolated-child"
            || status["graphics-context"].as<std::string>("") != "offscreen-pbuffer"
            || status["native-window"].as<std::string>("") != "absent"
            || status["input-source"].as<std::string>("") != "device-free")
            throw std::runtime_error("diagnostic child did not attest to the isolated headless runtime");
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
                  "  openmw-diagnosticctl <status|graphics|gameplay|performance|reload-shaders> [--endpoint name]\n"
                  "  openmw-diagnosticctl capsule [--endpoint name] [--output capture.omwdiag]\n"
                  "  openmw-diagnosticctl step [--endpoint name] [--frames count]\n"
                  "  openmw-diagnosticctl shutdown [--endpoint name]\n"
                  "  openmw-diagnosticctl <run|replay> --openmw openmw.exe --config openmw.cfg\n"
                  "      --artifacts directory [--save save.omwsave | --capsule capture.omwdiag]\n"
                  "      [--frames count] [--performance-samples count] [--fixed-dt seconds]\n"
                  "      [--setting \"Section:name=value\"]...\n"
                  "  openmw-diagnosticctl inspect-capsule --capsule capture.omwdiag [--entry name]\n";
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

    /// Compare the replay's observable initial world state with its capsule using stable domain fields.
    void validateReplayState(std::span<const std::byte> expectedBytes, std::span<const std::byte> actualBytes)
    {
        const YAML::Node expected = YAML::Load(payloadText(expectedBytes));
        const YAML::Node actual = YAML::Load(payloadText(actualBytes));
        for (const char* key : { "cell", "weather" })
        {
            if (expected[key].as<std::string>() != actual[key].as<std::string>())
                throw std::runtime_error(std::string("replay state mismatch for ") + key);
        }
        const YAML::Node expectedCamera = expected["camera"];
        const YAML::Node actualCamera = actual["camera"];
        if (!expectedCamera.IsSequence() || !actualCamera.IsSequence() || expectedCamera.size() != 3
            || actualCamera.size() != 3)
            throw std::runtime_error("replay state contains invalid camera coordinates");
        for (std::size_t index = 0; index < 3; ++index)
        {
            if (std::abs(expectedCamera[index].as<double>() - actualCamera[index].as<double>()) > 0.01)
                throw std::runtime_error("replay camera position does not match its capsule");
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
            command.mType = Diagnostics::CommandType::TracePerformance;
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
        return printResponse(Diagnostics::transact(endpoint, command));
    }

    /// Run the real engine invisibly against copied state, collect evidence, and shut it down.
    int runIsolated(std::span<char*> arguments)
    {
        const std::filesystem::path executableOption = findOption(arguments, "--openmw").value_or("openmw.exe");
        const std::filesystem::path sourceConfigOption = findOption(arguments, "--config").value_or(std::string());
        const std::filesystem::path artifactsOption = findOption(arguments, "--artifacts").value_or(std::string());
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
        std::optional<std::filesystem::path> isolatedSave;
        std::vector<std::byte> replayManifest;
        std::vector<std::byte> expectedWorldState;
        if (const auto capsulePath = findOption(arguments, "--capsule"))
        {
            const Diagnostics::Capsule capsule = Diagnostics::readCapsule(*capsulePath);
            const Diagnostics::CapsuleEntry* manifest = capsule.find("manifest.yaml");
            const Diagnostics::CapsuleEntry* snapshot = capsule.find("state/world.omwsave");
            const Diagnostics::CapsuleEntry* worldState = capsule.find("state/world.yaml");
            if (manifest == nullptr || manifest->mType != Diagnostics::CapsuleEntryType::Manifest || snapshot == nullptr
                || snapshot->mType != Diagnostics::CapsuleEntryType::WorldSnapshot || worldState == nullptr)
                throw std::runtime_error("diagnostic capsule does not contain its replay state");
            replayManifest = manifest->mData;
            expectedWorldState = worldState->mData;
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

        ChildProcess child(executable, childArguments);
        Diagnostics::Command hello{ 1, Diagnostics::CommandType::Hello, std::move(replayManifest) };
        const Diagnostics::Response ready = transactWhenReady(endpoint, hello, std::chrono::seconds(90), &child);
        if (printResponse(ready) != 0)
            return 2;
        validateIsolatedSession(ready);
        child.requireNonDisruptiveWindowState();
        std::cout << "desktop-window: absent\n"
                     "desktop-focus-acquired: false\n";

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
        validateReplayState(expectedWorldState, observed.mPayload);
        std::cout << "initial-state-parity: true\n";

        const std::uint32_t frames
            = static_cast<std::uint32_t>(std::stoul(findOption(arguments, "--frames").value_or("16")));
        const std::uint32_t performanceSamples = static_cast<std::uint32_t>(
            std::stoul(findOption(arguments, "--performance-samples").value_or(std::to_string(std::min(frames, 16u)))));
        if (performanceSamples == 0 || performanceSamples > frames || performanceSamples > 600)
            throw std::invalid_argument("performance samples must be within [1, min(frames, 600)]");
        Diagnostics::Command step{ requestId++, Diagnostics::CommandType::StepFrames, {} };
        for (std::size_t byte = 0; byte < sizeof(frames); ++byte)
            step.mPayload.push_back(static_cast<std::byte>((frames >> (byte * 8)) & 0xffu));
        requireSuccessfulResponse(Diagnostics::transact(endpoint, step));
        waitForScheduledFrames(endpoint, requestId, frames);
        std::cout << "executed-fixed-frames: " << frames << '\n';

        Diagnostics::Command performanceCommand{ requestId++, Diagnostics::CommandType::TracePerformance, {} };
        for (std::size_t byte = 0; byte < sizeof(performanceSamples); ++byte)
            performanceCommand.mPayload.push_back(static_cast<std::byte>((performanceSamples >> (byte * 8)) & 0xffu));
        const Diagnostics::Response performance = Diagnostics::transact(endpoint, performanceCommand);
        if (performance.mError != Diagnostics::ErrorCode::None)
            return printResponse(performance);
        std::ofstream performanceFile(artifacts / "performance.txt", std::ios::binary | std::ios::trunc);
        performanceFile.write(reinterpret_cast<const char*>(performance.mPayload.data()),
            static_cast<std::streamsize>(performance.mPayload.size()));
        if (!performanceFile)
            throw std::runtime_error("failed writing performance evidence");

        requireSuccessfulResponse(
            Diagnostics::transact(endpoint, { requestId++, Diagnostics::CommandType::CaptureGraphics, {} }));
        waitForGraphicsCapture(endpoint, requestId);
        child.requireNonDisruptiveWindowState();
        requireSuccessfulResponse(
            Diagnostics::transact(endpoint, { requestId++, Diagnostics::CommandType::ShutdownChild, {} }));

        const int exitCode = child.wait(std::chrono::seconds(30));
        if (fingerprintProfile(sourceDirectory) != before)
            throw std::runtime_error("source profile changed during isolated diagnostic run");
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
