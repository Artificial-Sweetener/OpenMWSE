#include <apps/openmw/mwrender/pagedref.hpp>

#include <gtest/gtest.h>

#include <map>

TEST(PagedRefKeyTest, PreservesRepeatedReferenceNumbersAcrossExteriorCells)
{
    const ESM::RefNum repeatedRef{ 1, 2 };
    const MWRender::PagedRefKey first{
        repeatedRef,
        ESM::ExteriorCellLocation(-196, -77, ESM::Cell::sDefaultWorldspaceId),
    };
    const MWRender::PagedRefKey second{
        repeatedRef,
        ESM::ExteriorCellLocation(-195, -77, ESM::Cell::sDefaultWorldspaceId),
    };

    std::map<MWRender::PagedRefKey, int> refs;
    refs.insert_or_assign(first, 1);
    refs.insert_or_assign(first, 2);
    refs.insert_or_assign(second, 3);

    ASSERT_EQ(refs.size(), 2);
    EXPECT_EQ(refs.at(first), 2);
    EXPECT_EQ(refs.at(second), 3);
}

TEST(PagedRefKeyTest, DistinguishesWorldspacesAtTheSameCoordinates)
{
    const ESM::RefNum repeatedRef{ 1, 2 };
    const MWRender::PagedRefKey defaultWorldspace{
        repeatedRef,
        ESM::ExteriorCellLocation(-196, -77, ESM::Cell::sDefaultWorldspaceId),
    };
    const MWRender::PagedRefKey otherWorldspace{
        repeatedRef,
        ESM::ExteriorCellLocation(-196, -77, ESM::RefId::stringRefId("other")),
    };

    EXPECT_NE(defaultWorldspace, otherWorldspace);

    std::map<MWRender::PagedRefKey, int> refs;
    refs.emplace(defaultWorldspace, 1);
    refs.emplace(otherWorldspace, 2);
    EXPECT_EQ(refs.size(), 2);
}
