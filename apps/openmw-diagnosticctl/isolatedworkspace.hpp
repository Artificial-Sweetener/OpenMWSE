#ifndef OPENMW_DIAGNOSTICCTL_ISOLATEDWORKSPACE_H
#define OPENMW_DIAGNOSTICCTL_ISOLATEDWORKSPACE_H

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace DiagnosticCtl
{
    /// Owns one native diagnostic child launched without a command shell.
    class ChildProcess
    {
    public:
        /// Launch the executable with explicit arguments and no inherited interactive window.
        ChildProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments);
        /// Terminate an unreaped child and release native process resources.
        ~ChildProcess();

        ChildProcess(const ChildProcess&) = delete;
        ChildProcess& operator=(const ChildProcess&) = delete;

        /// Return a completed child exit code without blocking, or no value while it remains active.
        std::optional<int> tryExitCode();
        /// Fail if the child owns a visible desktop window or the foreground window.
        void requireNonDisruptiveWindowState() const;
        /// Wait for a graceful child exit within the supplied timeout.
        int wait(std::chrono::milliseconds timeout);

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };

    /// Owns and securely cleans one temporary isolated-controller workspace.
    class TemporaryDirectory
    {
    public:
        /// Create a unique controller-owned directory and remember where run evidence belongs.
        explicit TemporaryDirectory(std::filesystem::path artifacts);
        /// Preserve diagnostic engine logs before removing the exact controller-owned directory.
        ~TemporaryDirectory();

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

        /// Return the absolute controller-owned workspace path.
        const std::filesystem::path& path() const noexcept;
        /// Preserve logs, remove the controller-owned workspace, and prove cleanup completed.
        void cleanup();

    private:
        /// Copy known engine logs from temporary writable roots without allowing cleanup to throw.
        void preserveEngineLogs() const noexcept;

        std::filesystem::path mPath;
        std::filesystem::path mArtifacts;
        bool mRemoved = false;
    };

    /// Records stable source-file identity used to prove profile isolation.
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
    ProfileFingerprint fingerprintProfile(const std::filesystem::path& root);
    /// Describe bounded, path-level evidence when source-profile integrity verification fails.
    std::string describeProfileDifference(const ProfileFingerprint& before, const ProfileFingerprint& after);
    /// Copy the selected profile's authored configuration into a writable isolated workspace.
    void copyProfileConfiguration(const std::filesystem::path& source, const std::filesystem::path& destination);
}

#endif
