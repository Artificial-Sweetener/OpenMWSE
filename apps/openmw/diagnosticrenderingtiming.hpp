#ifndef OPENMW_APPS_OPENMW_DIAGNOSTICRENDERINGTIMING_H
#define OPENMW_APPS_OPENMW_DIAGNOSTICRENDERINGTIMING_H

#include <span>
#include <string_view>

namespace OMW
{
    /// Identifies one stable render-bin GPU interval in diagnostic output.
    struct DiagnosticRenderBinPass
    {
        std::string_view mName;
        std::string_view mPrototype;
        int mBinNumber;
        std::string_view mGpuStatistic;
        std::string_view mCpuStatistic;
    };

    /// Return the stable render-bin intervals emitted by performance diagnostics.
    std::span<const DiagnosticRenderBinPass> diagnosticRenderBinPasses();

    /// Enable delayed GPU timestamps on available render-bin prototypes.
    void enableDiagnosticRenderBinTimings();
}

#endif
