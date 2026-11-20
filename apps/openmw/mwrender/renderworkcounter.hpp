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
    inline constexpr std::string_view transparentClassifiedLeafCounter = "Diagnostic Transparent Classified Leaves";
    inline constexpr std::string_view meshWaterLeafCounter = "Diagnostic Mesh Water Leaves";
    inline constexpr std::string_view meshWaterPrimitiveCounter = "Diagnostic Mesh Water Primitives";
    inline constexpr std::string_view meshWaterDrawCounter = "Diagnostic Mesh Water Draws";
    inline constexpr std::string_view meshWaterReflectionActiveCounter = "Diagnostic Mesh Water Reflection Active";
    inline constexpr std::string_view meshWaterReflectionHeightCounter = "Diagnostic Mesh Water Reflection Height";
    inline constexpr std::string_view meshWaterReflectionResolutionCounter
        = "Diagnostic Mesh Water Reflection Resolution";
    inline constexpr std::string_view planarReflectionSourceResolutionCounter
        = "Diagnostic Planar Reflection Source Resolution";
    inline constexpr std::string_view planarReflectionReceiverCountCounter
        = "Diagnostic Planar Reflection Receiver Count";
    inline constexpr std::string_view planarReflectionAnchorIdCounter = "Diagnostic Planar Reflection Anchor Id";
    inline constexpr std::string_view planarReflectionAnchorChangeCounter
        = "Diagnostic Planar Reflection Anchor Changes";
    inline constexpr std::string_view planarReflectionAnchorImpactCounter
        = "Diagnostic Planar Reflection Anchor Impact";
    inline constexpr std::string_view planarReflectionComparisonIdCounter
        = "Diagnostic Planar Reflection Comparison Id";
    inline constexpr std::string_view planarReflectionComparisonImpactCounter
        = "Diagnostic Planar Reflection Comparison Impact";
    inline constexpr std::string_view planarReflectionProxyDrawCounter = "Diagnostic Planar Reflection Proxy Draws";
    inline constexpr std::string_view planarReflectionSourcePlaneDistanceCounter
        = "Diagnostic Planar Reflection Source Plane Distance";
    inline constexpr std::string_view planarReflectionHandoffStateCounter
        = "Diagnostic Planar Reflection Handoff State";
    inline constexpr std::string_view planarReflectionPriorWeightCounter = "Diagnostic Planar Reflection Prior Weight";
    inline constexpr std::string_view firstPersonReplayCounter = "Diagnostic First Person Replay Drawables";
    inline constexpr std::string_view groundcoverInstanceTestCounter = "Diagnostic Groundcover Instances Tested";
    inline constexpr std::string_view groundcoverInstanceRenderCounter = "Diagnostic Groundcover Instances Rendered";
    inline constexpr std::string_view groundcoverBatchRenderCounter = "Diagnostic Groundcover Batches Rendered";
    inline constexpr std::string_view groundcoverDrawableRenderCounter = "Diagnostic Groundcover Drawables Rendered";
    inline constexpr std::string_view groundcoverConservativeInstanceTestCounter
        = "Diagnostic Groundcover Conservative Instances Tested";
    inline constexpr std::string_view groundcoverConservativeInstanceRenderCounter
        = "Diagnostic Groundcover Conservative Instances Rendered";
    /// Initialize diagnostic rendering work counters for a frame when scene statistics are requested.
    void resetDiagnosticRenderWorkCounters(osg::Stats& stats, unsigned frameNumber);

    /// Add one value to a diagnostic rendering work counter when scene statistics are requested.
    void addDiagnosticRenderWorkCounter(
        osg::Stats& stats, unsigned frameNumber, std::string_view counter, double value);
}

#endif
