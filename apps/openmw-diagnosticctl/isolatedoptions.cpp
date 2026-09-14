#include "isolatedoptions.hpp"

#include <cstdint>
#include <stdexcept>

namespace DiagnosticCtl
{
    /// Parse the bounded time allowed for a large isolated profile to finish loading.
    std::chrono::seconds parseStartupTimeout(const std::optional<std::string>& value)
    {
        constexpr std::uint64_t defaultSeconds = 180;
        constexpr std::uint64_t maximumSeconds = 3600;
        if (!value)
            return std::chrono::seconds(defaultSeconds);

        std::size_t consumed = 0;
        std::uint64_t seconds = 0;
        try
        {
            seconds = std::stoull(*value, &consumed);
        }
        catch (const std::exception&)
        {
            throw std::invalid_argument("--startup-timeout must be within [1, 3600] seconds");
        }
        if (consumed != value->size() || seconds == 0 || seconds > maximumSeconds)
            throw std::invalid_argument("--startup-timeout must be within [1, 3600] seconds");
        return std::chrono::seconds(seconds);
    }

    /// Select exactly one isolated world bootstrap without entering the interactive menu.
    void addWorldStartArguments(
        std::vector<std::string>& arguments, const std::optional<std::filesystem::path>& save, bool newGame)
    {
        if (save && newGame)
            throw std::invalid_argument("--new-game conflicts with --save and --capsule");
        if (save)
            arguments.insert(arguments.end(), { "--load-savegame", save->string(), "--skip-menu" });
        else if (newGame)
            arguments.insert(arguments.end(), { "--skip-menu", "--new-game" });
    }
}
