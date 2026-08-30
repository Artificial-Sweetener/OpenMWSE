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
