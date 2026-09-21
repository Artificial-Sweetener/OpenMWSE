#include <components/settings/distance.hpp>

#include <gtest/gtest.h>

TEST(DistanceSettingsTest, FractionalCellsProduceCanonicalWorldUnits)
{
    EXPECT_EQ(Settings::parseDistance("3.5", Settings::DistanceUnits::Cells), 28672.f);
    EXPECT_EQ(Settings::parseDistance("28672", Settings::DistanceUnits::WorldUnits), 28672.f);
    EXPECT_EQ(Settings::formatDistance(28672.f, Settings::DistanceUnits::Cells), "3.5");
}

TEST(DistanceSettingsTest, BothPresentationsRoundTripBeyondSliderLimits)
{
    for (const float value : { 0.f, 0.1f, 7168.125f, 1234567.f, std::numeric_limits<float>::denorm_min(),
             std::numeric_limits<float>::max() })
        for (const auto units : { Settings::DistanceUnits::Cells, Settings::DistanceUnits::WorldUnits })
            EXPECT_EQ(Settings::parseDistance(Settings::formatDistance(value, units), units), value);
}

TEST(DistanceSettingsTest, CompactDisplayPreservesCanonicalFloatPrecision)
{
    EXPECT_EQ(Settings::formatDistance(0.1f, Settings::DistanceUnits::WorldUnits), "0.1");
    const auto cells = Settings::formatDistance(123456.5f, Settings::DistanceUnits::Cells);
    EXPECT_LE(cells.size(), 10u);
    EXPECT_EQ(Settings::parseDistance(cells, Settings::DistanceUnits::Cells), 123456.5f);
}

TEST(DistanceSettingsTest, RejectsInvalidOrUnrepresentableInput)
{
    for (const auto text : { "", " ", "nan", "inf", "-1", "3.5 cells", "1.2.3", "1e400", "1e-100" })
        EXPECT_FALSE(Settings::parseDistance(text, Settings::DistanceUnits::Cells)) << text;
    EXPECT_FALSE(Settings::parseDistance("3.4028234663852886e38", Settings::DistanceUnits::Cells));
    EXPECT_EQ(Settings::parseDistance("  3.5\t", Settings::DistanceUnits::Cells), 28672.f);
}

TEST(DistanceSettingsTest, StepsBothUnitsWithoutImposingAnOpenmwSliderMaximum)
{
    using Settings::DistanceUnits;
    EXPECT_EQ(Settings::stepDistance(28672.f, DistanceUnits::Cells, true, true), 32768.f);
    EXPECT_EQ(Settings::stepDistance(28672.f, DistanceUnits::Cells, false, true), 24576.f);
    EXPECT_EQ(Settings::stepDistance(123456.5f, DistanceUnits::WorldUnits, true, false), 123457.5f);
    EXPECT_EQ(Settings::stepDistance(123456.5f, DistanceUnits::WorldUnits, false, false), 123455.5f);
    EXPECT_EQ(Settings::stepDistance(81920.f, DistanceUnits::Cells, true, false), 86016.f);
}

TEST(DistanceSettingsTest, StepsRespectZeroAndFiniteStorageBoundaries)
{
    using Settings::DistanceUnits;
    EXPECT_EQ(Settings::stepDistance(0.f, DistanceUnits::Cells, false, true), 0.f);
    EXPECT_EQ(Settings::stepDistance(0.5f, DistanceUnits::WorldUnits, false, true), 0.f);
    EXPECT_EQ(Settings::stepDistance(0.5f, DistanceUnits::WorldUnits, false, false), 0.5f);
    EXPECT_EQ(Settings::stepDistance(1.f, DistanceUnits::Cells, false, false), 1.f);
    const float largest = std::numeric_limits<float>::max();
    EXPECT_EQ(Settings::stepDistance(largest, DistanceUnits::WorldUnits, true, false), largest);
    EXPECT_LT(Settings::stepDistance(largest, DistanceUnits::WorldUnits, false, false), largest);
}

TEST(DistanceSettingsTest, UnlockedFogEndpointsRetainTheirViewingDistanceRatios)
{
    const Settings::FogDistanceRange range{ 8192.f, 12288.f };
    EXPECT_EQ(Settings::scaleFogDistanceRange(range, 16384.f, 32768.f, false, false),
        (Settings::FogDistanceRange{ 16384.f, 24576.f }));
}

TEST(DistanceSettingsTest, FogEndpointLocksAreIndependent)
{
    const Settings::FogDistanceRange range{ 8192.f, 12288.f };
    EXPECT_EQ(Settings::scaleFogDistanceRange(range, 16384.f, 32768.f, true, false),
        (Settings::FogDistanceRange{ 8192.f, 24576.f }));
    EXPECT_EQ(Settings::scaleFogDistanceRange(range, 16384.f, 8192.f, false, true),
        (Settings::FogDistanceRange{ 4096.f, 12288.f }));
    EXPECT_EQ(Settings::scaleFogDistanceRange(range, 16384.f, 32768.f, true, true), range);
}

TEST(DistanceSettingsTest, MixedFogLocksPreserveStrictEndpointOrdering)
{
    const Settings::FogDistanceRange range{ 8192.f, 12288.f };
    const auto lockedEnd = Settings::scaleFogDistanceRange(range, 16384.f, 32768.f, false, true);
    EXPECT_LT(lockedEnd.mStart, lockedEnd.mEnd);
    EXPECT_EQ(lockedEnd.mEnd, range.mEnd);

    const auto lockedStart = Settings::scaleFogDistanceRange(range, 16384.f, 4096.f, true, false);
    EXPECT_LT(lockedStart.mStart, lockedStart.mEnd);
    EXPECT_EQ(lockedStart.mStart, range.mStart);
}

#include <components/settings/shadowresolution.hpp>

TEST(ShadowResolutionSettingsTest, PreservesWholePixelValuesAndRejectsUnsupportedInput)
{
    EXPECT_EQ(Settings::parseShadowResolution(" 2304 ", 16384), 2304);
    EXPECT_EQ(Settings::parseShadowResolution("1537", 16384), 1537);
    EXPECT_EQ(Settings::parseShadowResolution("16384", 16384), 16384);
    for (const auto text : { "0", "-1", "1.5", "nan", "1e3", "16385", "2147483648", "", "1024px" })
        EXPECT_FALSE(Settings::parseShadowResolution(text, 16384)) << text;
    EXPECT_FALSE(Settings::parseShadowResolution("32768", 65536));
    EXPECT_FALSE(Settings::parseShadowResolution("1", 0));
}

TEST(ShadowResolutionSettingsTest, StepsBy256WithinHardwareAndRendererLimits)
{
    EXPECT_EQ(Settings::stepShadowResolution(2048, true, 16384), 2304);
    EXPECT_EQ(Settings::stepShadowResolution(2048, false, 16384), 1792);
    EXPECT_EQ(Settings::stepShadowResolution(1537, true, 16384), 1793);
    EXPECT_EQ(Settings::stepShadowResolution(128, false, 16384), 1);
    EXPECT_EQ(Settings::stepShadowResolution(16384, true, 16384), 16384);
    EXPECT_EQ(Settings::stepShadowResolution(32512, true, 65536), 32767);
    EXPECT_EQ(Settings::stepShadowResolution(std::numeric_limits<int>::max(), true, 16384), 16384);
    EXPECT_EQ(Settings::stepShadowResolution(2048, true, 0), 2048);
}
