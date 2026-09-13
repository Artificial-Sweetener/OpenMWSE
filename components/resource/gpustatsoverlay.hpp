#ifndef OPENMW_COMPONENTS_RESOURCE_GPUSTATSOVERLAY_H
#define OPENMW_COMPONENTS_RESOURCE_GPUSTATSOVERLAY_H

#include <osgText/Font>
#include <osgViewer/ViewerBase>

namespace osg
{
    class Camera;
    class Geode;
    class Stats;
    class Switch;
}

namespace Resource
{
    /// Extend OSG's profiler overlay with complete GPU-scene counts and timing ranges.
    void configureGpuStatsOverlay(osg::Switch& overlay, unsigned int cameraSceneChild, osg::Geode& timelineBars,
        osg::Camera& camera, const osgViewer::ViewerBase::Cameras& cameras, osg::Stats* viewerStats,
        osgText::Font* font, double& blockMultiplier, unsigned int frames, float timelineWidth, float characterSize);
}

#endif
