#ifndef OPENMW_COMPONENTS_SETTINGS_SHADOWRESOLUTION_H
#define OPENMW_COMPONENTS_SETTINGS_SHADOWRESOLUTION_H

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <string_view>

namespace Settings
{
    /// Bound square shadow textures by hardware support and OSG's signed-short texture dimensions.
    inline int shadowResolutionLimit(int hardwareLimit)
    {
        return std::clamp(hardwareLimit, 0, int(std::numeric_limits<short>::max()));
    }

    /// Parse a positive whole-pixel dimension without rounding or silently changing unsupported values.
    inline std::optional<int> parseShadowResolution(std::string_view text, int maximum)
    {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
            return std::nullopt;
        text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        int value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < 1
            || value > shadowResolutionLimit(maximum))
            return std::nullopt;
        return value;
    }

    /// Move by 256 pixels per side, stopping at the supported endpoints without integer overflow.
    inline int stepShadowResolution(int value, bool increase, int maximum)
    {
        const int limit = shadowResolutionLimit(maximum);
        if (limit == 0)
            return value;
        return static_cast<int>(
            std::clamp(static_cast<long long>(value) + (increase ? 256 : -256), 1LL, static_cast<long long>(limit)));
    }
}

#endif
