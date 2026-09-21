#include <gtest/gtest.h>

#include <components/widgets/box.hpp>

namespace
{
    TEST(ResponsiveWidth, UsesAllCompactSpaceThenPlateausAndGrows)
    {
        EXPECT_EQ(Gui::getResponsiveStretchWidth(500, 0.7f, 640), 500);
        EXPECT_EQ(Gui::getResponsiveStretchWidth(640, 0.7f, 640), 640);
        EXPECT_EQ(Gui::getResponsiveStretchWidth(800, 0.7f, 640), 640);
        EXPECT_EQ(Gui::getResponsiveStretchWidth(1000, 0.7f, 640), 700);
    }

    TEST(ResponsiveWidth, NeverShrinksAsAvailableWidthGrows)
    {
        int previous = 0;
        for (int available = 0; available <= 4096; ++available)
        {
            const int width = Gui::getResponsiveStretchWidth(available, 0.7f, 640);
            EXPECT_GE(width, previous);
            EXPECT_LE(width, available);
            previous = width;
        }
    }
}
