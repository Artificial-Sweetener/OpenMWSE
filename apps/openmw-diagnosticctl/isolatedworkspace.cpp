#include "isolatedworkspace.hpp"

#include <fstream>
#include <random>
#include <stdexcept>
#include <system_error>
#include <thread>

#include <components/files/hash.hpp>

#ifdef _WIN32
#include <components/misc/windows.hpp>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

struct DiagnosticCtl::ChildProcess::Impl
{
#ifdef _WIN32
    PROCESS_INFORMATION mProcess{};
#else
    pid_t mProcess = -1;
#endif
    bool mReaped = false;
    std::optional<int> mExitCode;
};

#ifdef _WIN32
namespace
{
    /// Apply the Windows command-line quoting algorithm to one argument.
    std::wstring quoteWindowsArgument(std::wstring_view argument)
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
}
#endif

DiagnosticCtl::ChildProcess::ChildProcess(
    const std::filesystem::path& executable, const std::vector<std::string>& arguments)
    : mImpl(std::make_unique<Impl>())
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
            &mImpl->mProcess))
        throw std::runtime_error("failed launching diagnostic child: " + std::to_string(GetLastError()));
#else
    std::vector<std::string> ownedArguments{ executable.string() };
    ownedArguments.insert(ownedArguments.end(), arguments.begin(), arguments.end());
    std::vector<char*> rawArguments;
    for (std::string& argument : ownedArguments)
        rawArguments.push_back(argument.data());
    rawArguments.push_back(nullptr);
    if (posix_spawn(&mImpl->mProcess, executable.c_str(), nullptr, nullptr, rawArguments.data(), environ) != 0)
        throw std::runtime_error("failed launching diagnostic child");
#endif
}

DiagnosticCtl::ChildProcess::~ChildProcess()
{
#ifdef _WIN32
    if (!mImpl->mReaped && mImpl->mProcess.hProcess != nullptr)
    {
        TerminateProcess(mImpl->mProcess.hProcess, 1);
        WaitForSingleObject(mImpl->mProcess.hProcess, 5000);
    }
    if (mImpl->mProcess.hThread != nullptr)
        CloseHandle(mImpl->mProcess.hThread);
    if (mImpl->mProcess.hProcess != nullptr)
        CloseHandle(mImpl->mProcess.hProcess);
#else
    if (!mImpl->mReaped && mImpl->mProcess > 0)
    {
        ::kill(mImpl->mProcess, SIGTERM);
        waitpid(mImpl->mProcess, nullptr, 0);
    }
#endif
}

std::optional<int> DiagnosticCtl::ChildProcess::tryExitCode()
{
    if (mImpl->mExitCode)
        return mImpl->mExitCode;
#ifdef _WIN32
    const DWORD waitResult = WaitForSingleObject(mImpl->mProcess.hProcess, 0);
    if (waitResult == WAIT_TIMEOUT)
        return std::nullopt;
    if (waitResult != WAIT_OBJECT_0)
        throw std::runtime_error("failed polling diagnostic child process");
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(mImpl->mProcess.hProcess, &exitCode))
        throw std::runtime_error("failed reading diagnostic child exit status");
    mImpl->mExitCode = static_cast<int>(exitCode);
#else
    int status = 0;
    const pid_t result = waitpid(mImpl->mProcess, &status, WNOHANG);
    if (result == 0)
        return std::nullopt;
    if (result != mImpl->mProcess)
        throw std::runtime_error("failed polling diagnostic child process");
    mImpl->mExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
    mImpl->mReaped = true;
    return mImpl->mExitCode;
}

void DiagnosticCtl::ChildProcess::requireNonDisruptiveWindowState() const
{
#ifdef _WIN32
    struct WindowState
    {
        DWORD mProcessId = 0;
        bool mVisible = false;
    } state{ mImpl->mProcess.dwProcessId, false };
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
        if (owner == mImpl->mProcess.dwProcessId)
            throw std::runtime_error("diagnostic child acquired desktop focus");
    }
#endif
}

int DiagnosticCtl::ChildProcess::wait(std::chrono::milliseconds timeout)
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

DiagnosticCtl::TemporaryDirectory::TemporaryDirectory(std::filesystem::path artifacts)
    : mArtifacts(std::move(artifacts))
{
    std::random_device source;
    mPath = std::filesystem::temp_directory_path()
        / ("openmw-diagnostic-" + std::to_string(source()) + "-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(mPath);
}

DiagnosticCtl::TemporaryDirectory::~TemporaryDirectory()
{
    if (!mRemoved)
    {
        preserveEngineLogs();
        std::error_code error;
        std::filesystem::remove_all(mPath, error);
    }
}

const std::filesystem::path& DiagnosticCtl::TemporaryDirectory::path() const noexcept
{
    return mPath;
}

void DiagnosticCtl::TemporaryDirectory::cleanup()
{
    preserveEngineLogs();
    std::error_code error;
    std::filesystem::remove_all(mPath, error);
    if (error || std::filesystem::exists(mPath))
        throw std::runtime_error("failed removing isolated diagnostic workspace");
    mRemoved = true;
}

void DiagnosticCtl::TemporaryDirectory::preserveEngineLogs() const noexcept
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

DiagnosticCtl::ProfileFingerprint DiagnosticCtl::fingerprintProfile(const std::filesystem::path& root)
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

std::string DiagnosticCtl::describeProfileDifference(const ProfileFingerprint& before, const ProfileFingerprint& after)
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

void DiagnosticCtl::copyProfileConfiguration(
    const std::filesystem::path& source, const std::filesystem::path& destination)
{
    const std::filesystem::path sourceDirectory = std::filesystem::is_directory(source) ? source : source.parent_path();
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
