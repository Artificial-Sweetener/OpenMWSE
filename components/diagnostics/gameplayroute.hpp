#ifndef OPENMW_COMPONENTS_DIAGNOSTICS_GAMEPLAYROUTE_HPP
#define OPENMW_COMPONENTS_DIAGNOSTICS_GAMEPLAYROUTE_HPP

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace Diagnostics
{
    inline constexpr unsigned gameplayRouteVersion = 1;
    inline constexpr std::size_t maximumGameplayRouteFrames = 10000;
    inline constexpr double maximumGameplayRouteFrameDuration = 0.2;

    /// Captures one rendered gameplay frame's authoritative player and camera state.
    struct GameplayRouteFrame
    {
        double mDuration = 0.0;
        std::string mCell;
        std::array<float, 3> mPosition{};
        std::array<float, 3> mRotation{};
        std::array<float, 3> mCameraAngles{};
        bool mFirstPerson = false;

        /// Compare every state field used to reproduce a gameplay viewpoint.
        bool operator==(const GameplayRouteFrame&) const = default;
    };

    /// Encode a bounded route using the versioned portable diagnostic representation.
    std::vector<std::byte> encodeGameplayRoute(std::span<const GameplayRouteFrame> frames);

    /// Decode and validate a versioned portable diagnostic route.
    std::vector<GameplayRouteFrame> decodeGameplayRoute(std::span<const std::byte> payload);
}

#endif
