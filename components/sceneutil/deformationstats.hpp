#ifndef OPENMW_COMPONENTS_SCENEUTIL_DEFORMATIONSTATS_H
#define OPENMW_COMPONENTS_SCENEUTIL_DEFORMATIONSTATS_H

#include <cstddef>
#include <string_view>

namespace osg
{
    class NodeVisitor;
    class Stats;
}

namespace SceneUtil
{
    inline constexpr std::string_view skinningVertexCounter = "Diagnostic Skinning Vertices";
    inline constexpr std::string_view nativeSkinningVertexCounter = "Diagnostic Native Skinning Vertices";
    inline constexpr std::string_view osgaSkinningVertexCounter = "Diagnostic OSGA Skinning Vertices";
    inline constexpr std::string_view morphVertexCounter = "Diagnostic Morph Vertices";
    inline constexpr std::string_view deformationUploadByteCounter = "Diagnostic Upload Bytes";
    inline constexpr std::string_view deformationCpuMillisecondCounter = "Diagnostic Deformation CPU Milliseconds";

    /// Identifies the CPU deformation implementation that produced diagnostic work.
    enum class DeformationPath
    {
        NativeSkinning,
        OsgaSkinning,
        Morphing,
    };

    /// Measure and attribute one diagnostic-only CPU deformation operation.
    class DeformationStats
    {
    public:
        /// Start timing when the current camera requests scene statistics.
        explicit DeformationStats(osg::NodeVisitor& visitor);

        /// Record completed deformation work and its dirty-array upload footprint.
        void record(DeformationPath path, std::size_t vertices, std::size_t uploadBytes) const;

    private:
        osg::Stats* mStats = nullptr;
        unsigned int mFrameNumber = 0;
        double mStartSeconds = 0.0;
    };

    /// Initialize all diagnostic deformation counters for one camera frame.
    void resetDiagnosticDeformationCounters(osg::Stats& stats, unsigned int frameNumber);
}

#endif
