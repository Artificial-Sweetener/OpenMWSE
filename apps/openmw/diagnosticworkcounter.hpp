#ifndef OPENMW_DIAGNOSTICWORKCOUNTER_H
#define OPENMW_DIAGNOSTICWORKCOUNTER_H

#include <span>
#include <string_view>

namespace OMW
{
    /// Identifies the OSG statistics store that owns a diagnostic work counter.
    enum class DiagnosticStatisticOwner
    {
        Viewer,
        PrimaryCamera,
    };

    /// Maps one stable performance-provider counter to its subsystem-owned OSG statistic.
    struct DiagnosticWorkCounter
    {
        std::string_view mName;
        std::string_view mStatistic;
        DiagnosticStatisticOwner mOwner = DiagnosticStatisticOwner::Viewer;
    };

    /// Return the bounded performance-provider counter schema in stable output order.
    std::span<const DiagnosticWorkCounter> diagnosticWorkCounters() noexcept;
}

#endif
