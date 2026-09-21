#ifndef OPENMW_COMPONENTS_SETTINGS_WINDOWMODE_H
#define OPENMW_COMPONENTS_SETTINGS_WINDOWMODE_H

#include <algorithm>
#include <utility>

namespace Settings
{
    /// Selects exclusive fullscreen, borderless windowed, or bordered windowed presentation.
    enum class WindowMode
    {
        Fullscreen = 0,
        WindowedFullscreen = 1,
        Windowed = 2,
    };

    /// Clamp borderless window dimensions to the selected display while preserving other modes verbatim.
    constexpr std::pair<int, int> clampWindowSize(
        WindowMode mode, int width, int height, int displayWidth, int displayHeight)
    {
        if (mode != WindowMode::WindowedFullscreen)
            return { width, height };
        return { std::min(width, displayWidth), std::min(height, displayHeight) };
    }
}

#endif
