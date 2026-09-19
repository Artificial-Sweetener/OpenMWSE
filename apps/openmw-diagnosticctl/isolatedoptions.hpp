#ifndef OPENMW_DIAGNOSTICCTL_ISOLATEDOPTIONS_H
#define OPENMW_DIAGNOSTICCTL_ISOLATEDOPTIONS_H

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace DiagnosticCtl
{
    /// Parse the bounded time allowed for a large isolated profile to finish loading.
    std::chrono::seconds parseStartupTimeout(const std::optional<std::string>& value);
    /// Select exactly one isolated world bootstrap without entering the interactive menu.
    void addWorldStartArguments(
        std::vector<std::string>& arguments, const std::optional<std::filesystem::path>& save, bool newGame);

    /// Forward explicitly requested presentation state to an isolated diagnostic child.
    void addDiagnosticPresentationArguments(std::vector<std::string>& arguments, bool headed, bool statsOverlay);
}

#endif
