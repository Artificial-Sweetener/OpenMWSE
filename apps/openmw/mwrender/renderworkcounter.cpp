#include "renderworkcounter.hpp"

#include <array>
#include <string>

#include <osg/Stats>

namespace MWRender
{
    void resetDiagnosticRenderWorkCounters(osg::Stats& stats, unsigned frameNumber)
    {
        if (!stats.collectStats("scene"))
            return;

        constexpr std::array counters{
            colorCopyCounter,
            depthCopyCounter,
            resolveByteCounter,
            transparentReplayCounter,
            transparentClassifiedLeafCounter,
            firstPersonReplayCounter,
        };
        for (const std::string_view counter : counters)
            stats.setAttribute(frameNumber, std::string(counter), 0.0);
    }

    void addDiagnosticRenderWorkCounter(
        osg::Stats& stats, unsigned frameNumber, std::string_view counter, double value)
    {
        if (!stats.collectStats("scene"))
            return;

        double current = 0.0;
        stats.getAttribute(frameNumber, std::string(counter), current);
        stats.setAttribute(frameNumber, std::string(counter), current + value);
    }
}
