#ifndef OPENMW_APPS_OPENMW_DIAGNOSTICRENDERINGTIMING_H
#define OPENMW_APPS_OPENMW_DIAGNOSTICRENDERINGTIMING_H

#include <optional>
#include <span>
#include <string_view>

namespace OMW
{
    /// Identify a stable render-bin interval and its camera owner in diagnostic output.
    struct DiagnosticRenderBinPass
    {
        std::string_view mName;
        std::string_view mPrototype;
        int mBinNumber;
        std::string_view mGpuStatistic;
        std::string_view mCpuStatistic;
        /// Select the viewer's primary camera when no dependent camera name is supplied.
        std::optional<std::string_view> mCameraName;
    };

    /// Return the stable render-bin intervals emitted by performance diagnostics.
    std::span<const DiagnosticRenderBinPass> diagnosticRenderBinPasses();

    /// Enable delayed GPU timestamps on available render-bin prototypes.
    void enableDiagnosticRenderBinTimings();
}

#endif
