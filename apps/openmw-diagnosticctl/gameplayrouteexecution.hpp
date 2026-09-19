#ifndef OPENMW_APPS_OPENMW_DIAGNOSTICCTL_GAMEPLAYROUTEEXECUTION_H
#define OPENMW_APPS_OPENMW_DIAGNOSTICCTL_GAMEPLAYROUTEEXECUTION_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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

    /// Resolve the first route state whose containing segment should retain rendered-frame evidence.
    std::size_t resolveGameplayRouteCaptureStart(
        std::optional<std::string_view> requestedStartFrame, std::size_t executedFrameCount);

    /// Return whether one scheduled segment intersects the requested retained-capture interval.
    bool shouldCaptureGameplayRouteSegment(
        std::size_t completedFrames, std::size_t scheduledFrames, std::size_t captureStartFrame);

    /// Reject a rendered segment that did not consume exactly its scheduled route states.
    void validateGameplayRouteProgress(
        std::size_t remainingBefore, std::size_t scheduledFrames, std::size_t remainingAfter);

    /// Extract the route-clearance YAML document from a combined route performance response.
    std::string extractGameplayRouteClearanceReport(std::string_view response);

    /// Extract the route-traversal YAML document from a combined route performance response.
    std::string extractGameplayRouteTraversalReport(std::string_view response);
}

#endif
