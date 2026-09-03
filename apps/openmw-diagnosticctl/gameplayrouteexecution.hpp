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

    /// Reject a rendered segment that did not consume exactly its scheduled route states.
    void validateGameplayRouteProgress(
        std::size_t remainingBefore, std::size_t scheduledFrames, std::size_t remainingAfter);
}

#endif
