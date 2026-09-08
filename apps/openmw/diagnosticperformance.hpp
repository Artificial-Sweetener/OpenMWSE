#ifndef OPENMW_DIAGNOSTICPERFORMANCE_H
#define OPENMW_DIAGNOSTICPERFORMANCE_H

#include <cstddef>
#include <iosfwd>
#include <span>

namespace osgViewer
{
    class Viewer;
}

namespace OMW
{
    struct DiagnosticRouteFrame;

    /// Summarizes one bounded series without hiding unavailable samples.
    struct DiagnosticDistribution
    {
        std::size_t mCount = 0;
        double mMedian = 0.0;
        double mP95 = 0.0;
        double mP99 = 0.0;
        double mWorst = 0.0;
    };

    /// Calculate deterministic linear-interpolated quantiles for one non-empty series.
    DiagnosticDistribution summarizeDiagnosticSamples(std::span<const double> samples);

    /// Emit the renderer interval for each consumed route state so loading draws retain their actual identities.
    void reportDiagnosticRouteFrames(std::span<const DiagnosticRouteFrame> frames, std::ostream& output);

    /// Emit bounded performance@15 query lifecycles, presentation, camera, light-state, and retained OSG statistics.
    void reportDiagnosticPerformance(
        unsigned firstFrame, unsigned lastFrame, osgViewer::Viewer& viewer, std::ostream& output);
}

#endif
