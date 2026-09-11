#ifndef OPENMW_COMPONENTS_SETTINGS_DISTANCE_H
#define OPENMW_COMPONENTS_SETTINGS_DISTANCE_H

#include <components/misc/constants.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace Settings
{
    /// Select the presentation of distances stored in Morrowind world units.
    enum class DistanceUnits
    {
        WorldUnits,
        Cells,
    };

    /// Prefer compact text that round-trips to the canonical float, retaining double precision when necessary.
    inline std::string formatDistance(float worldUnits, DistanceUnits units)
    {
        const double value
            = units == DistanceUnits::Cells ? double(worldUnits) / Constants::CellSizeInUnits : double(worldUnits);
        std::array<char, 64> buffer;
        auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), static_cast<float>(value));
        double parsed = 0;
        const auto conversion = std::from_chars(buffer.data(), result.ptr, parsed);
        const double canonical = units == DistanceUnits::Cells ? parsed * Constants::CellSizeInUnits : parsed;
        if (conversion.ec == std::errc{} && std::isfinite(canonical)
            && std::abs(canonical) <= std::numeric_limits<float>::max() && static_cast<float>(canonical) == worldUnits)
            return std::string(buffer.data(), result.ptr);
        result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        return std::string(buffer.data(), result.ptr);
    }

    /// Step by one world unit or a half cell, preserving finite storage and the setting's lower boundary.
    inline float stepDistance(float worldUnits, DistanceUnits units, bool increase, bool allowZero)
    {
        const double step = units == DistanceUnits::Cells ? Constants::CellSizeInUnits / 2.0 : 1.0;
        const double minimum = allowZero ? 0.0 : std::min(double(worldUnits), 1.0);
        const double candidate = std::max(minimum, double(worldUnits) + (increase ? step : -step));
        if (candidate > std::numeric_limits<float>::max())
            return worldUnits;
        float result = static_cast<float>(candidate);
        if (result == worldUnits && (increase || worldUnits > minimum))
            result = std::nextafter(worldUnits, increase ? std::numeric_limits<float>::infinity() : 0.f);
        return std::isfinite(result) ? result : worldUnits;
    }

    /// Parse a non-negative finite distance, rejecting overflow and positive values that underflow to zero.
    inline std::optional<float> parseDistance(std::string_view text, DistanceUnits units)
    {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
            return std::nullopt;
        text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        double value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !std::isfinite(value) || value < 0)
            return std::nullopt;
        if (units == DistanceUnits::Cells)
            value *= Constants::CellSizeInUnits;
        if (value > std::numeric_limits<float>::max())
            return std::nullopt;
        const float distance = static_cast<float>(value);
        if (value > 0 && distance == 0)
            return std::nullopt;
        return distance;
    }
}

#endif
