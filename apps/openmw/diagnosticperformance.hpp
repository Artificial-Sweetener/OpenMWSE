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

    /// Emit bounded performance@9 presentation, camera, composite, and retained OSG statistics.
    void reportDiagnosticPerformance(
        unsigned firstFrame, unsigned lastFrame, osgViewer::Viewer& viewer, std::ostream& output);
}

#endif
