#include "gameplayrouteexecution.hpp"

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <string>

namespace
{
    /// Parse one positive bounded decimal option without accepting partial values.
    std::size_t parseBoundedCount(std::string_view value, std::size_t maximum, std::string_view name)
    {
        std::size_t result = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc() || end != value.data() + value.size() || result == 0 || result > maximum)
        {
            throw std::invalid_argument(std::string(name) + " must be within [1, " + std::to_string(maximum) + "]");
        }
        return result;
    }
}

namespace DiagnosticCtl
{
    namespace
    {
        /// Extract one named route report through the following performance provider boundary.
        std::string extractRouteReport(std::string_view response, std::string_view reportMarker)
        {
            constexpr std::string_view performanceMarker = "\nprovider: performance@";
            const std::size_t reportBegin = response.find(reportMarker);
            const std::size_t reportEnd = response.find(performanceMarker, reportBegin);
            if (reportBegin == std::string_view::npos || reportEnd == std::string_view::npos)
                throw std::runtime_error(
                    "engine omitted " + std::string(reportMarker.substr(0, reportMarker.size() - 2)) + " evidence");
            return std::string(response.substr(reportBegin, reportEnd - reportBegin));
        }
    }

    std::string extractGameplayRouteClearanceReport(std::string_view response)
    {
        constexpr std::string_view reportMarker = "route-clearance:\n";
        return extractRouteReport(response, reportMarker);
    }

    std::string extractGameplayRouteTraversalReport(std::string_view response)
    {
        constexpr std::string_view reportMarker = "route-traversal:\n";
        return extractRouteReport(response, reportMarker);
    }

    void validateGameplayRouteProgress(
        std::size_t remainingBefore, std::size_t scheduledFrames, std::size_t remainingAfter)
    {
        if (scheduledFrames > remainingBefore || remainingAfter != remainingBefore - scheduledFrames)
            throw std::runtime_error("gameplay route did not consume its scheduled states; inspect the engine log");
    }

    GameplayRouteExecutionPlan makeGameplayRouteExecutionPlan(std::size_t availableFrameCount,
        std::optional<std::string_view> requestedFrameCount, std::optional<std::string_view> requestedSegmentFrameCount)
    {
        if (availableFrameCount == 0)
            throw std::invalid_argument("gameplay route must contain at least one frame");

        const std::size_t frameCount = requestedFrameCount
            ? parseBoundedCount(*requestedFrameCount, availableFrameCount, "route frame count")
            : availableFrameCount;
        constexpr std::size_t maximumPerformanceSamples = 600;
        const std::size_t maximumSegmentFrameCount = std::min(frameCount, maximumPerformanceSamples);
        const std::size_t segmentFrameCount = requestedSegmentFrameCount
            ? parseBoundedCount(*requestedSegmentFrameCount, maximumSegmentFrameCount, "route performance sample count")
            : maximumSegmentFrameCount;
        return { frameCount, static_cast<std::uint32_t>(segmentFrameCount) };
    }
}
