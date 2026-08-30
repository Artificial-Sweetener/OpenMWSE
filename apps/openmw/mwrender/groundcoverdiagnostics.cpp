#include "groundcoverdiagnostics.hpp"

#include "renderworkcounter.hpp"

#include <osg/Array>
#include <osg/BoundingSphere>
#include <osg/Camera>
#include <osg/Stats>
#include <osgUtil/CullVisitor>

#include <algorithm>
namespace MWRender
{
    void recordDiagnosticGroundcoverVisibility(osgUtil::CullVisitor& visitor, const osg::Vec4Array& transforms,
        float modelExtent, float viewDistance, std::span<const GroundcoverInstanceRange> visibleClusterRanges)
    {
        osg::Camera* camera = visitor.getCurrentCamera();
        osg::Stats* stats = camera ? camera->getStats() : nullptr;
        if (stats == nullptr || !stats->collectStats("scene"))
            return;

        std::size_t tested = 0;
        std::size_t visible = 0;
        const osg::Vec3f eyePoint = visitor.getEyePoint();
        for (const GroundcoverInstanceRange& range : visibleClusterRanges)
        {
            if (range.mFirst >= transforms.size())
                continue;
            const std::size_t end = range.mFirst + std::min(range.mCount, transforms.size() - range.mFirst);
            for (std::size_t index = range.mFirst; index < end; ++index)
            {
                const osg::Vec4f& transform = transforms[index];
                const float radius = modelExtent * transform.w() * 1.1f;
                const osg::BoundingSphere bounds(osg::Vec3f(transform.x(), transform.y(), transform.z()), radius);
                ++tested;
                if (std::max(0.f, (bounds.center() - eyePoint).length() - radius) > viewDistance
                    || visitor.isCulled(bounds))
                    continue;
                ++visible;
            }
        }

        const unsigned frame = visitor.getTraversalNumber();
        addDiagnosticRenderWorkCounter(
            *stats, frame, groundcoverConservativeInstanceTestCounter, static_cast<double>(tested));
        addDiagnosticRenderWorkCounter(
            *stats, frame, groundcoverConservativeInstanceRenderCounter, static_cast<double>(visible));
    }
}
