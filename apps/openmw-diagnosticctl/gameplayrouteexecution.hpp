#ifndef OPENMW_APPS_OPENMW_DIAGNOSTICCTL_GAMEPLAYROUTEEXECUTION_H
#define OPENMW_APPS_OPENMW_DIAGNOSTICCTL_GAMEPLAYROUTEEXECUTION_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace DiagnosticCtl
{
    /// Defines one bounded route replay and its performance checkpoint cadence.
    struct GameplayRouteExecutionPlan
    {
        std::size_t mFrameCount;
        std::uint32_t mSegmentFrameCount;
    };

    /// Resolve optional route limits while enforcing the diagnostic protocol's bounded sample window.
    GameplayRouteExecutionPlan makeGameplayRouteExecutionPlan(std::size_t availableFrameCount,
        std::optional<std::string_view> requestedFrameCount,
        std::optional<std::string_view> requestedSegmentFrameCount);
}

#endif
