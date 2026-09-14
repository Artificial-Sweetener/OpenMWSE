#ifndef OPENMW_MWRENDER_PAGEDREF_H
#define OPENMW_MWRENDER_PAGEDREF_H

#include <tuple>

#include <components/esm/exteriorcelllocation.hpp>
#include <components/esm3/refnum.hpp>

namespace MWRender
{
    /// Identifies one paged exterior reference without assuming reference numbers are unique across cells.
    struct PagedRefKey
    {
        ESM::RefNum mRefNum;
        ESM::ExteriorCellLocation mCell;

        /// Compare the complete identity used by paged-renderer state.
        friend bool operator==(const PagedRefKey&, const PagedRefKey&) = default;

        /// Order paged references deterministically for maps, sets, and scene handoff lists.
        friend bool operator<(const PagedRefKey& lhs, const PagedRefKey& rhs)
        {
            return std::tie(lhs.mRefNum, lhs.mCell) < std::tie(rhs.mRefNum, rhs.mCell);
        }
    };
}

#endif
