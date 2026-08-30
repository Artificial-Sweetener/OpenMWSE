#ifndef OPENMW_COMPONENTS_SDLUTIL_DIAGNOSTICPRESENTATIONTIMING_H
#define OPENMW_COMPONENTS_SDLUTIL_DIAGNOSTICPRESENTATIONTIMING_H

#include <string_view>

namespace osg
{
    class Stats;
}

namespace SDLUtil
{
    /// Names the wall-clock time spent presenting one completed frame.
    inline constexpr std::string_view diagnosticSwapBufferTimeStatistic = "OpenMW diagnostic swap buffers time taken";

    /// Direct bounded presentation timings to the supplied viewer statistics owner.
    void setDiagnosticPresentationTimingStatistics(osg::Stats* stats);

    /// Return whether bounded presentation timing is currently requested.
    bool diagnosticPresentationTimingCollectionEnabled() noexcept;

    /// Record the wall-clock presentation duration for one frame.
    void recordDiagnosticSwapBufferTime(unsigned frameNumber, double seconds);
}

#endif
