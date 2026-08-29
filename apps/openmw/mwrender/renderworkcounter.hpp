#ifndef OPENMW_MWRENDER_RENDERWORKCOUNTER_H
#define OPENMW_MWRENDER_RENDERWORKCOUNTER_H

#include <string_view>

namespace osg
{
    class Stats;
}

namespace MWRender
{
    inline constexpr std::string_view colorCopyCounter = "Diagnostic Color Copies";
    inline constexpr std::string_view depthCopyCounter = "Diagnostic Depth Copies";
    inline constexpr std::string_view resolveByteCounter = "Diagnostic Resolve Bytes";
    inline constexpr std::string_view transparentReplayCounter = "Diagnostic Transparent Replay Drawables";
    inline constexpr std::string_view firstPersonReplayCounter = "Diagnostic First Person Replay Drawables";

    /// Initialize post-processing work counters for a frame when scene statistics are requested.
    void resetPostProcessingWorkCounters(osg::Stats& stats, unsigned frameNumber);

    /// Add one value to a post-processing work counter when scene statistics are requested.
    void addPostProcessingWorkCounter(
        osg::Stats& stats, unsigned frameNumber, std::string_view counter, double value);
}

#endif
