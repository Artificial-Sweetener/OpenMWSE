#include <components/diagnostics/capsule.hpp>
#include <components/diagnostics/gameplayroute.hpp>

#include <gtest/gtest.h>

#include <limits>

namespace
{
    /// Create a representative route frame for codec tests.
    Diagnostics::GameplayRouteFrame frame()
    {
        return { .mDuration = 1.0 / 60.0,
            .mCell = "sys::default::exteriorcell:0,0",
            .mPosition = { 1.f, 2.f, 3.f },
            .mRotation = { 0.1f, 0.2f, 0.3f },
            .mCameraAngles = { 0.4f, 0.5f, 0.6f },
            .mFirstPerson = true };
    }

    TEST(GameplayRoute, RoundTripsEveryReplayField)
    {
        const std::vector source{ frame(), frame() };
        EXPECT_EQ(Diagnostics::decodeGameplayRoute(Diagnostics::encodeGameplayRoute(source)), source);
    }

    TEST(GameplayRoute, AttachesValidatedRouteWithoutChangingSourceCapsule)
    {
        Diagnostics::Capsule source;
        source.add({ "state/save.omwsave", Diagnostics::CapsuleEntryType::WorldSnapshot, { std::byte{ 1 } } });
        const std::vector frames{ frame(), frame() };
        const auto payload = Diagnostics::encodeGameplayRoute(frames);
        const auto result = Diagnostics::withGameplayRoute(source, payload);
        ASSERT_EQ(source.entries().size(), 1u);
        ASSERT_EQ(result.entries().size(), 2u);
        EXPECT_EQ(result.entries()[0], source.entries()[0]);
        const auto* route = result.find("state/gameplay-route.yaml");
        ASSERT_NE(route, nullptr);
        EXPECT_EQ(route->mType, Diagnostics::CapsuleEntryType::GameplayTrace);
        EXPECT_EQ(Diagnostics::decodeGameplayRoute(route->mData), frames);
        EXPECT_THROW(Diagnostics::withGameplayRoute(result, payload), std::invalid_argument);
        EXPECT_ANY_THROW(Diagnostics::withGameplayRoute(source, {}));
        EXPECT_EQ(source.entries().size(), 1u);
    }

    TEST(GameplayRoute, RejectsEmptyOversizedAndNonFiniteRoutes)
    {
        EXPECT_THROW(Diagnostics::encodeGameplayRoute({}), std::length_error);
        std::vector oversized(Diagnostics::maximumGameplayRouteFrames + 1, frame());
        EXPECT_THROW(Diagnostics::encodeGameplayRoute(oversized), std::length_error);
        auto invalid = frame();
        invalid.mPosition[1] = std::numeric_limits<float>::infinity();
        EXPECT_THROW(Diagnostics::encodeGameplayRoute(std::span(&invalid, 1)), std::invalid_argument);
    }

    TEST(GameplayRoute, RejectsUnsupportedOrMalformedPayloads)
    {
        const std::string unsupported = "route-version: 99\nframes: []\n";
        const auto* begin = reinterpret_cast<const std::byte*>(unsupported.data());
        EXPECT_THROW(Diagnostics::decodeGameplayRoute(std::span(begin, unsupported.size())), std::invalid_argument);
    }

    TEST(GameplayRoute, AcceptsTheMaximumFrameDurationExactly)
    {
        const std::vector source{ [] {
            Diagnostics::GameplayRouteFrame value = frame();
            value.mDuration = Diagnostics::maximumGameplayRouteFrameDuration;
            return value;
        }() };
        EXPECT_EQ(Diagnostics::decodeGameplayRoute(Diagnostics::encodeGameplayRoute(source)), source);
    }
}
