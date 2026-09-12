#include "deformationstats.hpp"

#include <array>
#include <string>

#include <osg/Camera>
#include <osg/NodeVisitor>
#include <osg/Stats>
#include <osg/Timer>

#include <osgUtil/CullVisitor>

namespace SceneUtil
{
    namespace
    {
        /// Add one value to a scene statistic owned by the current camera.
        void add(osg::Stats& stats, unsigned int frameNumber, std::string_view counter, double value)
        {
            double current = 0.0;
            stats.getAttribute(frameNumber, std::string(counter), current);
            stats.setAttribute(frameNumber, std::string(counter), current + value);
        }
    }

    DeformationStats::DeformationStats(osg::NodeVisitor& visitor)
        : mFrameNumber(visitor.getTraversalNumber())
    {
        if (visitor.getVisitorType() != osg::NodeVisitor::CULL_VISITOR)
            return;

        osgUtil::CullVisitor& cullVisitor = static_cast<osgUtil::CullVisitor&>(visitor);
        if (cullVisitor.getCurrentRenderBin() == nullptr)
            return;
        osg::Camera* camera = cullVisitor.getCurrentCamera();
        if (!camera)
            return;

        mStats = camera->getStats();
        if (!mStats || !mStats->collectStats("scene"))
        {
            mStats = nullptr;
            return;
        }
        mStartSeconds = osg::Timer::instance()->time_s();
    }

    void DeformationStats::record(DeformationPath path, std::size_t vertices, std::size_t uploadBytes) const
    {
        if (!mStats)
            return;

        switch (path)
        {
            case DeformationPath::NativeSkinning:
                add(*mStats, mFrameNumber, skinningVertexCounter, static_cast<double>(vertices));
                add(*mStats, mFrameNumber, nativeSkinningVertexCounter, static_cast<double>(vertices));
                break;
            case DeformationPath::OsgaSkinning:
                add(*mStats, mFrameNumber, skinningVertexCounter, static_cast<double>(vertices));
                add(*mStats, mFrameNumber, osgaSkinningVertexCounter, static_cast<double>(vertices));
                break;
            case DeformationPath::Morphing:
                add(*mStats, mFrameNumber, morphVertexCounter, static_cast<double>(vertices));
                break;
        }
        add(*mStats, mFrameNumber, deformationUploadByteCounter, static_cast<double>(uploadBytes));
        add(*mStats, mFrameNumber, deformationCpuMillisecondCounter,
            (osg::Timer::instance()->time_s() - mStartSeconds) * 1000.0);
    }

    void resetDiagnosticDeformationCounters(osg::Stats& stats, unsigned int frameNumber)
    {
        if (!stats.collectStats("scene"))
            return;

        constexpr std::array counters{
            skinningVertexCounter,
            nativeSkinningVertexCounter,
            osgaSkinningVertexCounter,
            morphVertexCounter,
            deformationUploadByteCounter,
            deformationCpuMillisecondCounter,
        };
        for (const std::string_view counter : counters)
            stats.setAttribute(frameNumber, std::string(counter), 0.0);
    }
}
