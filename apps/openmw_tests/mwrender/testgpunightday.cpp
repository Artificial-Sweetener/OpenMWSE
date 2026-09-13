#include <apps/openmw/mwrender/gpuscene.hpp>

#include <gtest/gtest.h>

namespace
{
    using MWRender::GpuScene;

    // Matches MWWorld::NightDayMode order.
    constexpr unsigned int sDefault = 1u << 0;
    constexpr unsigned int sNight = 1u << 1;
    constexpr unsigned int sInterior = 1u << 2;

    TEST(GpuNightDaySwitch, AssignsChildModes)
    {
        EXPECT_EQ(GpuScene::switchModes(3, 0), sDefault);
        EXPECT_EQ(GpuScene::switchModes(3, 1), sNight);
        EXPECT_EQ(GpuScene::switchModes(3, 2), sInterior);
    }

    TEST(GpuNightDaySwitch, FoldsInteriorDay)
    {
        EXPECT_EQ(GpuScene::switchModes(2, 0), sDefault | sInterior);
        EXPECT_EQ(GpuScene::switchModes(2, 1), sNight);
    }

    TEST(GpuNightDaySwitch, AssignsSoleChild)
    {
        EXPECT_EQ(GpuScene::switchModes(1, 0), sDefault | sNight | sInterior);
    }

    TEST(GpuNightDaySwitch, SkipsSpareChildren)
    {
        EXPECT_EQ(GpuScene::switchModes(4, 3), 0u);
        EXPECT_EQ(GpuScene::switchModes(3, 7), 0u);
    }

    TEST(GpuNightDaySwitch, CoversEachModeOnce)
    {
        for (unsigned int count = 1; count <= 5; ++count)
        {
            unsigned int seen = 0;
            for (unsigned int child = 0; child < count; ++child)
            {
                const unsigned int modes = GpuScene::switchModes(count, child);
                EXPECT_EQ(seen & modes, 0u) << "count " << count << " child " << child;
                seen |= modes;
            }
            EXPECT_EQ(seen, sDefault | sNight | sInterior) << "count " << count;
        }
    }
}
